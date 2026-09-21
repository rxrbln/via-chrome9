# Fixed-function fog and alpha-test shader keys

Status: this note separates two superficially similar GL1 effects which the
proprietary Chrome9 stack implements in different places.  Compatibility
alpha test is a fixed render-target operation in A00/A33.  Fixed-function fog
has both a PE-register route and an OpenGL-generated pixel-program route.  The
new native programs below were reproduced with the Windows compiler under
Wine and the Linux compiler under QEMU, but remain offline-only until bounded
silicon tests pass.

The machine-readable compiler rows are in
[fog-alpha-shader-corpus.tsv](fog-alpha-shader-corpus.tsv).

## Alpha test is not an OpenGL shader key

Linux proprietary `__glDpSTMUpdateAlphaTest:0x1ebe90` reads the compatibility
alpha-test enable, GL compare enum, and floating reference from context state.
It maps the eight `GL_NEVER..GL_ALWAYS` values at `0x200..0x207`, quantizes the
reference to an 8-bit value, and dirties the render state.  Its downstream
builders are `STM_AlphaTest_INV:0x194c40` and
`STM_AlphaFunc_INV:0x194ad0`.  Windows D3D `0x101fab20/0x101facf0` and the
byte-identical OpenGL copies independently produce the already live A00/A33
state:

```text
A00 bit 20 = alpha-test enable
A33[14:12] = NEVER..ALWAYS = 0..7
A33[10:0]  = ref == ff ? 400 : (ref << 2) | (ref >> 6)
```

Therefore the safe ioquake-era path is fixed A33, not TEXKILL.  It covers all
eight compare functions, consumes the actual fragment output alpha, and is
already measured for solid and sampled alpha plus D16 ordering.  The shader
compiler's TEXKILL operation is relevant only when the comparison value is
not the final output alpha or when a programmable API explicitly requests
discard.

For completeness, a narrow shader lowering was compiled dynamically:

```text
sub r1, source.aaaa, c0.aaaa
texkill r1
mov oC0, source
```

TEXKILL rejects a negative lane, so this form implements
`source_alpha >= c0_alpha`; it does **not** implement strict GREATER at exact
equality.  The source-alpha and reference values are replicated, avoiding a
kill caused by unrelated RGB lanes.  For sampled source, the Linux
configuration-two executable object is:

```text
A90..A9A 90000000 91000001 92020003 93000003
          94000000 95000002 96000000 97000000
          98000000 99000000 9a000000
TAU       201001b0 b14001b1
ALU       80006db0 b0140924 00f0806d 200003e0
          01c00530 301c0053 00f03f05 000d0420
          01c00530 301c0053 00f20005 000d0820
```

The first TAU samples texture 0, the first ALU computes the replicated signed
difference into the compiler's special temporary, the second ALU transfers
that value to the TEXKILL address path, the second TAU performs TEXKILL, and
the final ALU writes the sampled color.  Compiled metadata `+7c=1` is part of
the key.  The varying-only form and exact controls are in the TSV.

The Windows minimal allocation wrapper returns register zero for every
temporary.  Its sampled form consequently loses the final sampled source and
must not be copied into a packet.  It remains useful as an independent
semantic differential: it emits two TAUs, three ALUs, sets TEXKILL metadata,
and has the same SUB/TEXKILL/MOV schedule shape.  This is another concrete
reason the verifier must compare a complete closed object rather than opcode
fields alone.

## OpenGL fixed-function fog key

The proprietary OpenGL pixel-generator key byte is `state+0x18d7`.
`__glDpFFPSUpdateFogStateKey:0x204710` owns bits `[4:3]`: disabling fog clears
both bits, while the three enabled source/mode cases select key values 1, 2,
or 3.  It marks the generated-program cache dirty in both directions.
`__glDpFFPSGeneration:0x209920` calls
`__glDpGenerateFogBlend:0x2047b0` only when either bit is set.

