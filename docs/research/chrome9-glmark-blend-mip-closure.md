<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 glmark multi-texture blend and rectangular BGRA mip closure

This note closes the static design boundary for the two state combinations
which presently block jellyfish and terrain, and for the rectangular BGRA mip
resource used by refract.  It is a generic packet/resource contract, not a
scene classifier.  Pixel-shader compilation and arbitrary sampler count remain
separate work.

## Vendor evidence: blend is independent of the texture program

The three proprietary state managers all build blend state in a global RT0
state image, independently of texture-stage count and PS instructions:

| implementation | builder | relevant result |
|---|---:|---|
| Linux 5.75 | `STM_AlphaBlend_INV:0x196e30` | rebuilds A34--A37 and dirties their four register slots; does not read the active texture count |
| Windows D3D | `VTGUModeDX32A:0x101fd9e0` | rebuilds the same RGB/alpha equations in the global state image |
| Windows OpenGL | `VTGOGL32A:0x10251220` | byte-matched D3D builder, relocated by `0x533840` |
| Linux 5.75 | `stmFullUpdateHardware_invi:0x177490` | copies destination/blend state first, then independently emits image, mip-table, sampler, and PS blocks |

The paired Windows builders have the same control flow and table lookups.  In
particular, they read blend enable/equation/factor state and destination
properties, not the shader's TEX/ALU count.  Linux `STM_AlphaBlend_INV` marks
A34 through A37 dirty as one range and separately dirties the type-01 A00
gate.  This is strong executable evidence that "two textures plus blend" and
"sample, modify, then blend" need no alternate blend encoding.  The fragment
program produces oC0; the ordinary RT0 blend block consumes it.

The complete per-draw closure remains mandatory:

```text
A38..A3F = 38000000 39000000 3a000000 3b000000
           3c000000 3d000000 3e000000 3f000000
A34..A37 = equation/factor words
A53       = 53010c00 | remapped RGBA mask
type01 A00 bit 4 = blend enable
type01 A00 bit 20 = alpha-test enable only
A33       = comparison or canonical disabled 33007000
```

Do not couple blend enable to FE texture A00.  The latter is a different
register class: `00000011` means one active texture plus cache-clear pulse and
`00000021` means two active textures plus the pulse.

## Exact no-depth two-texture blend body

The current kernel's `chrome9_build_two_textured_triangle()` already contains
the needed construction.  Its blend branch is artificially selected only by
the D16+blend request kind; no emitted depth word is intrinsically required.
Selecting `blend=true, depth_active=false` gives a **266-dword** body: the
proven 262-dword two-texture body plus four dwords for closing A38--A3F.  No
dummy Z resource or depth-enable bit is needed.

The keyed layout is:

| body range/index | state |
|---:|---|
| `0..19` | destination H2; A50/A51/A52, all A38--A3F, A34--A37, A33 |
| `20..31` | texture image 0 H2 and payload |
| `32..39` | sampler 0 H2 and payload |
| `40..51` | texture image 1 H2 and payload |
| `52..59` | sampler 1 H2 and payload |
| `60..71` | common H2; A86/A87/A53/A00/A01/A88/A89 plus pad |
| `72..83` | two-texture FE configuration plus alignment |
| `84..115` | raster FVF/linkage plus alignment |
| `116..123` | PS constants |
| `124..135` | VS float constants |
| `136..143` | VS integer constants |
| `144..151` | full target/scissor A80--A83 |
| `152..179` | six-instruction VS |
| `180..187` | VS activation plus alignment |
| `188..207` | two-texture PS controls plus alignment |
| `208..215` | two TAUs |
| `216..231` | two ALUs, terminators, and alignment |
| `232..265` | immediate H2 plus three ten-dword vertices |

The state keys which must be asserted before publication are:

```text
body[4]       A50 target
body[6]       A52 destination format/pitch
body[7..14]   A38..A3F, all zero payloads
body[15..18]  A34..A37
body[19]      A33 or canonical disabled word
body[24/44]   texture 0/1 SF addresses
body[29/49]   texture 0/1 A30 formats
body[36/56]   sampler 0/1 A30
body[66]      A53
body[67]      type-01 A00
body[68]      A01=01c01540, depth disabled
body[76]      FE A00=00000021
body[78]      FE A08=08010102
body[148..151] A80..A83
body[232]     final immediate H2
```

