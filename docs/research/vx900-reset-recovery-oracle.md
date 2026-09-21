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

### Exact 7122 replay stream

The complete PCI-7122 path through `0x896b0` can be represented without
ambiguity as the following run-length table.  `group(S, V...)` means one
`MMIO[043c]=S` write followed by the listed values, in order, to
`MMIO[0440]`.  A range `a..b << 24` is inclusive.  There are no omitted calls
or hidden table lookups in this path.

```text
for offset = 0000..0038 step 4:       MMIO[offset] = 00000000
for offset = 1e00..1fe0 step 20:      MMIO[offset] = 00000000

group(00080000, 00000001)
group(00010000, 00..9a << 24)
for stage = 0..15:
        group(00020000 | stage<<24, 00..30 << 24)
for stage = 0..15:
        group(20020000 | stage<<24, 30..36 << 24)
group(fe020000, 00..0d << 24)

group(12030000,
      00030003 03030300 00040004 04040400
      01010001 00010101 00050005 05050500
      01060006 05060601 020b000b 00020202
      020d000d 050d0d02 00080008 08080800
      0109000c 08090c01 020b000b 080a0a02)

group(15030000,
      41e03c05 07714434 11b36ca5 2176e565 377bd680)

group(17030000,
      40d0340d 41a0681a 42709c27 0330cc33
      04711c47 05715c57 0711c471 08721c87
      0aa2a8aa 0c731cc7 0f63d8f6 11c4711c
      13d4f53d 15b56d5b 1755d575 18e6398e
      1ba6e9ba 1e1785e1 20581605 22689a26
      26198661 296a5a96 2c5b16c5 2f1bc6f1
      31ac6b1a 340d0340 364d9364 386e1b86
      3a6e9ba6 3c6f1bc6 3e4f93e4)

group(00030000, 16 * 00000000)
group(11030000, 32 * 00000000)
group(14030000,  8 * 00000000)
group(00040000, 00..52 << 24)

group(00fe0000,
      00001004 08000249 0a0002c9 0b0002fb
      0c000000 0d0002cb 0e000009 10000049
      110002ff 12000008 130002db 20000000
      40008408 44000000 46000000 47000404)

MMIO[041c] = 00100000; MMIO[0420] = 680a0000
MMIO[041c] = 00100000; MMIO[0420] = 02000000
```

This is 1,355 ordered writes: 31 direct clears and 1,324 transfer-port
writes.  For an extraction cross-check, serializing every write as the
little-endian pair `<u32 offset, u32 value>` yields 10,840 bytes and SHA-256
`4febac244d807fd2ec97a010e530a0d095bf4921b2722ca750c5e2e4bf803d03`.
The literal 20-word table is read from image VA `0x000eace0`; the 7122
generation branch is `0x8a1c0..0x8a342`.

This exact Windows stream must not silently replace Linux's already proven
Xorg-derived initializer.  In addition to the `377bd680` KMD table bug, it
clears only FE entries `00..0d` and type-04 entries `00..52`, then leaves the
7122 FE values `40=008408`, `46=000000`, and `47=000404`.  The Linux
`chrome9_init_3d_h5s2vp1()` deliberately initializes wider ranges and uses the
live-proven `40=00840f`, `46=000005`, and corrected `374bd680`.  The Windows
stream is the exact reset oracle; the Linux stream is the correct candidate
for post-reset operating state.

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

## Cross-generation idle and fence semantics

The older drivers expose several mechanisms called `idle`, `fence`, or
`reset`, but none can replace the VX900 proof used by the current driver:

| source | exact completion test | what it proves / does not prove |
|---|---|---|
| current Chrome9 job | exact alternating `DST_POS` cookie, then `0444.bit31`, then `0400 & 00101fe3 == 0` | the owned body reached its final H0, the reader stopped at its PID00 PAUSE, and all non-CR work drained; lone `0400.bit4` is legal |
| current Chrome9 STOP | `0400 & 00101ff3 == 0` after publishing PID10 | full reader plus engine idle for ring replacement; valid only when the old owned PID00 PAUSE was proved first |
| proprietary 5.75 Chrome9 | `0400 & 00001ff3 == 0`, at most 50,000 reads | omits the current driver's upper `00100000` work class; `WAIT_CHIP_IDLE` discards the helper's `-1` result and always returns success |
| classic DRM `via_wait_idle()` | first waits for `0400.bit17`, then for only `0400 & 00000083 == 0` | older VQ/CR model; excludes Chrome9 `bit4` and most 3D busy bits and cannot be copied to VX900 |
| Mesa 7.11.2 `viaCheckIdle()` | `(0400 & fffeffff) == 00020000` | same older assumption that VQ bit 17 is the idle sentinel; its waits are unbounded |
| Windows reset entry | pre-reset hang mask `00161fff`, post-pulse idle mask `00101ff3` | broad device-wide reset decision followed by full idle; it is not ordinary job retirement |

The classic DRM function named `via_cmdbuf_reset()` is especially misleading:
it appends a STOP and calls `via_wait_idle()` without checking the return.  It
does not pulse a hardware reset, replay state, invalidate translations, or
error fences.  It is evidence only for the old ring's normal STOP protocol.

The completion packets are equally generation-specific:

- Mesa 7.11.2 breadcrumbs are ordered 2D writes to mapped memory.  The light
  wait accepts the breadcrumb alone; the heavy wait separately spins on the
  old status predicate.  A breadcrumb therefore orders prior commands but is
  not, by itself, full engine idle.
