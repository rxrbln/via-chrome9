# Chrome9 HD game-critical raster, depth, stencil, and clear oracles

This note closes the fixed-function state needed immediately after the first
textured draws: depth compare/write, stencil compare/operations, face culling,
polygon offset, color masks, clears, and conservative point/line rendering.
All words below are complete HPara words, including the subaddress byte.

The encodings are **static evidence** unless a section explicitly says
**Measured**.  The initial live policy remains one isolated delta per guarded
submission, CPU initialization of every surface, poison guard pages, bounded
retirement, and quarantine on timeout.  In particular, do not combine the
first stencil, polygon-offset, or native line/point experiment with a new
format or cache transition.

## Evidence

The field definitions come from the *VX900 Series Chrome9 HD Open Graphics
Programming Manual*, preliminary revision 1.0, 2011-01-19:

- type-01 A01 enable bits, A13 depth function, A16--A1B depth bias/clamps,
  A23--A26 stencil state, A53 color mask, and A84/A85 line pattern;
- type-04 A20 primitive reuse, A23 shading/topology, A24 vertex count, and
  A2A/A2B point dimensions.

The independently compiled proprietary builders are:

- Linux `via_chrome9_dri.so`: `STM_Stencil_INV:0x1955e0`,
  `STM_DepthBias_INV:0x1959d0`, `STM_UpdateCull_INV:0x196a70`,
  `STM_ColorWriteMode_INV:0x194df0`, `STM_PointSize_INV:0x1935e0`,
  `STM_LineStippleEnable_INV:0x194530`, `STM_LinePattern_INV:0x194d80`,
  `stmPointListPrimitiveSetting_invi:0x1bbc20`,
  `stmLineStripPrimitiveSetting_invi:0x1bbcb0`, and
  `stmLineListPrimitiveSetting_invi:0x1bbf90`;
- Windows `VTGUModeDX32A.dll`: stencil `0x101fc060`, point-list
  `0x101f4eb0`, line-list `0x101f4f50`, and line-strip `0x101f5140`;
- Windows OpenGL `VTGOGL32A.dll`: byte-identical stencil builder
  `0x1024fa10` and corresponding topology builders at `0x10249930`,
  `0x102499d0`, and `0x10249bc0`.

The old UniChrome Mesa driver is only a policy cross-check here.  It advertises
line and point width 1.0 only in `via_context.c:565-575`; it is not evidence
that Chrome9 lacks the newer point-size registers.

## One complete state-closure rule

The first implementation should emit this whole logical block whenever any
member changes.  Relying on disabled fields retaining harmless values makes
later re-enable transitions difficult to audit.

| state | words which must be closed |
|---|---|
| color/depth/stencil enables | A01 |
| depth surface and compare | A10, A11, A12, A13 |
| depth bias and clamps | A16, A17, A18, A19, A1A, A1B |
| Z clip | A1E, A1F, A20 |
| stencil | A23, A24, A25, A26 |
| render target/write mask | A50, A51, A52, A53 |
| windows | A80, A81, A82, A83 |
| solid clear color | A86, A87 |
| topology | type-04 A20, A23, A24, A2A, A2B |

Safe neutral values are:

```text
A16 = 16000000                 # polygon offset = 0
A17 = 17000000                 # cross-byte offset/scale pieces = 0
A18 = 18000000                 # polygon scale = 0
A19 = 19800000                 # established adjacent clamp default
A1A = 1A800000
A1B = 1B803FBF
A1E = 1E000000                 # wide Z clip low words
A1F = 1F000000
A20 = 20007FFF                 # wide Z clip high bytes; not type-04 A20
A23 = 2300FFFF                 # neutral stencil ref/masks
A24 = 24070000                 # ALWAYS, KEEP/KEEP/KEEP
A25 = 2500FFFF
A26 = 26070000
A53 = 53010C0F                 # all RT0 channels writable
```

When stencil is disabled A23--A26 are inert, but writing the neutral block
prevents a later enable from inheriting a previous reference, mask, or face
operation.  When depth is disabled A10--A13 are likewise inert, but resource
lifetime is still tied to the command which last published their addresses.
Never bind D16 while A01 bit 4 is set; reject stencil without D24S8 rather than
silently aliasing depth bits.

## Depth compare and write

For the already established 32 by 32 linear SF D16 surface:

