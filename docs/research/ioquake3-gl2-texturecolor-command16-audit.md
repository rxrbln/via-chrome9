# Chrome9 command 16 audit for ioquake3 GL2 texturecolor

Status: implementation-grade audit against the current state-complete
one-texture builder, UAPI reservations, Linux/Xorg physical shader object and
fresh Windows compiler differential, 2026-09-21. Command `0x16`, capability
33 and native nonzero c0 have not run on VX900 silicon.

## Exact semantic boundary

ioquake3 `texturecolor_fp.glsl` is exactly:

```glsl
gl_FragColor = texture2D(u_DiffuseMap, var_Tex1) * u_Color;
```

The first program id admits only this closed key:

```text
one ordinary non-shadow sampler2D at logical unit zero
one direct vec2 coordinate input
one direct full vec4 load from fragment constant buffer zero
one componentwise full-RGBA multiply
one full gl_FragColor store
no discard, derivative, projection, bias, LOD, offset or other ALU
```

Copy propagation, scalarization and vec construction may vary, but all four
output lanes must trace to the same sample and the same four uniform lanes.
Record the linked coordinate semantic and the uniform's aligned byte offset;
do not hard-code `VAR0` or assume the application uniform is physically c0.

At draw time Mesa validates the constant-buffer range and repacks the selected
application vec4 into request c0. The kernel accepts a semantic program id,
not native instructions.

## Exact native object

The configuration-two Linux/Xorg executable object is:

```text
A90..A9a  90018000 91000001 92010002 93000001
           94000000 95000000 96000000 97000000
           98000000 99000000 9a000000
tail       cccccccc dddddddd
TAU        301001b0
ALU        0140b6d0 30080053 00f20005 0000007f
           01c00000 301c0000 00f20105 000d083f
```

Relative to the live TEX0-times-primary MODULATE object, only the first
ALU's second-source word changes:

```text
primary v0  30040053
float c0    30080053
```

Fresh dynamic Wine replay independently produced virtual-allocation word
`00080053`, while the equivalent primary probe produced `00040053`; TAU was
identical. Their input/final-ALU hashes are recorded in
`ioquake3-gl2-first-native-sequence.md`. Upload only the Linux physical object
above.

## Version-one request and state policy

Use the fixed 800-byte
`drm_chrome9_draw_textured_program_triangle` already designed in
`ioquake3-gl2-generic-texturecolor.md`:

```text
private command                     0x16
capability                          33, DRAW_TEXTURED_PROGRAM
version                             1
program                             TEX0_C0
constants.used_mask                 00000001
request size                        800 bytes
x86 ioctl                           43206456
```

The request always carries the complete destination/blend/alpha/scissor
envelope, even when blend and alpha test are disabled. This deterministic
four-dword destination delta avoids separate packet shapes for color mask,
BGRA destination or alpha-only state.

`PROGRAM_DRAW_DEPTH` in top-level flag bit zero selects D16. Depth compare
bits `[10:8]` and read-only bit 11 are otherwise the existing definitions:

* without depth, all depth bits, handle and pitch are zero;
* with depth, require an explicit supported comparison and a nonzero distinct
  guarded D16 handle;
* LESS and LEQUAL require the baseline depth-function capability;
* EQUAL and ALWAYS are accepted only when their separate capability bits are
  advertised; and
* read-only requires capability 23 and emits A01 bit two clear.

Do not use implicit zero-means-LESS compatibility in this new versioned
request.

The nested blend record is validated even when blend is disabled. Disabled
blend requires factors ONE/ZERO and ADD equations, but retains an explicit
color mask. Alpha test may be enabled independently. Extended factors 4..10
require capability 29. Destination and sampler formats remain independent:

```text
texture A30 = 30980000  XRGB/BGRX, sampled alpha one
texture A30 = 30990000  BGRA/ARGB, sampled stored alpha
target  A52 = 5298....  XRGB/BGRX store
target  A52 = 5299....  BGRA/ARGB store
```

The shader output alpha is always `sample.a * c0.a`. A BGRA source never
implicitly bypasses c0 alpha, and an XRGB source makes it exactly c0 alpha.
Fixed A33 and blend consume that fragment result. XRGB destination storage
does not make source alpha irrelevant when blend or alpha test observes it.

## Exact state-complete body

Let `D=0` without depth and `D=4` with D16. The body is exactly `232+D`
dwords. Every index through the c0 packet is identical to the current
state-complete one-texture MODULATE builder; after it, only the first ALU
source word differs.

