# Chrome9 native cull and polygon-offset lowering

Status: exact static packet deltas and bounded live-oracle plan, 2026-09-21.
Neither feature should replace the current Gallium draw implementation until
its isolated oracle passes.  The register encodings are cross-checked between
the VX900 manual, proprietary Linux GL, both Windows UMDs, and Mesa's actual
post-viewport winding convention.

## Cull fields and independent orientation evidence

Native cull changes exactly two bits in two packet namespaces:

```text
type-01 A01 bit 7      cull enable
type-04 A23 bit 8      culled physical winding: 0=CW, 1=CCW
type-04 A23 bit 7      back-face color linkage; unrelated, preserve it
```

For the current Gouraud packet the complete words are:

```text
disabled       A01 &= ~00000080    A23=23004f02
cull CW        A01 |=  00000080    A23=23004e02
cull CCW       A01 |=  00000080    A23=23004f02
```

Both A23 values keep bit 7 clear; only bit 8 changes.  `23004f02` while A01
bit 7 is clear does not cull anything.

The Windows D3D builder at
`VTGUModeDX32A.dll:0x101fabd0..0x101face4` and the byte-equivalent OpenGL
builder at `VTGOGL32A.dll:0x1024e890..0x1024e9a4` independently prove this
transition.  Their final public modes are 1=none, 2=CW, 3=CCW: mode 1 clears
A01 bit 7, mode 2 sets A01 bit 7 and clears A23 bit 8, and mode 3 sets both.

More importantly, both contain an exact orientation swap before that final
lowering.  When context field `+0x6190` is zero, D3D indexes the table at
`0x103366fc` and OpenGL indexes the identical table at `0x103993ec`:

```text
index 1 -> 1       none unchanged
index 2 -> 3       CW becomes CCW
index 3 -> 2       CCW becomes CW
```

The private field's name is not recovered, but the table proves that the
driver resolves render orientation before programming the physical winding.
It is unsafe to permanently equate API back face with A23 bit 8.

Linux `STM_UpdateCull_INV:0x196a70` independently implements modes 1/2/3 in
the same A01/A23 fields.  Older UniChrome Mesa is policy evidence only, but
`viaChooseTriangle()` likewise resolves `CullFaceMode` and `FrontFace` before
choosing one hardware winding.

## Exact Gallium-to-physical mapping

Modern Mesa has already performed the GL orientation conversion by the time
Chrome9 sees `pipe_rasterizer_state`.  In `st_atom_rasterizer.c`:

```text
front_ccw = (GL front face == CCW)
front_ccw ^= (clip origin == upper left)
front_ccw ^= (framebuffer orientation == Y_0_BOTTOM)
```

For a Y-top window, `st_atom_viewport.c` instead negates viewport scale Y and
reflects translate Y.  These paired rules make Gallium `front_ccw` describe
the physical post-viewport vertices.  The draw cull stage confirms the exact
coordinate convention: after viewport transformation it computes

```text
det = (v0.x-v2.x)*(v1.y-v2.y) - (v0.y-v2.y)*(v1.x-v2.x)
physical_ccw = det < 0
```

The sign looks reversed from a mathematical Y-up plane because Gallium render
surfaces use Y=0 at the top.  Chrome9 must not apply another framebuffer or
viewport-Y flip.  The complete mapping from already-resolved Gallium state is:

```text
cull NONE:             clear A01 bit 7
cull FRONT: culled_ccw = rasterizer->front_ccw
cull BACK:  culled_ccw = !rasterizer->front_ccw
cull FRONT_AND_BACK:   suppress the draw before submission
A23 bit 8 = culled_ccw
```

This formula applies to physical triangles emitted after draw clipping.  If
native cull is enabled, Mesa must prevent draw's cull stage from discarding
the same faces while retaining clipping, primitive decomposition, two-sided
color, and any still-software polygon offset.  The safe implementation is a
per-draw copy of the rasterizer passed to draw with only `cull_face=NONE`,
while retaining the original state for the formula above.  Do not globally
erase `front_ccw`: draw still needs it for two-sided color, unfilled mode, and
offset-mode selection.

## Packet locations and complete A01 words

Cull adds no dword and changes no address, pitch, FVF, shader, vertex, or
retirement field.  Current structured D16 packet locations are:

| path | A01 | type-04 A23 |
|---|---:|---:|
| untextured command 04 | body 19 | body 53 |
| untextured blend command 08 | body 23 | body 57 |
| one-texture D16, no blend | body 44 | body 77 |
| one-texture D16 blend | body 48 | body 81 |
| two-texture D16, no blend | body 64 | body 98 |
| two-texture D16 blend command 13 | body 68 | body 102 |
| proposed command 17 D24S8 | body 23 | body 69 |

