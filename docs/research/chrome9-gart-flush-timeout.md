# Chrome9 GART/GTI flush timeout after long GL2 runs

Status: static root-cause analysis (no target access).  Vendor addresses are
analysis VMAs in `VTGKModeDX32A.sys` (ImageBase `0x10000`, SHA-256
`618129602e2f…0a56`), disassembled with `objdump -M intel`.  Driver citations
are against
`src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/chrome9/`.
This extends [chrome9-gart-index-race-audit.md](chrome9-gart-index-race-audit.md);
the SEQ-index race described there is already fixed by `cdev->seq_lock`
(`chrome9_drv.h:140-154`) and is not the cause of the failures below.

## Observed failure

```text
GART translation flush failed first=401 count=25 ret=-110 SR6F=80
SF draw-cache eviction failed; mapping retained for reboot
(earlier runs) GART pre-write idle failed ... SR6F=80
(earlier runs) SF draw-cache bind cleanup failed
```

`chrome9_gart_set()` (`chrome9_engine.c:3108-3159`) wrote the PTEs, requested
`SR6F.bit7`, restored `SR6C.bit7`, and the bounded post-wait
(`chrome9_gart_wait_idle()`, `chrome9_engine.c:431-439`, 100 ms, 10-µs polls)
never saw bit 7 clear.  The pre-wait and bind-cleanup failures are the same
condition seen on the next transaction.  The engine was idle when the flush
was requested: every GART update runs under `submit_lock` after the previous
job has been retired by cookie, `0444.bit31` and `0400 & 00101fe3 == 0`
(`chrome9_wait_job()`, `chrome9_engine.c:4042-4100`).  The next job is built
only after `chrome9_gart_set()` returns (`chrome9_submit_draw()`,
`chrome9_engine.c:4487`).

## Vendor protocol, recovered

### Manual (VX900 Part I, internal pp. 36, 46-48)

| register | definition |
|---|---|
| `SR3B` | PCI Revision ID back door |
| `SR3F` | clock control: `[7:6]` CR, `[5:4]` 3D, `[3:2]` 2D, `[1:0]` DVD; `0x`=off, `10`=always on, `11`=gated by engine idle; reset default `AAh` |
| `SR66/67/69`, `SR79/7A/7C` | GTI/3D/SL-SF/CBU "request kill number" arbitration counters, default `C8h` |
| `SR6A`, `SR6B`, `SR6C[0]` | RTSF (GART table) base `[19:12]`, `[27:20]`, `[28]` in SL |
| `SR6C[7]` | *GART Table Write Protect Enable* - "enable to avoid hardware write" |
| `SR6C[6:5]` | GARB/GTI `SAMPLE_GFX1DATA/GFX0DATA` (undocumented function) |
| `SR6D`, `SR6E`, `SR6F[6:0]` | SL base in system memory `[28:21]`, `[36:29]`, `[43:37]` |
| `SR6F[7]` | *GTI Cache Flush* - "set by SW and reset by GTI" |

The manual gives no completion condition, no idle precondition and no
timeout.  The target reads `SR3F=ff` (all engine clocks idle-gated, not the
`AAh` reset default), `SR6C=00`, `SR6F=00` before driver load, PCI revision
`00` (`hardware-lab.md:8,103-104`).

### VIA Linux 5.75 KMD (`via_chrome9_dma.c`)

Only initialization and SG allocation update the table
(`via_chrome9_dma.c:112-156,256-301,430-454`).  Sequence: clear `SR6C.bit7`,
select `SR6F`, spin until bit 7 is clear, write PTEs, write `SR6F|80`,
immediately set `SR6C.bit7`.  There is no post-wait, no timeout and no
runtime map/unmap while rendering, so it does not exercise the failure case.

### Windows WDDM KMD (the only vendor path with live updates)

**Revision gating** (adapter init, VX900 `0x7122` branch at
`0x12233b..0x1225a0`; `+0x79a` is `SR3B`, read at `0x112833..0x11285d`):