- The proprietary H5/H6 fence uses T11 A04--A07, the `Idle` bit, optional
  interrupt-wait and optional system-memory write.  Its H5S2VP1/VX900 path
  instead emits the new FE30 packet with a **zero** engine-wait mask.  The
  source comment acknowledges this as a fence for a CR-busy issue, but zero
  wait does not prove arbitrary 3D retirement.
- Windows emits engine-specific FE3x wait masks: 3D uses FE33/`00200000`, 2D
  uses FE30/`00300000`, DMA uses FE37/`0031e000`, and the no-engine sentinel
  is `003fe000`.  This is the strongest fence-ordering evidence, but the
  current cookie plus owned PAUSE plus status drain remains safer until FE3x
  receives an independent live oracle.
- Windows `QueryCurrentFence` reading `0494` proves scheduler progress in the
  current hardware epoch.  It cannot prove that old DMA translations are
  unreachable after reset, and a stale pre-reset value must never complete a
  new Linux fence.

Always record `040c` as an error discriminator, but `040c=0` is not a
completion condition.  Likewise `043c/0440` are fetch/request cursors with
read-ahead and are diagnostic only.

## Recovery classes before destructive reset

Classify a timeout by independently sampling the ordered cookie, owned PAUSE,
non-CR work mask, full mask, and GTI flush result.  This gives four materially
different recovery actions:

1. **Late ordinary retirement.** A second bounded deadline observes the exact
   job cookie, `0444.bit31`, and `0400 & 00101fe3 == 0`.  Complete CPU
   ownership and guards, invalidate its PTEs with a successful SR6F
   pre/post-wait, remove its node, and release it without resetting the GPU.
   Preserve the fence's original timeout error; late completion is not a
   successful ioctl.
2. **Retired body, reader still active.** The exact cookie and non-CR idle are
   proved, but `0444.bit31` is absent.  Because the immutable stream after the
   final cookie contains only PAUSE/dummy padding, the job's data mappings may
   be CPU-synchronized and invalidated after a successful GTI flush.  The
   persistent ring mapping, ring contents, device/module lifetime and engine
   wedge remain quarantined.  Do not reuse its epoch or signal any success.
3. **Translation-only quarantine.** Command retirement is already proved but
   PTE invalidation or the SR6F post-wait timed out.  Retry that exact
   invalidation under the existing locks and a new deadline.  If it succeeds,
   the job can be released without an engine reset; if it fails, retain it.
4. **Unretired work.** A wrong/missing cookie, active non-CR busy bit, unknown
   fault, or pre-cookie active reader has no selective recovery proof.  Keep
   every reachable object and use only the fully gated destructive reset or a
   reboot.

This classification avoids resetting a healthy GPU merely because a 100-ms
deadline was too short, while retaining the no-late-DMA invariant.  PID10 is
not a rescue command for classes 2 or 4: both Linux and Windows publish a new
STOP only after the previous reader boundary is known.

## Preemption is not hang recovery

Windows `S3gPreemptCommand` at `0x837d0..0x83ac0` programs the documented T10
A08--A0B completion record, but its complete control flow rules it out as a
first Linux recovery mechanism:

- when multi-engine mode is active, `0x838b0..0x83942` spins without a
  timeout until pairs of driver bookkeeping fields become equal;
- only then does `0x83944..0x83a3b` select T10 and write A08, A09, A0A and the
  triggering A0B;
- after the writes it records the requested fence in software and returns;
  it does not poll `0400`, prove an owned PAUSE, invalidate GTI, or establish
  that the preempted command can no longer write memory.

The callback is a WDDM scheduler protocol for a responsive command reader,
not evidence that A0B can interrupt a wedged VP/PS/3D stage.  Even a delivered
preemption completion would order scheduler state, not prove every old SF PTE
safe to remove.  Keep A08--A0B disabled until normal FE3x fences, IRQ/ack, and
the completion-buffer domain have independent guarded tests.

## Reset and containment mechanism ranking

Use the narrowest mechanism whose precondition is already proved:

| mechanism | justified use | release authority |
|---|---|---|
| bounded late cookie/PAUSE/work poll | recover a job that retired after the normal deadline | yes, after CPU sync, guards and GTI invalidation |
| retry the same SR6F invalidation | recover a translation-only timeout after command retirement | yes, only if the post-wait self-clear succeeds |
| PID10 STOP | normal ring wrap/teardown from an owned PID00 PAUSE | yes for the old ring only after full `00101ff3` idle |
| clear PCI bus master | emergency containment before remote reboot | no; it neither resets internal stages nor proves posted/in-flight work drained |
| `SR4E.bit2` 2D engine reset | already-live lab recovery of an isolated 2D fault followed by complete 2D reinit | no authority over CR, 3D, GTI, or arbitrary job BOs |
| T10 A68 clear/restore bits | none yet | no; shipping Windows/Linux paths do not use them for recovery |
| T10 A08--A0B preemption | none yet | no; completion and memory reachability are unproved |
| vendor `SR4E`/`SR1A`/`SRC7` sequence plus full replay/rebuild | future device-wide destructive recovery | yes only after every bounded post-reset proof succeeds |
| generic PCI function/bus reset | prohibited | no VX900 evidence; this integrated display/chipset function has collateral scope outside the render engine |

