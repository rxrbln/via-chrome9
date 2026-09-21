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

1. Install the shared handler before exposing vblank support.
2. Under the IRQ lock, sample only `0200 & f7ff0800` into the enable shadow.
3. Clear stale IGA1 pending by writing `shadow | 00000008`, then flush-read.
4. Set `80080000` in the shadow and write it with no status bits set.
5. Flush-read, enable DRM vblank accounting, and verify that bit-3 interrupts
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
   already-pending bit-3 edge, record the current vblank sequence, and publish
   the pending `{old_fb,new_fb,expected_base,event}` ownership before the MMIO
   write.  This prevents the ISR from missing ownership if an edge races the
   write.
4. Ensure T11 A12 is disarmed and CR32 bit 0 was cleared during the full,
   blanked IGA1 enable, write the packed `0214` value, and read it back to
   flush the posted write.  There must be exactly one direct-base writer; do
   not perform indexed-VGA I/O in the ordinary flip callback.

ISR:

1. Read `0200` once.  If bit 3 is clear, do not claim IGA1 vblank.
2. Write `irq_enable_shadow | 00000008` and flush-read, acknowledging early as
   the Windows ISR does.
3. Call `drm_crtc_handle_vblank()` exactly once.
4. If a flip was armed before this new sequence, sample `0204` and `0214`.
   Complete/send its DRM event only when the predicate above holds.
5. Drop the old framebuffer's display pin/SF mapping only after that event/latch
   point.  The new framebuffer remains display-owned.  Since unbinding may
   sleep, the hard IRQ only marks the latch and transfers a reference to
   deferred atomic cleanup; it does not tear down the SF mapping itself.

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

Use two guarded 1024x768 XRGB8888 SF mappings with conspicuously different
solid colors and the existing 4096-byte pitch.  Keep CR48's channel at SF and
CR32 bit 0 clear.  Record, for every arm and IRQ:

```text
arm sequence, requested SF VA, 0200, 0204, 0214, handled sequence
```

Success requires repeated alternating flips where each event is delivered on
a post-arm bit-3 edge with `0204[1]=0`, masked `0214` equal to the requested
base, no guard corruption, and no stale frame after the old mapping is released.
First run with both mappings retained for the whole test; only then enable
post-latch old-mapping unbind.  A missing IRQ or a stuck `0204[1]` is a failed
oracle, not justification for a polling-only userspace-visible flip.
