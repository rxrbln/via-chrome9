# Chrome9 CR PAUSE, STOP, and ring-replacement lifecycle

This note isolates the command-reader lifecycle needed after the live VX900
post-gears timeout.  It deliberately does not propose accepting an active
reader as complete.

## Exact `0444` decoding

The VX800/VX820 Chrome9 HC3 manual, Part 2, CR readback register `44--47h`,
defines:

| Bit | Name | Meaning |
|---:|---|---|
| 31 | `HRAGPCYCPause` | `AGP_PAUSE && HAGPBPID == 00b` |
| 30 | `AGP_CYC` | AGP command-cycle flag |
| 29 | `AGP_SEL` | AGP command cycle active |
| 28 | `SAME_BP` | same pause address |
| 27 | `SAME_BEND` | same end address |
| 26 | `SAME_BJUMP` | same jump address |
| 25 | `AGP_PAUSE` | internal pause-cycle state |
| 24 | `AGP_CRRCMREQ` | AGP command request |
| 23 | `CRRCMACK` | command-request acknowledge |
| 22 | `AGP_HOLD` | request hold |
| 21 | `VQ_AGP_STOP` | request-VQ stop |
| 20 | `LAST_REQ` | last request command |
| 19 | `AGP_REQ_STOP` | stop reading commands |
| 18:10 | `AGP_RB_CNT` | requested data not yet returned |
| 9:0 | `VQ_CNT` | VQ count in framebuffer |

This yields three immediately useful classifications:

| `0444` | Exact interpretation | Driver action |
|---:|---|---|
| `60000000` | command-cycle flag plus active; **not PAUSE** | retain ring and mappings; never retire or replace from this value |
| `c0000000` | PID00 PAUSE qualified, command-cycle flag still set | legal PID00 append/resume point if non-CR work is idle |
| `80000000` | PID00 PAUSE qualified, command-cycle flag clear | also a legal PID00 append/resume point if non-CR work is idle |

Bit 30 must not be inherited from CX700/VX700 documentation, where an older
readback register called bit 30 "cycle finished."  On Chrome9 HC3 it is only
`AGP_CYC`.  Both the Windows VX900 KMD and the old Linux Chrome9 KMD test only
bit 31 when deciding whether the reader is at PID00 PAUSE:

* `VTGKModeDX32A.sys` SHA-256
  `618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`,
  analysis `0x1b6ad..0x1b6c5` and `0xa1474..0xa149d`;
* `via_chrome9_dma.c:489..498`, with `INV_AGPCMD_InPause=80000000` in
  `via_chrome9_3d_reg.h:124..125`.

The live snapshot `0444=60000000`, `0400=00000010` therefore says that the
reader remains active while every non-CR engine is idle.  It is not a weaker
form of successful retirement.

## Documented PID behavior

The VX900 manual's T10 A65 definition is exact:

* PID `00b`, PAUSE: the reader pauses until the pause address changes, then
  starts at the address immediately following the old PAUSE;
* PID `01b`, JUMP: the reader pauses until it receives another PID00/PID10
  pause address, then starts at A60/A61;
* PID `10b`, STOP: fetching finishes and does not restart until A68 bit 0,
  `HAGPBTrig`, is written.

The manual also requires at least two 128-bit quadwords between old and new
pause addresses.  A60/A62/A64/A66 ignore address bits 3:0; they require
16-byte address alignment.  Separately, both vendor drivers pad H5S2VP1
streams more strongly for ordinary PAUSE placement.

The normal old-Linux continuation path is decisive.  After publishing the
new stream bytes, `kickoff_dma_ring_inv()` computes a later PID00 PAUSE and
writes only A64 then A65 (`via_chrome9_dma.c:1053..1073`).  It does not rewrite
A60--A63, does not rewrite A66/A67, and does not trigger A68.