For jellyfish's `SRC_ALPHA/INV_SRC_ALPHA` on both lanes, full mask, no alpha
test, the exact RT subset is:

```text
A34..A37 = 34000020 35000148 36000410 37000244
A53       = 53010c0f
type01 A00= 00000011
A01       = 01c01540
A33       = 33007000
FE A00/A08= 00000021 / 08010102
```

The canonical existing allocation for this is private command `0x15` and
capability bit 31, `DRAW_TWO_TEXTURED_TRIANGLE_RUN`, specified in
`textured-triangle-run-list-design.md`.  Its state envelope deliberately has
independent DEPTH and BLEND flags and therefore covers no-depth two-texture
blend without an ioctl cross-product.  Implement that versioned request and
route each checked segment through this same builder.  If a smaller immediate
successor is chosen first, it needs a new command/capability allocation; do
not fake a depth attachment and do not reinterpret the old no-blend request's
reserved fields.

### Bounded first oracle

Use two independent guarded BGRA sources and a guarded BGRA target.  Texture 0
is uniform `80ff8040`; texture 1 is uniform `80ffff80`.  The established two-
texture program produces approximately `40ff8020`.  Over transparent black,
jellyfish's same-factor source-over stores approximately `10402008`: alpha is
`As*As`, unlike canonical independent-alpha source-over.  Allow one LSB per
multiplied/blended channel.
Run:

1. two-texture COPY;
2. the 266-dword two-texture source-over body;
3. one-texture REPLACE/COPY;
4. the source-over body again.

Require exact retirement, zero engine errors, unchanged source/guards, and a
valid following legacy draw.  This proves blend enable/disable closure as well
as stage count.  It is the generic color-only blend gate for jellyfish.

## Sampled/modified oC0 plus blend

Terrain overlay computes `oC0 = uniform_opacity * texture0`, then requests
`SRC_ALPHA/ONE` ADD.  The RT state is:

```text
A34..A37 = 34000020 35000154 36000410 37000264
A53       = 53010c0f
type01 A00= 00000011
A33       = 33007000
```

The program state is orthogonal.  It must contain one TEX and a multiply by a
fragment constant, publish the complete A90--A9A/TAU/ALU object, and write
oC0 before the ordinary blend block.  Do not lower opacity into vertex color
unless the compiler independently proves that transformation; it is uniform
fragment arithmetic in the source program.

The existing fixed one-texture MODULATE object already demonstrates the
packet composition:

```text
A90..A93 = 90018000 91000001 92010001 93000001
TAU      = 301001b0
ALU      = 0140b6d0 30040053 00f20005 00000820
A01      = 01ce1540
```

Its blend combination is currently rejected in both Mesa and the kernel, but
the rejection must not be misreported as evidence for alternate blend words.
The production object completed 11,158 immediate draws before one shutdown-
only failure.  At that failure all non-CR work was idle, the fetch cursor was
past a command boundary, and only the legacy DST_POS completion cookie was
missing.  The packet, source-over state, and normalized color had separately
passed.  This points first at completion/tail visibility, not oC0-to-blend
semantics.

The safe promotion sequence is therefore:

1. Add the recovered FE330004 coherent completion write and require its exact
   per-job ID in addition to cookie, owned PAUSE, and non-CR idle.
2. Run one immediate triangle with one TEX + one ALU and source-over; then
   COPY; then repeat.  Keep batching disabled for this first gate.
3. Replace the fixed multiply with the compiler's TEX + constant MUL object
   and run `SRC_ALPHA/ONE`; verify both covered color and destination alpha.
4. Sweep 2, 4, 8, 12, and 16 triangles only after immediate closure passes.
5. Remove the broad Mesa/kernel rejection only for the complete program/state
   class which has passed.  Keep invalid/incomplete native programs rejected;
   do not whitelist a benchmark shader identity.

If FE33 succeeds while the old cookie is absent, retire only under the
documented FE33+PAUSE+idle contract after its independent oracle is complete.
Do not perturb the known blend or PS words to chase a legacy cookie failure.

## Rectangular BGRA mip chain required by refract