```text
A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11004001                 # 64-byte pitch, SF, reading-Z cache off
A12 = 12380000                 # D16 format 0, linear, fixed-Z clamps
```

A13 bits `[18:16]` are the API comparison enum minus one:

```text
13000000  NEVER
13010000  LESS
13020000  EQUAL
13030000  LEQUAL
13040000  GREATER
13050000  NOTEQUAL
13060000  GEQUAL
13070000  ALWAYS
```

A01 controls the operation independently:

```text
01C01540  depth test off, depth write off
01C01548  depth test on,  depth write off
01C0154C  depth test on,  depth write on
```

Keep `A1E/A1F/A20 = 1e000000/1f000000/20007fff`.  The apparently reset
`20000000` makes the pixel-Z clip interval `[0,0]`, so a nonzero-depth draw can
vanish before A13 is evaluated.

### Bounded compare oracle

CPU-fill D16 with `8000`; clear the color target to black before every case.
Draw three non-overlapping constant-Z triangles (`4000`, `8000`, `c000`) with
one compare function and writes disabled.  Read color and confirm exactly:

| function | passing incoming depths |
|---|---|
| NEVER | none |
| LESS | `4000` |
| EQUAL | `8000` |
| LEQUAL | `4000`, `8000` |
| GREATER | `c000` |
| NOTEQUAL | `4000`, `c000` |
| GEQUAL | `8000`, `c000` |
| ALWAYS | all |

EQUAL and NOTEQUAL are especially useful because they do not depend on which
operand the hardware names source or destination.  Then prove independent
write control: with initial depth `8000`, draw red at `4000` using LESS and
read-only state, followed by green at `8000` using EQUAL and read-only state.
The pixel must be green and depth must remain `8000`.

Only the D16 LESS/write case is **Measured** on the live VX900.  Treat the
remaining matrix as a guarded static oracle.

## Stencil compare, masks, operations, and face state

Stencil is valid only with D24S8:

```text
A10 = 10000000 | (zs_sf_va >> 8)
A11 = 11008001                 # 128-byte pitch for 32 pixels, SF
A12 = 123E0000                 # format 6, D24S8, linear
```

The complete face blocks are:

```text
A23[23:16] = reference
A23[15:8]  = compare mask
A23[7:0]   = write mask
A24[18:16] = function: NEVER..ALWAYS = 0..7
A24[8:6]   = stencil-fail operation
A24[5:3]   = depth-fail operation
A24[2:0]   = depth-and-stencil-pass operation
```

A25/A26 have the same layout for the second face.  Operation values are the
D3D stencil enum minus one:

| code | operation |
|---:|---|
| 0 | KEEP |
| 1 | ZERO |
| 2 | REPLACE |
| 3 | INCRSAT |
| 4 | DECRSAT |
| 5 | INVERT |
| 6 | INCR (wrap) |
| 7 | DECR (wrap) |

A01 bit 4 enables stencil.  With two-sided stencil disabled the proprietary
driver mirrors A23/A24 into A25/A26.  With it enabled, the driver can exchange
the two blocks after accounting for render-target orientation.  Therefore a
Gallium driver must map API front/back after the viewport/Y transform; it must
not permanently call A23/A24 "front" and A25/A26 "back".

### Four low-risk stencil oracles

1. **Compare mask.** CPU-fill two rectangles with stencil `a1` and `b5`, use
   reference `a5`, compare mask `f0`, write mask `ff`, EQUAL, and REPLACE on
   pass.  Only the `a1` rectangle passes.  Keep depth testing off.
2. **Write mask.** Start from stencil `3c`, set ref `a5`, compare `ALWAYS`,
   write mask `0f`, and pass `REPLACE`.  Expect `35`, preserving the upper
   24 depth bits exactly.
3. **Saturate versus wrap.** `INCRSAT(ff)->ff`, `INCR(ff)->00`,
   `DECRSAT(00)->00`, `DECR(00)->ff`, and `INVERT(55)->aa`.  Use ALWAYS and
   change only the pass field.
4. **Depth-fail routing.** Use stencil ALWAYS, stencil-fail KEEP, depth-fail
   REPLACE, pass KEEP, and a depth comparison guaranteed to fail.  Color and
   depth remain unchanged while stencil becomes the reference.

After culling orientation is independently known, draw opposite-winding
triangles with distinct face references and REPLACE-pass operations to prove
the A23/A24 versus A25/A26 mapping.

## Culling and front-face

