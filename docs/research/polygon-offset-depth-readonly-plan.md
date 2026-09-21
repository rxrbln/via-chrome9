# Chrome9 HD D16 read-only and polygon-offset contract

This note defines the next bounded depth-state extension after the proven D16
and D16-RHW paths.  Read-only and native A16--A18 polygon offset remain a
static implementation plan, not a claim that those new hardware states have
run.  Mesa draw already has a software vertex-Z offset path, distinguished
below from the proposed native path.  The existing LESS/LEQUAL write-enabled
ABI and packets must remain unchanged until each new-state oracle passes.

The most important result is that depth read-only and polygon offset are
orthogonal:

- read-only changes only type-01 A01 bit 2, from the established test/write
  word to the corresponding test/no-write word;
- the depth surface, format, comparison, clear-data/mantissa state, and pixel-Z
  clip words do not change;
- polygon offset changes A16--A18 only.  It is not encoded in A14/A15.

## Independent evidence

The Windows files used here are
`VTGUModeDX32A.dll` SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
and `VTGOGL32A.dll` SHA-256
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`
from the VT3410 Windows 7 package.

The *VX900 Series Chrome9 HD Open Graphics Programming Manual*, preliminary
revision 1.0, pages 91--94, defines A01 bit 3 as Z-test enable, A01 bit 2 as
Z-write enable, and A10--A18 as the Z surface, compare, clear/mantissa, and
bias words described below.

The Linux proprietary driver independently supplies executable evidence:

- `via_chrome9_dri.so:STM_ZWBEnable_INV` at `0x195b90`, especially
  `0x195e40..0x195e7f`, conditionally sets or clears A01 bit 2 and then sets
  bit 3.  The disabled/unbound path at `0x195bd7` clears both bits with byte
  mask `f3`.
- `STM_ZFunc_INV:0x192cf0` replaces A13 bits 18:16 with the public comparison
  enum minus one.
- `STM_DepthBias_INV:0x1959d0` copies context `+0x1de4` bit-for-bit as the
  scale float, converts context `+0x1de8` units to the offset float, and
  splits those two IEEE-754 words across A16--A18 at
  `0x195ad9..0x195b18`.  The inactive path at `0x195a5e..0x195a70` zeros all
  payload bits of A16--A18.

The Windows D3D builder at `VTGUModeDX32A.dll:0x101fa910` and Z-function
builder at `0x101faa10` perform the same enable/write and comparison updates.
The exact write-mask branch is `0x101fa94f..0x101fa966`: a true context
`+0x1d44` ORs A01 bits 2 and 3, while false clears bit 2 and still ORs bit 3.
The disabled/unbound branch at `0x101fa96c` masks A01 with `ffffffe3`.

The byte-matching Windows OpenGL routine begins at
`VTGOGL32A.dll:0x1024e5d0`, not `0x1024e6d0`.  Its write test is at
`0x1024e60f`; `0x1024e618/0x1024e61c` set bits 2/3 and
`0x1024e622/0x1024e626` clear bit 2/set bit 3.  This is an independent
relocated build of the D3D transition, not a manual-only inference.

The Windows D3D bias builder is
`VTGUModeDX32A.dll:0x101fbde0..0x101fc058`; the independently relocated
OpenGL copy is `VTGOGL32A.dll:0x1024f790..0x1024fa08`.  In the D3D public
state dispatcher, `D3DRS_DEPTHBIAS` reaches `0x100ca453`, stores the exact
float bits at context `+0x1e64`, and dirties class `0x1e`.
`D3DRS_SLOPESCALEDEPTHBIAS` reaches `0x100ca477`, stores the exact bits at
`+0x1e68`, and dirties class `0x4a`.  The final builder splits the resulting
binary32 words into its hardware image at `+0x58/+0x5c/+0x5e/+0x60` at
`0x101fbfd7..0x101fc002`; the OpenGL copy does the same at
`0x1024f987..0x1024f9b2`.

Old Mesa 7.11.2 `via_state.c:viaChooseDepthState` is policy evidence only,
but agrees that a disabled depth test also disables depth writes.  Its older
UniChrome rasterizer does not program this Chrome9 state: `_NEW_TRI_OFFSET`
selects the software triangle template in `via_tris.c`, and
`tnl_dd/t_dd_tritmp.h` adds the computed offset to each vertex Z before
emission.  That is useful independent evidence for the required state-owner
rule below, not evidence that the older register layout matches Chrome9.

## Exact register meanings and deltas

For the established linear SF D16 path, the complete relevant words are:

```text
A01 affine, test/write       01c0154c
A01 affine, test/read-only   01c01548
A01 RHW, test/write          01ce154c
A01 RHW, test/read-only      01ce1548

