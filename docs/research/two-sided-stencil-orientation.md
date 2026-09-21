# Chrome9 two-sided stencil orientation

Status: exact static face-packing and orientation contract, 2026-09-21.
The first command-17 gate remains one-sided by requiring equal physical face
blocks.  Capability bit 38 is reserved for accepting independently lowered
CW and CCW state after the live winding matrix below passes.

## What the Windows builder actually does

Windows D3D `VTGUModeDX32A.dll:0x101fc060..0x101fc387` is more informative
than a simple field table.  On Chrome9 it:

1. requires bound private depth/stencil format `0x2033` at
   `0x101fc08b..0x101fc0b0`;
2. packs the ordinary stencil state into both A23/A24 and A25/A26 at
   `0x101fc0b6..0x101fc1e6`;
3. tests the two-sided enable at context `+0x1d4c` at `0x101fc1ec`;
4. when enabled, builds the second face from context
   `+0x1eb8..+0x1ed0`; and
5. on the Chrome9-class branch (`private +0x1c == 8`), tests orientation
   field `+0x1e3c == 2` at `0x101fc20f` and overwrites either A23/A24 or
   A25/A26 with that second face.

The second face goes to A23/A24 in the orientation-equal branch
`0x101fc218..0x101fc267`; otherwise it goes to A25/A26 through
`0x101fc269..0x101fc2e0`.  Thus the driver does not call A23 permanently
"front."  It first mirrors the ordinary state, then assigns the independent
face to one of two physical winding slots after orientation is known.

The independently relocated OpenGL builder at
`VTGOGL32A.dll:0x1024fa10` uses the same algorithm.  The nearby cull builder
proves the meaning of `+0x1e3c`: value 1 resolves front winding CW and value 2
resolves it CCW before its own render-orientation exchange.  This agrees with
the VX900 manual's physical face naming:

```text
A23/A24 = physical clockwise face
A25/A26 = physical counter-clockwise face
```

The Linux proprietary `STM_Stencil_INV:0x1955e0` independently packs the same
four words and exchanges face ownership only after its front-face decision.

## Gallium lowering

Gallium defines `stencil[0]` as API front and `stencil[1]` as API back;
`stencil[1].enabled` says that the two faces differ.  Dynamic references use
the same indices.  `pipe_rasterizer_state.front_ccw` has already incorporated
GL clip origin and framebuffer orientation, as proved in
[native-cull-bias-lowering.md](native-cull-bias-lowering.md).

The only correct physical mapping is:

```text
if stencil[1].enabled == 0:
    cw  = front state/reference
    ccw = front state/reference
else if rasterizer->front_ccw:
    cw  = back state/reference
    ccw = front state/reference
else:
    cw  = front state/reference
    ccw = back state/reference
```

This is equivalently:

```text
cw  = front_ccw ? back  : front
ccw = front_ccw ? front : back
```

Do not apply viewport scale or framebuffer orientation again.  Do not use the
cull mode to choose stencil faces: both face states remain programmed when
culling is off, and culling merely prevents the discarded face from reaching
the stencil test.  Native cull and two-sided stencil must nevertheless use
the same physical-winding decision or their interaction will be inverted.

Chrome9 Mesa currently discards `set_stencil_ref`.  It must retain the full
`pipe_stencil_ref`; reference 0 travels with `stencil[0]` and reference 1
with `stencil[1]` through the same mapping above.  Compare/write masks and
operations are static DSA state, while reference is dynamic draw state.

## Exact words and semantic translation

For either physical face:

```text
A23/A25 = subaddress<<24 | reference<<16 | compare_mask<<8 | write_mask
A24/A26 = subaddress<<24 | func<<16 | fail_hw<<6 | zfail_hw<<3 | zpass_hw
```

Compare functions are `NEVER..ALWAYS = 0..7`.  Gallium stencil operations
must pass through the exact hardware table:

```text
Gallium:  KEEP ZERO REPLACE INCRSAT DECRSAT INCR_WRAP DECR_WRAP INVERT
value:       0    1      2       3       4         5         6       7
hardware:    0    1      2       3       4         6         7       5
```

