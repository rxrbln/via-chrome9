# Chrome9 fixed-function state-transition closure

This note turns the separately recovered blend, alpha-test, fog, scissor, and
texture-format fields into one transition-safe two-texture state image.  The
goal is not another list of fields: it is to identify every word which must be
rewritten when a feature is enabled and then disabled without resetting the
engine.

The primary executable evidence is the Windows Direct3D user-mode binary
`binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll`
(SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`)
and its OpenGL copy `VTGOGL32A.dll` (SHA-256
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`).
The old Mesa 7.11.2 UniChrome driver is used only as an independent semantic
and lifecycle cross-check.  Its register generation predates H5/H6 and its
packet words must not be copied into a VX900 stream.

Alpha test, canonical source-over, BGRA destination writes, and D16 ordering
are live on the target VX900.  The partial-scissor and fixed-fog derivatives
below remain guarded experiments.  The texture-format table is exact static
recovery; only formats explicitly promoted in the hardware log are live.

## Two A00 namespaces must remain separate

The two-texture body contains two words whose subaddress is A00 but whose
parameter types differ:

* type-01 render-target A00 controls dither, RT blend, RT fog, and alpha test;
* subtype-FE general-texture A00 controls active texture count and the
  one-shot texture-cache transition.

For the current two-stage draw the latter is always `00000021`, paired with
general-texture A08=`08010102`.  One stage uses
`00000011/08010101`.  Blend, alpha-test, or fog lowering must modify only the
type-01 A00.  Applying `00100000`, `00000f00`, or `10` to subtype-FE A00
changes texture control rather than the intended render state.

For one render target, the normalized type-01 construction is:

```text
rt_a00 = 00000001                 # retained baseline bit 0
       | (blend_enable ? 00000010 : 0)
       | (fog_enable   ? 00000F00 : 0)
       | (alpha_enable ? 00100000 : 0)
```

The legacy Xorg COPY path keeps the hardware blend bit set and supplies COPY
equations, hence its usual A00=`00000011` even when the API operation is a
logical copy.  The structured successor has independently passed with bit 4
clear for disabled blending.  Both produce copy semantics, but a new builder
must choose one policy deliberately rather than infer blend state from the
legacy word.

## Stable two-texture body and index equations

Start from the established 262-dword, no-mip two-texture triangle.  Define:

```text
B = 4 when the destination block closes all A38..A3F, else 0
D = 4 when the five D16 words consume the old common-state pad, else 0
F = 4 when A70..A72 fog state plus alignment is appended, else 0
```

The exact total is `262+B+D+F`.  Alpha test consumes an existing slot in the
closed destination block and scissor replaces existing words, so neither
changes length.  With `B=4`, `D=4`, `F=4`, the state-complete body is 274
dwords and the important words are:

| state | body index/formula | full 274-dword index |
|---|---:|---:|
| A33 alpha compare | `19` when `B=4` | `19` |
| image-0 A30 format | `25+B` | `29` |
| image-1 A30 format | `45+B` | `49` |
| type-01 render A00 | `63+B` | `67` |
| A70/A71/A72 fog | `67+B..69+B` without D16; `72+B..74+B` with D16 | `76..78` |
| subtype-FE A00 | `72+B+D+F` | `84` |
| subtype-FE A08 | `74+B+D+F` | `86` |
| FVF A03 | `87+B+D+F` | `99` |
| raster A23/A24 | `94/95+B+D+F` | `106/107` |
| clip A82/A83 | `146/147+B+D+F` | `158/159` |
| RGBA-by-RGBA source-C word | `217+B+D+F` | `229` |
| immediate header | `228+B+D+F` | `240` |

The fog words follow A10--A13/A20 when depth is present.  The full common
group is then:

```text
body[60..63] header
body[64..70] A86,A87,A53,A00,A01,A88,A89
body[71..75] A10,A11,A12,A13,A20
body[76..78] A70,A71,A72
body[79]     cc000000
```

All words are self-addressed, but keeping this fixed order makes length and
pre-publication assertions deterministic.  A stage-0 mip table inserts 12
dwords after image 0; it shifts stage 1 and every later index but does not
alter these state semantics.

## Alpha-test enable and disable

Windows routine `0x101fab20..0x101fabc6` derives one Boolean and toggles only
type-01 A00 bit 20.  The bound-target checks can force it off for private
formats `1028`, `102c`, and `203c..203e`; ordinary UNORM targets retain the
requested value.  The routine dirties A00 in both directions.  Compare-state
builder `0x101facf0..0x101fadf9` and the byte-identical OpenGL path encode:

```text
A33[14:12] = NEVER..ALWAYS = 0..7
A33[10:0]  = ref == ff ? 400 : (ref << 2) | (ref >> 6)
```

For LESS/ref `80`, enable with:

```text
A33 = 33001202
type-01 A00 |= 00100000
```

Disable by clearing exactly A00 bit 20.  Windows leaves A33 resident because
the enable gates it.  A fixed-size state-complete successor can additionally
write the harmless canonical disabled value `33007000` (ALWAYS/ref zero) at
body 19; this avoids retaining a previous compare word without growing the
packet.  Subtype-FE A00 remains `00000021` throughout.

