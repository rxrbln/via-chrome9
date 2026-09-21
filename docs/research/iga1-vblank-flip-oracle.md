# VX900 IGA1 vblank and direct-flip oracle

This note narrows the first zero-copy KMS implementation to one IGA1, one
outstanding direct-base flip, and one unambiguous completion rule.  It
cross-checks the VX900 manuals against the old upstream VIA IRQ code and the
supplied Windows and Linux proprietary drivers.  It does not make the command
regulator's T11 automatic flip part of the userspace render ABI.

## Exact interrupt register

The VX900 Part II manual, Video Register printed pages 9--10, defines MMIO
`0200` as a mixed interrupt-enable, RW1C-status, and live-level register.  The
IGA1 fields are:

| bit | attribute | meaning |
|---:|---|---|
| 31 | RW | global graphics interrupt enable |
| 19 | RW | VGA/IGA1 VSYNC interrupt enable |
| 3 | RW1C | VGA/IGA1 VSYNC interrupt pending |
| 1 | RO | IGA1 currently in vertical blank |

The supplied Chrome9 header independently names the same values at
`via_chrome9_3d_reg.h:300..325`: `80000000`, `00080000`, `00000008`, and
`00000002`.  The old upstream driver independently uses bit 19 as the pipe-zero
enable and bit 3 as pending at `via_dri1.c:310..325` and
`via_dri1.c:2214..2319`.

Do not use bit 1 to count vblanks.  It is a level which can be sampled high
more than once.  Only a sampled bit-3 pending edge advances the DRM vblank
sequence.

All other `0200` sources share this dword.  From the manual's attributes the
complete control-bit mask is:

```text
0200 documented RW enables = f7ff0800
0200 IGA1 controls          = 80080000
0200 IGA1 RW1C status       = 00000008
0200 IGA1 blank level       = 00000002
```

`f7ff0800` includes only documented enables: bits 31--28 except status bit 27,
bits 26--16 except status bits 15--12, and bit 11.  In particular, it excludes
every documented status bit and RO bit 1.  Maintain a driver-owned enable
shadow under the IRQ lock.  Seed it once from `readl(0200) & f7ff0800`, then
change only owned enable bits.  An IGA1-only acknowledgement is exactly:

```c
writel(irq_enable_shadow | BIT(3), mmio + 0x0200);
readl(mmio + 0x0200);                 /* post/flush before leaving the ISR */
```

This is safer than writing the whole sampled dword: doing that writes one to
every simultaneously sampled RW1C source.  The old DRI1 handler does echo the
whole sample at `via_dri1.c:2268..2269`; that proves RW1C behavior but is not a
safe model for independently owned modern IRQ sources.

## Independent Windows sequence

The Windows 7 KMD provides a chip-matching executable oracle:

* `VTGKModeDX32A.sys:0x767e0..0x7691c` reads `0200`, sets global bit 31, and,
  when IGA1 exists, sets enable bit 19 and status bit 3 in the value written
  back.  Including bit 3 clears a stale pending interrupt as IGA1 is enabled.
  It similarly uses bits 17/15 for IGA2.
* Its top-level ISR at `0x77a60..0x77bb7` reads `0200`, `0204`, `1280`, and
  `c500`, and rejects the graphics interrupt if `0200==ffffffff` or global bit
  31 is clear.
* The IGA1 branch at `0x772c0..0x774ea` tests bit 3.  It acknowledges `0200`
  first at `0x7733d..0x77360`, reads the current packed IGA1 base from `0214`
  at `0x773a6..0x773c3`, converts it back to a byte address, and reports that
  address in its display-interrupt callback at `0x774a7..0x774e0`.

The Windows acknowledgement uses
`(sample & ffff6e56) | 80000000 | 00000008`.  That mask is useful evidence of
the target RW1C bit and required global gate, but it deliberately couples
other vendor interrupt classes.  A new driver should use its enable shadow and
the single classified RW1C bit instead.

The Windows enable routine also clears CRTC F3 bit 1 while display interrupts
are active.  The Part I manual identifies that bit as snapshot-mode graphics
PLL power-off, not an IGA1 interrupt gate.  It is irrelevant while the new
driver leaves snapshot mode disabled.  Windows does not modify CR11 in this
path.  The old DRI1 writes CR11 bits 5:4 as undocumented magic, conflicting
with the VX900 manual's legacy vertical-interrupt descriptions; do not copy
that write into the first VX900 oracle.