A discriminating packed pair is:

```text
front: ref=11 cmask=f0 wmask=0f func=ALWAYS
       fail=KEEP zfail=REPLACE zpass=INCR_WRAP
       packed state = 11f00f / 070016

back:  ref=22 cmask=0f wmask=f0 func=EQUAL
       fail=INVERT zfail=DECR_WRAP zpass=ZERO
       packed state = 220ff0 / 020179
```

When `front_ccw==0`, command 17 must contain:

```text
body[39] A23 = 2311f00f       # physical CW = front
body[40] A24 = 24070016
body[41] A25 = 25220ff0       # physical CCW = back
body[42] A26 = 26020179
```

When `front_ccw==1`, the two payload pairs exchange physical slots:

```text
body[39] A23 = 23220ff0       # physical CW = back
body[40] A24 = 24020179
body[41] A25 = 2511f00f       # physical CCW = front
body[42] A26 = 26070016
```

These examples also catch a raw-copy bug in the wrap/invert operation values.
The subaddress byte always follows the destination slot; exchanging whole
native dwords would leave `25/26` in the `23/24` positions.  Exchange semantic
payloads, then generate each destination word.

One-sided state emits the same resolved front payload in both slots,
independent of `front_ccw`.  Disabled stencil emits the canonical neutral
block:

```text
2300ffff 24070000 2500ffff 26070000
```

and clears A01 bit 4.

## Capability and validation boundary

Command 17 and base capability 36 already carry both physical face structures.
Its first implementation requires `cw==ccw`, which is sufficient for
one-sided stencil and makes a wrong orientation impossible.  Reserve:

```c
#define DRM_CHROME9_CAP_D24S8_TWO_SIDED_STENCIL (1ULL << 38)
```

Capability 38 consumes no ioctl ordinal and changes no request size.  It says
command 17 accepts unequal, independently validated CW and CCW structures.
Without bit 38, Mesa must reject/fallback any state with
`stencil[1].enabled`; callers that submit unequal blocks receive `-EINVAL`.

With bit 38, validate every face independently: funcs/ops `<=7`, masks/refs
`<=255`, all reserved fields zero.  Apply the Gallium-to-hardware operation
table separately to all six ops.  The kernel receives physical faces and
does no API-front interpretation.  This keeps viewport and GL conventions in
Mesa and packet validation in the kernel.

## Bounded live winding matrix

First establish physical slots without Gallium semantics.  Use command 17,
depth disabled, color mask zero, stencil ALWAYS/REPLACE-pass, full masks,
physical CW ref `33`, physical CCW ref `77`, and two non-overlapping raw
triangles:

```text
CW:  (4,4), (14,4), (4,14), det > 0
CCW: (18,4), (18,14), (28,4), det < 0
```

Starting from stencil zero, the CW region must become `33`, the CCW region
`77`, and the upper 24 depth bits plus target must remain unchanged.  Swap
the payloads without changing vertices and require the observed bytes to swap.

Then prove Gallium front/back mapping:

1. Set front ALWAYS/REPLACE ref `11`, back ALWAYS/REPLACE ref `22`, and
   `front_ccw=0`.  Raw CW must become `11`; raw CCW must become `22`.
2. Toggle only `front_ccw=1`.  CW must become `22`; CCW must become `11`.
3. Repeat through a Y-top window and Y-bottom FBO using the GL state tracker.
   The same API front/back semantics must survive both target orientations.
4. Enable native cull FRONT, then BACK.  Only the surviving face may update
   stencil, and its operation must match the same physical mapping used by
   cull.
5. Run one-sided -> two-sided -> one-sided and
   `front_ccw 0 -> 1 -> 0` without reinitializing 3D.  The mirrored rows must
   leave no stale independent face.

Use unequal refs, masks, and at least one INVERT/wrap pair after the initial
REPLACE test.  Verify all target/D24S8 guards, exact depth and stencil payload,
bounded retirement, work-mask clear, and `040c==0` for every row.  Keep
capability 38 disabled on any mismatch; quarantine on timeout, fault, or guard
mutation.
