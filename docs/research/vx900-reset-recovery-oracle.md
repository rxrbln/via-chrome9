# VX900 reset and recovery oracle

This note records the recovery sequence that is actually executed by VIA's
Windows 7 KMD.  It is a static oracle, not yet a silicon-proven Linux reset
recipe.  The distinction matters: a partial reset can leave the command reader
able to fetch from memory which Linux has already released.

The binary used here is:

```text
binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGKModeDX32A.sys
SHA256 618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56
PE32 i386, timestamp 2015-03-03
```

Addresses below are image virtual addresses as printed by GNU `objdump`.

## Exact lower reset helper

The hardware helper is `0x0008a500..0x0008a794`.  Its exact control flow is:

1. `0x8a510..0x8a52a`: call `0x45f10(dev, 0, 1)`, then
   `0x45f10(dev, 1, 1)`.  The callee at `0x45f10..0x45fcd` performs preserving
   indexed-register updates:

   - selector 0 clears `SR1B.bit0`, turning the primary LUT **on**;
   - selector 1 clears `CR92.bit7`, turning the IGA2 LUT **on**.

   These writes happen even if the engine is already idle and are not restored
   by the helper.  Reset therefore has display-visible collateral state.
2. `0x8a52a..0x8a544`: read `MMIO[0400]`, mask with `00161fff`, and take the
   reset path only when the result is nonzero.  The mask includes the four-DMA,
   CMDQ, MPEG/HQV, CR, 2D, and 3D busy classes.  A zero result only takes the
   logging/return path at `0x8a77e`.
3. `0x8a565..0x8a573`: write `MMIO[0200] = 00000000`.  This disables the
   complete graphics interrupt mask/global gate; it is not a CR-only mask.
4. `0x8a579..0x8a5b4`: write indexed sequencer `SR4E = 0f`, execute an empty
   loop of exactly `0x10000` iterations, then write `SR4E = 00`.  These are
   direct byte writes, not read/modify/write operations.  The iteration count
   is not a time specification.
5. `0x8a5b9..0x8a627`: read `SR1A` once, write `old | 40`, delay `0x10000`
   iterations, write `old & bf`, then delay again.  The manual calls
   `SR1A.bit6` PCI-bus software reset.  Other `SR1A` bits are preserved from
   the initial sample, but a concurrent writer would be lost.
6. Only for PCI ID `7122`, `0x8a63d..0x8a6eb` first loads the object at
   `[adapter+22f54]` and stores zero to its dword `+b40`.  It then reads
   `SRC7`, writes `old | 80`, delays, reads `SRC7` again, writes
   `current & 7f`, and delays.  The VX900 manual describes `SRC7` merely as
   Video-IP Debug Register 3; bit 7's reset meaning is undocumented.
7. `0x8a6eb..0x8a769`: repeatedly read `MMIO[0400] & 00101ff3` until zero.
   This loop is unbounded.  While it is busy, PCI IDs `7122` and `a122` with
   revision `0` or `1` also receive a literal `MMIO[0204] = 00000020` on every
   iteration.  Later revisions and other IDs do not receive that write.
8. After idle, `0x8a769..0x8a77c` calls the destructive default initializer
   `0x896b0`, then sets adapter byte `+22f01 = 1` to request deferred
   post-reset setup on the next submit.

The important correction is that `SR4E=0f` is not a 3D-reset assertion.
According to the VX900 manual, `SR4E[5:4]` controls 3D, `[3:2]` controls 2D,
and `[1:0]` controls HQV/DVD/capture.  Literal `0f` leaves `[5:4]=00`, requests
2D reset encoding `11`, and requests HQV encoding `11`; both `11` encodings
are undocumented.  The vendor's recovery must therefore depend on the wider
`SR1A` and `SRC7` pulses plus reinitialization.  `SR4E` alone is neither an
exact 3D reset nor a safe substitute for this sequence.

There is useful cross-generation corroboration for the otherwise vague
`SR1A` label.  The Chrome9 HCM manual names `SR1A.bit6` specifically
"Software Reset for 3D ECK"; the VX900 manual shortens it to "Software
Reset".  HCM additionally defined `SR1A.bit1` as ECK software reset, while
VX900 marks bit 1 reserved/RO.  The Windows VX900 helper preserves bit 1 and
pulses only bit 6.  This makes the `SR1A.bit6` pulse the only documented
3D-facing part of the vendor reset sequence; it still does not define the
full collateral/reset-state contract.

## Default-state replay is not restart