The generator appends these exact canonical D3D token fragments before its
common final result move:

```text
key 1, conditional prefix when the prior-result class is 1:
02000001 80080009 90ff0002

key 2:
03000005 80080009 90ff0002 a0aa000a
0200000e 80180009 80ff0009

key 3:
03000005 80080009 90ff0002 90ff0002
03000005 80080009 80ff0009 a000000a
0200000e 80080009 81ff0009

common tail:
03000005 800f0008 90ff0002 a0ff000a
```

The generator then records result temporary/class `r8` and the enclosing
function emits the saturated full-color output MOV.  Key 2 uses the EXP
opcode once; key 3 squares and scales the fog input, then uses the negated
EXP source.  These are exact program-generation facts.  Their numerical
W/Z/fog-coordinate conventions are not yet measured, so they are not a
standalone Mesa lowering recipe.

`__glDpFFPSUpdateFogConstant:0x204c30` copies the four-component fog color to
the generated constant image, computes two density-derived floats, and marks
both affected constant ranges dirty.  A fixed-function shader key therefore
consists of the token variant **and** its driver-owned constants; reusing the
program with stale constants is invalid.

## Compiler differentials for a bounded shader-fog form

A simpler NIR-facing form is

```text
oC0 = lerp(c0, color, v1.aaaa)
```

where `c0` is fog color, `v1.a` is a precomputed and clamped fog factor, and
`color` is either `v0` or sampled texture 0.  This avoids claiming the
proprietary key's unresolved coordinate/range equations.  Both UMD families
accept the exact LRP semantic, but lower it differently.

Windows finalizes varying-only LRP to one native ALU:

```text
00416db0 30080053 00f04005 00110820
```

Linux 5.75 configuration two lowers it to two ALUs:

```text
80400530 30140924 00f08005 00000020
00800530 b0200053 00f0416d 00000820
```

For sampled color, the Linux executable object is:

```text
A90..A9A 90018000 91000001 92010002 93000001
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       301001b0
ALU       80000530 30140924 00f08005 00000020
          00800530 b0200053 00f0416d 00000820
```

The one-record Windows sampled result is not executable under the current
minimal wrapper: the allocator aliases the sample temporary and changes its
source to `xxxx`.  The Linux object supplies the physical allocation and is
the only implementation candidate in this tranche.  Cross-version one-ALU
versus two-ALU LRP is also a warning not to invent a universal native LRP
encoding.

## Bounded promotion order

1. Keep compatibility alpha test on the live A00/A33 path.  Do not enable
   TEXKILL merely to implement `glAlphaFunc`.
2. Run the existing A70 secondary-alpha endpoint test before any shader fog;
   it proves the PE fog gate and factor direction without EXP/range math.
3. If shader fog is required, first run varying-only LRP over a guarded
   BGRA target with factors zero, one, and one-half, then run
   `off -> fog -> off` closure.
4. Add the sampled LRP object only after its ordinary texture REPLACE path
   passes in the same context.  Use a blue texture, red fog constant, and
   factors zero/one/half; all source/target guards and texture-cache closure
   remain mandatory.
5. TEXKILL remains rejected until a dedicated discard oracle confirms kill
   suppresses color, depth, and stencil writes and survives
   `ordinary -> kill -> ordinary` transitions.

No row here expands the current GL version or arbitrary shader admission.
Every row is an exact closed key; unmatched swizzles, partial writes,
additional constants, alternate TEXKILL comparisons, and generated fog
keys 1--3 remain fail-closed.

## Provenance

The dynamic Windows compiler was
`VTGUModeDX32A.dll`, SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
run under Wine with wrapper SHA-256
`a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c`.
The Linux compiler was `via_chrome9_dri.so`, SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
run through `stmPPSCompile_invi` and
`stmPPS_PostTranslationSetting_invi(configuration=2)` under QEMU.  Raw and
final Windows streams matched for all four rows; the TSV records finalized
forms only.
