# Chrome9 native ioquake3 GL2 fogpass design

Status: source-exact renderer audit, fresh Windows/Linux compiler replay and
implementation-grade packet design, 2026-09-21.  The five-ALU program and the
combined zero-texture/floating-varying packet have not executed on VX900
silicon.  Keep capability 37 disabled until every staged oracle below passes.

## Exact renderer operation

The audited renderer is ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`.  Its fragment shader is exactly:

```glsl
gl_FragColor = u_Color;
gl_FragColor.a = sqrt(clamp(var_Scale, 0.0, 1.0));
```

The vertex shader calculates:

```text
s = dot(vec4(position, 1), u_FogDistance) * 8
t = dot(vec4(position, 1), u_FogDepth)
eyeOutside = float(u_FogEyeT < 0)
fogged = float(t >= eyeOutside)
t = (t + 1e-6) * fogged / (t + 1e-6 - u_FogEyeT * eyeOutside)
var_Scale = s * t * u_Color.a * u_Color.a
```

Mesa can continue to execute that vertex program in the draw module.  The
native fragment successor receives only post-TNL position/RHW, the resulting
finite float `var_Scale`, and a job-local copy of `u_Color`.

`RB_FogPass()` fixes the remaining state.  Both forms use ordinary nonseparate
source-over, source `SRC_ALPHA` and destination `ONE_MINUS_SRC_ALPHA`, with
depth writes disabled.  `FP_EQUAL` selects EQUAL; the other form retains the
ordinary LEQUAL depth function.  Alpha test, textures, stencil and color-mask
special cases are absent.

Do not lower the scale through packed D1 alpha.  Quantizing the interpolant to
UNORM8 before `sqrt` changes the shader.  The exact path requires the existing
float D3 -> VS oT0 -> PS t0 linkage even though no texture lookup executes.

## Reproducible compiler result

The probe input is this `ps_2_0` semantic equivalent:

```text
mov_sat r0.x, t0.x
rsq     r0.x, r0.x
rcp     r0.x, r0.x
mov     oC0.rgb, c0
mov     oC0.a, r0.x
```

Its token stream and SHA-256 are:

```text
ffff0200 03000001 00110000 30000000
03000007 00010000 00000000
03000006 00010000 00000000
03000001 00070800 20e40000
03000001 00080800 00000000 0000ffff

3163f82597c9ac9739817b8029fc259d504ffc2d8c7d6e1a23e67a39dc8dbde6
```

The Wine probe used `VTGUModeDX32A.dll` SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`.
The Linux replay used `via_chrome9_dri.so` SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`
and production `stmPPS_PostTranslationSetting_invi(..., 2)`.  Both produced
the same five semantic records; configuration two adds the physical input
dependency bits shown here:

```text
A90..A9a  90058001 91000000 92000005 93000000
           94000000 95000000 96000000 97000000
           98000000 99000000 9a000000

ALU[0]    01c00530 d01c0053 0012c016 000d1020  # MOV_SAT t0.x -> r0.x
ALU[1]    01c00530 801c0053 001000b6 00070020  # RSQ r0.x
ALU[2]    01c00530 801c0053 001000b6 00060020  # RCP r0.x
ALU[3]    01c00530 501c0053 00708005 000d0820  # MOV c0.rgb -> oC0.rgb
ALU[4]    01c00530 801c0053 008200b6 000d0820  # MOV r0.x -> oC0.a

TAU count 0
```

The final ALU blob has SHA-256
`2e7d3a2a6bc2414b0abd9cfa1c8fa088aa8ee860241bbe69cc540795bb2604af`.
The compiled metadata is independently coherent: `+2c=00058000`,
`+6c=1`, ALU count five, TAU count zero, all A2T/T2A maps zero.  A one-record
`MOV oC0,t0` probe proved the same input class; Linux configuration two
changed its varying source to the physical dependency form `00f2c005` and
set the low resource bit in A90.

The zero endpoint remains a mandatory silicon gate.  Mathematically
`RCP(RSQ(0))` is zero, but neither compiler replay proves Chrome9's special
value behavior.  Negative input is first saturated to zero, so both zero and
negative cases must be observed before this program is advertised.

## Narrow semantic UAPI reservation

Reserve private command `0x18` as `DRAW_FOG_TRIANGLE` and capability bit 37
as `DRAW_FOGPASS`.  This is not a raw shader-upload ABI.  Version one accepts
only the program above and only these two complete state tuples:

```text
source/destination blend       SRC_ALPHA / ONE_MINUS_SRC_ALPHA
RGB and alpha equation         ADD / ADD
color mask                     RGBA
depth                           guarded D16, read-only
depth function                 LEQUAL or EQUAL
alpha/stencil/textures          disabled / disabled / zero
```

EQUAL additionally requires capability 32.  Read-only requires capability
23.  Destination format is guarded XRGB8888 or BGRA8888; the existing direct
flip eligibility remains independently XRGB-only.  No texture handle,
sampler, mip state or sampled-format field belongs in this request.

The request transports one finite normalized c0 vec4 and three vertices.  A
vertex exposes float4 post-TNL position plus one finite fog scale.  The kernel
synthesizes canonical packed D1 `ffffffff`, packed D2 zero and D3
`(fog_scale, 0.0)`.  This prevents userspace from smuggling additional
varyings into the native object.  Require positive finite RHW, safe X/Y/Z,
nondegenerate triangle, canonical target/depth pitches, scissor bounds and all
reserved fields zero.

Target and D16 handles are distinct.  Resolve both in one `drm_exec`
transaction, fence both conservatively WRITE, publish both GART mappings in
deterministic handle order, prepare/check both pairs of guards and preserve
both references through retirement or quarantine.  Constants and vertices
are job-owned copies before ring publication.

## Exact state-complete packet

Let `D=0` for a diagnostic no-depth form and `D=4` for the production D16
form.  Version one advertises only D16, but retaining `D` in this map makes the
insertion boundary auditable.  The complete body is `216+D` dwords:

```text
body 0..19          destination, A38..A3f, source-over A34..A37,
                    D16 A33 at body 19
