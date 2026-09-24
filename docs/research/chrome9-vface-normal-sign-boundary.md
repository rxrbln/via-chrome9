# Chrome9 vFace and normal-sign boundary

Status: production Linux compiler differential, D3D profile audit, offline
only, 2026-09-24.  The compiler accepts an out-of-profile `vFace` token, but
the recovered object and draw state do not preserve enough information to
implement GLSL `gl_FrontFacing`.  The generic Mesa backend must continue to
reject FACE until a distinct rasterizer-to-pixel-shader face-orientation
source is found and silicon-tested.

## Profile qualification: this is not a valid vFace program

Microsoft's D3D9 register contract makes `vFace` new in pixel shader 3.0.  It
requires a declaration and, in that profile, can only be used as the
condition source for `setp`, `if_comp`, or `break_comp`; it is not a valid
source to ordinary `CMP`.  The recovered vendor pixel frontend recognizes
PS 1.0 through 1.4 and PS 2.0, and no declaration-capable PS 3.0 path has been
recovered.  The existing PS 3.0 framing control accepts a bare MOV, but both
ordinary and centroid `dcl` candidates are rejected by the Windows and Linux
frontends with no native stream.

Consequently the byte differential below is deliberately an invalid-profile
parser/backend discriminator.  It proves that the vendor builder notices the
generic MISC register class, then loses the vPos/vFace index.  It does not
prove that valid D3D exposes a face source, that native CMP may consume one,
or that any raster state produces it.  The fact that the analogous
out-of-profile vPos route later passed on silicon cannot be generalized to
vFace: the two tokens become the same native source id before draw time.

## Smallest exact compiler differential

The default ioquake3 lightall fragment shader needs the vector selection

```text
front face: -normal.xyz
back face:  +normal.xyz
```

Using the D3D convention in which `vFace` is positive on the front face, the
smallest intentionally out-of-profile PS 2.0 discriminator is:

```text
CMP oC0.xyz,vFace.xxxx,-T0,T0

ffff0200 05000058 00070800 10001001 31e40000 30e40000 0000ffff
```

Configuration-two compilation and production postprocessing produce:

```text
active resources  0 0 0 0
ALU count          1
ALU                82c00550 500c0055 0072c300 00090820
TAU count          0
metadata           +2c=00058000 +34=00000000 +40=00000001
                   +68=00000001 +6c=00000009 +80=00000001
A90..A9A           90058009 91000000 92000001 93000000
                   94000000 95000000 96000000 97000000
                   98000000 99000000 9a000000
```

Replacing only the source token with `vPos.xxxx`, register index zero,

```text
ffff0200 05000058 00070800 10001000 31e40000 30e40000 0000ffff
```

produces the same resource tuple, ALU word, metadata and controls byte for
byte.  This extends the existing one-record MOV differential to the actual
three-source, vec3 normal-sign selection: the compiler parser notices the
MISC register class, but the backend discards the vPos/vFace index.

The final `...58009` is not a face selector.  Bit zero is the direct T0 route
and bit three is the relocated MISC source.  At VX900 draw time the common
MISC-enable bit 19 changes A90 to `900d8009`, irrespective of whether source
text named vPos or vFace.

## A8A is position-origin state, not face state

The recovered draw path emits A8A=`8a020000` when MISC is live.  A8A bit 3
chooses direct PEY versus `bottom_y-PEY`, bits 15:4 carry the bottom-Y value,
and bit 17 is the known ECO bit.  It changes fragment-position Y origin.  No
bit in the compiler object, A90 relocation result, A8A value, or recovered
MISC metadata changes between vPos and vFace.

Consequently these bytes prove only that a MISC operand can feed native CMP.
They do **not** prove that raster face orientation reaches that operand.  If
the current object were used for FACE, it would compare a position component
and flip normals across screen space instead of triangle orientation.

## Synthetic sign discriminator

For offline resource accounting only, FACE can be removed without a boolean
operation by reserving a scalar sign `s` with the convention `+1` for front
and `-1` for back:

```text
surfNormal = -s * var_Normal.xyz
```

Packing `s` into the otherwise dead `var_LightDir.w` lane keeps the linked
fragment interface at six vec4 varying slots rather than adding a seventh.
The mechanically transformed mode-zero default lightall sources have
SHA-256:

```text
VS 3afed6042934313a00bfd8416d42e76f321b5809574dd1e5952b765371edb793
FS dc623f4baf85ef408aabe8a856867688dc636753054eaac85928184f91a8fdc7
```

After standard nonnegative SQRT lowering, the linked fragment NIR contains
95 ALU nodes and five TEX nodes.  FACE, `b2b1`, BCSEL and SQRT are gone; the
normal sign costs MUL plus NEG, while SQRT becomes RSQ plus RCP.  The interface
remains six smooth varyings and five dense samplers.

This is a resource discriminator, not a proposed implementation.  An
ordinary vertex output cannot generically know post-clipping primitive facing,
and interpolating a value that is not constant across a primitive would be
wrong.  The experiment is semantics-exact only under the explicit oracle that
every fragment receives `s=+1` for a front primitive and `s=-1` for a back
primitive.  A real driver still needs a face-orientation producer or a
separately qualified two-pass/two-sided rendering strategy.

The standalone GLSL linker probe also stops short of a final native count:
its NIR retains sampler dereference sources and uniform `load_deref`
intrinsics, whereas the Mesa state tracker lowers those before the Gallium
driver sees the shader.  With FACE and SQRT removed, the first raw-probe
failure is therefore a three-source TEX (`texture_deref`, `sampler_deref`,
`coord`), followed by uniform dereferences.  Those are probe-pipeline gaps,
not new Chrome9 ISA operations.  Final ALU/TEX/temp pressure must be measured
only after reproducing the state-tracker lowering; it must not be inferred by
silently treating all sampler dereferences as unit zero.

## Implementation and proof ladder

1. Keep FACE rejected in the semantic compiler; never alias it to POSITION.
2. Add an offline negative test asserting that the invalid-profile
   vPos-index-zero and vFace-index-one CMP objects remain byte-identical.
3. Recover a distinct face-orientation route or state bit and prove it with
   adjacent front/back triangles whose normals produce asymmetric colors.
4. Require the same draw to exercise nonzero viewport origin and both A8A Y
   orientations, proving position-origin state cannot masquerade as facing.
5. Only then lower `bcsel(FACE,-n,n)` to the qualified source and native CMP
   or an algebraically exact sign multiply.
6. Run the fully state-tracker-lowered default lightall object and require
   five TAU records, six varying routes, final ALU at most 96, physical temps
   at most the hardware limit, and no missing/extra sampler or varying mask.

## Provenance

- Linux production compiler:
  `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
- Offline callback/finalizer harness:
  `tools/re/chrome9_pps_probe.c`, SHA-256
  `16fb55d972a6950e6809e27981a860dd1b825788b56cd039a7d4e79d4af4d02b`.
- Existing relocation and draw-state derivation:
  `chrome9-ps-misc-input-relocation.md` and
  `chrome9-vpos-cmp-frc-contract.md`.
- Exact default-lightall source and NIR inventory:
  `ioquake3-gl2-eight-unit-linkage.md`.
- Microsoft D3D9 PS 3.0 register contract:
  <https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx9-graphics-reference-asm-ps-registers-ps-3-0>.
- The vendor PS 3.0 declaration-negative corpus:
  `chrome9-ps30-dcl-negative-boundary.tsv`.
