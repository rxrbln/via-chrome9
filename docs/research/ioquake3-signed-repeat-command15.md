# VX900 signed two-texture repeat and command-15 program selection

## Result

The next ioquake3 GL2 world blocker after the packed lightall program is not
another fragment shader.  It is the current two-texture vertex validator:
base-texture coordinates in `q3dm1` legitimately range far outside `[0,1]`,
while the lightmap coordinates remain normalized.  VX900 receives both as raw
IEEE-754 float2 vertex attributes and applies each sampler's A32 address mode
after interpolation, before forming the texel fetch address.  Neither the
Windows nor legacy open-source driver clamps or converts S/T while preparing
texture state.

The conservative first extension is wrap-specific:

```text
each REPEAT axis: finite, abs(value) <= 32.0
each CLAMP_TO_EDGE or MIRRORED_REPEAT axis: current [0,1] rule
filter: current nearest/linear, level zero only
```

This tuple covers the captured q3dm1 base/lightmap data without claiming
general signed CLAMP or MIRROR behavior for the two-stage path.  Reserve
capability bit 40, `TWO_TEXTURE_SIGNED_REPEAT_COORDS`, for it.  No ioctl
structure or packet length changes.  The q3 world tuple is unit 0
REPEAT/REPEAT and unit 1 CLAMP/CLAMP, but the validator applies the same
per-axis rule to either unit rather than encoding application-specific roles.

The reserved command-15 run ABI has a separate issue: its `flags` bits 0 and
1 already mean DEPTH and BLEND, while existing two-texture commands use the
same bits as semantic program selectors.  Because command 15 is not merged,
rename its offset-36 `reserved0` word to `program` and keep flags exclusively
for run state.  The structure stays 200 bytes and its ioctl number stays
`0x40c86455`.

Capability 40 is now live-proven on VX900 through signed REPEAT coordinates
`+/-31.109375` on both units, with guards and engine status clean.  The larger
q3-derived extrema and transition sequence below remain useful regressions.

## Actual q3dm1 coordinate requirement

The analysis input is the original `pak0.pk3` used on the target:

```text
pak0.pk3 SHA-256:
e77abad2466f45a0a7ea018445528f9b95a0fe7789fa1abc1a7718bbf0754b08

maps/q3dm1.bsp: 1,906,708 bytes, IBSP version 46
SHA-256:
3d2dad8e6ba05914fc90619a6517ecdee7e5eb50f86013a14a84bb3c61cd702e
```

Parsing the 44-byte vertex lump and 104-byte face lump gives 13,978 vertices
and 2,097 faces: 1,942 polygon, 113 patch, and 42 mesh faces.  The 2,022
lightmapped polygon/patch faces reference 10,982 distinct vertices.

| coordinate over lightmapped vertices | minimum | maximum | vertices with either component outside `[0,1]` |
|---|---:|---:|---:|
| base S/T | `-3.7753200531`, `-19.0` | `17.1171875`, `21.5` | 4,762 / 10,982 |
| lightmap S/T | `0.00390625`, `0.00390625` | `0.99609375`, `0.98828125` | 0 / 10,982 |

Across all faces the base-T minimum is `-28.5`; the other extrema above do
not change.  An absolute bound of 32 is consequently sufficient for every
q3dm1 vertex, including non-lightmapped geometry, while remaining a small
policy envelope for the first silicon test.

The BSP contains 5,308 explicit lightmapped polygon triangles across 70
`(shader, lightmap)` keys.  This also quantifies the later batching gain: if
sorted only by those immutable keys, they require 327 native list segments of
at most 18 triangles and 91 command-15 run ioctls of at most 144 triangles.
Patch tessellation and visible-frame culling are deliberately outside that
static count.