The strongest already-live two-texture derivative uses alpha LESS/ref `80`
with D16 LESS/write: near alpha `c0` is killed and farther alpha `40` stores
green/depth `8000`.  The transition oracle is equally important: immediately
follow it with alpha-disabled opaque COPY, then repeat the alpha-tested draw.
The middle draw must not inherit A00 bit 20, and the third must not depend on
the disabled canonical A33.

Old Mesa independently uses `(GL_func-GL_NEVER)` directly as the compare
selector and clears only its alpha-test enable on disable
(`viaChooseColorState:1209..1219`).  Its reference is only an eight-bit older
generation field; it confirms enable/compare lifecycle but not the VX900 A33
fixed-point packing.

## Blend enable, equations, and constant closure

Every state-complete transition writes all eight constant words, even while
they are zero:

```text
38000000 39000000 3A000000 3B000000
3C000000 3D000000 3E000000 3F000000
```

The safe disabled/COPY equation is:

```text
34000150 35000114 36000590 37000224
```

Disabled blending clears type-01 A00 bit 4.  Enabling canonical source-over
sets that bit and uses:

```text
34000020 35000148 36000590 37000244
```

This independent-alpha form is live on destination format 99: half-alpha red
over transparent green stored `80807f00`.  It is preferable to copying RGB
factors into the alpha lane, which would store alpha near `40`.  The full
equation/function/factor tables and nonzero constant layout are in
[blend-alpha-packet-recipes.md](blend-alpha-packet-recipes.md).

The zero constants are part of the transition contract, not decorative
defaults.  ZERO/ONE and CONST/INV_CONST share factor selectors, so a future
nonzero blend color followed by a packet which clears only the historical
A38/A3B/A39/A3C subset can silently change COPY.  Before exposing constants,
route every subsequent draw through this fully closed block or expand every
legacy body to all A38--A3F.

Old Mesa 7.11.2 supports only ADD and rejects separate RGB/alpha factors in
`viaBlendEquationSeparate` and `viaBlendFuncSeparate`.  That is an older-chip
software limitation, not a Chrome9 capability result: the Windows H5/H6
builders and live independent-alpha result prove the wider state.  Its useful
cross-check is lifecycle: disabling blend clears the gate, not the factor
register image.

## Fog enable, disable, and FVF closure

Windows `0x101fbb70..0x101fbc15` reconstructs the enable mask rather than
setting one undocumented bit.  With Boolean zero it clears A00 `[11:8]`;
with Boolean one it sets exactly `00000f00`.  It preserves all other A00
fields and dirties A00 in both directions.  This independently proves:

```text
legacy COPY, fog off:  A00=00000011
legacy COPY, fog on:   A00=00000F11
successor blend off:   A00=00000001
successor blend off,
fog on:                A00=00000F01
```

The lowest-risk fixed-fog source uses normalized secondary-color alpha and
avoids the unresolved W/Z/range equations:

```text
A70 = 70000020                    secondary alpha, local mode
A71 = 71400000                    R=1, G=0
A72 = 72000000                    B=0
A03 = 030052D1                    normalized packed COLOR1
```

With white-times-blue two-texture output and red fog, submit D2 alpha endpoint
triangles `00` and `ff`.  Red/blue or blue/red identifies the factor
convention.  This F=4 body is a diagnostic only until it passes.

The disable transition must clear A00 `00000f00` and restore
A03=`030052d0`.  Windows leaves A70--A78 resident because A00 gates them.  A
fixed F-layout may additionally close the first three words as
`70000000 71000000 72000000`, but the first live closure should use the
ordinary already-proven F=0 two-texture packet: A00 fog bits clear, base FVF,
no fog words.  It must reproduce the unfogged color, followed by another fog
endpoint draw.

Old Mesa's `viaChooseFogState:1249..1272` has the same lifecycle: enable the
fog gate and populate parameters; disable only the gate.  Its emitter writes
fog parameters only while enabled.  This independently supports gate-based
closure while its byte-packed color/range words remain generation-specific.

## Scissor is a rectangle replacement, not an enable bit

The Windows viewport/scissor builder around
`0x101f7949..0x101f7aa6` intersects D3D's exclusive RECT with the
drawable/viewport, and its state-image dirty path covers A80--A83 together.
For a 32-square target the disabled/full state is:

```text
80000020 81000020 82000020 83000020
```

The first two words are the color window.  Scissor `[8,24)x[8,24)` replaces
only the latter pair:

```text
A82 = 82008018
A83 = 83008018
```

There is no separately recovered scissor-enable bit.  Disabling scissor means
writing A82/A83 back to the full target on the next draw.  Always emit all
four words in the fixed clip block, keep right/bottom exclusive, intersect in
software, and treat an empty result as a no-op until zero-area hardware
behavior is measured.  The replacement changes no packet size: in the full
274-dword layout A82/A83 remain body 158/159.