The command-17 A23 location is later because its closed D24S8/stencil block
adds 12 words to command 08.  Examples of the A01 enable delta are:

```text
color only                 01c01540 -> 01c015c0
D16 test/write             01c0154c -> 01c015cc
D16 test/read-only         01c01548 -> 01c015c8
D24S8 write+stencil        01c0155c -> 01c015dc
D24S8 read-only+stencil    01c01558 -> 01c015d8
```

Perspective paths preserve `01ce....`; cull adds the same `0x80`.  Stencil
physical CW/CCW face assignment must use the same post-viewport winding
decision as cull.  A cull implementation that swaps A23 bit 8 but not A23/A24
versus A25/A26 can silently apply the wrong two-sided stencil operations.

## Cull live oracles

First test the hardware names in raw post-viewport coordinates, without
depth, stencil, blending, scissor, or texture.  For a 32x32 target:

```text
CW by Gallium's Y-top convention:
    v0=(4,4), v1=(28,4), v2=(4,28), det=+576
CCW, obtained by swapping v1/v2:
    v0=(4,4), v1=(4,28), v2=(28,4), det=-576
```

Reset the target between rows and use a different source color per row:

| A01 bit7 | A23 bit8 | expected survivor |
|---:|---:|---|
| 0 | 0 then 1 | both windings |
| 1 | 0 | CCW only; CW is culled |
| 1 | 1 | CW only; CCW is culled |

Then run `CW -> disabled -> CCW -> disabled` in one initialized context.  The
disabled rows must draw both windings and prove closure of A01 bit 7; the two
cull rows prove A23 bit 8.  Keep type-04 A23 bit 7 unchanged and assert all
four target guards, retirement, work mask, and `040c` after every row.

Only after that raw test passes, run a GL-level orientation matrix with
`front_ccw={0,1}`, `cull_face={FRONT,BACK}`, and both a window Y-top target
and an FBO Y-bottom target.  Feed the mapping above exactly once.  The same GL
semantic face must disappear in both targets even though Mesa reaches it by
opposite viewport/front-state transformations.  This catches both a missing
flip and a double flip.

## Depth-bias register packing

The manual equation is:

```text
Zbias = max(dZ/dX, dZ/dY) * scale + offset
```

Whether the derivative magnitudes are implicit in the hardware's `max` is
not yet live-proven; the slope oracle below deliberately distinguishes signed
from absolute behavior.  Linux `STM_DepthBias_INV:0x1959d0`, Windows D3D
`0x101fbde0..0x101fc058`, and Windows OpenGL
`0x1024f790..0x1024fa08` all agree on the binary32 split:

```text
A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000) |
                     (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

Disable is exactly `16000000 17000000 18000000`.  Adjacent state is mandatory
for a state-complete successor:

```text
A19/A1A/A1B = 19800000 1a800000 1b803fbf
A1E/A1F/A20 = 1e000000 1f000000 20007fff
```

A19--A1B reconstruct the -1/+1 clamp constants and enable enhanced Z
precision.  A1E--A20 are the wide pixel-Z interval.  A14/A15 are clear data
and a mantissa mask, not bias; leave them untouched.  Texture-stage A20 is a
different header-qualified namespace and must also remain untouched.

The public slope `factor` is copied bit-for-bit into `scale`.  Public `units`
is multiplied before conversion to `offset`:

| depth allocation | proprietary multiplier | approximately | one unit |
|---|---:|---:|---:|
| D16, 16 bpp | `38000080` | `3.0518044e-5` | about 2 D16 LSB |
| D24S8/D24X8/D32, 32 bpp | `34000001` | `1.1920930e-7` | about 2 D24 LSB |

The D3D resource-bpp branch is `0x101fbe19..0x101fbe3a`; its double constants
are at `0x1005f760` and `0x1005f768`.  OpenGL repeats it at
`0x1024f7c9..0x1024f7ea`.  Linux tests resource field `+0x158==0x10`, whose
bpp meaning is independently proved by `sfmGetPatchColorKey`.

Useful exact constant-only words are:

```text
units=-1, D16:    offset=b8000080 -> 16000080 170000b8 18000000
units=+1, D16:    offset=38000080 -> 16000080 17000038 18000000
units=-1, D24S8:  offset=b4000001 -> 16000001 170000b4 18000000
units=+1, D24S8:  offset=34000001 -> 16000001 17000034 18000000
offset=-0.25:     offset=be800000 -> 16800000 170000be 18000000
```

The last row is a raw-packing discriminator, not an API `units` value.

Windows additionally gates bias by primitive fill mode: point, line, and fill
enables at context `+0x3f34/+0x3f38/+0x3f3c` are compared with current mode
1/2/3 at `0x101fbe3a..0x101fbe7f`.  Native triangle bias may therefore lower
only `offset_tri`; `offset_point` and `offset_line` remain in draw until their
expanded-primitive paths have separate proof.  Nonzero Gallium
`offset_clamp` has no recovered Chrome9 field and must also remain software.

## Bias packet growth and state ownership

The proposed command-17 D24S8 packet already closes A16--A1B and A1E--A20;
native bias changes only body 30--32 and does not grow its 204-dword body.

Existing D16 packets contain A10/A11/A12/A13/A20 but omit the full bias/clamp
block.  A state-complete native-bias version replaces those five words with
13, a net growth of eight.  For command 08 the exact map becomes:

```text
body[26..29] A10..A13
body[30..35] A16..A1B
body[36..38] A1E..A20
body[39]     A33 or canonical disabled A33
body[40]     next H2 header
body[65]     type-04 A23
body[176..179] immediate H2 header
body[180..197] vertices
body[198..199] pad
```

Thus command 08 grows 192 -> 200 dwords.  Applying the same closed eight-word
growth gives these three-vertex lengths:

| family | current | native-bias closed |
|---|---:|---:|
| one texture D16 REPLACE, no blend | 228 | 236 |
| one texture D16 MODULATE, no blend | 232 | 240 |
| one texture D16 REPLACE, blend | 232 | 240 |
| one texture D16 MODULATE, blend | 236 | 244 |
| two texture command 13 | 270 | 278 |

Bias never changes A01 depth enable/write, A12 format, A13 compare, A20 clip,
or the D16/D24S8 BO address and pitch.  Depth read-only still applies bias to
the incoming comparison value while suppressing only the stored Z update.
Stencil operations are selected from the biased depth comparison result.

Exactly one owner may apply offset.  For native filled-triangle bias, Mesa
must pass a rasterizer copy to draw with `offset_tri=0` while retaining
clipping/culling and the original factor/units for request lowering.  Leaving
both enabled doubles the factor and units; clearing the original state before
lowering silently loses them.

## Bias live oracles

Use LESS, a flat incoming Z of 0.5, stored depth exactly 0.5, read-only depth,
and distinct color.  Read-only isolates comparison bias from depth-storage
rounding.

1. **D16 constant sign and units.** Neutral bias fails equal LESS.  D16
   `units=-1` (`16000080 170000b8 18000000`) must pass and write color;
   `units=+1` must fail.  Stored depth remains byte-identical in all rows.
2. **D24S8 scale selection.** Repeat with D24S8, stencil disabled, and the
   `b4000001/34000001` pair.  Negative passes, positive fails, and all upper
   depth plus low stencil bytes remain unchanged.  Reusing the D16 multiplier
   produces a roughly 256-times larger displacement and is a validation
   failure even if the sign-only image happens to match; follow with a near
   threshold chosen between the two magnitudes.
3. **Large raw packing check.** With D16, exact `offset=-0.25` must make the
   coplanar second triangle pass.  This verifies A16/A17 byte assembly
   independently of the API units conversion.
4. **Slope magnitude.** Set units zero and factor +1.  Use one triangle with
   positive dZ/dX and zero dZ/dY, and an otherwise identical triangle with
   negative dZ/dX.  Absolute-slope behavior biases both equally; a signed
   `max` biases only the positive case.  General GL native offset is allowed
   only if both agree with draw's `max(abs(dZ/dX),abs(dZ/dY))` result.
5. **Read-only plus stencil Z-fail.** On D24S8, choose a bias that changes an
   unbiassed pass into a fail, set stencil zfail=REPLACE and zpass=KEEP.  The
   stencil byte alone must change.  This proves ordering without enabling a
   depth write.
6. **Closure and owner check.** Run negative -> neutral -> positive -> neutral
   in one context, then compare software-only and hardware-only images for
   units +/-1 and nonzero slope.  An image matching twice the requested
   offset proves that both owners remained active and must disable the native
   path.

Every row uses canonical guarded objects, poisoned guards, bounded retirement,
and post-retirement work-mask/`040c` checks.  A mismatch keeps the capability
disabled.  A timeout or guard change enters the existing quarantine path.

## Promotion boundary

Native cull can be exposed only after raw CW/CCW and GL window/FBO orientation
matrices pass.  Native bias can initially cover filled triangles with Z16 or
D24S8, zero clamp, and the recovered fixed formats.  Point, line, float depth,
nonzero clamp, and any unresolved negative-slope behavior stay in draw.

Neither feature needs to be coupled to command-17 stencil bring-up.  Keeping
software cull/offset through the first D24S8 tests is the safer dependency
order; these packet deltas become performance successors after their own live
proof.