| VX900 revision | action |
|---|---|
| `<= 1` | `SR3F = (SR3F & 3f) | 80` - **CR clock forced always-on** (`0x12234a..0x122374`; again in CR init at `0x3b240..0x3b278` before programming `041c`) |
| `<= 2` | flags `+0x9c8 |= 0xc000`: `WBINVD` (+`MFENCE` on unmap) and two port-`0x81` delays before the flush write |
| `== 3` only | flag `+0x9cc |= 4`: single flush write followed by the completion helper |
| all | flag `+0x9c0 |= 0x400000`: allow the dummy-2D kick in the completion helper |

**SEQ access helpers**: read `0x1ad10` writes the index byte and reads a
16-bit word from `3C4`, retrying up to three times until the returned index
matches; write `0x1adc0` issues one 16-bit `OUT 3C4, (data<<8)|index`, so
index and data are written atomically.

**Map** (`0x2ddf0..0x2e0cd`): read `SR6F` once and OR `80` (no pre-wait,
`0x2de49..0x2de61`); write PFN`|02000000` PTEs; optional `WBINVD`
(`0x2dfe6..0x2e00c`); write the flush byte (`0x2e013`); then either call the
completion helper (rev 3, `0x2e030`, **return value ignored**) or write the
same byte nine more times with no polling (`0x2e03a..0x2e0c5`, ten writes
total).

**Unmap** (`0x2db10..0x2ddda`): same single `SR6F` sample.  With a dummy
page address it writes `dummyPFN|02000000` and residency `1`
(`0x2db6e..0x2dbc6`), otherwise `80000000` (`0x2dce7..0x2dd22`).  Then one
flush write (rev 3, no completion wait) or twelve writes total.  The
dummy-page caller is `S3gBuildPagingBuffer` *UnMap Aperture* (`0xd61ca`,
string at `0xecd14`); the `80000000` caller is allocation destruction
(`0x2e871`, dummy argument zero).  WDDM paging evictions therefore redirect
PTEs to a valid scratch page rather than invalidating them.

**Ring kick** (`0xb8d49..0xb8eb1`): under flag `+0x9c8.bit24` (not set in
the VX900 branch) the KMD issues twelve `SR6F|80` writes right after the
`A64/A65` publication, i.e. with the command reader running.  A flush is
therefore not required to be issued with the engine idle.

**SR6C**: no immediate-index write sets `SR6C.bit7`; initialization
(`0x2e27f..0x2e2ad`) only ORs base bit 28 into the firmware value.  With the
documented default `00` Windows runs with GART write protection disabled
and never toggles it.

**Completion helper** (`0x2f6e0..0x2f9c3`, rev 3 only):

```text
5 immediate reads of SR6F; bit7 clear -> success
KeSetTimerEx(+0x118, due 0, period 1 ms); start = KeQuerySystemTime
loop: wait timer; read SR6F; clear -> cancel timer, success
  elapsed = now - start (100 ns)
  if (elapsed % 10 s) < 1 ms:  (first system-clock tick in practice)
     print "*** GTI Flush Check Waiting %ds ***"
     if MMIO[0400] == 0 and +0x9c0.bit22:
        print "*** Insert Dummy 2D MMIO Commands %ds ***"
        MMIO 0004=80000300 001c=(scr+1000)>>3 0014=scr>>3 0008=00101010
             0048=0 0018=0 0010=0 000c=00100010 0000=cc000001
        wait (0400 & 00101ff3)==0, 1 ms steps, <=5000   (0x13bb0)
  if elapsed > 600 ms:
     print "*** GTI Flush Check Timeout ***"
     SR6F &= 7f (software clear); return failure
```

`scr` is the adapter VRAM scratch offset `[adapter+0x734]`; the command is a
small SRCCOPY (`ROP cc`) between two local-VRAM scratch rectangles.  The
kick is only issued when **all** of `0400` reads zero, i.e. when no GTI
client is active.

### What the vendor evidence establishes

1. A pending `SR6F.bit7` is expected to need GPU memory traffic to retire:
   Windows injects a local 2D blit exactly when the engine is fully idle
   and the flush is still pending.
2. On VX900 revisions 0-2 the vendor never waits for the flush to clear; it
   writes the request ten or twelve times back-to-back and lets subsequent
   DMA traffic complete it.  Rewriting bit 7 while it is set is routine.
3. On revisions 0-1 the CR clock must not be idle-gated.
4. The vendor timeout for the completion check is 600 ms with 1-ms polling,
   ending in a software clear, and the map path ignores the result.