The engine's texture policy agrees with the data.  ioquake3
`renderergl2/tr_image.c` selects `GL_CLAMP_TO_EDGE` only for
`IMGFLAG_CLAMPTOEDGE`, otherwise `GL_REPEAT`.  `renderergl2/tr_bsp.c` creates
lightmaps with `IMGFLAG_CLAMPTOEDGE`; ordinary images default to repeat.  The
captured diagnostic configuration uses `r_picmip 0` and
`r_textureMode GL_LINEAR`, so the immediate world path is bilinear level-zero
sampling, not mip selection.  The general renderer default remains a mip
filter and needs the separate guarded mip successor before a complete global
capability claim.

For the captured world tuple the sampler words are exact:

```text
unit 0 base image: A31 = 31102490, A32 = 32000052  # linear, REPEAT S/T
unit 1 lightmap:   A31 = 31102490, A32 = 32000049  # linear, CLAMP S/T
```

## Coordinate representation and address ordering

There is no fixed-point S/T conversion in texture setup.

The existing two-texture immediate record carries:

```text
x y z rhw diffuse specular s0 t0 s1 t1
```

The H6 declarations are A04 `04006342` and A05 `05008442`.  In the recovered
declaration encoding, format `42` is two float32 components; the Type-00
payload contains the original IEEE-754 bit patterns.  The fixed VS MOV routes
D3 and D4 to native oT0 and oT1.  A01 keeps perspective correction enabled,
so rasterization interpolates these float coordinates using RHW before the
texture units consume them.

Sampler preparation is orthogonal:

```text
A32 = 32000040 | (wrap_t << 3) | wrap_s
```

The paired Windows address builders are
`VTGUModeDX32A.dll:0x101d9250` and
`VTGOGL32A.dll:0x10241a70`.  They translate U/V/W API state independently:

| API address mode | A32 field code |
|---|---:|
| BORDER | 0 |
| CLAMP | 1 |
| WRAP / repeat | 2 |
| MIRROR | 3 |
| MIRRORONCE | 7 |

They update the sampler's A32 fields, not vertex memory or the H6 declaration.
The image builders separately emit dimensions/base/format, and the filter
builders separately emit A30/A31.  No coordinate-value test or software
fract/clamp operation occurs in these texture-state builders.

Legacy Mesa independently has the same separation.  UniChrome
`via_state.c:get_wrap_mode()` maps `GL_REPEAT`, `GL_CLAMP_TO_EDGE`, and
`GL_MIRRORED_REPEAT` to `HC_HTXnMPMD_S/T*` bits.  `viaChooseTextureState()`
writes those bits independently to `regHTXnMPMD[0]` and `[1]`; it neither
examines nor rewrites vertex S/T.  The older field positions differ from
Chrome9 A32, so this is ordering/semantics evidence, not a packet template.

The resulting pipeline contract is:

```text
raw float D3/D4 -> perspective interpolation -> oT0/oT1
                -> per-unit A32 address operation
                -> normalized coordinate-to-texel address
                -> nearest/linear fetch
```

This order is also the only one consistent with the already-passed VX900
single-unit NPOT oracle: identical out-of-range float coordinates returned
different texels when only A32 changed among clamp, repeat, and mirror.  The
FE-general A0b/A0c words remain zero in executable texture packets; they are
not required for ordinary wrap and must not be guessed into this successor.

## Exact UAPI extension