The helper `0x896b0..0x8a4f5` is part of recovery, not optional state setup:

- `0x896c0..0x896eb` zeros every dword at MMIO `0000..0038` inclusive;
- `0x896eb..0x89729` zeros one dword every `0x20` bytes at
  `1e00..1fe0` inclusive;
- on `7122`/`a122`, `0x8974c..0x89777` selects transfer port `043c=00080000`
  and writes `0440=00000001` before the large 3D default-state stream;
- it replays the complete attribute, texture-stage, palette/table, FVF,
  shader, and configuration defaults through `043c/0440`;
- `0x8a498..0x8a4bf` writes `041c=00100000`, `0420=680a0000`;
- `0x8a4c5..0x8a4ec` writes `041c=00100000`, `0420=02000000`.

The last A68 word has trigger bit zero.  The function does not program
A60--A67 and does not start a usable command ring.  It also does not restore
`0200`, the page table, any GART sequencer register, the ring contents, or a
fence value.

There is now negative executable evidence against using T10 A68's other
control bits as an abbreviated recovery.  An exhaustive disassembly search of
this VX900 KMD finds only these literal T10 A68 emissions:

```text
680a0000   reset/default replay, trigger clear       (0x8a4ae)
680a0001   ring install, threshold 0a plus trigger   (0x770f2,
                                                     0xa1764, 0xc7f49)
```

The `68000000`/`68000001` words at `0xc66e3`, `0xc6a13`, `0xc6fc8`,
`0xc7194`, and nearby sites belong to command-stream blocks selected with
T11 (`041c=00110000`), followed by T11 A69/A6A branch state.  They are not
writes to T10's main-ring A68 register.  In the proprietary Linux KMD source,
T10 bit 3 `INV_HAGPBClear`, bit 2 `INV_HRSTTrig_RestoreAGP`, and bit 1
`INV_HRSTTrig_RestoreAll` are defined in `via_chrome9_3d_reg.h:183..187` but
have no use anywhere in the driver.

Thus neither shipping vendor driver uses T10 A68 bits 3:1 for ordinary ring
replacement or hard-hang recovery, and the Windows reset path does not use
the manual's bit 4 fence-queue clear either.  Those bits remain documented but
unvalidated.  Do not substitute `680a0002`, `680a0004`, `680a0008`, or
`680a0010` for PID10/full idle plus destructive reset/replay.

Packet-exact replay and corrected runtime state differ at one known word.  The
Windows KMD executable itself pushes de-gamma tail `377bd680` at `0x89a13`
(and again in its duplicate initializer at `0x11d6b4`), matching the old Linux
KMD C expression which ORs both `119 << 20` and `884 << 20`.  The proprietary
Linux GL UMD executable instead emits `374bd680`, exactly
`(884 << 20) | (757 << 10) | 640`.  Thus `377bd680` is the exact Windows reset
oracle but is also a cross-driver-confirmed KMD table bug; a Linux replay
intended to restore correct GL/sRGB state should use `374bd680` and document
this deliberate correction.

The Windows next-submit path at `0x83dbb..0x83e0b` consumes `+22f01`.  It
first performs `MMIO[0200] |= 80a00000`, then invokes a chip callback and
clears the byte.  For `7122` that callback is `0xa17a0`, which reconstructs a
4-MiB software ring and calls `0xc7c00` to install the complete A60--A68 tuple.
The order is therefore IRQ restoration, command-reader reconstruction, flag
clear, and only then ordinary submission.  The recovered ring tuple is:

```text
041c = 00100000
0420 = 60 | ((P           ) & 00fffff0) | 00000001  # SF start
0420 = 61 | ((P           ) >> 24)
0420 = 62 | ((P + 00400000) & 00ffffff)
0420 = 63 | ((P + 00400000) >> 24)
0420 = 64 | ((P + head_pad - 10) & 00ffffff)
0420 = 65 | ((P + head_pad - 10) >> 24)             # PAUSE ID 00
0420 = 66ffffff
0420 = 67ffffff                                      # jump = fffffff0
0420 = 680a0001                                      # threshold 0a, trigger
```

This proves that default replay without a new owned ring is incomplete
recovery.

The mask `80a00000` is independently decoded by the VX900 manual: bit 31 is
the global interrupt gate, bit 23 is DMA1-transfer-done enable, and bit 21 is
DMA0-transfer-done enable.  It contains no dedicated CR-interrupt enable; CR
completion is status `0204.bit5` under the global gate.  Linux must not copy
the two DMA enables unless it owns and handles them.