5. No vendor path requires 3D idle, CR PAUSE or display idle before a flush.
6. Paging unmaps keep PTEs valid (dummy page); `80000000` is written only
   when an allocation is destroyed.

## Deviations in the current driver

| # | current driver | vendor | effect |
|---|---|---|---|
| D1 | flush always requested with every 2D/3D engine idle and the next job withheld until bit 7 clears (`chrome9_engine.c:3145-3150`, `4487`) | rev 0-2: no wait; rev 3: kick 2D when idle | completion depends on incidental GTI traffic (IGA1/cursor SF fetch, X glamor); if none arrives, bit 7 stays `1` |
| D2 | `SR3F` left at firmware `ff` (only read, `chrome9_drv.c:472`); CR, 3D, 2D clocks idle-gated | rev 0-1: `SR3F[7:6]=10` (CR always on) | the idle CR, a GTI client, can be clock-gated while the flush waits for it |
| D3 | one flush write, then passive polling | 10/12 writes (rev 0-2) or rewrite-free poll plus kick (rev 3) | a request that is not taken by the GTI is never re-issued |
| D4 | 100 ms busy poll, 10 µs interval, hard failure | 600 ms, 1 ms timer, kick, software clear | shorter window and no progress action |
| D5 | evicted and unmapped PTEs become `80000000` (`chrome9_engine.c:37,3331,3695`) | paging unmap uses a valid dummy page | any late or speculative access to an evicted range translates through an invalid entry whose GTI behavior is undefined |
| D6 | `SR6C.bit7` cleared and restored around every update (`3135-3147`) | Windows never sets it; 5.75 toggles it only at init | undocumented interaction with a pending flush ("avoid hardware write"); the earlier ordering fix (restore before post-wait) reduced but did not remove failures |
| D7 | IGA1 scanout and cursor fetch through SF/GART (`CR48[7:6]=01`, `direct-sf-scanout-review.md:24-30`, `chrome9_kms.c:974-978`) during every flush | WDDM primaries are normally in the local segment (not verified for this KMD) | display requests compete with the flush; vendor never qualified this combination |
| D8 | byte MMIO `83c4/83c5` with a pre-wait before PTE writes | atomic 16-bit I/O writes, no pre-wait | not a cause while `seq_lock` covers all users; noted for completeness |

Not deviations: PTE publication ordering (UC BAR2 writes, read-back of the
last PTE, `wmb()`, `chrome9_engine.c:3139-3143`) is stronger than the 5.75
KMD.  VGA arbitration plus `seq_lock` is correct.  Flushing with published
but unretired work does not occur in the synchronous model; the vendor
allows it anyway.

## Root-cause ranking

1. **Flush requested into a quiescent GTI (D1 + D2 + D3).**  Every flush is
   issued with no render traffic and with CR/3D/2D clocks gated; the driver
   then blocks the only traffic source it controls.  Completion depends on
   display/cursor SF fetches or X activity arriving and being accepted.
   This matches the rare, workload-length-dependent occurrence (ioquake3 GL2
   performs two flushes per draw-cache miss, hundreds per second) and the
   fact that the pre-wait of the next transaction also sees `80`.  It
   directly matches the vendor's explicit workaround set for this revision.
   Confidence: high that it contributes, medium that it is the sole cause.
2. **Invalid-PTE access around eviction (D5).**  If any GTI client
   (texture prefetch/over-fetch past a BO without a trailing guard page,
   stale sampler base, or a display request during a flip) translates
   through an `80000000` entry, the GTI may hold that request and the flush
   cannot retire.  A kick would then not help.  Confidence: medium; Mesa
   BOs carry guard pages, X/glamor and dumb BOs may not.
3. **SF scanout/cursor traffic and `SR6C.bit7` toggling (D6, D7).**  Both are
   combinations no vendor path uses at runtime.  Confidence: low-medium; the
   earlier SR6C reorder evidence is two passing runs only.
4. **Timeout length (D4).**  100 ms versus 600 ms matters only if completion
   is slow but finite; with D1 it can be unbounded.  Low.

## Recommended protocol

All steps keep the existing locks (`submit_lock` -> `vm_lock` -> VGA +
`seq_lock`).  `v = (SR6F & 7f) | 80`.