The Windows VX900-specific path independently does exactly the same thing.
The function at `VTGKModeDX32A.sys:0xc8190..0xc834b` computes the number of
newly appended dwords from the ring's saved and current CPU cursors.  If the
cursor is not already 128-byte aligned, it aligns to 16 bytes, emits
`fe020420 0000041c 00300000 cc000000`, and pads with `cc0000cc` through the
next 128-byte boundary (`0xc81c4..0xc82c3`).  It computes PID00 PAUSE as the
aligned cursor minus 16, selects T10, writes A64 and then A65, and returns
without any A60--A63 or A68 write (`0xc82c5..0xc834b`).  Thus the exact normal
continuation lifecycle is cross-checked in both vendor kernels, including the
VX900 executable rather than only shared source.

Ring replacement is different.  Windows helper `0x1b690..0x1b85c` changes an
already owned PID00 stream to a later PID10 STOP by writing only A64/A65, then
waits until `0400 & 00101ff3` is zero.  Only afterward does the VX900
initializer `0xc7c00..0xc7f6f` install a complete A60--A67 tuple and trigger
it with `680a0001`.  This is the independently recovered legal replacement
sequence.

## Audit of the current driver

For source SHA-256
`76eead63de72e1341babe39583c7bc0b23edea7046e6bd66af2fee04d1ecfe49`,
`chrome9_program_ring()` rewrites A60--A67 and writes A68 `680a0001` on every
submission (`chrome9_engine.c:1153..1174`).  Calls at the submit sites switch
between two one-page slots while the previous slot is merely at PID00 PAUSE.

That is not the documented PID00 continuation and is not the vendor
replacement lifecycle.  A68 bit 0 is specifically the restart for PID10
STOP.  Repeated full reinstallation from PID00 may work in short tests but is
not a supported synchronization primitive.  The persistent live
`60000000` state is consistent with this lifecycle mismatch; it is not yet
proof that the mismatch is the sole cause.

The existing job retirement order itself should remain strict:

1. exact per-job cookie;
2. `0444.bit31`;
3. `!(0400 & 00101fe3)`.

Do not weaken step 2 to accept bit 30, bit 29, or `60000000`.

## Minimal safe incremental patch

The smallest patch that preserves the current two-page ping-pong organization
is deliberately conservative: perform a real PID10 replacement before every
slot switch.  This costs one short dummy transition per submission but avoids
introducing a circular allocator before the lifecycle is live-proven.

### 1. Build a STOP trailer into every slot

Replace the duplicated builder tails with a helper such as:

```text
chrome9_finish_ring(base, cursor, &pause, &stop)
```

It must:

1. emit the existing `ccccccc0, ddd00000` dummy pairs until `cursor` is
   128-byte aligned;
2. set `pause = cursor - base - 16`, exactly as today;
3. starting at `pause + 16`, emit the vendor STOP trailer prefix
   `fe020420 0000041c 00300000 cc000000`;
4. emit alternating `ccccccc0, ddd00000` until the next 128-byte cursor
   alignment, and set `stop = cursor - base - 16`;
5. continue the same legal dummy pattern through the readable tail of the
   page so speculative CR fetch never sees unclassified `cccccccc` words;
6. require `stop > pause`, `stop - pause >= 0x20`, both addresses 16-byte
   aligned, and `stop <= PAGE_SIZE - CHROME9_CR_READ_AHEAD`;
7. flush the entire slot, then execute the existing `dma_wmb()`.

Using a second 128-byte cursor boundary makes `stop-pause` normally `0x80`,
which exceeds the documented two-quadword minimum and matches the stronger
H5S2VP1 padding policy already used for PID00.  Windows' generic STOP helper
only aligns its cursor to 32 bytes, so 128 bytes is conservative rather than
a newly invented minimum.

Return both offsets from `chrome9_build_prime()` and every job builder.  Store
the published pair in engine state, for example `active_pause` and
`active_stop`; do not infer the active slot from a job that timed out before
retirement.

### 2. Convert the owned PID00 reader to PID10 STOP

Add a bounded helper such as:

```text
chrome9_stop_active_ring(engine)
```

Preconditions:

* the active slot and its `active_pause`/`active_stop` were published by this
  driver and have remained immutable;
* `0444.bit31 == 1`;
* `0400 & 00101fe3 == 0`;
* the STOP trailer and at least `CHROME9_CR_READ_AHEAD` following bytes are
  still mapped and initialized;
* all CPU writes to that trailer precede the MMIO update.