At instruction level, `0x767e0..0x7691c` performs one final `0200` write.  It
reads `0200` at `0x767f0..0x76801`, unconditionally ORs global bit 31 at
`0x76804..0x7680d`, and for an active IGA1 ORs enable bit 19 and RW1C bit 3 at
`0x76836..0x76848`.  It then writes the accumulated dword at
`0x76906..0x76916`.  This proves the required values and ordering, but not that
echoing every other bit from the sampled mixed-status register is safe.  The
control-only Linux shadow above preserves the order without acknowledging
unowned status classes.

### Minimal base-invariant vblank cadence oracle

Before enabling page flips, prove the interrupt path while the framebuffer
base and all SF mappings remain unchanged.  The bounded lab-only sequence is:

```text
pre: engine retired, 040c=0, fixed IGA1 active, save 0200/0204/0214/CR32/CR48

under irq_lock:
    control = readl(0200) & f7ff0800
    shadow = control | 80000000 | 00080000
    writel(shadow | 00000008, 0200)    # enable and clear stale IGA1 RW1C
    readl(0200)                        # posting barrier

IRQ:
    sample = readl(0200) exactly once
    reject ffffffff, global-clear, or bit3-clear
    writel(shadow | 00000008, 0200)    # early, narrow acknowledgement
    readl(0200)
    drm_crtc_handle_vblank(crtc)
    record count, sequence, timestamp, 0204, and 0214
```

Accept only after at least 120 consecutive IGA1 IRQs at the fixed 60-Hz mode,
with a monotonic DRM vblank sequence, no implausible sub-millisecond IRQ storm,
unchanged masked `0214`, unchanged `0204.bit1`, no engine error, and no display
corruption.  A practical timing bound is 14--20 ms for every interior interval;
log outliers rather than silently fabricating an event.  This is cadence
validation, not a flip, so no framebuffer reference or PTE may change.

Abort immediately if acknowledgement fails to clear bit 3, the rate exceeds
200 IRQ/s, an unowned `0200` enable changes, `0214` changes, `0204.bit1`
changes, or SSH/display health is lost.  On abort or normal completion, clear
only owned bit 19, write the control shadow plus bit 3 to clear the final
edge, flush-read, and `synchronize_irq()`.  Clear global bit 31 only if this
oracle introduced it and no other enabled source remains.  Restore neither
the whole sampled `0200` dword nor a literal zero.

Only after this oracle passes may one armed flip attach its pending state to a
recorded sequence and apply the three-part completion predicate.  The cadence
oracle is deliberately useful even if the later base latch still fails: it
separates missing interrupt ownership from CR32/base-latch semantics.

## Exact direct-base latch

The Part II manual, Video Register printed page 10, defines MMIO `0204[1]` as
the read-only **Graphics Starting Address Update Status**:

```text
0 = updated
1 = not yet updated
```

Hardware sets the bit when software writes packed IGA1 base register `0214`
and clears it when the starting address is updated.  This is the missing IGA1
latch observation.  It is distinct from `0200[1]` (currently in blank) and
`0200[3]` (vblank interrupt pending).

For direct by-frame presentation, Part I printed page 61 defines CR32 bit 0 as
zero for flip-by-frame and one for flip-by-scan-line.  The Windows IGA1 helper
at `0x795e4..0x79633` selects that bit, and its normal 7122 path at
`0x79683..0x796de` preserves `0214[31:29]` while inserting the new two-byte-unit
address:

```text
new_0214 = (old_0214 & e0000000) | ((sf_byte_address >> 1) & 1fffffff)
```

Before the direct write it sends T11 A12=`12000000` at
`0x79580..0x795bc`, disabling automatic swapping.  The proprietary Linux DRI
independently emits T11 A10--A12 for by-frame automatic flips and clears CR32
bit 0 on the IGA1 path.  Therefore direct `0214` and an armed T11 flip are two
mutually exclusive mechanisms.

The proprietary Xorg binary supplies an address-packing cross-check, though
not an interrupt completion primitive.  Its exported
`VIASetStartingAddress` (`via_drv.so:0x4cc10`) shifts the IGA1 byte address
right once and writes CR0D, CR0C, CR48, and CR34 at
`0x4cd7a..0x4ce2b`.  This agrees with Windows `0214`; Xorg has no independent
proof that a direct write has latched, so its programming return must not be
treated as flip completion.