The WDDM `RestartFromTimeout` entry at `0x8a8c0..0x8a918` does not add missing
GPU restore operations.  It sets adapter dword `+bcc=1`, manipulates two
kernel timer/event objects, calls `0x13150`, and returns.  It never touches the
MMIO transfer ports, ring, PTE memory, or GART sequencer indices.  Likewise the
`S3gResetDevice` entry at `0x78290..0x78322` calls `0x8a500` directly and then
issues an OS/device notification; there is no hidden translation rebuild
between the public callback and the lower helper.

## What reset does not do

No instruction in `0x8a500..0x8a794` or `0x896b0..0x8a4f5` accesses the
VX900 GART controls `SR6A`, `SR6B`, `SR6C`, `SR6F`, or `SR7B`, and neither
function edits PTE memory.  It follows that the Windows sequence does not
prove any of the following:

- reset disables SF translation;
- reset invalidates the GTI translation cache;
- reset makes a previously reachable ring, target, texture, or depth BO safe
  to release;
- old PTE contents survive reset coherently;
- the page-table base and size survive every global reset pulse.

Conversely, the Windows driver restarts an SF ring without reprogramming those
registers, so it plainly *assumes* that its selected reset pulses preserve the
translation setup and page-table contents.  That is useful implementation
evidence but not a Linux teardown guarantee: it neither proves that an
in-flight translation has stopped nor that a differently initialized VX900
retains every undocumented cache state.  Read back and, if necessary,
reprogram all owned GART state after reset before exposing a new ring.

There is no known VX900 translation-disable bit.  `SR6C.bit7` is documented as
GART-table *write protection*, not translation enable.  Clearing it is not a
teardown operation.  `SR6F.bit7` requests a GTI-cache flush and self-clears;
it is the only recovered PTE-publication/invalidation mechanism, and both its
pre-wait and post-wait need a deadline.

The proprietary Linux KMD contains no hang recovery.  Its cleanup merely
clears `SR6C.bit7`, then later unmaps/frees the table and backing without
stopping CR or invalidating PTEs.  That is a source bug, not corroborating
evidence.  The old DRM/Xorg sources likewise provide idle waits and a 2D reset
experiment but no full Chrome9-HD recovery sequence.

## Safe Linux implementation boundary

For a stream timeout where CR is paused but a 3D stage remains busy, the only
currently justified action is quarantine:

1. stop accepting new submissions and mark the engine wedged;
2. record `0200`, `0204`, `0400`, `040c`, `041c`, `043c`, `0444`, `045c`,
   `0460`, `0464`, the indexed selector, `SR1A`, `SR1B`, `SR3F`, `SR4E`,
   `SR6A..SR6F`, `SR7B`, `CR92`, and `SRC7`;
3. retain every potentially reachable BO, DMA mapping, PTE, SF allocator node,
   ring page, page table, MMIO lifetime, device reference, and module
   reference;
4. do not signal success fences and do not run normal GEM/PTE teardown.

A future destructive recovery implementation should be a device-wide state
machine, not `engine_quiesce()` pretending the engine became idle:

```text
block submitters and IRQ-driven completion
snapshot coexistence/display/GART registers
fail all outstanding jobs and advance the software reset epoch
execute a bounded, silicon-tested equivalent of the vendor reset pulses
require 0400 & 00101ff3 == 0 under a second deadline
replay the complete 3D defaults
reprogram/verify SR6A/B/C/7B and all owned PTEs
barrier; request SR6F.bit7; wait for its self-clear
rebuild and trigger an owned dummy-only A60--A68 ring
prove PAUSE plus full idle and a harmless parser side effect
restore only the interrupt bits owned by this driver
restore display LUT/coexistence state changed by reset
release quarantined mappings only after the preceding proof
```

If the reset or post-reset proof times out, the quarantine remains.  A remote
reboot is safer than freeing resources still reachable by CR or GTI.

## Current `engine_quiesce` and recoverable-timeout audit

The current sysfs quiesce entry first unplugs DRM, requires zero open files,
and calls `chrome9_engine_fini()` (`chrome9_drv.c:2458..2490`).  The engine
teardown holds `submit_lock`, stops admission, then refuses every already
wedged or quarantined engine before touching its mappings
(`chrome9_engine.c:2693..2727` in the pre-two-texture tree).  That refusal is
correct for reachability but means the entry is not yet a recovery path.

There are two importantly different timeout classes:

* `0444` must be decoded before treating a reader as quiescent.  The local
  VX800/VX820 Chrome9 HC3 manual's CR readback register `44--47h` names bit 31
  `HRAGPCYCPause`, bit 30 `AGP_CYC` (command-cycle flag), and bit 29
  `AGP_SEL` (command-cycle active).  Bits 28/27/26 report same pause/end/jump
  addresses, bit 25 is the internal pause-cycle flag, bits 24/23 are request
  and acknowledge, and bits 18:10 count requests without returned data.  This
  matches the VX900 Windows test of bit 31 and every live VX900 PAUSE sample.
  Consequently `0444=60000000` is an active command cycle (`AGP_CYC|AGP_SEL`),
  explicitly **not** an owned PAUSE.  Paired with `0400=00000010`, it means
  the reader remains active while all non-CR engines are idle.
  The same manual identifies readback `3c--3fh` as the current executed AGP
  command.  For the current one-texture MODULATE+blend builder in slot 1,
  the six-dword leading H0 plus alignment put body dword zero at `1020`.
  `043c=00001200` therefore maps to body dword 120, the final four zero words
  of the VS-integer group; the blend delta moves the clip H2 header to body
  124 at `1210`.  The final cookie H0 begins at `1410` and the owned PAUSE is
  `1470`, so this sample is about `0x210` bytes before the cookie and
  `0x270` before PAUSE, not a reader which narrowly missed its PAUSE poll.
  This address conclusion is conditional on A60 naming slot 1; record A60--A65
  and the expected/current cookie with every such diagnostic.

For the later **one-texture plus blend plus D16** successor, both closed-state
deltas precede the constant/clip groups, so the corresponding map is eight
dwords later than the original 232-dword Xorg body:

```text
body[120..127] = FE020440 0000043C 33030000 CC000000
                 00000000 00000000 00000000 00000000
body[128..135] = FE020440 0000043C 00010000 CC000000
                 80000020 81000020 82000020 83000020
```

The first group is the unchanged VS integer-constant upload and the second is
the clip group.  Because the leading H0 and alignment make the body begin at
slot offset `20`, slot-1 `043c=1200` names body dword 120, the start of the
integer-constant H2 header.  Its four-zero payload begins at `1210` and the
clip header begins at `1220`; the combined body's final cookie begins at
`1420` and its PAUSE remains `1470`.  The clip header is byte-for-byte the
Windows/Xorg form and the old 5.75 KMD's
`addcmdheader2_invi()` construction: `FE02|(043c+4)`, `043c`, parameter type
`00010000`, software flag/pad, followed by A80, A81, A82, A83 in order.  Do
not reorder or rewrite it based on this cursor.

The manual calls `043c` the current executed-command address, but live PAUSE
samples let it advance as much as the request FIFO's read-ahead beyond the
decoded PAUSE.  Operationally it is only a fetch-cursor bound: data through
the indicated 16-byte block have reached the CR side, while decoding may lag.
Capture `0440` (current request address) with `043c` to bracket memory fetch;
neither proves the instruction at that address executed.  The open 5.75 KMD
uses `043c` only as `CurHW` for ring-overwrite bounds and deliberately repeats
every decisive read because, in its words, the value is sometimes unreliable.
A one-sample cursor is therefore weaker still.  The smallest safe
correction is therefore diagnostic, not a clip-state delta: assert/dump actual
ring dwords 120--135 plus A60--A65 and both cursor registers.  If those words
match, gate the unvalidated blend+D16 combination back to its separately
live-proven predecessors rather than perturbing the proven H2 framing.  A
generic “header cannot start at slot offset `200`” theory is already
contradicted by the live one-texture predecessor: its clip H2 header occupies
body dword 120 at exactly that address.  Do not add speculative padding on the
basis of this cursor alone.
* An owned reader at PAUSE may leave only command-reader bit 4 set.  The
  proven readiness predicate is `0444.bit31 && !(0400 & 00101fe3)`, not the
  full `00101ff3` STOP/idle predicate.  The fixed self-test already makes
  this distinction at `chrome9_drv.c:1460..1468`.  Job retirement is also
  safe with the non-CR mask only after its cookie was observed, because the
  cookie orders the body before PAUSE.  Treating lone bit 4 as a hang creates
  a false quarantine which needs no reset.
* Any other persistent busy bit is a real or unclassified engine timeout.
  PAUSE proves that the command reader stopped delivering new work, but does
  not prove already delivered 2D/3D/DMA work retired.  This class still needs
  the device-wide reset state machine above; freeing anything is unsafe.

