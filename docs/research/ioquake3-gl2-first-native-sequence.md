# ioquake3 GL2 first native sequence on Chrome9

Status: source-exact startup and reduced-renderer analysis, fresh dynamic
Windows compiler cross-check, and implementation-ready bounded sequence,
2026-09-21. The proposed programmable draw and full fragment-constant upload
have not run on VX900 silicon. Existing one-/two-texture, D16, blend and
alpha-test paths cited below are the live foundation.

## Result

The next ioquake3 OpenGL2 step does **not** require a general native vertex
shader ABI. Mesa's draw module already executes the application vertex shader
and exposes post-TNL outputs. The bounded driver should repack those outputs
into the existing fixed Chrome9 VS inputs, then select a kernel-owned fragment
program.

The smallest truthful sequence is:

1. qualify the complete 32-vector fragment-constant store and advertise 128
   fragment uniform components;
2. admit the exact generic `texture0 * primary` fragment shader, including
   CPU specialization of `u_AlphaTest` values zero and one;
3. admit the reduced lightall world shader as
   `texture0 * texture1 * primary`, splitting its one packed coordinate vec4
   into hardware D3 and D4 after software VS execution; and
4. admit texturecolor as `texture0 * c0` through the job-local program draw.

Steps two and three reuse the current fixed native VS objects. Step two can
reuse existing structured one-texture commands. Step three uses the reserved
two-texture-primary flag on existing two-texture commands. Only step four
needs private command `0x16` and a job-local float constant payload.

This sequence is intentionally a reduced GL2 renderer path. Run it initially
with normal, specular, deluxe, parallax, cube, sunlight and shadow dlight
features disabled. Default normal/specular lightall is a later multi-sampler
ALU program and must remain rejected.

## What the retained link error does and does not prove

The Chrome9 run retained at
`/root/tmp/chrome9/ioquake3-gl1-probe-v1.log:66..82` reports only:

```text
------- GLSL_InitGPUShaders -------
error: Too many fragment shader default uniform block components
error: Too many fragment shader uniform components
ERROR: shaders failed to link
```

It does not print the program name or the renderer cvars in force. Therefore
that historical error cannot honestly be labelled generic, lightall or
pshadow from the log alone.