The completion predicate for one serialized direct flip is:

```text
a post-arm 0200.bit3 vblank was handled
and 0204.bit1 == 0
and (0214 & 1fffffff) == ((requested_sf_va >> 1) & 1fffffff)
```

The masked `0214` comparison is diagnostic protection against another writer;
`0204[1]==0` is the documented hardware-latch proof.  Merely observing the
new value read back from `0214` is not sufficient because a posted/programmed
register value need not prove that display fetch has adopted it.

It is legal for `0204[1]` to clear before software samples it, for example when
the write occurs during a blank.  Do not require a preceding observation of
one.  Conversely, if the first post-arm vblank still finds bit 1 set, keep the
flip pending and try the next bounded vblank; never complete it from elapsed
time alone.

## Minimal implementation state machine

IRQ installation and vblank enable:

1. Under the IRQ lock, sample only `0200 & f7ff0800` into the enable shadow,
   mask owned bit 19, and clear stale IGA1 pending by writing
   `shadow | 00000008` followed by a flush-read.
2. Install the shared handler while the owned IGA1 source remains masked; only
   then expose vblank support.
3. From the DRM `enable_vblank` callback, set `80080000` in the shadow, clear
   stale bit 3 with one narrow RW1C write, then write the status-free shadow
   and flush-read.
4. Enable DRM vblank accounting and verify that bit-3 interrupts
   advance at the fixed mode's expected cadence.  A read of `ffffffff` or a
   cleared global bit is not a handled device interrupt.

Arming one flip:

1. Retire the render dependency; pin and bind the complete new SF scanout BO.
   Keep the current BO pinned and mapped.
2. Refuse a second flip while one is pending.  The generic 7.2 atomic helper
   normally prevents this by keeping the preceding commit's `flip_done`
   incomplete until its retained event is sent: nonblocking successors return
   `-EBUSY` and blocking successors wait in the dependency path.  The runtime
   refusal is a fail-closed invariant check.  Also refuse to overwrite `0214`
   if `0204[1]` still reports a previous update pending.
3. Take a DRM vblank reference.  Under the IRQ/state lock, acknowledge any
   already-pending bit-3 edge, record a driver-private monotonic handled-IRQ
   serial, and publish the expected base before the MMIO write.  The old and
   new framebuffer references remain owned by the sleeping atomic update
   thread.  A DRM vblank sequence alone is insufficient for the race below.
4. Ensure T11 A12 is disarmed and CR32 bit 0 was cleared during the full,
   blanked IGA1 enable, write the packed `0214` value, and read it back to
   flush the posted write.  There must be exactly one direct-base writer; do
   not perform indexed-VGA I/O in the ordinary flip callback.

ISR:

1. Read `0200` once.  If bit 3 is clear, do not claim IGA1 vblank.
2. While holding the driver IRQ lock, write
   `irq_enable_shadow | 00000008`, flush-read, increment the private IRQ
   serial, and release the driver lock.
3. Call `drm_crtc_handle_vblank()` exactly once outside the driver lock.  DRM
   can invoke `enable_vblank` while holding its own vblank locks; nesting the
   inverse order in the ISR would deadlock.
4. Reacquire the driver lock.  If a flip was armed before this private edge
   serial, sample `0204` and `0214`.  If arming raced after the acknowledge but
   before the DRM callback, it recorded this already-incremented serial and
   the old edge is therefore ignored.
   Complete/send its DRM event only when the predicate above holds.
5. Drop the old framebuffer's display pin/SF mapping only after that event/latch
   point.  The new framebuffer remains display-owned.  Since unbinding may
   sleep, the hard IRQ only records the result and completes the sleeping
   atomic update thread; it does not send the event or tear down an SF mapping.

This deliberately allows a one-frame-late event if ordering is ambiguous.  A
late event is recoverable; unmapping a framebuffer which display may still
fetch is not.

## Failure-safe disable and teardown

Normal vblank disable is ordered:

1. clear bit 19 in the enable shadow while preserving every other enable;
2. write `shadow | BIT(3)` to disable and clear a final IGA1 pending edge;
3. flush-read `0200` and `synchronize_irq()`;
4. call the matching DRM vblank-off path and drop its vblank reference.

Do not clear global bit 31 while any other shadowed source remains enabled.
Do not write literal zero or `80000000` to `0200`; both destroy unrelated
owners' enables.

