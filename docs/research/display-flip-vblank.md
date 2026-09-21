# VX900 display flip and vblank details

This note keeps presentation out of the render ABI while recording the exact
display-side mechanisms needed after a render or 2D-copy fence.  The sources
are the VX900 Open Graphics Programming Manual, the proprietary Linux DRI
`via_chrome9_dri.so`, the supplied Chrome9 KMD source, and the old upstream VIA
DRI1 IRQ implementation.  No display packet described here is safe for direct
userspace submission.

## Command-regulator automatic flip

The manual defines T11 A10--A12 for IGA1, A18--A1A for IGA2, and A30--A32 for
the combined/current IGA selection.  In every triplet:

| word | bits | meaning |
|---|---:|---|
| base low | `23:3` | framebuffer byte address bits `23:3` |
| base low | `1:0` | `00=SL`, `01=SF`, `10=SM`, `11=LL` |
| base high/count | `7:0` | framebuffer byte address bits `31:24` |
| base high/count | `23:8` | frame-flip count |
| control | `3` | enable automatic swapping |
| control | `2` | skip waiting for blank when set |

`VT3353_Flip:0xeccc0` is selected explicitly for PCI IDs 1122, 5122, and
**7122** by `sfmFlip_inv:0xede76..0xedf09`.  It first calls
`stmAddCRWaitChipIdle_invi` and then emits this exact header-2 packet for IGA1:

```text
FE020420 0000041C 00110000 CC000000
10BBBBB8 110000HH 12000008 CC000099
```

Here `HHBBBBB8` denotes an eight-byte-aligned 32-bit framebuffer byte address:
the A10 word is `10000000 | (base & 00fffff8)`, and A11 is
`11000000 | (base >> 24)`.  A12 bit 3 enables the swap and bit 2 is clear, so
the CR waits for blank.  Flip count is zero.  The exact IGA2 and combined forms
change only subaddresses:

```text
IGA2:     18BBBBB8 190000HH 1A000008 CC000099
combined: 30BBBBB8 310000HH 32000008 CC000099
```

The preceding idle/order command is now exact too.  `VT3353_Flip` passes both
engine booleans as one to `stmAddCRWaitChipIdle_invi`; its 1122/5122/7122
branch at `0x175aac..0x175dff` emits:

```text
FE000000 00000001 00000000 CC000000
0000006C 00C00000 CCCCCCCC CCCCCCCC
```

The executable's helper name and the separate single-engine encodings prove
that `006c=00400000` waits/orders 2D, `006c=00800000` waits/orders 3D, and
`006c=00c00000` requests both.  It is therefore the required producer-idle
barrier before the automatic flip, not a substitute for observing a display
vblank after the new base is armed.

These shipping templates encode location `00`, SL.  They do not demonstrate
scanout directly from translated SF.  A new driver must not change the low
location bits merely because render and 2D access through SF works; display
fetch is a separate client and needs its own test.

Before emitting IGA1 or IGA2 forms, the DRI chooses frame-versus-line flip
strategy in indexed VGA state.  For IGA1 it clears or sets CR32 bit 0; for
IGA2 it clears or sets CR88 bit 5 (`0xecda0..0xecdd9`,
`0xececa..0xecf15`).  The manual names these respectively `Real-Time Flipping`
and `Flip Strategy`, with zero meaning by-frame and one by-line.  A tear-free
page flip must use by-frame.  The combined path updates both bits.

`VT3353_Flip` has an optional post-packet H0 `006c` write selected by its last
argument (`0xece4e..0xece99`).  Its exact values are `00000001` for IGA1,
`00000002` for IGA2, and `00001200` for the combined path.  These values are
pipe-specific and distinct from the `00400000/00800000/00c00000` engine-idle
mask.  Their bit semantics are not in the public manual, so they should be
treated as vendor flip-follow tokens and tested only in the kernel-only T11
oracle.  They are still not a returned fence word or a display completion
event.  The fact that the CR accepted either packet proves neither that the
requested blank occurred nor that the display latched the new base.

## Direct scanout-base registers

The Windows 7 KMD independently exposes the packed MMIO aliases used by its
`S3gSetVidPnSourceAddress` path.  The dispatcher at `0x69e10` selects the IGA1
helper at `0x79080` or IGA2 helper at `0x79710`.  In both helpers the value at
input offset `+08` is carried as a byte address (with optional source/surface
offsets added before the final programming):

* IGA1 shifts it right once at `0x79683..0x79688`, reads MMIO `0214`, preserves
  `[31:29]`, replaces `[28:0]`, and writes the result at
  `0x796a9..0x796de`.  This is independent confirmation of the two-byte unit.
  The shifted address is also saved in the KMD shadow at object offset
  `+11aa0`.
* IGA2 does **not** shift the byte address.  At `0x79d70..0x79da6` (and the
  non-deferred path `0x79db4..0x79de9`) it reads MMIO `0218`, preserves
  `[31:29]` and `[2:0]` with mask `e0000007`, replaces `[28:3]` with
  `address & 1ffffff8`, and writes it back.  The preservation of `[2:0]` is
  important: these are not address bits and a base update must not blindly
  zero or copy them.

Immediately before either packed-base update, the KMD writes `00110000` to
MMIO `041c` and `12000000` to `0420` (`0x79580..0x795bc` and
`0x79c0f..0x79c4a`).  Decoded through the public manual, that is T11 A12 with
payload zero: **disable IGA1 framebuffer automatic swapping**.  It is neither
an idle wait nor a completion fence.  The fact that the IGA2 helper performs
the same write suggests a blanket transition into direct-base mode, not an
IGA2-specific flip operation.  Thus a driver should not mix an armed T11
auto-swap and a direct `0214/0218` base write without first explicitly
disarming and ordering the former.