Capability bit 40 allocates no command or request field:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS (1ULL << 40)
```

The existing texture-state words already carry the semantic wrap mode.  When
the capability is absent, preserve the old two-texture `[0,1]` rule byte for
byte.  When present, apply the wider rule independently by axis:

1. an axis whose wrap is REPEAT is finite and has absolute IEEE magnitude at
   most `0x42000000` (`32.0f`);
2. an axis whose wrap is CLAMP_TO_EDGE or MIRRORED_REPEAT still passes the
   existing `[0,1]`, signed-zero-tolerant validator; and
3. the request otherwise passes the existing exact program, image, filter,
   resource, depth/blend, guard, and packet gates.

A bitwise validator avoids host floating-point mode dependencies:

```c
static bool chrome9_safe_texcoord(u32 value, u32 wrap)
{
        u32 magnitude = value & ~BIT(31);

        if (!chrome9_float_finite(value))
                return false;
        if (wrap == DRM_CHROME9_TEXTURE_WRAP_REPEAT)
                return magnitude <= 0x42000000;
        return (!(value & BIT(31)) || !magnitude) &&
               magnitude <= 0x3f800000;
}
```

This accepts positive and negative zero and finite subnormals.  It rejects
NaN, infinity, and every magnitude above 32.  It is a conservative kernel
safety policy, not a claimed silicon numeric limit.

Apply the rule consistently to commands 0x0a, 0x0d, and 0x13, including all
program selectors 0/1/2.  It changes validation only: the builder must copy
the accepted S/T dwords unchanged.  No coordinate reduction belongs in Mesa
or the kernel because it would alter derivatives at primitive boundaries and
break perspective-correct interpolation.

Initially do **not** infer any of these broader contracts:

- signed two-stage CLAMP or MIRROR;
- magnitudes above 32;
- a non-unit projector Q;
- mipmapped signed-repeat sampling; or
- border/mirror-once API equivalence.

Each can be promoted independently after its own guarded two-stage oracle.

## Minimal no-wedge silicon oracle

The completed activation used the already-proved two-texture command 0x0a:
three distinct guarded
BOs, a small XRGB target, and two 32x32 canonical sources.  Fill unit 0 with
four eight-column bands in order blue, red, green, white.  Fill unit 1 opaque
white.  Use nearest filtering initially, unit-0 A32 `32000052`, unit-1 A32
`32000049`, and constant T=`0.5` on both units.  Constant coordinates avoid a
large interpolated derivative and keep a failed address unit within mapped
source plus guards.

The compact transition regression is:

| draw | unit-0 S | unit-1 S/T | expected unit-0 XRGB |
|---:|---:|---:|---|
| 1 | `-0.375` (`bec00000`) | `0.5/0.5` | green |
| 2 | `+0.5` (`3f000000`) | `0.5/0.5` | green |
| 3 | `+1.375` (`3fb00000`) | `0.5/0.5` | red |
| 4 | `-0.375` | `0.5/0.5` | green again |

The signed -> in-range -> positive-overflow -> signed transition catches
stale sampler state without requiring an engine reset.  Repeat it with
linear A31 `31102490` only after nearest passes.

The live bound sweep additionally sampled both units at signed REPEAT
coordinates `+/-1.109375`, `+/-16.109375`, and `+/-31.109375`; all expected
texels, source/target guards, retirement, and engine status passed.  Thus the
32 policy is measured rather than inferred solely from q3 assets.  Windows
publishes `MaxTextureRepeat=128`, as detailed in
[signed-wrap-mipmap-nearest-contract.md](signed-wrap-mipmap-nearest-contract.md),
but the initial UAPI intentionally remains four times narrower.

Then exercise q3-derived extrema as constant-coordinate triangles.  Compare
each against an in-range coordinate with the same mathematical fractional
part rather than assuming a color by eye:

```text
-3.7753200531 -> fract = 0.2246799469
+17.1171875   -> fract = 0.1171875
-19.0         -> fract = 0.0
+21.5         -> fract = 0.5
-28.5         -> fract = 0.5
```

For every pair require byte-identical covered pixels.  Only after all
constant-extreme draws pass, use one triangle spanning the q3 minimum and
maximum to validate interpolation plus repeated addressing.  After every
draw compare both source BOs and all leading/trailing guards, target outside
the scissor, retirement cookie, CR PAUSE, work-mask idle, and error registers.
Run command 0x0d D16 LESS/LEQUAL and command 0x13 D16+blend only after the
no-depth oracle is clean.  Negative requests (33.0, infinity, NaN, wrong
wrap tuple, out-of-range unit 1) must fail before GEM lookup/publication.

## Command-15 flag/program collision

The reserved command-15 structure currently places a zero-only `reserved0`
at offset 36, while `flags` at offset 8 uses:

```text
bit 0 DEPTH
bit 1 BLEND
bits 10:8 depth function
bit 11 depth read-only
```

Existing commands 0x0a/0x0d/0x13 instead use low bits 0 and 1 as the
two-texture semantic program selector.  Copying that selector into command
15 flags would turn a primary-modulate draw into DEPTH and a TEX0-alpha draw
into BLEND.  Masking either interpretation cannot resolve the ambiguity.

The ABI-compatible correction is possible because command 15 is reserved,
not merged:

```c
struct drm_chrome9_draw_two_textured_triangle_run {
        /* offsets 0..32 unchanged */
        __u32 destination_format;       /* 32 */
        __u32 program;                  /* 36: semantic enum, was reserved0 */
        struct drm_chrome9_texture_state textures[2]; /* 40..111 */
        /* offsets 112..199 unchanged */
};