Clock gating through `SR3F` is not a reset.  Turning a busy clock off can
freeze an in-flight transaction and make status less informative; recovery
must preserve or force-on required clocks until full idle is proved.

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

### Fence failure and reset-epoch ordering

Reset must not signal quarantined reservation fences merely because the reset
pulse was issued.  Fence signaling permits waiters to free or repurpose the
BO, so the exact safe order is:

```text
block submissions; detach/disable completion IRQ handling
set -EIO on every affected fence, but leave it unsignaled
perform reset and prove 0400 & 00101ff3 == 0
invalidate every old transient PTE; complete the SR6F post-wait
perform CPU ownership transition for each retained DMA mapping
remove old SF nodes and make every old command address unreachable
signal the error fences
drop vmaps, GEM references, DMA-map roles and initial fence references
```

A job whose PTE invalidation still fails remains quarantined with an unsignaled
error fence.  For an already-retired, already-signaled error fence, recovery
must not signal it twice; it still cannot drop the mapping until invalidation
succeeds.

Keep `dma_fence_context` and its `atomic64` sequence monotonic across a
successful reset.  Add a separate `reset_epoch` for diagnostics/ring
ownership and increment it before accepting new work; do not recycle fence
sequence numbers or infer completion from the pre-reset `0494` value.  The
alternating H0 cookie also needs a new baseline: after the destructive proof,
read the current `DST_POS` and choose a next cookie unequal to that value.
Only the new owned ring may then write it.  This prevents a stale pre-reset
cookie from satisfying the first post-reset poll.

The first rebuilt ring should contain a harmless ordered H0 cookie write plus
the normal dummy/PID00 tail, not only dummy words.  Require the new cookie,
`0444.bit31`, and non-CR idle before declaring the reset epoch operational.
This proves fetching and H0 execution from the reinstalled SF ring; PAUSE
alone proves less.  Restore the saved `DST_POS` only during final engine
teardown, never between the recovery prime and the first real job.

### Translation and display ordering

The vendor reset assumes its existing GART survives, but Linux should not
resume on that assumption.  After reset/full-idle proof and before installing
the recovery ring:

1. read back `SR6A`, `SR6B`, `SR6C`, `SR7B` and every ring PTE;
2. reprogram the owned GART aperture and ring PTEs if any differ;
3. invalidate quarantined transient data PTEs rather than republishing them;
4. preserve and, if necessary, rewrite active scanout PTEs because IGA fetch
   is independent of CR;
5. issue one barrier plus bounded `SR6F.bit7` flush and require self-clear;
6. only then install and prove the new ring epoch.

An initial destructive-reset oracle should run with KMS disabled.  A later
production path must save the interrupt-enable policy before Windows' global
`0200=0`, acknowledge stale owned `0204` status, restore only driver-owned
enable bits, and restore both LUT controls changed by the reset helper.  It
must not raw-restore pending/RW1C status or enable Windows' DMA interrupt bits
unless Linux has handlers for them.

### Preserve, rebuild, or never restore

The reset transaction cannot use one undifferentiated register snapshot.
Some values are display ownership and must survive; others are command
latches whose old contents are precisely what recovery is trying to discard.
The following split follows the exact Windows write set and the current
Chrome9 driver's ownership model.

| state | reset effect | Linux policy after bounded full idle |
|---|---|---|
| `0200` | literal zero disables the global graphics gate and every display/video/engine IRQ enable in the dword | snapshot only documented RW enable bits `f7ff0800` under the IRQ lock; restore the driver's current enable shadow, not the sampled live/RW1C/RO bits; do not copy Windows' `80a00000` DMA enables |
| `0204` | early 7122/a122 revisions receive narrow `00000020` CR acknowledgement while busy | never raw-restore; acknowledge only owned stale RW1C sources, and separately require `0204.bit1==0` before considering an IGA1 base update latched |
| `SR1B.bit0`, `CR92.bit7` | both are cleared before the hang test, including the already-idle return path | save and restore the two LUT-control bits exactly; `CR92` is not in the current fixed-IGA1 snapshot and must be added to a reset snapshot |
| `SR1A`, `SRC7` | bit 6 and, on 7122, bit 7 are pulsed; other bits are read/modify/written | save for comparison; preserve other bits, but finish with reset bits deasserted rather than reasserting a stale one |
| `SR4E` | literal `0f`, delay, literal `00` destroys the whole old byte | record for diagnostics, but do not raw-restore reset requests; require the final deasserted value and rebuild the affected engines |
| `SR3F` clocks | not written directly, but wider reset collateral is unproved | save, force required 2D/3D clocks on through all idle/GART/ring proofs, then restore the prior gating policy |
| `SR6A/B/C`, `SR7B`, page-table memory | not touched by the vendor helper, which assumes survival | read back and verify against the driver's owned aperture; rewrite base/size/mode and every persistent ring/scanout PTE if needed |
| `SR6F` | not touched | never raw-restore this self-clearing command register; issue a new bit-7 GTI flush with pre- and post-deadlines |
| PCI command/BME | not touched by the vendor helper | verify it did not change; do not use clearing BME as proof of engine retirement |
| `0214`, `CR0c/0d/34/48`, `CR32` | no direct Windows write, but these define active IGA1 SF fetch and flip policy | while KMS is active, preserve base and source/mode bits and retain the active scanout mapping; compare after reset and restore only while blank and serialized with atomic KMS |
| active IGA1 timing/PLL/FIFO/output | no direct Windows write; collateral of `SR1A`/`SRC7` is unknown | first live reset must keep KMS disabled; production must snapshot the exact active state written by `chrome9_iga1_program()`, not the pre-KMS firmware snapshot, and compare before deciding whether a blanked replay is needed |
| MMIO `0000..0038` | all 15 dwords are zeroed | these are 2D command/mode/pitch/dimension/address/cookie latches; never restore a pre-hang command or address. Re-run `chrome9_init_2d_h6()` and establish a new cookie baseline |
| MMIO `0048`, `0058`, `006c` | not directly zeroed, but are part of the current 2D save/test surface | do not preserve an old client operation; the first post-reset guarded 2D packet must provide complete state. Capture only for before/after diagnostics |
| MMIO `1e00..1fe0` | one dword per `0x20` slot is zeroed | CBU-rotation state is destroyed. The current plane accepts rotation zero only, so zero is the intended rebuild; reject recovery with any future CBU/video owner until that owner's replay exists |
| `041c/0420`, `043c/0440`, A60--A68 | selectors/defaults are overwritten; A68 trigger ends clear | never restore cursors or the old ring tuple. Replay defaults, then install a newly owned ring and new pause/cookie epoch |
| `DST_POS`/H0 cookie | cleared as part of `0010` | never restore during recovery; choose the alternate sentinel from the post-reset read and prove the new H0 write |
| `0494` current fence | no explicit write; reset semantics unknown | treat every pre-reset value as stale; retain the monotonic Linux fence timeline and use a separate reset epoch |