The same helpers select the direct-write strategy in VGA state: IGA1 changes
CR32 bit 0 at `0x795e4..0x79633`, and IGA2 changes CR88 bit 5 at
`0x79c72..0x79ccd`.  This corroborates the proprietary Linux DRI decode above.
One PCI-1122/mode-4 IGA1 branch at `0x79641..0x79681` ORs `c0000000` into the
byte address before shifting and writes `0214` without preserving its normal
upper bits.  That is a chip/mode-specific exception, not evidence that bits
31:30 generally belong in a linear base address; keep it out of the 7122
baseline.

For modesetting and atomic KMS the indexed display registers remain relevant:

* IGA1 uses standard CR0C/CR0D for the low start-address word, CR34 for the
  next byte, and CR48 `[4:0]` for the high bits.  CR48 `[7:6]` selects
  `00=SL`, `01=SF`; other values are reserved.  The proprietary Xorg
  `VIASetStartingAddress:0x4cd7a..0x4ce2b` divides the byte address by two
  before packing CR0D, CR0C, CR34, and CR48, proving the standard word unit.
* IGA2 uses CR62 `[7:1]` for byte-address bits `[9:3]` and bit 0 for
  linear/tiled mode, CR63 for `[17:10]`, CR64 for `[25:18]`, and CRA3 `[2:0]`
  for `[28:26]`.  CRA3 `[5:4]` selects `00=SL`, `01=SF`.  Consequently its
  linear start address is at least eight-byte aligned.
* IGA1 pitch is CR13 plus its documented overflow fields.  IGA2 pitch is CR66
  plus CR67 `[1:0]`; the field represents the display horizontal offset in
  the hardware's documented units.  Reconstruct and validate the complete
  mode-specific pitch before accepting a framebuffer.

IGA2 CR61 bit 5 is a read-only `start address change/Flip Flag`.  More usefully
for the first IGA1 path, the Part II manual defines MMIO `0204[1]` as Graphics
Starting Address Update Status: writing `0214` sets it, and hardware clears it
when the start address is updated.  Complete an IGA1 direct flip only on a
post-arm vblank for which that bit is clear and the masked `0214` base matches.
The full race and teardown rules are in
[iga1-vblank-flip-oracle.md](iga1-vblank-flip-oracle.md).

## Vsync interrupt and blank-status matrix

The supplied Chrome9 definitions in `via_chrome9_3d_reg.h:305..345` give the
VX900 routing:

| function | register/bit |
|---|---:|
| global graphics interrupt enable | MMIO `0200[31]` |
| IGA1 vsync interrupt enable | MMIO `0200[19]` |
| IGA1 vsync pending/status | MMIO `0200[3]` |
| IGA2 vsync interrupt enable | MMIO `0200[17]` |
| IGA2 vsync pending/status | MMIO `0200[15]` |
| IGA1 currently in vertical blank | MMIO `0200[1]` |
| IGA2 currently in vertical blank | MMIO `0204[2]` |
| IGA1 start-address update pending | MMIO `0204[1]` |
| CR render-fence interrupt status | MMIO `0204[5]` |

The enable/status pairs are not interchangeable with the blank-level bits.
Use the pending edge to call `drm_crtc_handle_vblank()`, and use the blank
level only for bounded diagnostics or a carefully implemented scanout-base
update.  Maintain one DRM sequence per active CRTC.

The manual also exposes an IGA2-local indexed control at CR69: bit 6 enables
its display interrupt, while bit 7 reads status and clears it when written as
one.  Whether this local gate is required in addition to MMIO 0200 bit 17 must
be characterized on VX900; preserve firmware state and begin with read-only
observation before changing it.

The old upstream `via_dri1.c:2214..2319` handles only pipe zero and uses the
older but matching MMIO-0200 IGA1 layout (`enable=1<<19`, `pending=1<<3`).  It
increments a software counter, calls `drm_handle_vblank`, and acknowledges by
writing sampled pending state back.  It is useful corroboration, not a modern
two-CRTC implementation.  The supplied Chrome9 KMD IRQ handler at
`via_chrome9_drm.c:1040..1099` does not handle either display interrupt at all;
it handles CR bit `0204[5]` and DP hotplug only.

All interrupt-control updates must be locked read-modify-writes which preserve
unrelated enables.  The old Chrome9 branch setup sometimes writes only
`80000000` to MMIO 0200, destroying IGA/hotplug/video enables.  Ack only
classified RW1C pending bits sampled by the handler; never echo arbitrary
status/control bits.  Install and test the handler before enabling any CR
interrupt-wait fence, since a lost CR interrupt can pause command fetch.

## Modern presentation order

The conservative copy-present sequence is:

1. render into the internal SF target and retire its render fence;
2. execute the guarded SF-to-SF 2D copy described in
   [sf-2d-copy-present.md](sf-2d-copy-present.md) and retire its 2D fence;
3. hand that fence to the atomic commit dependency and program/latch only a
   scanout-capable BO;
4. send the DRM page-flip event on the first vblank which observes the new
   CRTC state, not on render-fence retirement;
5. keep the old framebuffer pinned until that display event/latch point.

The T11 automatic-flip packet is a useful kernel-only oracle because it
explicitly waits blank, but it should not be placed in the render command
allowlist.  Initially, direct atomic CRTC programming plus normal DRM vblank
accounting is easier to validate and recover.  If direct base writes tear or
fail to latch, the T11 SL template provides the next controlled discriminator:
same scanout BO, same mode, one kernel-generated by-frame auto-swap packet,
bounded CR fence, then verify the base/IGA2 flip flag at the following vblank.