For CRTC disable or driver teardown, keep every possibly scanned-out SF mapping
valid while interrupts are masked.  Disarm T11, blank/disable or restore IGA1
using the existing display sequence, and only then unbind the active mapping.
SR01 bit 5 blanking alone is not documented as a memory-fetch-stop proof, so a
failure before the complete CRTC disable/restore is not permission to unmap.

If `0204[1]` remains set beyond the bounded flip window, the IRQ cadence stops,
the expected base mismatches, or VGA ownership prevents the disable/restore:

* do not send a successful page-flip event;
* do not unpin either the old or new framebuffer;
* quarantine both SF mappings as potentially display-reachable;
* stop accepting flips and proceed to a full CRTC disable/recovery while those
  mappings remain valid.

Only confirmed latch completion or confirmed display disable transfers the old
mapping out of display ownership.  On an unrecoverable teardown failure,
retaining the mappings until device power-down/removal is safer than freeing
pages which an active display client may DMA-read.

## First live oracle

### 2026-09-21 direct-write result: address accepted, latch did not complete

The first bounded fullscreen run rejects the earlier assumption that a
polled blank transition is sufficient for this KMS image.  Initial scanout was
FB 40 at SF byte address `00011000`, with `0214=00008800` and
`0204=00000c04`.  Xorg then requested FB 42 at plane offset zero and SF byte
address `00312000`.  The driver wrote and read back:

```text
requested byte address       00312000
expected packed 0214         00189000
observed 0214                00189000
observed 0204                00000c06
observed 0200                00080002
```

The packed address is bit-exact: `00312000 >> 1 = 00189000`.  This proves the
MMIO write reached the documented starting-address shadow.  It does not prove
that the display fetch latch adopted it.  `0204.bit1` remained set for the
complete 100-ms bounded poll, spanning several 60-Hz blank intervals, and the
driver reported `-ETIMEDOUT`.  The mapped old and new framebuffers were
retained.  A one-shot Xorg stop produced generic helper `flip_done` timeouts,
then completed the full pre-KMS VGA restore.  Debugfs framebuffer state became
empty, `0214` returned to zero, `0204` returned to `00000c04`, and both `0400`
and `040c` were zero.  The failure was therefore contained without a GPU
fault or leaked active framebuffer.

The final `0200=00080002` has IGA1 VSYNC enable bit 19 and live blank level
bit 1, but lacks global interrupt-enable bit 31 and has no sampled pending
bit 3.  This is not the complete Windows interrupt state.  The Windows
`S3gSetVidPnSourceAddress` call chain configures IGA1 display interrupts before
calling the exact `0x79080` source-address helper.  Re-disassembly of
`0x79580..0x796fb` confirms that the normal VX900 branch performs T11 A12
disarm, optional CR32 bit-zero selection, and the masked `0214` write, then
returns.  It emits no hidden post-write trigger and does not poll `0204`.
Inventing another MMIO write is therefore unsupported by the executable.

The only exact missing call-chain prerequisite is the complete Windows-style
interrupt setup and acknowledgement.  Whether that setup is required for the
hardware latch itself, rather than only for WDDM completion reporting, remains
unproved.  Other bounded checks are CR32 readback during the active mode and
the exact FB 42 object size.  Offset zero is valid for a true offset-zero/dumb
allocation; if FB 42 instead has the 3,153,920-byte canonical guarded size,
offset zero addresses its leading guard and is not a valid pixel view even
though it does not explain the stuck hardware latch.

Direct flips must remain disabled by default after this result.  The next live
gate is not another base write:

1. implement the narrow `0200` enable shadow and IRQ acknowledgement already
   specified above;
2. with the base unchanged, prove repeated bit-3 IGA1 edges and monotonically
   advancing DRM vblank sequence at the fixed mode cadence;
3. capture active-mode CR32, CR48, `0200`, `0204`, and `0214` before arming;
4. acknowledge stale bit 3, record the vblank sequence, then issue one flip
   while both mappings remain pinned; and
5. on each of a bounded number of genuine post-arm IRQs, record `0204` and
   `0214`; complete only when bit 1 clears and the address matches.

If bit 1 remains set after three genuine frame edges, retain both mappings,
withhold the event, block subsequent flips, and perform the already-proven
full restore.  Do not complete from elapsed time, `0200.bit1`, or the exact
`0214` readback.  A CR32 by-line experiment changes presentation semantics
and belongs only after the by-frame IRQ gate; it is not a justified workaround
for the failed first run.