For the current fixed mode, an active-state snapshot must at least cover all
indices in `chrome9_saved_sr[]` and `chrome9_saved_cr[]`, plus `SR1A`, `SR3F`,
`SR4E`, `SR6A/B/C/F`, `SR7B`, `CR92`, `SRC7`, VGA selector bytes, Misc,
graphics/attribute state, `0200`'s enable shadow, `0204`, and `0214`.  The
existing `chrome9_kms_state` arrays contain the state which preceded KMS
activation; using them as the reset snapshot would incorrectly restore
firmware mode instead of the active DRM mode.

The current driver's exact GART recovery image is also derivable, rather than
an opaque “restore GART” step.  Let `G` be the configured 64-KiB table byte
offset (required to equal `vram_size - 0x10000`) and `D` the coherent ring DMA
base.  The required post-reset values are:

```text
SR6A = (G >> 12) & ff
SR6B = (G >> 20) & ff
SR6C = (saved_SR6C & ~81) | ((G >> 28) & 1) | 80
SR7B = (saved_SR7B & f0) | 04

PTE[0..15] = ((D + index*1000) >> 12) & 3fffffff
every unowned PTE = 80000000
every retained shmem/scanout PTE =
        ((dma_page >> 12) & 3fffffff) | 02000000
```

`SR6C.bit7` is cleared while editing and restored only after the table is
published and flushed.  The proof is: compare all four programmed control
bytes, compare all 16 ring entries plus every active-scanout entry, require
every quarantined transient range to read `80000000`, execute a posted table
read and `wmb()`, request `SR6F.bit7`, and require bit 7 to self-clear before
and after the request under separate deadlines.  A matching control tuple
without the PTE readback and self-clearing flush is not enough, because reset
may preserve the aperture registers while leaving cached translations stale.

There are likewise two distinct idle proofs.  Immediately after the reset
pulses and before touching old translations, require
`0400 & 00101ff3 == 0`; this is the release authority for old in-flight engine
work.  After rebuilding the ring, require a newly generated cookie,
`0444.bit31`, and `0400 & 00101fe3 == 0`; this proves the new ring fetched,
executed H0, drained work, and stopped at its owned PAUSE.  Record `040c==0`
at both boundaries.  Neither a clear `040c`, a preserved `0494`, nor PAUSE
without the new cookie substitutes for these predicates.

### Bounded Linux transaction

The following is a patch-ready transaction boundary.  Every failure from
step 1 through step 14 leaves `wedged=true`, submissions blocked, old PTEs and
SF nodes retained, active scanout pinned, and error fences unsignaled.

1. Serialize reset against PM/remove and atomic KMS and set a `resetting`
   flag.  Briefly take `submit_lock`, set `ready=false`, and take stable fence
   references to every quarantine job; then drop it, lock all retained object
   reservations with `drm_exec`, reacquire `submit_lock`, and revalidate job
   membership and the reset epoch.  This avoids the normal submit path's
   `dma_resv` -> `submit_lock` inversion.  No normal PID10 ring replacement is
   attempted for an active-reader hang.
2. Mark all quarantined fences `-EIO` without signaling them.  Keep
   `dma_fence_context` and `fence_seqno`; increment a separate `reset_epoch`.
3. With IRQ and VGA-index locks held in their established order, snapshot the
   preserve set above and mask engine/display completion handling.  Preserve
   the active scanout pin and framebuffer reference.
4. Execute the exact destructive pulse order: both LUT helper calls;
   `0200=0` plus posted read; `SR4E=0f`, delay, `SR4E=00`; pulse
   `SR1A.bit6`; on 7122 pulse `SRC7.bit7`.  Preserve the indexed-selector
   bytes around every indexed access.