The source order at ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1` is exact:

```text
generic variants       tr_glsl.c:936..986
texturecolor            tr_glsl.c:989..1002
fogpass variants        tr_glsl.c:1004..1038
dlight variants         tr_glsl.c:1041..1063
lightall variants       tr_glsl.c:1066..1222
shadowfill variants     tr_glsl.c:1224..1257
pshadow                 tr_glsl.c:1259..
```

With Chrome9's current four-component fragment limit, every generic and
dlight fragment has one active non-sampler component, texturecolor and
fogpass have four, and reduced lightall has one. The first oversized program
depends on cvars:

| renderer configuration | first source-ordered oversized candidate | active non-sampler components |
|---|---|---:|
| normal or specular mapping enabled | lightall index 1, `USE_LIGHTMAP` plus normal/specular path | 13: enable-textures vec4, normal-scale vec4, specular-scale vec4, alpha-test int |
| normal/specular off, sunlight mode nonzero | first lightall shadowmap variant | at least 7: two light vec3 values plus alpha-test int |
| normal/specular/sunlight/dlight-shadow off | pshadow | 14: three vec3 axes, vec4 origin, float radius |
| `r_dlightMode >= 2` | lightall index 0 due global `USE_SHADOWMAP` | at least 7 |

Shadowfill is not the five-component counterexample it initially appears to
be. `u_LightOrigin` and `u_LightRadius` are dead unless `USE_DEPTH` is defined,
and `GLSL_InitGPUShaders()` does not define it for shadowfill. The linked
fragment is the constant black/alpha-one branch.

The target's current archived configuration has all seven reduced-renderer
cvars at zero. That makes pshadow the source-derived first oversized candidate
for a new run with that configuration, but the config file was written after
the retained historical log, so it does not retroactively identify that
log's failing program.

Regardless of which program fails first, raising the cap only to c0 or c0/c1
is insufficient. Startup later constructs programs requiring up to 71 active
fragment components. The recovered hardware store provides 128, so the
truthful link gate is still capability 34 and the complete two-bank upload
described in `ioquake3-gl2-fragment-constants.md`.

## Current Mesa fail-closed points

The current Chrome9 state makes the implementation order observable:

* `chrome9_init_shader_caps()` reports only 16 bytes for fragment constant
  buffer zero, causing the retained link failure before any draw;
* the fragment classifier has closed GL1 texture/primary keys but does not
  recognize the complete GLSL integer alpha-test tree or retain its uniform
  byte offset;
* `chrome9_vbuf_get_vertex_info()` normally finds unit-zero and unit-one
  coordinates as two separate GENERIC/TEXCOORD outputs; fast lightall has one
  packed vec4, so treating the next generic output as unit one can select
  `var_Color` as coordinates; and
* texturecolor has no bit-exact fragment-float-constant submission path.

The packed case therefore needs an explicit layout enum. It must never fall
through the ordinary `generic[0]`/`generic[1]` lookup. Shader classification
records the packed coordinate semantic and the separate primary semantic;
vbuf verifies both outputs exist and splits only the recorded coordinate.
This prevents a visually plausible but wrong draw whose second texture
samples from interpolated color.

## The three first executable fragment keys

### Generic

`generic_fp.glsl:12..32` is exactly:

```text
sample = texture2D(unit0, diffuse_st)
alpha  = sample.a * primary.a
RGB    = sample.rgb * primary.rgb
A      = alpha
discard selected by integer u_AlphaTest
```

The color expression is the existing one-texture MODULATE object:

```text
A90..A93  90018000 91000001 92010002 93000001
TAU        301001b0
ALU        0140b6d0 30040053 00f20005 0000007f
           01c00000 301c0000 00f20105 000d083f
```

At shader creation Mesa must record the linked fragment input locations for
the vec2 coordinate and vec4 primary independently. GLSL varyings normally
arrive as generic semantics; requiring `COLOR0` because the source variable
is named `var_Color` is wrong. At draw time the software VS result is repacked
as D1 normalized AARRGGBB plus D3 S/T, and the current fixed native VS routes
those to oD0 and oT0.

`u_AlphaTest` is a direct constant-buffer-zero integer load at a recorded
aligned byte offset. Read it at draw time. Value zero disables A33; value one
selects the already-live GREATER/zero test. Values two and three remain
rejected until the exact-half A33 `0x200` versus byte-derived `0x202` oracle
passes. The integer bits are not a PS float constant.

No new ioctl is intrinsically required for generic. Select the existing
one-texture no-depth, blend, D16, or D16+blend request according to the
already validated fixed state.

### Reduced fast-light world

With normal and specular mapping disabled, lightall index 1 defines
`USE_LIGHT`, `USE_FAST_LIGHT`, and `USE_LIGHTMAP`. Its vertex shader writes:

```text
var_TexCoords.xy = attr_TexCoord0.st
var_TexCoords.zw = attr_TexCoord1.st
var_Color        = u_VertColor * attr_Color + u_BaseColor
```

The fragment source at `lightall_fp.glsl:273..298,514..520` reduces exactly
to:

```text
RGB = texture0.rgb * texture1.rgb * primary.rgb
A   = texture0.a * primary.a
```

Texture1/lightmap alpha is not read. Therefore the recovered full-RGBA
three-factor object is exact when texture unit 1 is XRGB (A30 format 98,
sampled alpha one), while texture unit 0 may be XRGB or BGRA:

```text
A90..A93  90000000 91000001 92020003 93000002
TAU        301001b0 311211b1
ALU        0140b6d0 30202053 00f20005 0000007f
           0140b6d0 30040053 00f20105 0000007f
           01c00000 301c0000 00f20105 000d083f