Mesa 7.11.2 deliberately does not prove these words.  `viaScissor()` flushes
DMA on state changes, converts GL bottom-origin coordinates to an exclusive
DRM clip rectangle, and `via_ioctl.c:898..900` intersects it in software
before emitting work.  This agrees on exclusive ends and the need for an
ordered transition, while confirming that old UniChrome source is not an
independent H6 register oracle.

The bounded live sequence is full -> `[8,24)` -> full on three disjoint
regions without resetting the context.  The first and third must cover their
entire primitive; the middle must alter only the intersection.  Check pixels
away from both triangle and rectangle edges and preserve all guards.

## Texture-format transitions

Each texture stage has two different subaddress-30 words:

```text
image A30   = 30000000 | (format_code << 16)
sampler A30 = 30000000 | (lod_bias << 12) | (max_lod << 6) | min_lod
```

They share a numeric subaddress but live in different type-02 groups.  Never
put a sampler LOD word at body `25+B/45+B`, or an image format in the sampler
block.  The immediately useful image words are:

| resource format | image A30 |
|---|---:|
| X8R8G8B8 | `30980000` |
| A8R8G8B8 | `30990000` |
| X8B8G8R8 | `30B80000` |
| A8B8G8R8 | `30B90000` |
| R5G6B5 | `30890000` |
| X1R5G5B5 | `30880000` |
| A1R5G5B5 | `308A0000` |
| A4R4G4B4 | `308B0000` |
| L8 / A8 / A8L8 / L16 | `30130000` / `301B0000` / `30150000` / `30160000` |
| DXT1 / DXT3 / DXT5 | `30290000` / `302A0000` / `302B0000` |

Windows image builder `0x101d7c10` and the OpenGL copy `0x10240740` rebuild
base, dimensions, layout, and format from the bound resource.  Changing
format therefore requires revalidating cpp/block size, canonical pitch,
complete mip range, and BO guards, not merely changing A30.  The no-mip
state-complete format words stay at body 29 and 49; a stage-0 mip table moves
stage 1 and later state by 12.

An exact no-layout-change alpha discriminator uses the same 32-bpp stage-1
storage twice.  Fill it with `00ff0000`, make stage 0 opaque white, use the
RGBA-by-RGBA program, a format-99 target, full write mask, no blend/alpha/depth
tests, and draw disjoint regions:

```text
stage-1 A30=30990000 (A8R8G8B8) -> 00ff0000
stage-1 A30=30980000 (X8R8G8B8) -> ffff0000
```

The second result proves XRGB alpha synthesis and, because only image A30
changes at body 49, catches a stale format descriptor directly.  Follow it by
the first word again and require transparent red.  Issue the established
texture-cache transition for every job and retain the shared source mapping
until the final fence.

Test R5G6B5 separately because its 64-byte pitch and `0x800` useful range
change bounds.  Saturated `f800/07e0/001f/ffff` quadrants multiplied by an
opaque-white second stage must return red/green/blue/white; A30 is
`30890000`.  Compressed formats come only after this narrower-cpp transition.

Two stages to one is closed by the general texture count and program/FVF
state, not by zeroing a stale stage-1 image descriptor.  The following
one-stage draw must explicitly emit:

```text
subtype-FE A00/A08 = 00000011 / 08010101
one-stage TAU, PS count, VS, linkage, and eight-dword vertices
active sampler A30 = 30000000       # L0 only, zero bias
```

The count/control/program make stage 1 unreachable; the sampler A30 prevents
stale mip selection on the still-active stage 0.  Preserve the retired stage-1
BO only until the two-stage fence, then normal reservation lifetime rules
apply.

Old Mesa independently rebuilds format/image state for every enabled unit,
emits a complete sampler/combiner block, and clears all global texture-pipeline
enables when no unit remains (`viaChooseTextureState:868..951`).  It may remap
API unit 1 onto hardware unit 0 when unit 0 is absent, an older-generation
optimization which must not be copied into Chrome9's count-based A00/A08 and
fixed PS/VS linkage.

## Minimal ordered regression

After the base two-texture and D16 paths are stable, one guarded sequence can
catch every stale transition without combining unproved features in the same
first draw:

1. two-texture BGRA source-over plus alpha LESS/D16, using the already-live
   270-dword state-complete packet;
2. the same resources with blend/alpha/depth disabled, constants zero,
   canonical COPY equations, and A33=`33007000`;
3. full scissor, `[8,24)` scissor, then full scissor;
4. A8R8G8B8 -> X8R8G8B8 -> A8R8G8B8 reinterpretation on the fixed 32-bpp
   source above;
5. fixed-fog endpoint draw only after stages 1--4 pass, immediately followed
   by the ordinary F=0 unfogged packet; and
6. a one-texture opaque REPLACE draw, then the original two-texture draw.

Every job retains target, all active textures, optional depth, command ring,
and every PTE through its fence.  Validate body length and the index formulas
above before publication.  On timeout quarantine all reachable mappings;
never use a following closure draw as recovery from a packet which may still
be executing.