5. Replace the vendor's unbounded loop with a fixed deadline for
   `0400 & 00101ff3 == 0`.  Revision 0/1 may write only `0204=00000020`
   while polling.  A posted read follows every pulse and acknowledgement.
6. Run `chrome9_init_2d_h6()` and the corrected
   `chrome9_init_3d_h5s2vp1()`; then reapply the T11 A06/A07 and T10 A02
   command-reader preparation.  Do not combine Windows and Xorg FE tails.
7. Read back `SR6A/B/C/7B`, the owned page table, and PCI BME.  Reprogram the
   driver's aperture if any field differs.  Republish only persistent ring
   and active-scanout PTEs; leave every old transient job entry invalid.
8. Issue `wmb()`, set `SR6F.bit7`, and require it to self-clear under both the
   preflush and postflush deadlines.  Read back every ring and scanout PTE.
9. Refill both software ring slots with dummies.  Install a new A60--A68 tuple
   pointing only at a newly selected slot and a new PID00 PAUSE; never reuse
   the saved A64/A65 or `active_slot` as authority.
10. Submit an address-free H0 write of a new alternating cookie before that
    PAUSE.  Require the exact cookie, `0444.bit31`,
    `0400 & 00101fe3 == 0`, and `040c==0`.  Then publish PID10 and require
    full `0400 & 00101ff3 == 0` before installing the ordinary paused ring;
    require its new `0444.bit31` plus non-CR idle once more.
11. While still unavailable to userspace, run one guarded 2D fill and the
    fixed guarded 3D oracle.  Require destination samples, all guards,
    cookie/PAUSE/work idle, and `040c==0` for both.
12. If KMS is active, compare the saved active display state.  On any changed
    timing/base/source field, blank IGA1, replay the active DRM mode and
    `0214`, require `0204.bit1==0` and base readback, then unblank.  Restore
    the two LUT bits even if the engine was found idle.
13. Restore the saved `0200` **enable shadow** under the IRQ lock, after
    narrowly acknowledging stale owned status.  Resume completion handling;
    do not write back the raw sampled dword.
14. With every quarantined object's reservation still held, complete the DMA
    CPU-ownership transition, remove old SF nodes, signal the `-EIO` fences,
    and drop mappings/references.  Only then clear `wedged/resetting`, set
    `ready=true`, release the quarantine module/device hold, and finish the
    `drm_exec` transaction.

### Pulse duration recovered on the target CPU

The binary has **five**, not one, identical delay loops: one while `SR4E=0f`,
one while `SR1A.bit6=1`, one after that bit is cleared, one while
`SRC7.bit7=1`, and one after that bit is cleared.  There is no loop between
`SR4E=00` and the initial `SR1A` read.  The loop body is exactly:

```asm
mov    eax,DWORD PTR [ebp-local]
add    eax,1
mov    DWORD PTR [ebp-local],eax
cmp    DWORD PTR [ebp-local],10000h
jae    done
jmp    loop
```

Neither the old Linux KMD, classic DRM, nor Mesa 7.11.2 contains this full
reset or an alternative time-valued delay.  The Windows binary imports
`KeStallExecutionProcessor` but deliberately does not use it here, so static
analysis cannot turn the iteration count into a portable hardware minimum.

It can, however, be measured on the actual target CPU without touching the
GPU.  The exact dependency chain above was compiled with the same loop
opcodes and run for 4,096 samples, three times, pinned to CPU 0 on the target
VIA Nano U3500 at a reported fixed 1.000 GHz:

```text
run  min (ns)  p50 (ns)  p95 (ns)   p99 (ns)   max (ns)
 1     405079    432597    1782978    2022813    2947442
 2     405079    432806    1785842    2044882    3356432
 3     405149    574095    1781721    2274871    7636865
```

The repeatable minimum is about 405 microseconds, or about 6.18 target CPU
cycles per iteration; scheduler/power-management interference only lengthens
it.  The hardware-free source is retained on the target as
`/root/tmp/chrome9/chrome9_reset_loop_bench.c`, SHA-256
`444f1ff11bdcfd2a35a1b220bfc14636dafe613e06b86b98d95b554c8ab74f32`;
the measured executable SHA-256 is
`10fbd879cc72bdfdeb0fcbc724a4c6445f4670855b5f2182c9bfcbf2b6a999ae`.

This resolves the target-specific starting floor, not a portable silicon
timing specification.  The first Linux oracle should use `udelay(500)` at all
five Windows delay positions: it exceeds the measured minimum by roughly
23%, cannot wake early, preserves the vendor ordering, and totals only 2.5 ms
of busy wait.  Keep the value an explicit logged lab parameter.  Do not add an
invented delay between `SR4E=00` and `SR1A`, and do not interpret a successful
500-microsecond test as permission to shorten it.  The status, PTE, ring and
guard proofs remain the authority; the delay alone proves nothing.

Reservation locking also needs special care.  The quarantined fence is
already installed in each BO reservation, so recovery may use `drm_exec` to
lock the retained objects but must not wait on the reservation fence it is
itself trying to resolve.  Preserve the established order `dma_resv` before
`submit_lock` before `vm_lock`: briefly freeze admission and take stable job
references, drop `submit_lock`, acquire all reservations with `drm_exec`, then
reacquire `submit_lock` and revalidate `reset_epoch`/job membership before
touching transient PTEs.  Holding `submit_lock` while contending on
`dma_resv` would invert the normal submit path and can deadlock recovery.