body 20..31+D       common state; D16 A10..A20 occupies body 31..35
body 32+D..43+D     zero-texture config
body 44+D..75+D     eight-dword D0/D1/D2/D3 raster/FVF state

body 76+D..83+D     c0 H2 store, selector 22030000, four float lanes
body 84+D..95+D     VS float constants H3
body 96+D..103+D    VS integer constants H2
body 104+D..111+D   clip/scissor H2
body 112+D..135+D   five-record position/D1/D2/D3->oT0 fixed VS H2
body 136+D..143+D   VS activation, A60=60800004

body 144+D..147+D   PS-control H2 header
body 148+D..158+D   A90..A9a above
body 159+D          cccccccc
body 160+D          dddddddd
body 161+D..163+D   alignment

body 164+D..167+D   ALU H2 header, selector 20030000
body 168+D..187+D   five finalized ALU records above
body 188+D..191+D   immediate-vertex H2 header
body 192+D..215+D   three eight-dword vertices
```

There is no TAU H2 packet.  The exact zero-texture control packet is:

```text
fe020440 0000043c fe020000 cccccccc
00000001 01002521 08010100 05000000
0d810000 cccccccc cccccccc cccccccc
```

The raster object deliberately retains the proven one-coordinate linkage:

```text
00000308 010000c2 020041d1 030052d0
04006342 1200000f 61800000 623081ff
20000000 23004f02 24000003 25f6cc00
28ffcc00 40800000 41400000 4200493f
43800000 44400000 4500493f 46800000
47400000 4800003f 49000000 50000000
51000000 52147fff
```

`A20=20000000` is independent triangle-list topology.  D3 is two float32
lanes and the fixed VS ends with the live one-texture instruction
`0008c14c 00000000 00000000 0008028f`, but config `08010100` keeps all
texture units disabled.  This exact cross-combination is compiler- and
state-derived, not yet silicon-proven; it is why the staged test begins with
a single triangle and bounded timeout.

For D16, use `A01=01ce1548`, disabled alpha-test `A33=33007000`, and:

```text
ordinary fog   A13=13030000       # LEQUAL
FP_EQUAL fog   A13=13020000       # EQUAL

A34=34000020 A35=35000148 A36=36000410 A37=37000244
A53=53010c0f A00=00000011
```

Those are the existing live-proven nonseparate source-over and read-only D16
encodings.  Do not replace them with separate-alpha blending on BGRA: the GL
state intentionally computes destination alpha as
`src.a*src.a + dst.a*(1-src.a)`.

## Mesa classifier and draw gate

The fragment classifier may normalize SSA copies, scalarization and vec
construction, but must prove exactly:

```text
one float varying scalar x
one direct four-lane constant-buffer-zero vec4 load
sat = clamp(varying, 0, 1)
alpha = sqrt(sat)
output.rgb = constant.rgb
output.a = alpha
```

Reject a second varying, texture operation, indirect/short constant load,
different clamp endpoints, use of c0.a in the fragment result, discard,
derivative, side effect or partial output.  Record the linked varying semantic
and aligned constant offset rather than assuming VAR0/c0.

At draw time validate the CPU-visible constant-buffer range and repack the
selected vec4 into request c0.  Require post-TNL one-component float output,
finite values and the exact D16/blend state above.  The software VS is allowed
to consume its much larger vertex-uniform set; this capability does not raise
native VS or fragment constant limits globally.

## Guarded silicon order

1. Submit an all-scale-one red triangle over green with LEQUAL/read-only D16.
   It must become exact opaque red, preserve depth and all four guards, retire,
   and permit a following known-good draw.
2. Submit scale zero, then scale `-0.25`.  Source alpha must be exact zero and
   the green destination must remain unchanged.  Any NaN-colored pixel,
   missing retirement or guard change permanently fails the object.
3. Submit constant scale `0.25`.  On XRGB, red over green should be near
   `00807f00`; permit only the narrow one-LSB neighborhood needed for native
   RSQ/RCP approximation.  This distinguishes the arithmetic from endpoint
   substitution.
4. Prefill D16 with `0x8000`: incoming `0x4000` and `0x8000` pass LEQUAL,
   `0xc000` fails.  Switch to EQUAL and prove only `0x8000` passes, then switch
   back to prove A13 closure.  Depth bytes remain unchanged throughout.
5. Repeat on BGRA initialized with alpha zero.  The scale-0.25 center alpha is
   near `0x40`, not `0x80`, proving the required nonseparate alpha equation.
6. Finish with negative tests for non-finite scale/c0, missing D16, writable
   depth, wrong blend, any texture, unknown flags/program/version, aliasing
   handles, short BOs and damaged guards.  Every rejection occurs before GEM
   mapping or ring publication.

Passing this sequence qualifies only the exact ioquake fogpass.  It does not
advertise general square root, arbitrary ALU programs, generic float varyings,
application native shaders or fixed-function fog.