```

If unit 1 is BGRA and its stored alpha is not provably one, only the recovered
primary-alpha-bypass four-ALU object is exact. Do not silently use the full
RGBA object in that case.

The key new Mesa layout is **one packed vec4 fragment input**, not two linked
coordinate inputs. Record the linked input and require exactly:

```text
unit 0 coord = packed.xy
unit 1 coord = packed.zw
primary      = separate linked vec4
```

The expected linked NIR can be scalarized or copy-propagated, so a classifier
must match this semantic DAG rather than instruction order or a hard-coded
`VAR0` number: unit-zero TEX takes packed lanes xy, unit-one TEX takes lanes
zw, RGB is their product times all three primary RGB lanes, and output alpha
is unit-zero alpha times primary alpha. It also recognizes the same exact
`u_AlphaTest` branch and records its direct integer load offset. A unit-one
alpha leaf in the output, even if algebraically multiplied by one in a test
capture, is a different full-RGBA key.

The draw module executes the application VS. The vbuf stage reads that one
post-TNL vec4, writes its xy into immediate D3 and zw into immediate D4, and
packs primary into D1. The kernel continues receiving the existing ten-dword
vertex:

```text
x y z rhw diffuse specular s0 t0 s1 t1
```

The existing six-record native VS remains unchanged:

```text
00380150 00000000 00000000 0008010f
0008014c 00100150 00384154 0004021f
0008414c 00000000 00000000 0008422f
0008814c 00000000 00000000 0008423f
0008c14c 00000000 00000000 0008028f
0009014c 00000000 00000000 0008029f
A60 = 60800005
```

Thus this is a post-TNL packing extension, not a native VS compiler or UAPI.
It must reject missing/aliased lanes, a second packed coordinate value,
projectors, non-2D samples, or extra fragment arithmetic.

Use capability 25 and top-level program flag bit zero on the existing
two-texture commands. No-depth and D16 use commands `0x0a` and `0x0d`; the
state-complete D16+blend form uses `0x13` after its program-bit extension.
The exact body lengths, indices, format matrix and four-BO lifetime are in
`two-texture-primary-uapi-design.md`.

### Texturecolor

`texturecolor_fp.glsl:7..10` is exactly `texture0 * u_Color`. It needs the
native c0 source, not a UNORM8 D1 approximation:

```text
A90..A93  90018000 91000001 92010002 93000001
TAU        301001b0
ALU        0140b6d0 30080053 00f20005 0000007f
           01c00000 301c0000 00f20105 000d083f