## Current `engine_quiesce` and recoverable-timeout audit

The original audit used Chrome9 kernel
`b6b6a1921d4421f02c13a22188b244c890e1b4d1`; the patch-ready refresh and exact
current-source hashes appear below.  The sysfs quiesce entry first unplugs DRM,
requires zero open files, and calls `chrome9_engine_fini()`.
Engine teardown holds `submit_lock`, stops admission, then refuses every
already wedged/quarantined engine or live scanout mapping before touching its
GART/ring allocation.  That refusal is correct for reachability but means the
entry is not yet a recovery path.

The timeout lifetime is deliberately asymmetric.  A cookie/PAUSE/work timeout
sets `-ETIME`, links the job into `engine->quarantine`, and leaves its fence
unsignaled plus all GEM references, vmaps, SGT roles, PTE image and SF node
retained.  A post-retirement PTE-invalidation timeout quarantines the same
mapping but may signal its error fence because rendering and CPU ownership
were already proved.  `chrome9_fence_release()` frees only the PTE array and
job allocation; a recovery path must explicitly perform the missing DMA
sync, unmaps, GEM puts, SF-node removal, quarantine-list removal and initial
fence put in the appropriate case.  Merely deleting the list entry would leak
or prematurely free resources.

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

Before attempting destructive reset, add the bounded non-destructive tranche
below.  This audit was refreshed against the current source snapshot whose
`chrome9_engine.c` SHA-256 is
`294eb659db2a25b909dca8d1de51603355fd3df03133e6f97eba9f97f8af54a5` and
whose `chrome9_drv.c` SHA-256 is
`aa9f64d7858581624544dbb7ab489f2d1777acdfef9d41c4ac4d60a04f14ddc4`.

### Current state is almost sufficient, but `published` is ambiguous

`struct chrome9_job` now retains all one-to-four GEM roles, vmaps, cached
shmem SGT pointers, the complete PTE image and page counts, its `drm_mm_node`,
cookie, segment/ring PAUSE and STOP, and `retirement_proven`.  That last bit is
set only after `chrome9_wait_job()` has observed, in order, this job's cookie,
`0444.bit31`, and no `0400 & 00101fe3` work.  It therefore exactly identifies
a post-retirement GTI-invalidation quarantine.  The normal timeout path leaves
it clear.

Two details prevent a recovery helper from safely inferring the remaining
class from existing booleans:

* `published=true` normally means `chrome9_append_segment()` published this
  job's A64/A65 tuple.  But when wrap first publishes PID10 STOP for the *old*
  ring and that STOP times out, both submitters manually set the new job's
  `published=true`, even though its body was never copied/installed.  Its
  `ring_pause` is not an owned late-retirement target.  Treating every
  `published && !retirement_proven` job as late work would be unsafe.
* A GTI failure can occur before command publication.  The BOs have already
  received `dma_sync_sgtable_for_device()` and the reservation fence is
  attached if the bind succeeded far enough to build a command.  An initial
  bind failure can instead reach the same quarantine before either action.
  In both cases transient PTE state remains reachable but no command references
  it.  This is translation-only recovery, not a command timeout; only the
  former subcase needs a device-to-CPU ownership transition.

Add an explicit, immutable quarantine reason at the four call-site classes,
rather than deriving it later:

```text
CHROME9_QUAR_GTI_UNPUBLISHED  out_prepublication invalidation failed
CHROME9_QUAR_GTI_RETIRED      retirement_proven, final invalidation failed
CHROME9_QUAR_JOB_TIMEOUT      append succeeded, chrome9_wait_job timed out
CHROME9_QUAR_RING_STOP        wrap PID10 STOP was published but timed out
```

Also record `publish_epoch=engine->ring_epoch` only after the new job tuple is
actually published.  Do not set it in `CHROME9_QUAR_RING_STOP`.  A reason enum
is preferable to testing whether the fence is signaled: signaling describes
userspace completion, not hardware ownership, and is too indirect to be a
memory-lifetime invariant.

The exact current call-site state is:

| reason | this job body | retirement | fence | DMA ownership |
|---|---|---|---|---|
| `GTI_UNPUBLISHED` after initial bind failure | never published | not applicable | not attached, signaled error | CPU |
| `GTI_UNPUBLISHED` after later build/append rejection | never published | not applicable | attached, signaled error | device |
| `GTI_RETIRED` | published | cookie + PAUSE + work idle proved | attached, signaled error | CPU |
| `JOB_TIMEOUT` | published | not yet proved | attached, unsignaled error | device |
| `RING_STOP` | never published; only the old ring's PID10 was published | old-ring STOP unproved | attached, unsignaled error | device |

Use a job `device_owned` bit, initialized clear, set at every existing
`dma_sync_sgtable_for_device()` group and cleared at the CPU transition.  This
is less error-prone than a `cpu_owned` default because it represents the only
state which requires recovery work and cleanly covers both unpublished GTI
subcases.

### Engine busy versus a lost interrupt

There is no render-completion IRQ in the current submit path.  Jobs end with a
2D `DST_POS` cookie and `chrome9_wait_job()` polls MMIO three times; it neither
enables `0200.bit31`, consumes `0204.bit5`, waits on an IRQ, nor submits an
FE3x interrupting fence.  Therefore a current `JOB_TIMEOUT` cannot be
classified as “lost IRQ.”  If the second ordered poll succeeds, it was late
hardware/software polling completion.  If it fails, the hardware is still
busy or its state is unclassified and the mapping remains quarantined.