Use two guarded 1024x768 XRGB8888 SF mappings with conspicuously different
solid colors and the existing 4096-byte pitch.  Keep CR48's channel at SF and
CR32 bit 0 clear.  Record, for every arm and IRQ:

```text
arm sequence, requested SF VA, 0200, 0204, 0214, handled sequence
```

Success requires repeated alternating flips where each event is delivered on
a post-arm bit-3 edge with `0204[1]=0`, masked `0214` equal to the requested
base, no guard corruption, and no stale frame after the old mapping is released.
Stage the first single transition separately and disable/restore immediately
after its exact completion record.  A missing IRQ or a stuck `0204[1]` is a
failed oracle, not justification for a polling-only userspace-visible flip.

## Bounded kernel implementation and live gate

The isolated implementation built on 2026-09-21 deliberately enables this
path only for the conjunction `fixed_iga1 && !staging_scanout &&
direct_flips=1`.  Other KMS configurations do not call `drm_vblank_init()` and
retain their previous no-vblank behavior.  The direct path requests the PCI
IRQ shared, installs the simple-pipe vblank callbacks, and allows exactly one
synchronous flip wait.  It uses these hard bounds:

```text
one armed flip
three handled post-arm IGA1 edges
100 ms absolute completion timeout
success = post-arm edge && !(0204 & 2) && masked 0214 == expected
```

The ISR does not touch GEM references, reservations, GART PTEs, engine locks,
VGA indexed registers, or DRM events.  The process-context pipe update already
holds the new display pin and the old active display pin while it waits.  On
success it sends the retained event and releases only the old display pin.  A
post-publication failure blocks all later direct flips, withholds the event,
and retains both possible display mappings until the existing full IGA1
restore.  A pre-publication error retains only the old active mapping.

Static acceptance before any hardware load is:

1. a clean `W=1` external-module build with `CONFIG_DRM_CHROME9=m`;
2. strict `checkpatch.pl` on the isolated diff;
3. `tools/chrome9_iga1_flip_model.py`, including unrelated shared IRQ,
   exact latch, three-edge failure, ownership-preserving disable, and the
   acknowledge/arm/DRM-callback race; and
4. review that the mapping-cache commit remains an unchanged parent and no
   existing render reservation, fence, guard, or SF-unbind path moved.

The first live gate must be staged, not inferred from a successful modeset:

1. Load only with the already-proven fixed direct-SF mode plus
   `chrome9.direct_flips=1`; preserve SSH recovery and do not start Xorg.
2. Hold `0214` and every SF mapping fixed while observing at least 120 handled
   IRQs.  Require monotonic DRM sequence, each interior cadence in 14--20 ms,
   no sub-millisecond storm, unchanged `0204[1]`, unchanged masked `0214`, and
   no unowned `0200` control change.
3. Use two guarded 1024x768 XRGB8888 GEM framebuffers at 4096-byte pitch, one
   solid red and one solid green.  Keep both GEM handles and guarded backing
   storage alive in the oracle.  Issue one red-to-green flip, verify its exact
   completion record, and immediately perform a full CRTC disable/restore.
   Only after that single-transition containment gate passes, re-enable and
   alternate exactly one page flip per event for at least 120 flips.
4. For every completion capture the private arm/edge serial, `0200`, `0204`,
   expected and observed masked `0214`, plus DRM event sequence.  Require the
   event on a genuine later bit-3 edge, `0204[1]==0`, exact address match, the
   expected visible color, intact leading/trailing guards, `040c==0`, and no
   command-reader or GART fault.
5. Stop after the first timeout, third-edge failure, cadence violation, stale
   interrupt, unexpected base, guard change, or lost display/SSH.  Verify that
   another flip is rejected, both mappings remain pinned, and a full CRTC
   disable restores pre-KMS VGA state before any mapping is released.
6. The normal success path unpins each old display mapping only after its
   exact event/latch proof.  During the 120-flip run verify no stale color or
   freed-page fetch; a failed post-publication transition must instead retain
   both display mappings until the full restore.

On teardown the driver logs handled/shared-none IRQ counts, minimum/maximum
cadence, completed flips, and failed flips.  A clean gate has `completed=120`
or more, `failed=0`, no storm-like minimum cadence, and an exact match between
userspace events and visible/address transitions.  No polling observation of
`0200[1]`, time passage, or `0214` readback can substitute for that result.