```

Mesa records the direct 16-byte-aligned constant-buffer-zero offset and copies
the four IEEE dwords bit-exactly at draw time. This is the first key that needs
private command `0x16`, capability 33, and its job-local constants record.
The kernel owns every ALU, TAU and scheduler word. Userspace supplies only the
semantic program id and constant values.

## Fresh dynamic Windows compiler check

The PE32 probe with SHA-256
`a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c`
was rerun under Wine 11.10 against the exact D3D UMD. Fixed scratch outputs
were removed before each run and `chrome9-shader-used.bin` matched the input.

For `texld r0,t0,s0; mul oC0,r0,v0`:

```text
input SHA-256  4f0ccbe39de980f4d4a7f1d56146dd736093053881a7ea664de5623b287dbe55
ALU            0140b6d0 00040053 00f00000 00000820
ALU SHA-256    d24f6a9bfe123b76f274ed7673ab27e3e7e597672bc82cc8adc4e476f4ddf475
TAU            201001b0
TAU SHA-256    6bc2d9f1a19245224609b68f16f1fede46da85317e0860339a6c34e2d1aa980d
```

For `texld r0,t0,s0; mul oC0,r0,c0`:

```text
input SHA-256  35a5cdc630b5f224218aec449b0d03ea9868c26b089009feeefe71184f8555dd
ALU            0140b6d0 00080053 00f00000 00000820
ALU SHA-256    7fef06bbdf25b61fe1e50eeada7a660507d1a0eb14e0aab66e35e7d363925067
TAU            201001b0
TAU SHA-256    6bc2d9f1a19245224609b68f16f1fede46da85317e0860339a6c34e2d1aa980d
```

The single operand differential, `00040053 -> 00080053`, independently
proves primary versus c0 source selection while TAU is unchanged. These
Windows words use the probe's identity/virtual allocation and are not upload
templates. The executable packet rows above remain the Linux/Xorg
configuration-two physical objects.

## Capability and UAPI order

Keep the already reserved namespace:

```text
cap 25  TWO_TEXTURE_PRIMARY_MODULATE
cmd 16  DRAW_TEXTURED_PROGRAM_TRIANGLE
cap 33  DRAW_TEXTURED_PROGRAM
cap 34  FRAGMENT_CONSTANTS_32
```

The dependency rules are asymmetric:

* cap 34 alone permits Mesa to report 512 bytes / 128 components for fragment
  constant buffer zero and therefore removes the startup link gate;
* cap 25 permits only the exact two-texture-primary templates on the existing
  structured two-texture commands;
* cap 33 plus command 16 permits semantic one-texture program draws;
* texturecolor execution requires both cap 33 and a constant capability;
* none of these capabilities advertises native application VS execution,
  arbitrary fragment ALU, flow control, derivatives, explicit LOD, cube maps,
  or more than the separately supported sampler count.

Full constant upload must be in the same validated draw job. Use the two H3
packets for c0..c15 and c16..c31; never expose a persistent
set-constants-then-draw ioctl. Unsupported linked programs are retained as
shader objects but every draw rejects before GEM mapping or ring publication.

## Staged capture and silicon order

1. **Link-only diagnostic.** Expose cap 34 only after both H3 banks pass raw
   c15/c16/c31 readback oracles. Start renderer2 with the reduced cvars and
   `CHROME9_DUMP_NIR=1`. Record the first actual draw NIR/state; source order
   is not a substitute for runtime order.
2. **Generic mode zero.** Distinct texture and primary lanes, no depth/blend,
   then D16 LEQUAL/read-only and common blend tuples. Verify pixels, depth,
   guards, cookie and following known-good draw.
3. **Generic alpha mode one.** Alpha strip `00,01,7f,ff`; zero must be
   rejected and all nonzero values kept. Run on/off/on closure.
4. **Packed-coordinate layout.** Software VS emits one vec4
   `(s0,t0,s1,t1)` with four quadrant combinations. Distinct red/green
   textures prove D3/D4 were split in the correct order before testing the
   three-factor ALU.
5. **Fast lightall product.** XRGB lightmap, BGRA diffuse, nonwhite primary.
   Expected alpha is `diffuse.a * primary.a`, independent of lightmap storage
   alpha. Repeat with D16 and the exact world fixed state.
6. **Texturecolor c0.** Zero/one c0 lanes first, fractional lanes second;
   surround with generic draws to prove constant/program closure.
7. **Actual q3dm1.** Only after all prior gates, run renderer2 with reduced
   cvars. Keep every other shader key fail-closed and capture the next first
   rejection.

Negative tests must cover swapped packed xy/zw, one missing coordinate pair,
unexpected varying consumers, indirect/short constant loads, non-finite c0,
wrong sampler units, unit-1 BGRA under the full-RGBA fast-light key, unknown
alpha modes, unsupported alpha-half modes, and every reserved/unknown UAPI
bit. A shader link, frame counter, clean MMIO status or absence of a warning
is not a pixel oracle.

## Confidence boundary

Startup ordering, reduced lightall algebra, packed VS varying shape, generic
and texturecolor expressions are **source exact**. Dynamic Windows hashes and
operand differentials are **compiler exact**. Linux/Xorg physical program
objects and packet layouts are **static compiler/driver exact**. The existing
fixed VS, GL1 one-/two-texture, D16, blend and alpha-test pieces have separate
live evidence. Full H3 constant banks, nonzero native c0 under command 16,
the packed lightall varying split, and the three-factor primary object remain
**provisional until guarded VX900 readback**.
