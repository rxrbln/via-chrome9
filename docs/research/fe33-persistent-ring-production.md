# FE33 production completion in the persistent Chrome9 ring

Status: implementation design from source-exact vendor evidence, plus the
`kind=7` and wrap-at-zero live failures.  This note changes no kernel code.
It narrows `chrome9-fe3x-completion.md` to the first safe polling tranche and
adds the ring-restart proof which the newer wrap failure requires.

The audited current `chrome9_engine.c` has SHA-256
`cda93dc05de1db84459127e68871c8c5966c08bf1b1aaf0c1d273f634d998a5b`.
The old Linux `via_chrome9_dma.c` has SHA-256
`958733a18208f5386ce12e4d2c5946974247bc91d448d8db49d29b88a7dcff2b`.
The Windows 7 KMD `VTGKModeDX32A.sys` has SHA-256
`618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`.

## Implementation status (2026-09-24)

Kernel Chrome9 commit `665a532` implements the first tranche behind
`fe33_fence=1` (default off).  Every 3D builder ends through
`chrome9_emit_3d_tail()`: `CC000000` padding, `FE330004` with wait
`00200000`, then the unchanged H0 sync, A1C/A1D reset, dummy and cookie.
The 2D copy-rectangle path emits no FE33.  The slot is a dedicated
`dma_alloc_coherent()` page (16-byte aligned, below 4 GiB, never an SF/GART
page); IDs are nonzero, monotonic and never wrap (`-ENOSPC` at `U32_MAX`).
`chrome9_emit_fe33()` poisons `slot[0]=~id`, writes canaries
`5afe3301..3` and issues `dma_wmb()` before the segment is copied.
`chrome9_wait_job()` polls the exact ID first, then cookie, PAUSE and idle.
A timeout logs the slot, `0494` and `040c` and uses the existing quarantine;
a canary change is only logged and counted.  The init self-test checks the
golden vector below, the `ffffffff`/`fffffff0` extremes and all rejections.
Engine init logs `fe33=on|off`; the first completion logs its ID and job
kind; teardown logs `fe33=<completions>/<last id>` and canary faults.

Layout check: for the largest page-staged builders (34-triangle batch with
blend, 24-triangle textured run, D16 batch) the extra 16 bytes stay inside
existing slack, so PAUSE/STOP do not move.  The 544-triangle D16 RHW run
keeps its `9c70`/`9cf0` golden.

Not silicon-tested.  Gates 2--6 below remain open.

## Bounded first result

The first production-capable record is system-write-only `FE330004` with the
3D wait mask `00200000`.  It must be emitted by the kernel, to a kernel-owned
coherent DMA slot, after the last 3D body dword and before the legacy tail.
Keep the current H0 synchronization, A1C/A1D reset, dummy, `DST_POS` cookie,
PID00 PAUSE, and PID10 STOP trailer for the first oracle.  The exact staged
order is:

```text
3D body
CC000000 padding to 16-byte boundary, if needed
FE330004 completion, wait=00200000, exact ID, coherent DMA address
existing H0 006c=00800000
existing H2 A1C/A1D reset                  # retained only for one-variable A/B
existing H2 00300000 dummy
existing H0 DST_POS cookie
existing PID00 PAUSE and immutable PID10/read-ahead closure
```

Putting FE33 before the legacy H0 is intentional.  The FE33 wait field is the
vendor production ordering mechanism.  If FE33 completes but the later cookie
does not, the fault is in the redundant legacy synchronization/tail rather
than the draw or FE33.  Putting H0 first, as an earlier diagnostic proposal
did, leaves the observed H0 hard stop ahead of the new discriminator.  No
legacy packet is removed in this tranche.

Do not initially enable interrupt generation, interrupt-wait, or queue-skip.
`FE330006` (system write plus IRQ) is the second tranche after polling passes.
`FE330036`, although used by Windows, couples progress to unproved IRQ and
fence-queue behavior and is not a safe bring-up packet.

## Exact packet and golden vector

The old Linux macro `ADDCmdHeader3X_INVI()` at
`via_chrome9_3d_reg.h:520..533` supplies the bit-exact grammar and inserts
`CC000000` until the record starts on a 16-byte boundary.  Windows independently
uses FE33 plus wait `00200000` for 3D.  For system-write-only mode:

```text
d0 = fe330004
d1 = 00200000 | ((id & 000000ff) << 24)
d2 = ((id >> 8) & 00ffffff) | ((dma_addr & 00000ff0) << 20)
d3 = (dma_addr >> 12) & 000fffff

id       = ((d2 & 00ffffff) << 8) | (d1 >> 24)
dma_addr = (d3 << 12) | ((d2 >> 24) << 4)
```

The byte-exact encoder/decoder golden is:

```text
id       = 12345678
dma_addr = 01088120

fe330004 78200000 12123456 00001088
```

This vector is preferable to ID 1 because it exercises the split ID fields
and both address fields.  Required rejections are a non-16-byte-aligned
address, nonzero upper 32 address bits, zero/reserved ID, a mode other than
`4` in the first tranche, and a wait mask other than `00200000`.