There is a narrower split-retirement fact for the active-reader sample, but it
must not weaken the quiesce predicate.  If `DST_POS` equals the exact
quarantined job's alternating cookie, and the owned immutable stream is known
to contain only its aligned PAUSE and dummy padding after that final H0 write,
then `0400 & 00101fe3 == 0` proves the preceding 2D/3D body has drained.  That
can justify a device-to-CPU cache transition and, after a successful GTI
invalidation, release of only the job's data mappings.  It does **not** prove
that command fetching stopped: retain the current ring page, its PTE, the CR
state, and the engine lifetime, do not reuse either ping-pong slot, and keep
the engine unavailable until `0444.bit31` arrives or a proved replacement or
reset succeeds.  A late snapshot of `DST_POS` alone is weaker than observing
the ordered poll: first confirm that it equals this job's cookie rather than a
prior alternating value.

The `60000000`/pre-cookie instance is not eligible for that split recovery.
Nor may the normal PID00-to-PID10 replacement helper be applied: both open
Linux and Windows paths first require the old reader at PAUSE before changing
A64/A65.  Retargeting STOP while this slot is active and prefetched would
mutate CR ownership without a proven boundary.  If it does not naturally
reach PAUSE under the late deadline, only the separately validated destructive
reset state machine or reboot can make its ring storage releasable.

Before attempting destructive reset, add a bounded late-retirement recovery.
It can recover work which completed just after the ordinary 100-ms deadline:

1. Require the already unplugged, zero-open, no-KMS state; take
   `submit_lock`, leave `stopping=true` and `ready=false`, and do not accept a
   new ioctl.  A timed-out job's `job->slot`, not `engine->active_slot`, names
   the published ring: `active_slot` is updated only after successful
   retirement.  Store each published PAUSE as well for diagnostics/rebuild.
2. For each published quarantine job, require its exact `DST_POS` cookie,
   `0444.bit31`, and `0400 & 00101fe3 == 0` under one new deadline.  Merely
   sampling `043c` remains insufficient.  A non-published VM-only quarantine
   skips the hardware-retirement test but still requires a successful GTI
   invalidation before release.
3. Perform the normal device-to-CPU DMA/cache transition and guard checks,
   preserving the fence's existing timeout error even if late data are valid.
   Under `vm_lock`, retry invalidating the complete retained PTE range with
   `chrome9_gart_set(..., NULL, page_count)`.  Only a successful SR6F
   post-wait permits removing its `drm_mm_node` and clearing `sf_bound`.
4. Splice recovered jobs to a private release list.  Drop both driver locks
   before unmapping, dropping GEM references, or dropping initial fence
   references.  Already signaled error fences are not signaled again.  Drop
   the quarantine device/module hold only after the protected list is empty.

This is a genuine no-reboot recovery for a late completion or GTI retry; it is
not permission to reclassify a persistent 3D busy bit.  If the cookie, PAUSE,
non-CR idle, or GTI post-wait still fails, retain the quarantine.

For a later lab-only destructive recovery, the reusable pieces already in the
tree are the exact H6 2D initializer, the H5S2VP1 3D replay,
`chrome9_gart_set()`/GTI flush, `chrome9_build_prime()`, and
`chrome9_program_ring()`.  They must be composed in this order after the
vendor reset pulses: require the full `0400 & 00101ff3` idle mask, initialize
2D then 3D, reapply the T11 `06/07` and T10 `02` CR setup, reprogram and
verify every owned GART control/PTE, flush GTI, and install a new dummy-only
A60--A68 ring.  Prove its PAUSE/non-CR idle plus a harmless parser side effect
before releasing old mappings.  `active_slot` cannot be trusted across the
reset; start a new software epoch from the newly primed slot.

## First live reset experiment

Do not start with a timed-out 3D job.  On a remotely recoverable machine, first
exercise the sequence while the owned dummy ring is paused and all engine bits
are idle.  Keep scanout memory outside every owned range, save all registers
listed above, and use bounded waits throughout.  A pass requires:

- the full status mask clears;
- GART base/size and PTE readback are restored exactly;
- a newly rebuilt dummy ring reaches its owned PAUSE;
- a subsequent known-good guarded 2D fill and the smallest known-good 3D draw
  both complete;
- display scanout and both LUT-enable bits retain their pre-test state.

Only after repeated idle-reset passes should a deliberately wedged off-screen
workload be used to validate actual recovery.  The Windows busy loops and
literal `0x10000` delays must never be copied without deadlines.
