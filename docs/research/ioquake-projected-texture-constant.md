# Projected texture and current-color constants in ioquake3 GL1

Status: live NIR capture plus static packet/compiler audit, 2026-09-21. This
note defines implementation keys and guarded tests. It does not promote
arbitrary projective coordinates, fragment constants, or native shaders.

## Live programs and source provenance

The VX900 ioquake3 run captured two adjacent fixed-function fragment programs
in both of these logs:

```text
/root/tmp/chrome9/ioquake3-gl1-projected-copy-v1.log
sha256 e0894b03ee4b0f34c2b896c40ea1fd66c3cc209866e0a571257e3bd44930bd22

/root/tmp/chrome9/ioquake3-gl1-projected-modulate-v1.log
sha256 0c82cb7df8350000aaae567ba4915881bd0b7f254f3ad5a81c520c8c8821c2d2
```

Both use one smooth `vec4 TEX0`, take `TEX0.xy` as the coordinate and
`TEX0.w` as the projector, and load one complete `vec4` at uniform offset
zero from `state.currentAttribMaybeVPClamped[2]`. The first result is:

```text
t = tex(unit=0, coord=TEX0.xy, projector=TEX0.w)
c = load_uniform(base=0, offset=0, vec4)
out = t * c
```

The second has deliberately different alpha semantics:

```text
p = t * c
out = vec4(p.r, p.g, p.b, c.a)
```

Call the second form **constant-alpha bypass**, not
`texture*c0.aaaa`. The latter replicates `c.a` into all four multiplier
lanes and computes `t * c.aaaa`; it is already a separate Chrome9 key. The
three expressions coincide only in special cases.