Classify the late sample by the first failed ordered predicate:

| cookie | owned PAUSE | non-CR work | meaning | release |
|---|---|---|---|---|
| wrong | any | any | this body has not proved its ordered final write | no |
| exact | clear, including `0444=60000000` | clear | body drained, reader still active | not in the first tranche |
| exact | set | nonzero | CR stopped but an already-dispatched engine is busy | no |
| exact | set | zero | late ordinary retirement | yes, after CPU sync and successful GTI invalidation |

Sample `040c`, `043c`, and `0440` for diagnosis only.  Neither a clear error
register nor a fetch cursor upgrades any row.  `0400.bit4` alone is the legal
paused reader and is excluded by `00101fe3`; using the full `00101ff3` mask
would falsely turn normal PAUSE into a busy engine.

A future IRQ-driven implementation may call a timeout “lost IRQ” only when an
independent, same-epoch completion record proves the exact fence ID (the
Windows path uses an FE3x system-memory write and `0494`), while the cookie,
owned PAUSE and non-CR-idle proof also succeeds.  `0204.bit5` merely reports a
pending CR interrupt and `0494` alone can be stale; neither permits release.
Bring up that path separately as already specified: coherent system write by
polling, then interrupt generation without interrupt-wait, exact RW1C ack of
only `0204.bit5`, and only then interrupt-wait.  Recovery of today's cookie
jobs must not consult `0494` or alter IRQ registers.

### Smallest safe recovery transaction

Keep the first implementation behind the existing irreversible
`engine_quiesce=1` flow: DRM is unplugged, `open_count` is zero, admission is
permanently closed with `stopping=true, ready=false`, and any live or
quarantined scanout mapping makes recovery return `-EBUSY`.  This deliberately
recovers resources for clean teardown; it does not resume userspace submission
and therefore needs no reset-generation ABI yet.

Recover only `GTI_UNPUBLISHED`, `GTI_RETIRED`, and `JOB_TIMEOUT`.  A
`RING_STOP` job remains reboot/destructive-reset-only in this tranche.  In
particular, do not mistake the later disappearance of one busy bit for proof
that the partially replaced ring can be stopped a second time.

The transaction is:

1. Under `submit_lock`, freeze the engine and take a temporary fence reference
   for the sole quarantined job.  A first wedge prevents subsequent submits,
   so more than one job is an invariant failure and must return `-EUCLEAN`.
   Drop `submit_lock` before reservation locking.
2. Lock every retained, distinct GEM reservation with `drm_exec`, in the same
   role order used by structured submit.  Do **not** call
   `dma_resv_wait_timeout()`: the attached fence is precisely the unsignaled
   fence being recovered.  Reacquire `submit_lock` and revalidate list
   membership, reason, `stopping`, and the fence identity before MMIO.
3. For `JOB_TIMEOUT`, additionally require
   `publish_epoch == engine->ring_epoch`,
   `ring_pause == engine->active_pause`, and
   `ring_stop == engine->active_stop`.  Then repeat the existing ordered,
   bounded retirement proof: exact `DST_POS == job->cookie`, followed by
   `0444.bit31`, followed by `!(0400 & 0x00101fe3)`.  Never accept `043c`, a
   cookie alone, full-idle `00101ff3`, or a PAUSE without the cookie.  On
   success set `retirement_proven=true`; on any miss, leave every resource and
   fence unchanged.
4. If `device_owned` is set, run
   `dma_sync_sgtable_for_cpu(..., DMA_BIDIRECTIONAL)` for every present role,
   flush/invalidate each entire mapped GEM size, and `dma_rmb()`.  Record a
   per-job transition by clearing `device_owned` so the retired-GTI case is not
   transitioned twice.  Preserve exact logical guard
   offsets in the job and run the same guard checks for a late job.  A failed
   guard upgrades/logs the fence error but does not forbid PTE invalidation and
   release: the existing normal path also releases after an intra-object guard
   failure once DMA retirement is proved.  The fence keeps its original
   `-ETIME`/`-EIO`; late completion is never reported as success.
5. Still under `submit_lock`, take `vm_lock` and retry exactly
   `chrome9_gart_set(engine, job->sf_node.start >> PAGE_SHIFT, NULL,
   job->page_count)`.  Only its successful SR6F prewait, invalid-PTE writes,
   posted read, barrier, flush request and postwait authorize
   `sf_bound=false`, `drm_mm_remove_node()`, and `node_allocated=false`.
   A failed retry changes no software ownership and returns `-EBUSY`.
6. Remove the job from the quarantine list, but keep the temporary fence
   reference.  Drop `vm_lock` and `submit_lock`.  While `drm_exec` still owns
   every reservation, call `drm_gem_shmem_vunmap_locked()` for each mapped
   role.  The SGT pointers came from `drm_gem_shmem_get_pages_sgt()` and are
   object-cached; do not `sg_free_table()` or invent a separate SGT put.  Finish
   `drm_exec`, signal an unsignaled error fence exactly once, clear the job's
   object pointers, put each retained GEM lookup reference, and finally drop
   the original and temporary fence references in an order that keeps the job
   alive throughout cleanup.