```text
init (once, rev = SR3B, rev <= 1):
    saved_3f = SR3F; SR3F = (SR3F & 3f) | 80        # CR clock always on
    teardown restores saved_3f
    allocate one DMA32 scratch page; SCRATCH = pfn | 02000000

update(first, count, ptes or NULL):
    [SR6C: keep the current 5.75 order by default: clear bit7 before PTEs]
    write PTEs; NULL -> SCRATCH (not 80000000); 80000000 only at teardown
    readl(last PTE); wmb()
    SR6F = v                     # no failing pre-wait: a set bit is re-requested
    SR6C |= 80                   # restore protect immediately (default variant)
    if SR6F.bit7 == 0 within 5 reads: done
    for up to 9 more writes: SR6F = v; if bit7 == 0: done      # rev <= 2 vendor burst
    drop seq_lock/VGA; kick_gti(); re-take
    poll SR6F every 1 ms up to 600 ms; every 10 ms: SR6F = v, and if
        (0400 & 00101fe3) == 0: kick_gti()
    timeout: record SR3F SR6C SR6F 0400 0444 040c CR48 index;
             keep current quarantine (no software clear)

kick_gti():   # needs submit_lock, which every chrome9_gart_set() caller holds
    publish a kernel-owned ring segment containing only the H0 cookie write
    and PID00 PAUSE (the ring PTEs never change, so stale versus new
    translation is irrelevant), then chrome9_wait_job()-equivalent retirement
```

Prefer the ring kick over the Windows direct-MMIO blit: that blit writes
`0010`, which is `CHROME9_2D_REG_DST_POS`, the retirement cookie register
(`chrome9_drv.h:99`), and it would overwrite 2D state owned by the CR.

Freeing pages is safe only after `SR6F.bit7` is observed clear following the
invalidation.  For eviction, keep the GEM reference and the `drm_mm` node on
a pending list until that observation (at the end of `chrome9_gart_set()` or
at the next job retirement) instead of wedging the engine on the first
timeout.

A/B variable once the above is stable: the Windows `SR6C` policy (leave bit 7
at the firmware value `0`, never toggle).

## Minimal silicon test

Both tests use debugfs counters added for this purpose: flush count, 
immediate-clear count, latency histogram (`<10 µs`, `<1 ms`, `<10 ms`,
`<100 ms`, `>=100 ms`), rewrites, kicks, timeouts.

**T0 - characterization, table unchanged (memory-safe).**  Under
`submit_lock` with all jobs retired, request `SR6F=v` without changing any
PTE and time the self-clear for 10,000 iterations in four conditions:
(a) firmware `SR3F`, IGA1 on SF; (b) IGA1 blanked (`xset dpms force off`);
(c) `SR3F[7:6]=10`; (d) (b) plus a ring kick after 50 µs.  Hypothesis 1
predicts long or unbounded latency in (b), clearly reduced tails in (a) and
(c), and immediate completion after the kick in (d).  A stuck request in T0
does not expose freed memory; recover with the kick or reboot.

**T1 - stress reproduction.**  A userspace tool binds 96 distinct small
texture BOs (more than `CHROME9_DRAW_CACHE_MAX_ENTRIES=64`, so every draw
evicts and binds) and issues one textured triangle per BO round-robin,
while X runs glamor copies (for example `x11perf -copywinwin500`), the
cursor moves continuously, and DPMS toggles every 30 s.  Run until 10^6
flushes.  Pass criterion: old kernel reports at least one `>=100 ms` flush or
the existing timeout message (expected most often during DPMS off); fixed
kernel reports zero timeouts and zero flushes needing more than one kick.
Repeat once with PTEs for evicted ranges set to `80000000` instead of the
scratch page to isolate hypothesis 2.  The old-kernel run can wedge the
engine and needs a reboot, as today.

## Open questions

- Whether `SR3B` reads `00` on the target (PCI revision is `00`); read it
  once read-only before gating the `SR3F` change on it.
- Whether display SF fetch retires a pending flush; T0 (a) versus (b)
  answers this.
- Meaning of `SR6C[6:5]` and whether the GTI ever writes the table
  (`SR6C.bit7` "avoid hardware write").