Then select T10 with `041c=00100000` and write only:

```text
A64 = 64000000 | ((slot_base + active_stop) & 00fffff0)
A65 = 65000200 | ((slot_base + active_stop) >> 24)
```

Preserve the vendor order A64 then A65.  Do not write A60--A63, A66/A67, or
A68.  Changing A64/A65 is what releases PID00; the new PID10 value stops the
reader at the prebuilt trailer.

Poll the full vendor STOP mask until
`(0400 & 00101ff3) == 0`.  PID10 is not represented by `0444.bit31`, because
bit 31 is explicitly qualified by PID00; do not wait for bit 31 during this
phase.  Keep `0444` only as diagnostic data.

If this poll times out, make no further CR writes.  Quarantine both ring
pages, their PTEs, the page table, and every still reachable object until
reboot.

### 3. Install the other slot only after STOP idle

After the full STOP-idle proof, the existing full tuple is legal:

```text
A60/A61 = new start, with A60.bit0 = 1 for SF
A62/A63 = new end
A64/A65 = new PID00 PAUSE
A66/A67 = fffffff0 sentinel
A68     = 680a0001
```

`chrome9_program_ring()` should therefore be renamed to make its restricted
role obvious, for example `chrome9_install_stopped_ring()`, and called only:

* at cold initialization when no old reader is live; or
* immediately after `chrome9_stop_active_ring()` proved the full busy mask
  clear.

After triggering the new slot, keep the existing cookie, PID00 PAUSE, and
non-CR-idle retirement checks.  On success, record its pause/stop offsets as
the new active tuple.  On any timeout, do not attempt a replacement from
`60000000` or another active state.

### 4. Use the same STOP transition for teardown

`engine_fini()` currently proves PID00 PAUSE and non-CR idle, then starts
invalidating mappings.  Before touching any ring PTE it should move the owned
reader to its prebuilt PID10 STOP and require the full `00101ff3` idle mask.
Only that STOP proof makes ring and translation teardown consistent with the
Windows lifecycle.

## Ring and wrap invariants

These invariants are required even after the conservative patch is replaced
with a larger monotonic ring:

* Ordinary append/resume changes only A64/A65 while at owned PID00 PAUSE.
* A new PID00 address is strictly forward within the same A60--A63 range and
  at least `0x20` bytes beyond the old address.
* Every byte from the old pause through the new pause plus
  `CHROME9_CR_READ_AHEAD` is published, legal command/dummy data, mapped, and
  immutable before A64/A65 changes.
* The new pause is far enough before A62/A63 end that observed read-ahead
  cannot cross the end and wrap to A60/A61.
* No CPU writer reuses a prefetched ring region merely because the executed
  address passed it.  Reuse starts only after PID10 full idle or a separately
  live-proven circular-space protocol.
* A60--A63 and A66--A68 never change while the reader is active or merely at
  PID00 PAUSE.
* Ring, PTE, and page-table lifetimes extend through PID10 full idle.  A job
  cookie and non-CR idle can retire that job's data BO, but cannot by
  themselves release command storage.

For a future production ring, program a larger start/end range once, append
monotonically, and advance only PID00 A64/A65.  Near end-of-ring, transition
to PID10 STOP, prove full idle, then install the wrapped/replacement range.
PID01 should remain unused until its two-stage handshake is independently
live-proven.

## First live oracle after the v4 diagnostic

The first lifecycle-only test should avoid adding new 3D state:

1. prime to PID00 PAUSE and record `0444`, `0400`, executed address, pause,
   and stop;
2. update only A64/A65 to the prebuilt PID10 STOP;
3. require bounded full idle and record the same registers;
4. install the other slot with A60--A68 and run one already-proven color
   triangle;
5. require its exact cookie, PID00 PAUSE, non-CR idle, and known pixel result;
6. repeat for both slots, then stress at least 1000 replacements before
   reintroducing D16.

Expected transition samples are PID00 `c0000000` or `80000000`, transient
active `60000000`, then full `0400` idle at PID10.  No correctness decision
should depend on the final PID10 value of `0444`; the authoritative STOP proof
is the full engine mask used by the Windows KMD.