7. Recheck `0444.bit31 && !(0400 & 00101fe3)` after the last release.  If the
   quarantine and scanout lists are empty, clear `wedged` only to let
   `chrome9_engine_fini()` execute its existing PID10 STOP, ring invalidation,
   saved-GART restore and teardown.  Leave `stopping=true` and `ready=false`.
   Release the quarantine's extra device reference only here; retain the
   engine's normal `module_hold` until successful engine teardown.  If STOP or
   restore subsequently fails, the existing teardown path reacquires/retains
   the safety hold.

Factoring role iteration into three small helpers keeps this tranche bounded:
`chrome9_job_lock_resvs()`, `chrome9_job_sync_for_cpu()`, and
`chrome9_job_release_locked_vmaps()`.  The first patch should not implement the
more complex “body retired but reader still active” split: that needs a second
lightweight ring-quarantine object after BO release and gains nothing for the
owned-PAUSE late-retirement case.

This is a genuine no-reboot release for a job which completed just after the
ordinary deadline and for a late SR6F/GTI flush.  It is not permission to
reclassify a persistent 3D bit, a `60000000` active command cycle, the
wrap-time PID10 STOP class, or any job whose cookie/epoch/PAUSE/work/GTI proof
fails.

### Minimum no-wedge validation

Add counters for reason, attempted recovery, late-retired, GTI-retried,
released, and refused.  Validate without injecting a real GPU hang:

1. A fault injection immediately before final GTI invalidation creates
   `GTI_RETIRED`; allow SR6F to clear, quiesce, and require one released job,
   an error fence, invalid PTE readback, an empty SF node, and successful normal
   teardown/GART restore.
2. Inject the same one-shot invalidation failure before publication and prove
   the CPU ownership transition occurs before vunmap.
3. Shorten only the software retirement deadline so a known-good oracle lands
   in `JOB_TIMEOUT`; recovery must accept only the exact cookie/epoch/owned
   PAUSE/non-CR-idle tuple, preserve `-ETIME`, and then pass normal teardown.
4. Corrupt each discriminator in turn (cookie, epoch, active PAUSE, work mask,
   SR6F postwait) and require no PTE/node/vmap/GEM/fence/list mutation.
5. Force wrap-time PID10 STOP timeout and require unconditional refusal even
   though `published` is set.  This is the regression test for the current
   boolean ambiguity.

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

Do not start with a timed-out 3D job.  The minimal oracle is an **idle reset**
over the already proved persistent dummy ring.  It is deliberately arranged
so the display driver has no live ownership:

1. Boot or load with `kms=0` and `fixed_iga1=0`; refuse the oracle if a
   `chrome9_kms_state` exists, an active/blocked framebuffer or scanout mapping
   exists, any DRM file is open, or any framebuffer console is bound to this
   PCI function.  Use remote SSH as the only control path.  Do not substitute
   “screen appears idle” for these checks.
2. Require the current owned ring at PID00 PAUSE, exact known last cookie,
   `0400 & 00101fe3 == 0`, and `040c==0`.  This is safe idle input even though
   the paused command reader leaves `0400.bit4` set and therefore makes the
   Windows pre-reset `00161fff` test enter the destructive path.
3. Snapshot the complete preserve matrix, all 16,384 PTEs, both ring slots,
   active ring bounds/pause, PCI command, `0200/0204/0214`, and hash every
   retained CPU buffer.  Allocate no new mapping after this point.
4. Execute one exact pulse sequence with the five delays set to **1,000 us**.
   Verify each asserted and deasserted indexed byte by readback.  Use a
   100-ms bounded full-idle poll, the exact replay/rebuild transaction above,
   and stop immediately on the first failed predicate.
5. Prove the new GART image, new H0 cookie, owned PAUSE, non-CR idle, full
   PID10 idle, error register, guarded 2D fill and guarded fixed 3D draw.  Then
   compare every display/coexistence snapshot and restore only the classified
   preserve set.
6. After a clean unload and preferably a cold reboot, repeat once at the
   measured **500-us** floor.  There is no value in binary-searching below the
   known Windows-equivalent floor during bring-up.

The 1-ms first pass is the safer discriminator: success proves ordering and
rebuild without yet claiming the smaller timing bound; failure proves the
problem is not a marginally short pulse.  Only a 1-ms pass followed by a
separate 500-us pass supports selecting 500 us as the lab default.

A pass requires:

- the full status mask clears;
- GART base/size and PTE readback are restored exactly;
- a newly rebuilt dummy ring reaches its owned PAUSE;
- a subsequent known-good guarded 2D fill and the smallest known-good 3D draw
  both complete;
- display scanout and both LUT-enable bits retain their pre-test state.

Hard-stop conditions are any unexpected display owner, failed reset-byte
readback, `0400 & 00101ff3` at the post-pulse deadline, nonzero `040c`, changed
PCI BME, GART-control/PTE mismatch, `SR6F.bit7` timeout, wrong cookie, missing
owned PAUSE, guard/canary corruption, or any display snapshot difference not
covered by the explicit restore.  On a hard stop retain all mappings and the
module/device holds and recover only by reboot; do not try another delay rung.

Only after repeated idle-reset passes should a deliberately wedged off-screen
workload be used to validate actual recovery.  The Windows unbounded status
poll must never be copied, and the recovered 500-us pulse floor does not relax
any post-reset deadline.