### Destination, image and common state

```text
body 0..3       destination H2 header
body 4          A50 target address
body 5          A51
body 6          A52 target format/pitch
body 7..14      A38..A3f
body 15..18     A34..A37 blend factors/functions/mask
body 19         D16: A33; no-depth: alignment/pad

body 20..23     texture-image H2 header
body 24         surface address >> 8
body 25         A18 = 18001101
body 26         A20 = 20000000 | height<<12 | width
body 27         A21 = 21000001
body 28         A22 size/log field
body 29         A30 = 3098/3099 according to source format
body 30..31     alignment

body 32..35     sampler H2 header
body 36         A30 = 30000000, explicit level zero
body 37         A31 filter word
body 38         A32 wrap word
body 39         cccccccc

body 40..43     common H2 header
body 44         A86 = 860000ff
body 45         A87 = 870000ff
body 46         A53 from normalized destination/color-mask state
body 47         A00 from normalized blend/alpha state
body 48         A01 = 01ce1540, 01ce154c, or 01ce1548
body 49         A88 = 88001ed0
body 50         A89 = 89000800
body 51         no-depth: A33 when enabled; D16: A10 depth address
body 52..55     D16 only: A11 pitch, A12=12380000,
                           A13 compare, A20=20007fff
```

No-depth alpha test lives at body 51. D16 needs body 51 for A10, so the
state-complete depth form places A33 at body 19. This is the recovered/live
command-12 collision solution; do not append a second A33 or overwrite A10.
With alpha disabled, preserve the current canonical closure values rather
than omitting state selectively.

Although TEX0_C0 does not read D1, retain the MODULATE raster/interpolation
closure:

```text
A01 no depth/write/read-only  01ce1540 / 01ce154c / 01ce1548
A02                          020041d1
vertex D1                    ffffffff
vertex D2                    00000000
```

Canonical D1/D2 prevents stale linkage from hiding a wrong c0 selector.

### Constants, programs and vertices

```text
body 52+D .. 63+D    config plus alignment
body 64+D .. 95+D    raster/FVF plus alignment

body 96+D            fe020440, c0 H2 header
body 97+D            0000043c
body 98+D            22030000
body 99+D            cccccccc
body 100+D..103+D    c0.x, c0.y, c0.z, c0.w bit-exact

body 104+D..115+D    VS float constants H3
body 116+D..123+D    VS integer constants H2
body 124+D..131+D    clip/scissor H2
body 132+D..155+D    five-record fixed VS H2
body 156+D..160+D    VS activation, A60=60800004
body 161+D..163+D    alignment

body 164+D..167+D    PS-control H2 header
body 168+D..180+D    A90..A9a plus cc/dd
body 181+D..183+D    alignment
body 184+D..187+D    TAU H2 header
body 188+D            301001b0
body 189+D..191+D    alignment
body 192+D..195+D    ALU H2 header
body 196+D..199+D    TEX0*c0 MUL
body 200+D..203+D    output MOV

body 204+D..207+D    immediate-vertex H2 header
body 208+D..231+D    three eight-dword vertices
```

The D16 body shifts config and every later group by four, so c0 begins at
104, first MUL at 200 and vertex payload at 212. Destination, texture,
sampler and the common prefix through A10 do not shift.

The c0-only H2 packet is intentional for version-one TEX0_C0. The request's
remaining 31 vectors are required to be zero and never emitted. This command
does not itself qualify capability 34: the separate full-store oracle must
prove the two 64-dword H3 packets at selectors `22030000` and `22030080`
before Mesa reports 512 bytes of fragment constants.

## Kernel validation and lifetime

All semantic failures occur before GEM lookup, GART binding or ring
publication. Require:

* version one, program TEX0_C0, exact flags and every reserved field zero;
* `used_mask == 1`, c0 lanes finite, all 124 unused constant dwords zero and
  both constant-record reserved qwords zero;
* destination and texture dimensions 1..2048, canonical 32-byte pitches,
  overflow-safe byte sizes and guarded native layouts;
* level-zero 2D XRGB/BGRA source, ordinary normalized sampling, legal
  CLAMP/REPEAT/MIRROR and NEAREST/LINEAR state;
* finite positive RHW, finite S/T, bounded X/Y/Z, nondegenerate triangle,
  canonical vertex color `ffffffff` and specular zero;