For the captured full kind-7 segment whose 3D body ends at ring offset
`71e0`, insertion is deterministic:

```text
71e0  fe330004 ID+00200000 ADDR+ID ADDR
71f0  fe000000 00000001 00000000 cc000000    # retained H0
7200  0000006c 00800000 cc000000 cc000000
7210  fe020440 0000043c 00010000 cc000000
7220  1c000000 1d000000 cccccccc cccccccc
7230  fe020420 0000041c 00300000 cc000000
7240  fe000000 00000001 00000000 cc000000
7250  00000010 COOKIE
7270  ccccccc0 ddd00000                         # PID00 PAUSE
72f0  ccccccc0 ddd00000                         # PID10 STOP
```

The extra 16-byte record consumes existing alignment slack, so this particular
PAUSE and STOP remain `7270/72f0`.  That is a useful layout golden, not a
general assumption: every builder must recompute its pause/stop and rerun
`segment_stop + 0x220 <= capacity` after insertion.

## Destination and cache/coherency contract

Allocate one coherent page with `dma_alloc_coherent()` after the device's
24--32-bit coherent mask is installed.  Initially use one 16-byte slot.  The
old KMD also reserves `FENCE_SIZE=0x10`, but its aligned
`kmalloc()+virt_to_phys()` technique is not valid with modern DMA/IOMMU APIs.

The FE33 address is the returned DMA/bus address.  It is neither a CPU physical
address nor an SF/GART virtual address.  Validate 16-byte alignment,
`upper_32_bits(addr)==0`, and checked `addr+15 <= U32_MAX`.  The completion
page must never consume transient GART PTEs: its direct PCI-master write is
specifically the proof needed before those PTEs can be removed.

Before publication:

```text
WRITE_ONCE(slot[0], poison_not_equal_to_id)
WRITE_ONCE(slot[1..3], canaries)              # diagnostic tranche
dma_wmb()
build and copy the complete command segment
flush the copied ring range with the existing ring cache helper
dma_wmb()
publish A64/A65, or the full restart transaction below
```

Do not call `dma_sync_single_*()` or CPU `clflush` on a
`dma_alloc_coherent()` completion page.  The explicit `dma_wmb()` orders the
CPU poison before hardware can fetch FE33; the existing ring flush plus its
second `dma_wmb()` orders command bytes before CR publication.  Poll with
`READ_ONCE(slot[0])`; after exact equality execute `dma_rmb()` before consuming
any accompanying software state.  The other three dwords are canaries only;
production software metadata lives outside the device-writable slot.

PCI bus mastering must remain enabled until every FE33 write is proved or the
allocation is quarantined.  On timeout, do not poison, reuse, unmap, or free
the slot.  A late direct write remains possible even if the SF ring and BO
mappings have separately been retained.

## ID and lifetime rules

Use a nonzero, monotonically increasing `u32` hardware ID and exact equality.
Record `{hardware_id, dma_fence_seqno, ring_epoch, slot_index}` in the job
before the publication write which can make FE33 executable.  The old driver's
`id <= readback` test and 31-bit wrap are not wrap safe.

The bounded first implementation has one synchronous outstanding job and one
slot.  Reuse is legal only after release completion, not merely after the
memory word appears.  Do not wrap the hardware ID in the first implementation:
if increment would produce zero, refuse new submissions and require a proved
PID10/full-idle engine restart with a fresh coherent page and software epoch.
This removes stale same-slot equality from the initial correctness problem.

Freeing a completion page requires STOP/full-idle proof for every epoch which
could reference it.  A timed-out page follows the same reboot-scoped quarantine
as the ring and mappings.  Merely signalling an error fence does not shorten
that hardware lifetime.

## Persistent append transaction

The current ring appends only at `active_pause+0x10`.  Since PAUSE is placed at
`128-byte cursor - 16`, every append base is 128-byte aligned; a record aligned
in the page-aligned staging buffer therefore remains 16-byte aligned after
copying into the persistent ring.

Under `submit_lock`, use this order:

1. Require the old owned PID00 PAUSE, old exact cookie, and zero non-CR work.
2. Reserve the next nonzero ID; poison the coherent slot and issue
   `dma_wmb()`.
3. Build the complete segment, with FE33 immediately after the body and the
   old tail afterward.  Store ID/address/epoch in the job.
4. Validate the FE33 encode/decode round trip and the complete STOP plus
   `0x220` read-ahead bound.
5. Copy to `ring_cpu+append`, flush the entire copied interval, and issue
   `dma_wmb()`.
6. Publish the expected job/ID to any poll/IRQ observer before the MMIO which
   can start the reader.  For a normal append, select T10 and write A64 then
   A65 PID00, followed by a transfer-port readback.
7. Immediately record `published`, absolute pause/stop, append cursor, and
   epoch even if the following waits fail.

The current `chrome9_append_segment()` records ownership after A65.  That is
adequate while no IRQ consumes it, but the FE330006 tranche must make the
active completion tuple visible before A65; an IRQ may arrive before the
function reaches its existing bookkeeping.

Wait in this order:

```text
exact coherent FE33 ID                    # notification completion
exact DST_POS cookie
0444.bit31 == 1                           # owned PID00 PAUSE
(0400 & 00101fe3) == 0                    # non-CR engine idle
CPU/GART ownership transition succeeds   # release completion
```

The memory ID alone never authorizes PTE removal: CR may still consume the
cookie and closure.  Conversely, a cookie/PAUSE with a missing FE33 ID is not
permission to disable the new check; it is a packet/address/coherency failure
and must retain the resources while FE33 is disabled or investigated.

## Wrap/reinstall: A68 is exact-once, not a retry mechanism

The newer live failure was a wrapped kind-6 segment installed at ring offset
zero: `pause=0470`, `stop=04f0`, old cookie, `043c=0480`,
`0444=c0000000`, and `0400=00000010`.  This differs from the active kind-7
failure (`0444=20000000`), but it does **not** prove that A68 was ignored:
`0444.bit31` says the reader is at a PID00 PAUSE, and `043c=0480` is exactly
one quadword after the newly programmed `0470` pause.  Because `043c` is a
fetch/request diagnostic with read-ahead and VX900 pause-address readbacks
are not validated, the snapshot cannot distinguish “new ring fetched to its
new PAUSE but work/tail did not execute” from a stale pause latch.  The old
cookie proves only that the final marker did not execute.

Static vendor evidence is unambiguous about the legal control sequence:

* transition the old owned PID00 reader to its later PID10 STOP by writing
  only A64 then A65;
* require `(0400 & 00101ff3)==0`;
* write the complete A60--A67 tuple;
* write `680a0001` exactly once.

Windows' VX900 installer at `VTGKModeDX32A.sys:0xc7c00..0xc7f6f` does not
poll A68 and does not retrigger it.  After its one A68 write it returns and
ordinary publication later advances only A64/A65.  The old Linux normal
kickoff does the same A64/A65-only advance.  Therefore:

* never retry A68 after `0444.bit31` is visible;
* never use A68 for an ordinary append;
* never treat a second A68 write as a timeout recovery;
* do not use A68 bits 1--4 as guessed reset controls.

The code-ready way to remove ambiguity is a two-step wrap, matching Windows'
separation of ring installation from ordinary submission:

1. STOP and prove full idle for the old epoch.
2. Fill the rebuilt ring with legal dummy pairs and place a tiny prime at
   offset zero containing a unique kernel cookie followed by the normal
   dummy/PID00/STOP closure.
3. Flush the full ring, `dma_wmb()`, program A60--A67, and trigger
   `680a0001` once.
4. Require the unique prime cookie, PID00, zero non-CR work, and no `040c`
   error.  This proves that the new epoch, not a stale PAUSE indication,
   executed.
5. Set `append=prime_pause+0x10` and publish the already-built real job through
   the ordinary A64/A65 append path.  Its FE33 then diagnoses 3D completion
   independently.

The prime marker must differ from both the preceding job's cookie and the real
job's final cookie.  If preserving the current two-value cookie ABI makes that
impossible, use a separate kernel-only MMIO marker value during wrap and only
restore the alternation for the real job.  No userspace-visible state depends
on `DST_POS`.

An even smaller diagnostic-only alternative is to put a unique start cookie
at the start of the wrapped segment, before any 3D work.  It distinguishes
“A68/new stream never executed” from “body/FE33/tail stalled”, but the two-step
prime is the stronger production gate because it exercises installation before
the real job is published.

## Failure classification and implementation gates

On the first failure, log the four FE33 dwords, DMA slot address and four slot
dwords, expected ID, ring epoch, whether the job was an ordinary append or a
full reinstall, start/prime/final cookies, A60--A68 software shadows, `0494`,
`043c`, `0440`, `0444`, `0400`, and `040c`.

| restart marker | FE33 ID | final cookie / PAUSE / work | classification |
|---|---|---|
| absent after reinstall | wrong | incomplete | ring restart/visibility failure; A68 retry forbidden |
| exact | wrong | incomplete | new ring started; draw, FE33 decode/address, or 3D wait failure |
| exact | exact | incomplete | 3D wait and system write completed; legacy tail/CR failure |
| exact/not applicable | wrong | exact / set / idle | FE33 packet or coherent-address failure; quarantine and disable FE33 |
| exact | exact | exact / set / idle | release completion |

Before enabling FE33 for normal Mesa submissions, require all of these:

1. offline golden encode/decode and misalignment/overflow tests;
2. a cold-boot no-work `FE330004` oracle with exact ID and unchanged canaries;
3. fixed, one-texture replace, one-texture MODULATE+blend, and 16-triangle
   batch jobs with exact ID/cookie/PAUSE/pixels;
4. a forced ring wrap which passes the unique prime and then the real job;
5. an intentionally poisoned/wrong expected ID which times out without freeing
   the page or PTEs;
6. sustained ID increments with no slot reuse before release completion.

Only after those pass should `FE330006` and the `0200.bit31`/`0204.bit5` IRQ
path be enabled.  `FE330010`, queue-skip, multiple outstanding slots, ID wrap,
and asynchronous PTE release remain separate implementation gates.