The two controls are independent:

```text
A01 bit 7    enable back-face culling
A23 bit 8    culled winding: 0 = clockwise, 1 = counter-clockwise
A23 bit 7    back-face VS color selector; it is not cull enable
```

Starting from the proven solid triangle word:

```text
no cull:   A01=01C01540 A23=23000F02
cull CCW:  A01=01C015C0 A23=23000F02
cull CW:   A01=01C015C0 A23=23000E02
```

Draw one triangle, then the same coordinates with vertices 1 and 2 exchanged;
exactly one must survive each enabled setting.  First report hardware winding,
then derive GL front face after the active Y orientation.  Closure sequence:
cull one winding, clear only A01 bit 7, then enable and select the opposite
winding.  Keep A23 bit 7 unchanged throughout.

## Polygon offset

The manual equation is:

```text
Zbias = max(dZ/dX, dZ/dY) * scale + offset
```

The proprietary Linux builder splits two IEEE-754 words across A16--A18:

```text
A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000) | (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

For `offset=-0.25` (`be800000`) and `scale=0` the exact words are:

```text
16800000 170000BE 18000000
19800000 1A800000 1B803FBF
```

The bounded oracle clears D16 to far, draws a red triangle at Z=0.5, then a
coincident green triangle at Z=0.5 with LESS.  With zero A16--A18, red remains;
with the negative constant bias, green must replace it.  Verify depth changes
in the expected direction as well as color.

The public `factor` maps directly to `scale`.  The proprietary driver
multiplies API `units` by `0x38000080` for a 16-bit depth allocation and by
`0x34000001` for a 32-bit allocation.  This resource test is now closed:
`STM_DepthBias_INV:0x195a10` compares surface field `+0x158` with `0x10`, and
`sfmGetPatchColorKey:0xb6afc..0xb6b52` independently branches on the same
field values `8/16/32` before performing byte/word/dword loads.  The field is
therefore bits per pixel, not an opaque format class.  The constants decode
to approximately `3.0518044e-5` for D16 and `1.1920930e-7` for the 32-bpp
D24/D32 family.

Fill, line, and point offset enables are selected independently from context
fields at `+0x3c40/+0x3c44/+0x3c48`.  A structured ABI may now lower units
according to the bound depth format, but it must still carry those three
polygon-mode enables independently.  Always close the disabled state by
emitting zero in A16--A18.

## Color masks

A53 bits `[3:0]` independently enable B, G, R, A writes.  Preserve the other
established payload bits:

```text
none  53010C00
B     53010C01
G     53010C02
R     53010C04
A     53010C08
RGB   53010C07
RGBA  53010C0F
```

A01 bit 6 is the global color-write enable.  For normal drawing retain it and
use A53 for the API mask.  Clear A01 bit 6 only for depth/stencil-only draws.
A5B/A63/A6B are the corresponding masks for render targets 1--3.

With destination format 99, prefill `44332211` and draw source `80a0b0c0`.
Expected results are R-only `44a02211`, G-only `4433b011`, B-only
`443322c0`, A-only `80332211`, RGB `44a0b0c0`, and none `44332211`.  The
A-only case is **Measured** on the live VX900; retain the rest as bounded
per-channel regression vectors.

## Safe clear paths

The proprietary `sfmClear_inv:0xe9470` waits for idle and delegates to its
ordinary surface blitter.  No evidence supports a magic baseline clear
packet.  `STM_FastZClear_INV:0x195480` depends on private metadata and optional
auxiliary allocations, so fast/coarse Z remains disabled.

### Color

Reuse the measured 188-dword full-screen quad, color/scissor bounds 0..32,
solid `A23=23000f02`, and program the clear color as:

```text
A86[23:16] = R; A86[15:8] = G; A86[7:0] = B
A87[7:0]   = A
```

Honor A53 for API masked clears.  This path is preferable to assuming that
the old 2D engine can address an SF render target.

### D16 depth

Use the same quad with constant vertex Z and:

```text
A01 = 01C0150C                 # color off, depth test/write on
A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11004001
A12 = 12380000
A13 = 13070000                 # ALWAYS
```

Z=0 must produce `0000`; Z=1 must produce `ffff`.  Uncovered pixels and guard
pages retain poison.

### D24S8 stencil only

To replace only stencil with byte `SS` while preserving depth:

```text
A01 = 01C01510
A10 = 10000000 | (zs_sf_va >> 8)
A11 = 11008001
A12 = 123E0000
A23 = 23SSFFFF
A24 = 24070002                 # ALWAYS, KEEP, KEEP, REPLACE
A25 = 25SSFFFF
A26 = 26070002
```

CPU-initialize words to `depth24<<8 | old_stencil`; covered words must retain
all upper 24 bits and change only the low byte.  This recipe is static/unrun.
CPU clear remains the bootstrap fallback for every unproved format.

## Native lines and points

The manual and two independently compiled proprietary drivers agree on the
normal, non-indexed topology state.  These are exact static reconstructions,
not yet live results.

### Topology words

Type-04 A23 bits `[3:0]` select point `0`, line `1`, triangle `2`, rectangle
`4`, wireframe triangle `6`, or point triangle `7`.  A24 is the exact vertex
count.  For the established command-mode FVF:

| primitive | type-04 A20 | type-04 A23 | A24 count |
|---|---:|---:|---:|
| point list, flat A | `20000000` | `23001F00` | number of points |
| line list, Gouraud | `20000000` | `23004F01` | `2 * line_count` |
| line strip, Gouraud | `20206000` | `23004F01` | `line_count + 1` |

For point list, Windows `0x101f4ec5..0x101f4ee6` clears A20 reuse bits
`[21:8]`, clears the A23 topology/shading region with mask `ffff1fe0`, then
ORs `1e00`; Linux `0x1bbc32..0x1bbc53` performs the same byte operations.
For line list, Windows `0x101f509c..0x101f50a4` installs topology 1 plus all
edge flags while preserving the separately selected shading mode, and clears
A20 reuse.  For line strip, Windows `0x101f52cc..0x101f52de` installs
`00206000` under mask `ffe060ff`; the Linux builder installs the same normal
cycle.  Specialized antialias/stipple branches use different cycles and are
not part of this oracle.

The normal baseline already has A01 bits 23 and 22 set (`01c01540`): canonical
line direction and the diamond rule.  Keep A01 bit 5 edge antialiasing, bit 1
stipple, and bit 0 line pattern clear for the first test.  A one-pixel line is
the only initial native width; this matches the old Mesa conservative limit.

### Point size

Type-04 A2A is default width and A2B is default height/minimum, both signed
`s[8].15`, clamped by hardware to 1..2048.  The Linux builder independently
calls `INV_FloatToP_E8_N(value, 15)` and stores the low 24 bits.  Therefore a
constant 1x1 point is:

```text
A2A = 2A008000
A2B = 2B008000
```

First draw isolated points at integer and half-integer locations into a
poisoned 32x32 target; require exactly one pixel per point and no guard-page
change.  Then test 2x2 with `2a010000/2b010000`.  Do not enable A01 bit 21
point sprite until ordinary point coverage passes.

### Line pattern, deferred

If later required, A84 low 16 is the pattern, consumed LSB first.  A85 low 16
is the repeat factor; setting A85 bit 23 resets the counter and self-clears.
A01 bit 0 enables the line pattern.  A safe first pattern oracle is
`A84=8400aaaa`, `A85=85800001`, followed by one horizontal 16-pixel line;
the exact phase must be read back from color rather than assumed.  Disable by
closing A01 bit 0 and restore A84/A85 to neutral values.

Native lines/points should remain behind a debug capability until these
bounded tests retire cleanly.  Mesa can lower points and wide/stippled lines
to triangles in the meantime, using the already proven triangle path.

## Transition regression matrix

Each row is one submission sequence with explicit state re-emission between
draws:

1. depth LESS/write -> depth EQUAL/read-only -> depth disabled;
2. D24S8 stencil REPLACE -> stencil disabled depth-only -> stencil INVERT;
3. cull CW -> cull disabled -> cull CCW;
4. negative polygon offset -> zero offset -> negative offset;
5. A-only mask -> RGBA -> no channels -> RGB;
6. triangle list -> line list -> point list -> triangle list;
7. color clear -> D16 clear -> ordinary depth-tested color draw;
8. D24S8 stencil-only clear -> ordinary depth+stencil draw.

For every row verify the target, depth/stencil surface where applicable, all
guard pages, the retirement cookie, CR PAUSE, and engine work bits.  A timeout
is not a failed pixel comparison: quarantine every published BO/PTE/ring
mapping and do not attempt another 3D command before reboot or a separately
proved reset sequence.