#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_MODULATE           0
#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_PRIMARY_MODULATE   1
#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_TEX0_ALPHA_PRIMARY 2
```

The numerical values deliberately match `(legacy_flags & 3)` for the
per-triangle requests, but command 15 transports them in a dedicated word.
Reject program 3 and every higher value.  Program 1 requires cap25; program 2
requires cap39 and initially BGRA texture 1.  Capability 40 is orthogonal and
controls only coordinate acceptance.  Run flags never contain program bits.

This changes neither `sizeof(struct)=200`, `offsetof(vertices_ptr)=168`, nor
the x86 ioctl value `0x40c86455`.  Both UAPI copies still need explicit size
and offset assertions.

The program-dependent list accounting is:

| program | PS ALUs | fixed body before vertices (`B=D=0`) | body for T triangles |
|---|---:|---:|---:|
| 0, legacy two-factor | 2 | 232 | `232 + B + D + 30*T` |
| 1, full-RGBA primary | 3 | 236 | `236 + B + D + 30*T` |
| 2, TEX0-alpha primary | 4 | 240 | `240 + B + D + 30*T` |

Here `B=4` for the closed blend envelope and `D=4` for D16.  At the maximum
`T=18`, program 2 with depth+blend is 788 dwords and still has PAUSE `0xcf0`,
STOP `0xd70`, and read-ahead end `0xf90`.  At `T=19` even the smaller variants
cross to STOP `0xdf0` and read-ahead `0x1010`; the maximum remains 18.

A future stage-0 H3 mip table adds 12 dwords.  Program 2 + depth + blend + mip
would be 800 dwords at T=18, crossing the page-safe boundary; that exact key
must reduce its segment maximum to 17.  Do not add mip metadata to command 15
or retain 18 blindly until the run ABI grows a complete guarded mip envelope.

The immutable Mesa run key must include the new `program` word and both wrap
modes.  A program or wrap change ends the run.  The kernel validates program,
capabilities, formats, and signed-repeat eligibility before copying userspace
vertices or looking up BOs.

## Implementation order

1. Add cap40 and the wrap-specific validator to the three existing
   one-triangle two-texture commands.
2. Keep offline tests for exact boundary bit patterns: `-32`, `+32`, signed
   zero accepted under the exact wrap tuple; next representable values,
   `+/-inf`, and NaNs rejected; unit 1 remains `[0,1]`.
3. Preserve the passed nearest signed sweep, then add the transition, linear,
   q3-extreme, and interpolated regressions above.  Re-run all old normalized
   two-texture/depth/blend guards.
4. Advertise cap40 and relax Mesa per axis only when the selected wrap is
   REPEAT.
5. Correct command 15's offset-36 word before implementing the reserved run;
   generate and assert body sizes by program rather than one fixed constant.
6. Gate the run independently with program 0, 1, then 2 and with signed-repeat
   coordinates.  The per-triangle path remains the fallback until cap31 is
   advertised.

The 1,304-bind runtime capture after map load contains no third unsupported
fragment object after the packed lightall pair: it returns to already-known
packed generic, constant-color, and texturecolor objects.  That makes this
coordinate/state gate the evidence-backed next feature, rather than a
speculative shader extension.

## Concrete command-15 patch and transition map

Command 15 has never been merged or advertised, so this is a correction to a
reservation, not an ABI break.  There is no deployed userspace transition to
support.  Any experimental kernel must keep cap31 clear until it implements
the corrected field.  A zero-filled old experimental request is naturally
program 0, but that compatibility accident is not a contract.

UAPI, in both kernel and Mesa copies:

1. rename only the offset-36 `reserved0` member to `program`;
2. add the three semantic enum values above;
3. retain flags, all following offsets, structure size 200, and ioctl value;
4. retain cap31 as the transport capability and require cap25/cap39 only for
   enum 1/2; and
5. keep cap40 orthogonal because it changes coordinate validation, not the
   shader object.

Kernel handler and builder:

1. validate run flags against only DEPTH, BLEND, depth-function, and
   read-only bits;
2. validate `program <= 2`, then capability and texture-1 format gates before
   vertex copying or BO lookup;
3. store a dedicated internal `two_texture_program`, rather than preserving
   semantic bits in `request->flags`;
4. have legacy 0x0a/0x0d/0x13 handlers decode their low-two-bit selector into
   that internal enum and pass only depth state to common depth validation;
5. select PS control/TAU/ALU and fixed body count from one closed table keyed
   by the enum; assert program 0/1/2 counts independently; and
6. make the list chunk bound derive from the chosen fixed body plus B/D
   deltas.  It is 18 for all current level-zero keys, but the calculation must
   fail closed rather than embed that result in every branch.

Mesa transport:

1. lower each accepted NIR classifier to the same internal enum used by the
   per-triangle path;
2. put that enum in command 15 `program`, never in run `flags`;
3. include it in the zero-initialized immutable run key;
4. on a legacy per-triangle fallback translate enum 1/2 back to the existing
   low flag bit only after its semantic capability gate; and
5. split/flush the run on any program, wrap, filter, view, depth, blend, or
   resource change.

Offline tests must assert size/offset/ioctl constants, reject program 3 and
unknown flags independently, and byte-compare a one-triangle run body against
the corresponding legacy body for all three programs.  Boundary tests use 18
then 19 triangles for every B/D/program key and verify PAUSE, STOP, read-ahead,
first/last vertex, and the complete selected PS object.

## Boundary after runtime program 24

The live semantic trace labels the newly closed BGRA packed-lightall family
as program 24 in current Mesa.  Across the recorded 1,304 post-load binds,
the only formerly unsupported pointers were the two byte-semantic-identical
lightall objects.  After those binds the workload returns to supported packed
generic, constant-color, and texturecolor families; there is no evidence for
an immediate program-25 shader.

The ranked next boundaries are therefore state/transport:

1. **Mip minification.**  The diagnostic capture used `GL_LINEAR`, but the
   retained normal q3 run requests `GL_LINEAR_MIPMAP_NEAREST`.  Signed repeat
   and nearest mip selection are consecutive requirements.  The exact A30
   `30000040`, A31 `31102491`, guarded L1 table, and body delta are in
   [signed-wrap-mipmap-nearest-contract.md](signed-wrap-mipmap-nearest-contract.md).
   This is the likely first correctness rejection after cap40/program24.
2. **Run batching.**  Even when state is supported, q3dm1 contains 5,308
   explicit lightmapped triangles.  Command 15 removes syscall/map/fence
   repetition but adds no rendering semantics; its corrected program enum is
   prerequisite to batching program24 safely.
3. **Fresh runtime state capture.**  Depth EQUAL/read-only, extended blend,
   alpha test, and two-texture D16+blend already have closed successors.  A
   post-cap40 rerun should log the first draw-state rejection and sampler
   levels before adding another shader key.  Fog, dynamic lights, extra
   samplers, or arbitrary ALU are not justified by the existing bind trace.