A10                          10000000 | (depth_sf_va >> 8)
A11                          11000001 | ((depth_pitch >> 5) << 13)
A12                          12380000
A13                          13000000 | (depth_func << 16)
A16/A17/A18, no offset       16000000 17000000 18000000
A19/A1A/A1B                  19800000 1a800000 1b803fbf
A1E/A1F/A20                  1e000000 1f000000 20007fff
```

The manual gives these exact field interpretations:

| word | fields used by the bounded D16 path |
|---|---|
| A10 | depth SF virtual address in 256-byte units |
| A11 | bits 22:13 pitch in 32-byte units; bits 12/11 reading-Z cache enable/clear; bit 10 cache width; bit 9 direct write-through; bits 1:0 location (`01` = SF) |
| A12 | bit 23 selects W instead of Z; bits 21/20/19 force 1.0 to 1.0-, clamp above 1.0, and clamp negative to zero; bits 18:16 format (`000` = D16); bit 0 tiled mode |
| A13 | bits 18:16 comparison; bits 7:0 power-of-two Z normalization factor |
| A20 | high bytes of the floating far and near per-pixel Z clip values |

Thus `11000001 | ((pitch >> 5) << 13)` deliberately disables both
reading-Z cache controls and direct write-through, selects the existing
128-bit/default cache mode, and places the linear surface in SF.  Likewise,
`12380000` is D16 linear Z, forces/clamps the fixed-point endpoints, and does
not enable separated stencil.  These words do not change when writes are
disabled or bias is enabled.

`depth_func` is the native three-bit value:

```text
0 NEVER    1 LESS       2 EQUAL      3 LEQUAL
4 GREATER  5 NOTEQUAL   6 GEQUAL     7 ALWAYS
```

The delta table is deliberately explicit about fields that do **not** move:

| transition | A01 | A10 | A12 | A13 | A14 | A15 | A20 |
|---|---|---|---|---|---|---|---|
| write to read-only | clear bit 2 | same address | `12380000` | same function | unchanged | unchanged | `20007fff` |
| read-only to write | set bit 2 | same address | `12380000` | same function | unchanged | unchanged | `20007fff` |
| LESS to EQUAL | unchanged | same address | `12380000` | `13010000` to `13020000` | unchanged | unchanged | `20007fff` |
| zero to nonzero offset | unchanged | same address | `12380000` | unchanged | unchanged | unchanged | `20007fff` |

For affine or RHW packets, the read-only delta is therefore exactly `-4` in
A01.  A10 and A12 continue to bind the same D16 resource and format because a
read-only test still reads the depth surface.  A13 remains live because the
test still runs.  A20 must remain `20007fff`; reset value `20000000` collapses
the pre-depth-test pixel-Z interval to zero and can reject ordinary nonzero Z
before A13.

### A20/A21/A22 are header-qualified namespaces

The register byte alone is not a global register identity.  Three packet
headers reuse `A20`, and confusing them would corrupt texture dimensions or
primitive assembly:

| packet namespace | A20 | A21 | A22 |
|---|---|---|---|
| type 01 common/depth | far/near clip high bytes, proven `20007fff` | reserved/omitted | reserved/omitted |
| type 02 texture stage N | `(height << 12) | width` | level-0 length, current `21000001` | layout/POT/log-size; clear POT/log fields for NPOT |
| type 04 raster/FVF | primitive vertex-cycle/index mode, current `20000000` | reserved | reserved |

The type-01 manual jumps directly from depth A20 to stencil A23; there is no
depth A21 or A22.  A bias successor must therefore stop its depth closure at
type-01 A20.  It must leave each type-02 stage's A20/A21/A22 exactly as
lowered from that texture, including the independently proven NPOT A22
clearing rule, and leave type-04 A20 at the selected primitive mode.

### A14/A15 are not depth bias

The manual resolves an important ambiguity:

- A14 bits 23:0 are the low three bytes of Z/W clear data;
- A15 bits 7:0 are the highest byte of Z/W clear data;
- A15 bits 23:8 are the negative mask applied to the low 16 mantissa bits of
  rendered Z before format conversion, comparison, and writeback.

Thus the shorthand that A14/A15 are only clear data is incomplete.  A15's
high half can alter ordinary rendered depth even when no fast clear occurs.
Neither read-only nor polygon offset requires an A14/A15 update.  The first
successor must not introduce writes to these registers: retain the proven
context image, keep fast-Z clear A27 bit 0 disabled, and vary only the words
under test.  If a later state-complete context image explicitly closes them,
`14000000/15000000` is the manual-neutral clear value and zero mantissa mask,
but that is a separate guarded delta rather than a prerequisite for this ABI.

## Polygon-offset encoding

The hardware equation is:

```text
Zbias = max(dZ/dX, dZ/dY) * scale + offset
```

The exact split of the two IEEE-754 binary32 words is:

```text
A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000)
                  | (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

For the address-free first discriminator, use scale `+0.0` and offset `-0.25`
(`be800000`):

```text
16800000 170000be 18000000
```

Closing the feature is exactly:

```text
16000000 17000000 18000000
```

A19--A1B remain the proprietary initialization defaults shown above.  They
reconstruct -1.0/+1.0 clamp bounds and enable enhanced Z precision; they are
context prerequisites, not part of each bias toggle.

The reconstruction is bit-exact: A19 payload `800000` plus A1B low byte `bf`
forms low boundary `bf800000` (-1.0), while A1A payload `800000` plus A1B
bits 15:8 `3f` forms high boundary `3f800000` (+1.0); A1B bit 23 enables the
enhanced-precision path.  Likewise, A1E `000000` plus A20 bits 15:8 `7f`
forms far clip `7f000000`, and A1F `000000` plus A20 bits 7:0 `ff` forms near
clip `ff000000`.  Thus `20007fff` is a deliberately broad signed pixel-Z
interval, not the [0,1] fixed-format clamp (that clamp is controlled by A12).

The public polygon `factor` becomes `scale` without conversion.  Public
`units` does not become `offset` directly: the Linux builder multiplies it by
`0x38000080` when bound-resource field `+0x158` is `0x10`, and by
`0x34000001` otherwise.  The Windows D3D builder independently tests the
bound resource's `+0x124` against `0x10` at `0x101fbe19..0x101fbe3a` and
selects doubles at `0x1005f760/0x1005f768`.  Rounded to binary32, those are
exactly `0x38000080` (`3.051804378628731e-5`) and `0x34000001`
(`1.1920930376163597e-7`).  OpenGL repeats the test and multiplication at
`0x1024f7c9..0x1024f7ea`.  `sfmGetPatchColorKey:0xb6afc..0xb6b52`
independently proves that field is bits per pixel: values 8, 16, and 32 select
byte, word, and dword loads from the surface.  Therefore D16 uses
`0x38000080` (approximately `3.0518044e-5`), while the 32-bpp D24S8, D24X8,
and D32 family uses `0x34000001` (approximately `1.1920930e-7`).

The current Mesa 26.2 Chrome9 path already has a conservative software
lowering.  `chrome9_bind_rasterizer_state()` passes the rasterizer to draw;
`draw_pipe_validate.c` selects the offset stage for `offset_tri`, and
`draw_pipe_offset.c` computes

```text
max(abs(dZ/dX), abs(dZ/dY)) * offset_scale + offset_units * MRD * 2
```

for a fixed-point depth buffer, then adds it to all three window-space vertex
Z values.  `chrome9_set_framebuffer_state()` calls `draw_set_zs_format()`, so
Z16 supplies the correct MRD.  `MRD * 2` for Z16 is precisely the proprietary
`0x38000080` scale above.  Mesa 7.11.2 used the same style of software
triangle lowering in `tnl_dd/t_dd_tritmp.h`.

This gives a usable decal fallback today for unclamped filled triangles: keep
hardware A16--A18 neutral and let draw own polygon offset.  It is exact for a
planar triangle as long as the adjusted vertices do not hit the software
`SATURATE` endpoints; it remains an approximation to the manual's stated
per-fragment placement in the general clipped/clamped case.  A future
hardware-bias path must pass a copy with all three `offset_*` enables cleared
to draw while retaining the original state for A16--A18 lowering.  Leaving
`offset_tri` enabled in draw and also programming hardware bias applies the
offset twice and is a hard pre-submit error, not an acceptable approximation.

Point, line, and fill enables are separate proprietary context booleans at
`+0x3c40`, `+0x3c44`, and `+0x3c48`, respectively.  This ordering is exact:
`__glEnableDisable:0x28d798..0x28d84b` maps GL enums
`GL_POLYGON_OFFSET_POINT` (`2a01`) and `GL_POLYGON_OFFSET_LINE` (`2a02`) to
public bytes `+0x60e5/+0x60e6`, while deferred fill state is copied through
`+0x60e7` at `__glUpdateDeferedAttributes:0x28f958..0x28f984`.
`__glDpSTMUpdatePolygonOffset:0x1ecb44..0x1ecb65` copies those bytes in order
to the three hardware-context booleans.  Finally,
`STM_DepthBias_INV:0x195a26..0x195ad9` selects point/line/fill only when
current primitive mode `+0x1dfc` is 1/2/3, otherwise it writes neutral
A16--A18.  The first implementation is mode 3 triangle-fill only; accepting a
point or line enable without the corresponding primitive path is incorrect.

## Bounded ABI plan

Do not reinterpret the original D16 ioctls silently.  Existing unknown flag
validation provides a useful fail-closed boundary, and old userspace expects
zero comparison bits to mean legacy LESS even though native comparison zero
means NEVER.

The smallest independently shippable extension is a capability-gated
`DEPTH_READ_ONLY` flag on the existing structured D16 successors:

```text
CAP_D16_DEPTH_READ_ONLY present + flag clear -> established write path
CAP_D16_DEPTH_READ_ONLY present + flag set   -> clear A01 bit 2
cap absent or non-D16 ioctl                  -> reject flag with EINVAL
```

This adds no fields and changes no old packet.  It must be combined only with
an accepted explicit depth function; the current implementation may continue
to limit that set to LESS and LEQUAL until the eight-function oracle passes.
Depth-test disable should continue through the color-only draw path.  Do not
encode write-on/test-off, because both proprietary implementations suppress
writes when the API depth test is disabled.

### Read-only code-level integration map

At the current UAPI snapshot, flags 0/1 belong to per-ioctl batch/blend state
and bits 8--10 hold the depth function.  Bit 11 is free in every D16 request,
so the concrete non-colliding definition is:

```text
DRM_CHROME9_DEPTH_READ_ONLY = 1U << 11
DRM_CHROME9_CAP_DEPTH_READ_ONLY = 1ULL << 23
```

Capability bit 23 is reserved in the canonical
[UAPI allocation ledger](chrome9-uapi-allocation.md); bits 21 and 22 belong to
the mip and RHW-run successors.  The request flag bit remains 11 once
published.  Capability absence keeps the existing unknown-bit rejection and
guarantees old kernels fail with EINVAL rather than execute a write-enabled
approximation.

One helper should derive the A01 word so emission and assertions cannot
diverge:

```text
affine base       01c01540
perspective base  01ce1540
+ test            00000008
+ write unless READ_ONLY 00000004
```

The resulting exact words remain `01c0154c/01c01548` and
`01ce154c/01ce1548`.  Apply that helper at every structured D16 emission site:

| builder/path | current A01 location | perspective selector |
|---|---:|---|
| untextured D16, blend, batch, RHW batch | `body[19 + delta]` | RHW-batch kind |
| one-texture D16 | `common[4]`, asserted at `body[44 + delta]` | MODULATE program |
| two-texture D16 | `common[4]`, asserted at `body[64]` | retain its established affine word |

Do not alter the fixed depth oracle's hard-coded write-enabled word; it is a
regression reference, not a general UAPI path.  No memmove, body-length, A10,
A12, A13, A14, A15, or A20 assertion changes are needed for read-only.

The capability is all-or-nothing across these six public entry paths:

| ioctl/path | validator/emitter requirement |
|---|---|
| `DRAW_TRIANGLE_D16` | allow bit 11 through `chrome9_validate_depth_flags()`, lower A01 |
| `DRAW_TRIANGLE_D16_BLEND` | same; blend flags remain independent |
| `DRAW_TRIANGLE_BATCH` with `TRIANGLE_BATCH_DEPTH` | allow bit 11 only when the depth bit and BO are present; copy it into the retained batch flags |
| `DRAW_TRIANGLE_D16_RHW_BATCH` | require the depth bit as today; lower perspective A01 |
| `DRAW_TEXTURED_TRIANGLE_D16` | preserve the explicit nonzero comparison requirement and `SOURCE_OVER`; lower A01 in the common group |
| `DRAW_TWO_TEXTURED_TRIANGLE_D16` | preserve the explicit nonzero comparison requirement; lower A01 in the common group |

`chrome9_validate_depth_flags()` is the one common unknown-bit gate, but the
generic batch needs the additional `READ_ONLY && !TRIANGLE_BATCH_DEPTH`
rejection beside its existing function-without-depth check.  Mesa must OR bit
11 into both immediate argument structs and the persistent `crender->batch`
or `batch_rhw` template before triangles accumulate; adding it only to the
non-batched draw loses the state on the highest-volume path.

Kernel validation must accept bit 11 only on a D16 request.  In the generic
batch ABI, reject `READ_ONLY` when `TRIANGLE_BATCH_DEPTH` is clear, exactly as
for a depth-function field without a depth BO.  If the capability is global,
all advertised D16 ioctl variants must implement it before the cap is set;
otherwise textured or two-textured draws could silently lose an API write
mask transition.  Initially retain the depth BO's existing write reservation,
cache ownership, and post-retirement synchronization even for read-only
requests.  A later read-only reservation optimization is not worth weakening
serialization during the first gate, and the full-payload immutability oracle
below depends on a coherent CPU readback.

Be careful with the current submitter's role-based object aliases.  For an
untextured draw, `job->depth` is the D16 BO and receives a WRITE fence.  For a
one-texture D16 draw, `job->depth` is actually the sampled texture and receives
a READ fence, while `job->texture2` is the D16 BO and receives WRITE.  For a
two-texture D16 draw, `job->depth`/`job->texture2` are texture 0/1 with READ
fences and `job->texture3` is D16 with WRITE.  The first read-only
implementation intentionally leaves all of those fence uses unchanged.  Any
later optimization must identify the semantic D16 role by draw kind; testing
the C member name would downgrade the wrong object.

Mesa needs one cached capability boolean.  Its draw-state gate changes from
requiring `depth_writemask` to requiring either the writemask or that cap, and
sets bit 11 in every selected D16 argument: plain/blend, ordinary/RHW batch,
one-texture, and two-texture.  It never sets the bit on the color-only path.
The state object already retains `pipe_depth_stencil_alpha_state`, so no CSO
layout change is required.  This is the immediate ioquake decal prerequisite:
a typical decal pass needs depth testing with depth writes off, whereas its
polygon offset can continue through draw's bounded software stage until the
native bias path is proved.

The patch-ready capability plumbing is deliberately small:

1. add bit 11/cap 23 to both kernel UAPI and Mesa's synchronized UAPI copy;
2. accept bit 11 in `chrome9_validate_depth_flags()`, add the no-depth batch
   rejection, and include cap 23 in `chrome9_get_param_ioctl()` only in the
   final enabling commit;
3. cache cap 23 in `chrome9_context` during `chrome9_context_create()`;
4. in `chrome9_draw_state_supported()`, permit a false writemask only with
   that cached cap;
5. in `chrome9_emit_triangle()`, derive one semantic depth flag word and copy
   it into `depth_args`, `depth_blend_args`, `texture_depth_args`,
   `two_texture_depth_args`, and the initial `crender->batch`/`batch_rhw`;
6. lower that same request bit in all three kernel packet builders, then run
   old write-enabled and new read-only smokes before advertising the cap.

This ordering lets mismatched Mesa/kernel combinations fail closed: new Mesa
does not set the bit without cap 23, while old kernels reject the unknown bit.

Hardware polygon offset needs two binary32 values and should use a new
structured successor rather than consume or rename reserved fields in an old
ioctl.  The original untextured D16 request has only one 32-bit reserved word,
while other D16 requests have differing spare layouts; partially repurposing
them would make one Gallium rasterizer state mean different things by shader
path.  A minimal common depth-state record is:

```text
u32 flags;               /* TEST, WRITE, BIAS_FILL; all others rejected */
u32 func;                /* native 0..7, explicit even for NEVER */
u32 bias_offset_float;   /* exact IEEE-754 binary32 */
u32 bias_scale_float;    /* exact IEEE-754 binary32 */
```

The first advertised version should be narrower than the record permits:

1. require TEST and a retained, distinct guarded D16 BO;
2. accept WRITE independently; reject write without test;
3. accept only triangle fill, linear D16, and the already proven destination
   formats/layouts;
4. when BIAS_FILL is clear, require both bias words to be positive zero and
   emit neutral A16--A18 on every draw;
5. in the first diagnostic capability, require `scale == +0.0` and allow only
   `offset == +0.0` or `offset == -0.25`; reject NaN, infinity, subnormal,
   negative zero, and every other bit pattern;
6. after the raw oracle passes, a production D16-only capability may accept
   finite values in a deliberately small range.  Mesa can lower GL `units`
   with the proved D16 multiplier `0x38000080`; because kernel code cannot use
   floating point, userspace should pass the resulting finite binary32 offset
   and the kernel should validate its bits and bounds;
7. kernel generation owns all packet words.  Userspace never supplies A01 or
   A10--A20 directly;
8. hardware bias and draw software bias are mutually exclusive for a draw.
   Assert that the draw-facing rasterizer copy has `offset_point`,
   `offset_line`, and `offset_tri` clear before setting `BIAS_FILL`.

This split permits read-only depth to land without coupling it to an
unmeasured units conversion.  The full successor should preserve the current
128-byte batch envelope only if a new layout can do so naturally; ABI safety
is more important than reusing old `reserved[]` bytes.  A new ioctl number and
capability leave all existing structure sizes and semantics untouched.

### Polygon-offset packet-growth map

Unlike read-only, a bias/clip-closed successor grows the type-01 group by
exactly eight dwords.  A shared helper should generate this ordered 13-word
sequence and all builders should assert it rather than carry independent
literal lists:

```text
A10 A11 A12 A13
A16 A17 A18 A19 A1A A1B
A1E A1F A20
```

A14/A15 remain absent and inherit the established initialized context for the
first gate; adding their nominal zero values would combine an unmeasured
mantissa-mask transition with bias.  The helper emits neutral A16--A18 and the
same A19--A1B/A1E--A20 closure even when offset is disabled, so bias cannot
leak into a later COPY or texture draw.

That last property is global, not local to the new ioctl.  A16--A18 persist in
the 3D context and every current legacy D16 builder omits them.  Therefore a
production kernel must use the expanded helper for **every** D16 draw that can
follow a biased draw: plain, blend, ordinary batch, RHW batch, one texture,
and two texture.  The fixed depth diagnostic must also establish neutral bias
before its primitive if it remains callable after userspace submissions.  It
is sufficient to close at the start of each D16 draw; color-only draws may
leave the dormant value because the next D16 draw closes it before vertices.

Do not advertise native bias if only the bias-enabled successor emits
A16--A18.  A one-shot raw oracle may do that only on an isolated boot with no
later 3D work and immediate successful unload/reboot; a timeout still invokes
the normal quarantine rule.  For production, changing the internal packets of
old D16 ioctls to emit neutral state does not change their UAPI semantics, but
it does require all old-path regression oracles and exact-length assertions to
pass before the capability bit is exposed.

For the current untextured D16 bodies the exact layout becomes:

```text
                   no blend             blend (delta = 4)
A01                19                   23
A10..A13           22..25               26..29
A16..A1B           26..31               30..35
A1E..A20           32..34               36..38
A33/padding        35                   39
next header        36                   40
type-04 A20        60                   64
type-04 A23/A24    61/62                65/66
clip A80..A83      112..115             116..119
vertex header      172                  176
first vertex       176                  180
```

The untextured body formula changes from `170 + 18*T` to `178 + 18*T`
without blend, and from `174 + 18*T` to `182 + 18*T` with blend.  One triangle
is therefore 196/200 dwords.  Keep the proven maximum of 34 triangles; do not
spend the remaining staging-page margin by increasing it, and retain a final
pointer-versus-page bound assertion before appending sync/status/cookie.

For one texture, let `d = 4` only when the existing blend destination closure
is present.  The current texture-stage words remain at body
`[22+d]/[23+d]/[24+d]` for type-02 A20/A21/A22.  Only the later type-01 common
group grows:

```text
A01                 body[44+d]
A10..A13            body[47+d]..body[50+d]
A16..A1B            body[51+d]..body[56+d]
A1E..A20 depth      body[57+d]..body[59+d]
type-04 A20         body[84+d]
```

The resulting D16 body lengths are 236 dwords for non-blend REPLACE, 240 for
non-blend MODULATE, 240 for blend REPLACE, and 244 for blend MODULATE.  The
REPLACE/MODULATE distinction still selects affine/perspective A01 exactly as
before; it does not change the bias representation.

For two textures, the two independent type-02 A20/A21/A22 triplets remain at
body `[22..24]` and `[42..44]`.  The type-01 depth and later raster state
become:

```text
A01                 body[64]
A10..A13            body[67]..body[70]
A16..A1B            body[71]..body[76]
A1E..A20 depth      body[77]..body[79]
type-04 A20         body[101]
```

The two-texture D16 body grows from 266 to 274 dwords.  Both texture A22 words
continue to carry their own POT/log-size decision; neither depth read-only nor
polygon offset may touch them.  This separation is the key assertion for an
NPOT two-texture decal gate.

## Live oracles

Every case uses separate guarded color and D16 BOs, canonical pitches,
CPU-to-device ownership transfer before submission, bounded synchronous
retirement, post-retirement CPU sync, guard verification, and an engine-error
check.  Run each as one isolated state delta after the established D16-RHW
suite.  On timeout or nonzero engine error, quarantine 3D until reboot; do not
attempt an in-place retry with another state.

### 1. Near/equal/far comparison matrix

CPU-fill depth with `8000` and color with black.  With writes disabled, draw
three non-overlapping triangles at normalized 16.16 Z `4000`, `8000`, and
`c000`.  These are respectively near, equal, and far relative to stored
`8000`.  The exact expected passing triangles are:

| function | expected |
|---|---|
| NEVER | none |
| LESS | near |
| EQUAL | equal |
| LEQUAL | near, equal |
| GREATER | far |
| NOTEQUAL | near, far |
| GEQUAL | equal, far |
| ALWAYS | near, equal, far |

After every draw, all depth samples must still be `8000`.  EQUAL and NOTEQUAL
are useful operand-order checks; NEVER and ALWAYS expose accidental function
aliasing.

The write-enabled LEQUAL subset is **Measured** on live VX900.  With depth
cleared to `8000`, the Gallium oracle observed equal blue as
`000000ff/8000`, retained it for the farther draw as `000000ff/8000`, and
accepted near red as `00ff0000/4000`.  Kernel commit
`8a16b931bacdaa292d81ea30c8c99b7a369b2cad` and Mesa Chrome9 commit
`b607b9ca59588dad3787d860e84bcacbceeba6fb` completed with engine-error MMIO
`040c=0`.  This proves native A13 `13030000` and its existing write path; it
does not prove the A01 read-only delta or any other compare row.

### 2. Independent read/write discriminator

Use one overlapping interior sample and clear depth to `8000`:

1. red at `4000`, LESS/read-only: color becomes red, depth stays `8000`;
2. green at `6000`, LESS/write: color becomes green, depth becomes `6000`;
3. blue at `7000`, LESS/write: it is farther than stored `6000`, so both
   color and depth remain green/`6000`.

This proves that read-only does not make the test inert, does not secretly
write depth, and does not prevent a later write-enabled transition.

### 3. Coplanar constant-offset discriminator

Clear depth to `ffff` and color to black.  Keep A14/A15 unchanged and fast-Z
clear disabled throughout.

1. Draw red at Z=0.5 (`8000`) with LESS/write and neutral A16--A18.  It must
   pass and establish the stored plane.
2. Draw the identical green triangle at the same Z with LESS/write and neutral
   A16--A18.  It must fail because equality does not pass LESS.
3. Change only A16--A18 to the exact `-0.25`, scale-zero words and draw green
   again.  It must replace red, and sampled D16 must move toward near (below
   `8000`; the expected ideal is approximately `4000`).
4. Restore neutral A16--A18 and draw an identical blue triangle at Z=0.5.  It
   must fail against the biased stored value, proving that the bias state was
   closed rather than inherited.

Use Z=0.5 rather than zero: A12 bit 19 clamps negative post-bias values to
zero, which would make a Z=0 case a poor discriminator.  Before treating an
exact `4000` result as ABI truth, record the actual D16 readback and allow a
tight one-ulp implementation tolerance for the manual's fixed-format scaling
rule.

After that large raw-offset gate, validate the production D16 units lowering
with factor zero and `units = -1`.  Its exact post-format offset bits are
`b8000080`, the sign-flipped D16 multiplier.  Against stored `8000`, an equal
incoming plane should move approximately two D16 steps toward near, pass
LESS, and store `7ffe` (allow one adjacent code until rounding is measured).
The `units = +1` control uses `38000080`, moves toward far, and must fail LESS
against `8000`.  This small-offset pair distinguishes the proved multiplier
from an accidental direct use of integer units without relying on a large
quarter-range displacement.

### 4. Combined read-only bias control

Only after the three isolated gates pass, repeat the coplanar green draw with
negative offset and read-only state.  Green must win the color comparison,
while the stored red depth remains exactly `8000`.  Restore neutral bias and
draw blue at Z=0.5 with EQUAL/read-only; blue must then pass.  This detects an
implementation that incorrectly applies bias to stored depth or couples the
write mask to the compare input.

### 5. Textured namespace and transition matrix

Repeat the independent read-only and coplanar tests through all shader packet
families that bind D16.  Use textures with unequal RGB channels so a stale
solid-color result cannot pass accidentally:

| path | required cases | additional invariant |
|---|---|---|
| one texture REPLACE | write, read-only, negative bias | type-02 A20/A21/A22 and sampled texel unchanged |
| one texture MODULATE/RHW | write, read-only, negative bias | varying RHW result unchanged except depth acceptance |
| two texture MODULATE | write, read-only, negative bias | both sampled inputs contribute; both texture state triplets unchanged |
| untextured RHW | write, read-only, negative bias | perspective A01 changes only at bit 2 for read-only |

For the two-texture row use distinct NPOT dimensions, for example the proven
30x18 and 22x14 pair.  Assert type-02 words at body `[22..24]` and `[42..44]`
before submission and four texture/target guards afterward.  A depth helper
that accidentally writes type-01 values into either texture namespace should
therefore fail in validation rather than reach hardware.

Run the sequence negative-bias one-texture -> neutral two-texture -> neutral
untextured without reinitializing 3D.  The latter two draws must behave as
unbiased.  Then reverse the first transition: read-only textured ->
write-enabled textured, and verify that only the latter changes the complete
D16 payload.  Compare the entire depth payload byte-for-byte for read-only
cases, not just the four guards.

Finally render the same unclamped coplanar triangles twice, once with Mesa's
existing software offset and neutral A16--A18 and once with draw offset
disabled plus hardware A16--A18.  Factor zero/units +/-1 and units zero with a
nonzero-slope factor must agree within the measured D16 rounding tolerance.
Any result consistent with the sum of both offsets is an ownership failure and
must disable the hardware-bias capability.

## Mesa exposure gate

Until live hardware bias validation completes, retain draw's software polygon
offset and emit neutral A16--A18.  Do not silently ignore `offset_tri`, but do
not claim a native bias fast path either.  Reject `offset_point`,
`offset_line`, nonzero `offset_clamp`, floating-point depth, or a depth format
other than Z16 from the bounded hardware path; those states remain with draw
or fall back.

After the read-only gate passes, Mesa may lower
`pipe_depth_state.writemask == false` only when the kernel advertises the new
capability; otherwise reject/fallback before submission.  After the raw bias
gate and software-versus-hardware comparison pass, the statically proved D16
`units` multiplier permits switching filled Z16 triangles to hardware bias.
Make that switch per draw, clear draw's offset enables, and keep every other
case on the software stage.  A diagnostic raw-offset ioctl is evidence
tooling, not by itself truthful general GL polygon-offset support.