Refract allocates a `2*canvas_width` by `2*canvas_height` `GL_RGBA` texture,
clamped to the advertised maximum while preserving aspect ratio.  It renders
to L0, calls `glGenerateMipmap`, and samples with linear min/mag plus linear
mip interpolation and clamp-to-edge.  Typical bases such as 1280x960,
1600x1200, and 2048x1536 are rectangular NPOT and require levels through L10
or L11.

The exact generic state is:

```text
w_l      = max(width0  >> l, 1)
h_l      = max(height0 >> l, 1)
pitch_l  = ALIGN(w_l * 4, 32)
size_l   = pitch_l * h_l
offset_l = ALIGN(end_of_previous_level, 256)

image A18 = 18001101
image A20 = 20000000 | (height0 << 12) | width0
image A21 = 21000001
image A22 = 22000000 | (floor(log2(height0)) << 4)
                       | floor(log2(width0))
image A30 = 30990000                 # BGRA
sampler A30 = 30000000 | (last_level << 6)
sampler A31 = 31102492               # linear min/mag, linear D
sampler A32 = 32000049               # clamp-to-edge S/T
```

Set A22 bit 15 only when both dimensions are powers of two.  The Windows D3D
and OpenGL image builders at `0x101d7c10` and `0x10240740`, plus Linux
`stmHWPrepareTextureRegs_invi:0x1b22f0`, use exact logical dimensions and
floor-log exponents for NPOT.  Their sampler builders do not branch on the
POT bit before selecting trilinear or clamp.

Emit the fixed eight-entry H3 table for L1--L8:

```text
FE03043C 00000008 10030000 CC000000
entry(L1) ... entry(L8)
entry(L) = 40000000 | ((offset_l - offset_0) >> 8)
```

All entries must be initialized; zero only unreachable entries.  A30 may
select L9--L11, which have no extra H3 slots.  In the vendor canonical 32-bit
layout every level at or below 8x8 occupies one aligned 256-byte block, so the
shipping allocator makes the implicit tail consecutive after L8.  The current
Mesa guarded layout has the same property even though it preserves an
internal guard between L0 and L1.  Validate:

```text
offset(Lm) = offset(L8) + (m - 8) * 0x100,  m = 9..last_level
```

This tail equation is executed vendor allocation policy plus a hardware
inference.  Keep rectangular BGRA mip exposure behind bounded silicon tests
until L9--L11 fetches pass.  The exact current-layout vectors are in
`chrome9-glmark-bgra-mip-corpus.tsv`.

### Kernel validation rule

Relax the present square-POT-BGRX-512 predicate only as one atomic contract:

* BGRX or BGRA, 2-D, linear, one layer/sample, dimensions 1..2048;
* canonical 32-byte pitch and checked level sizes for every reachable level;
* current guarded L0 offset and exact actual BO size, including internal and
  final guards;
* each L1--L8 relative offset 256-byte aligned and fitting 24 bits;
* complete chain `last_level <= 11`, with exact minified dimensions;
* for L9--L11, the consecutive 256-byte tail rule above;
* H3 entries derived in kernel, never raw userspace addresses;
* texture READ ownership through retirement and a cache-clear pulse in the
  consuming job.

The current kernel also aliases `request.depth_object_size` to the texture L0
size in this path.  Rename/split that internal field before adding D24 sampled
resources or multiple mipmapped samplers; resource role must determine
ownership, not the historical slot name.

### Live promotion sequence

1. BGRA 30x18, force each L0--L4 with nearest-D, then trilinear L0/L1.
2. BGRA 1280x960, force L7/L8/L9/L10 with distinct colors.
3. Repeat 1600x1200 to separate unequal small-level shapes.
4. BGRA 2048x1536, force L8/L9/L10/L11.
5. Render a nonuniform pattern to L0, generate the chain through Mesa's CPU
   mipmap path, then sample a trilinear footprint from at least three LOD
   ranges.  This checks format-99 channel/alpha preservation, not merely solid
   address colors.
6. Run mip -> L0-only -> mip and render-to-texture -> sample -> render again
   without context reset.  Verify guards, all source levels, completion, and
   the first following packet.

Only after those steps should refract's rectangular BGRA chain leave the
fail-closed path.  Rounding to POT, forcing BGRX, dropping mip levels, or
clamping the sampler to L8 changes visible GL semantics.