* valid scissor, destination format, normalized blend/alpha/color-mask state
  and capability-gated factors/functions;
* target and texture distinct; with depth, all three handles distinct; and
* no unsupported mip, projection, border, sRGB, stencil, multisample or layer
  state.

The source BO is reserved/fenced READ, target WRITE and optional depth WRITE.
Keep depth conservatively WRITE even in read-only mode until reservation and
cache behavior is separately relaxed. Resolve all objects in one `drm_exec`
transaction with deterministic handle ordering. Guard preparation and
verification cover every mapped role. Any error after publication follows
the existing wedge/quarantine policy and retains all BO references until
retirement or teardown.

Constants are copied into job-owned staging before publication. A persistent
`SET_CONSTANTS` ioctl is forbidden because another file/context could
interleave a program draw.

## Mesa draw-time contract

The fragment classifier records:

```text
program             TEX0_C0
sampler unit        0
coordinate semantic linked from the FS input
constant buffer     0
uniform byte offset direct and 16-byte aligned
```

At every draw:

1. require one sampler and one sampler view at unit zero;
2. validate `buffer_offset + uniform_offset + 16` with checked arithmetic
   against `buffer_size` and the backing resource or user-buffer extent;
3. copy four dwords with `memcpy`, without float conversion or UNORM packing;
4. reject exponent `0xff` lanes in the first-live implementation;
5. set request c0 and `used_mask=1`, zeroing the complete remainder; and
6. select command 16 only when the kernel advertises capability 33.

The software draw VS emits position and the recorded coordinate. Vbuf writes
canonical D1/D2 and D3 S/T. It validates only consumed S/T as finite; no
projector is present and unused R/Q defaults are irrelevant.

## Guarded silicon sequence

Every positive case uses distinct guarded BOs, outside prefill, exact interior
readback, all guard checks, cookie retirement, MMIO status and a following
known-good legacy draw.

1. **Selector discriminator.** BGRA texel `80402010`, c0 `(1,0,1,1)`.
   Expected BGRA target pixel is `80400010`. A stale D1-white MODULATE path
   instead leaves green `0x20`, immediately distinguishing c0 selection.
2. **Fractional constant.** Same texel, c0 `(1,0.5,0.25,0.5)`. Expected pixel
   is `40401004`, subject only to the final established UNORM quantization.
3. **Constant closure.** Alternate magenta/green lane masks through c0, then
   run `TEX0_PRIMARY -> TEX0_C0 -> TEX0_PRIMARY -> TEX0_C0`. Verify every
   transition and that legacy D1 never inherits c0.
4. **Source format alpha.** With c0 alpha one-half, compare XRGB source
   (fragment alpha `0x80`) against BGRA source alpha `0x80` (fragment alpha
   approximately `0x40`) on a BGRA target.
5. **Source-over.** XRGB red source times c0 alpha one-half over transparent
   green. Canonical source-over must produce `80807f00`, proving c0 alpha
   reaches blend rather than stale D1 alpha.
6. **Alpha test.** Repeat alpha disabled/GREATER-zero/disabled with c0 alpha
   zero then nonzero. Rejected fragments preserve both destination and D16.
7. **Depth.** Clear D16 to `0x8000`; LEQUAL at equality passes, LESS at
   equality fails, and nearer `0x4000` passes. Repeat write and read-only,
   then capability-gated EQUAL/ALWAYS only after their independent live
   oracles.
8. **Scissor/color mask.** Four distinguishable c0 lanes prove each channel
   mask and the inside/outside scissor boundary without relying on alpha.

Negative requests cover wrong masks, nonzero unused constants/reserved words,
NaN/Inf, short/misaligned/overflowed constant buffers, wrong sampler unit,
extra NIR operation, missing coordinate, aliased BOs, malformed pitches or
guards, depth fields without the depth flag, unsupported comparison/factor,
and alpha state at the wrong packet location. Snapshot the generated body:
only selector `22030000` may carry c0, and the first ALU must be exactly the
physical `30080053` row.

## Confidence boundary

The GLSL expression and uniform roles are **source exact**. Native PS words,
c0 selector, state blocks and packet indices are **compiler/code exact**.
Current command-12 D16/blend/alpha collision handling and the underlying
one-texture state have separate live evidence. Command 16, arbitrary nonzero
float c0, its combined state transitions and the full H3 constant banks are
**provisional until guarded VX900 readback**.