The source of the uniform is also bounded. In ioquake3 revision
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`, ordinary shader stages call
`ComputeColors()` in `code/renderergl1/tr_shade.c`. `CGEN_CONST` copies the
four constant bytes into every `tess.svars.colors` element, `AGEN_CONST`
replaces its alpha byte, and `RB_IterateStagesGeneric()` submits that storage
through `glColorPointer(..., GL_UNSIGNED_BYTE, ...)`. Those values therefore
arrive as constant-valued primary `COL0`, not fragment c0.

The captured uniform form is generated when the color array is disabled and
the compatibility current color is used. Concrete ioquake paths include the
outer skybox (`tr_sky.c`, `glColor3f(identityLight,...)`), cinematic 2-D
draws (`tr_backend.c`), and the optional `REPLACE_MODE` optimized lightmap
path. `glColor3f` also implies current alpha one. This distinction prevents
misclassifying every `rgbGen const` shader stage as a fragment-constant draw.

Legacy UniChrome independently keeps these concepts separate.
`via_texcombine.c` maps `GL_CONSTANT` to the texture environment constant,
`GL_PRIMARY_COLOR` to diffuse, and stage-zero `GL_PREVIOUS` to diffuse. It
selects RGB and alpha operands separately, including `GL_SRC_ALPHA`
replication. That is semantic corroboration, not a Chrome9 packet oracle.

## Exact semantic keys

Admit the following keys independently. Identity MOV/FSAT wrappers may be
accepted only where the existing closed classifier already accepts them.

### PTC0: projected texture times full c0

Require exactly one non-array, non-shadow 2-D texture instruction on unit
zero, coordinate `TEX0.xy`, projector `TEX0.w`, one full c0 load at offset
zero, one identity four-lane multiply, and one full color store. Reject any
second texture or constant, partial store, discard, derivative, indirect
offset, alternate output, or extra arithmetic.

The required result is:

```text
out.rgba = texture0.rgba * c0.rgba
```

BGRA and XRGB sources are both semantically valid. A BGRA source is needed to
prove that sampled alpha is multiplied rather than bypassed.

### PTC0A: projected texture times replicated constant alpha

This is the projected spelling of the existing
`CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE` key:

```text
out.rgba = texture0.rgba * c0.aaaa
```

Require the same projected sample and full store, but the second multiply
source must replicate lane 3 in all four lanes. BGRA and XRGB sources are
both valid. This key was not the second ioquake program in the capture, but
the Windows and Linux compiler corpus closes its native ALU mapping.

### PTC0_KEEP_A: projected RGB product with constant alpha

Match the captured vec4 construction exactly: RGB lanes 0--2 come from one
common PTC0 product and alpha comes directly from the same c0 lane 3.

The existing full MODULATE program is an exact lowering only when the source
view is XRGB/BGRX, because format 98 forces sampled alpha to one:

```text
texture0.a = 1  =>  (texture0*c0).a = c0.a
```

Reject BGRA for this alias. This restriction matters even with an XRGB render
target because source alpha is consumed by source-over blending before the
destination alpha channel is discarded. A general BGRA implementation needs
a distinct partial-output fragment program and is outside this gate.

## Q=1 closure

The structured Chrome9 vertex packet carries only S and T to the sampler. It
does not submit the NIR projector as a native third coordinate. The mapping
is therefore exact only when every post-TNL `TEX0.q` is finite and bitwise
`0x3f800000`. Validate all three vertices after software TNL and before the
ioctl. Q=1 makes the NIR division an identity; it does not by itself validate
c0 precision or alpha semantics.

The existing type-02/FE texture state remains exact and unchanged:

```text
A00 00000011
A01 01002521
A08 08010101
A05 05000000
A0D 0d810000
```

Do not accept any other Q by silently dropping it. Native projective A18 state
and a software `s/q,t/q` lowering require their own clipping, derivative and
LOD proof.

## Two exact Chrome9 implementations

### Existing D1 lowering

The current Mesa constant path does **not** execute a native c0 fragment
instruction. It reads c0 on the CPU, converts each component with
`float_to_ubyte`, packs `AARRGGBB` into every vertex D1, and selects the live
one-texture `texture*v0` MODULATE object:

```text
A90..A93 90018000 91000001 92010002 93000001
TAU       301001b0
MUL       0140b6d0 30040053 00f20005 0000007f
MOV       01c00000 301c0000 00f20105 000d083f
```

`A02=020041d1` is mandatory so D1 is normalized. The measured `...d0`
variant feeds 0..255 values and saturates. Because all three D1 values are
identical, interpolation cannot introduce a spatial gradient. Keep
perspective-correct diffuse enabled as in the proven MODULATE path:

```text
no depth A01 = 01ce1540
D16     A01 = 01ce154c
```

This lowering is exact for c0 components already representable as UNORM8,
including zero and one. It is not a generally exact implementation of a
32-bit fragment uniform: for example, float `0.5` becomes `128/255` before
the texture multiply and can change an 8-bit result by one. Q=1 does not
remove this double quantization. A truthful bounded implementation must do
one of the following:

1. use D1 only after proving every consumed c0 lane equals its reconstructed
   UNORM8 value; or
2. use the native c0 object below.

The ioquake `CGEN_CONST`/`AGEN_CONST` path naturally satisfies the byte-grid
property but arrives as COL0 anyway. Current-color `glColor3f` paths do not
in general satisfy it: ioquake uses values including 0.2 and 0.6. Do not
promote PTC0 merely because the current nonprojected smoke tolerates one LSB.

### Native c0 fragment object

The proprietary Xorg helper `Modulate_H5_via_src_onepix_mask` supplies the
executable scheduled identity-sample/constant multiply operand
`30080053 00f20005`. The Linux configuration-two compiler independently
emits the same semantic source with the closed c0 resource mask. The exact
scheduled full object is:

```text
A90..A93 90018000 91000001 92010002 93000001
A94..A9a 94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       301001b0
MUL       0140b6d0 30080053 00f20005 0000007f
MOV       01c00000 301c0000 00f20105 000d083f
```

The static Xorg evidence is localized: `via_drv.so:0x32670` is the builder,
`0x3272e..0x32795` writes the complete A90--A9a control row, and
`0x32900..0x32b7e` constructs and writes the two ALUs. This is stronger than
splicing the Linux compiler's direct-output instruction into the current
packet.

Upload the four unmodified c0 float dwords under selector `22030000`. The
existing one-texture body already emits this four-dword constant packet, but
currently fills it with zero. Program and constant changes must overwrite all
four lanes before submission; never inherit the driver-owned RGBX c0.

The Windows D3D compiler gives an independent semantic differential. Its
`TEX0_C0` row maps to the Linux executable object above after configuration
two. Its minimal Windows allocation callback aliases sampled temporaries, so
the Windows finalized texture word is not itself a command-stream object.

For replicated constant alpha, use the corpus-closed native first ALU:

```text
MUL tex0*c0.aaaa = 0140b6d0 b0200053 00f0806d 00000820
```

That row is a Linux configuration-two object, not yet the scheduled two-ALU
Xorg packet. Do not obtain it by editing only low swizzle bits: replicated
alpha changes the split high modifier too. Until a scheduled/live native
PTC0A object is qualified, the D1 replicated-alpha lowering is the safer
bounded path, subject to the same UNORM8 precision gate.

The opposite `tex0.aaaa*c0` expression is unrelated. The Windows direct
output optimizer loses its sampled-alpha swizzle, and it cannot be lowered
through D1 because the required replicated value is texture-dependent.

## No-depth and D16 packet closure

Both PTC0 implementations fit the existing one-texture MODULATE body. No BO,
sampler, FVF, VS, fence, guard or packet-length change is needed. For the
native c0 form, replace only the first MUL's primary source with c0 and fill
the already-present PS constant payload.

With `B=4` for the closed blend destination block and zero otherwise, and
`D=4` for D16 and zero otherwise, the exact body length is:

```text
228 + B + D dwords
```

Key positions are:

```text
common A01             body[44 + B]       01ce1540 | depth bits
PS c0 H2 header        body[92 + B + D]
PS c0 selector         body[94 + B + D]   22030000
PS c0 payload          body[96+B+D .. 99+B+D]
fragment ALU H2        body[188 + B + D]
first MUL              body[192..195 + B + D]
output MOV             body[196..199 + B + D]
immediate vertex H2    body[200 + B + D]
```

For D16, insert the existing A10--A13/A20 group, retain guarded Z16, and
accept only the already-capability-gated LESS or LEQUAL functions. The
required values are `A01=01ce154c` and
`A13=13010000`/`13030000`. No-depth restores `A01=01ce1540`; it must not
inherit depth-test/write bits. Blend state, when admitted, changes neither c0
nor the fragment object, but PTC0_KEEP_A still requires XRGB source so its
source alpha is exact.

Retaining `01ce...` is deliberately conservative for both D1 and native-c0
paths. Native c0 does not consume diffuse, but the word is vendor-exact for
the generalized textured image and avoids a transition-only correction-state
hazard. Type-02/FE A01 remains `01002521` in all cases.

No new kernel ABI is required for the D1 lowering: Mesa submits ordinary
MODULATE vertices after the strict c0/Q/format checks. Native c0 needs a new
closed program selector plus four c0 dwords in the structured request; the
kernel must synthesize the exact object and reject non-finite constants,
unknown flags and nonzero reserved fields. Userspace must never submit raw
ALU words.

## Silicon-safe discriminators

Use a BGRA texel `804080ff` and c0=`80ff8040`:

```text
PTC0 full product       approximately 40404040
PTC0_KEEP_A             approximately 80404040
PTC0A (c0.aaaa)         approximately 40204080
```

The first two differ only in alpha and therefore require either BGRA target
readback or source-over onto a known destination. Repeat PTC0_KEEP_A with an
XRGB view; it must match full PTC0 there. Binding BGRA to PTC0_KEEP_A must
reject before the ioctl.

Run each positive vector in this order:

1. no depth, Q=1 at all vertices;
2. D16 prefilled `0x8000`, LEQUAL at `0x8000` passes;
3. LESS at `0x8000` preserves both color and depth;
4. LESS at `0x4000` passes and updates depth;
5. no depth again to prove depth-state closure.

Check source, target and depth guards, submitted/rejected deltas, retirement
cookie, CR/work status and old-new-old program transitions. Negative cases
independently use one Q=`2.0`, a NaN c0 lane, short or offset constant buffer,
BGRA with PTC0_KEEP_A, missing view/sampler, extra instruction, and a
non-byte-grid constant on the D1-only path. Every negative case must stop
before native emission.

For the D1 precision discriminator, compare c0=`0.5` against a native c0
oracle using texture components chosen near an 8-bit rounding boundary. A
one-LSB difference is evidence that D1 is an approximation, not permission to
widen the accepted constant domain.

## Implementation order

1. Add exact projected PTC0 and PTC0_KEEP_A classifiers; preserve the common
   base-product identity when recognizing the vec4 alpha bypass.
2. Reuse MODULATE only for Q=1. For PTC0_KEEP_A also require XRGB source.
3. Initially admit D1 only for byte-grid constants and add positive/negative
   no-depth and D16 smoke vectors.
4. Qualify the scheduled native-c0 object with the vectors above, then add a
   structured selector and four-dword c0 upload to remove the byte-grid
   restriction.
5. Treat projected `tex*c0.aaaa`, arbitrary Q, BGRA alpha bypass and
   `tex.aaaa*c0` as separate gates.
