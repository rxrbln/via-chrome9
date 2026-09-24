# ioquake3 GL2 texturecolor and reduced-lightall linked NIR keys

Status: exact linked-NIR capture audit and implementation-ready bounded
matcher design, 2026-09-21.  The texturecolor shader is source exact and is
observed as Chrome9 classifier program 18 in both renderer startups.  Six
reduced-lightall programs have byte-identified linked NIR and the first of
them is now the live gameplay rejection.  The native c0 command and general
three-factor program remain subject to their separate VX900 promotion gates.

## Runtime order and evidence boundary

The two renderer startups in
`/root/tmp/chrome9/ioquake3-gl2-generic-semantic-v1.log` each create exactly:

```text
48 generic fragment programs
 1 texturecolor fragment program
 6 fogpass fragment programs
 3 dlight fragment programs
24 lightall fragment programs
```

The single texturecolor object is classified as program 18 in both passes
and is bound during `Com_TouchMemory`.  It is omitted from the
unsupported-only NIR dump precisely because the old constant-color matcher
already recognizes its algebra.  Its GLSL source is exactly:

```glsl
gl_FragColor = texture2D(u_DiffuseMap, var_Tex1) * u_Color;
```

This establishes the one-sample/full-vec4 product and the runtime ordering,
but no archived log contains that supported object's `source_blake3` line.
Do not invent a hash for it.

With the reduced renderer configuration, six of the 24 lightall links are
the same two-sampler semantic program.  Their exact dump identities are:

| GLSL name | `source_blake3` words | sampler declaration ids | alpha declaration id |
|---|---|---|---:|
| GLSL175 | `1478d42b 1a41c331 6700f05a 4cf4983b d59174c5 d23b3d1f f924acd4 f291dc37` | 3, 4 | 5 |
| GLSL187 | `cd84a1b0 16f2f056 91d5e576 7c45c2f2 dd3ffa2f a664d5ff 1e6a9653 6005226b` | 4, 5 | 6 |
| GLSL199 | `82b0eeb5 beb816bc 55233f66 a0bcfd1d 71e1f3a8 5387e207 ef9ba26f 9d297fa9` | 15, 16 | 17 |
| GLSL211 | `035714ac 6224be39 40679a9a 722a7b3e fa5d2064 47b05b40 5c54f591 1e76f347` | 16, 17 | 18 |
| GLSL223 | `2034644c 68642484 efbb1e3b f44325d1 c0023bdd c1471e07 4fd71a7e 0888449c` | 4, 5 | 6 |
| GLSL235 | `e872b9bd 30daf16a 69e64c12 a6e5af61 027d7ad9 0b10e469 a478a849 671498ae` | 16, 17 | 18 |

The declaration ids and hashes vary because these fragment programs are
linked against different startup permutations.  They are not semantic
selectors.  Every one has the identical input locations, constant offset,
texture units, control-flow tree and color DAG below.  A live q3dm1 capture
alternates object pointers but reaches this same NIR as the first gameplay
shader after the generic path.

## Exact texturecolor key

The initially admissible linked form is one straight-line block:

```text
coord    = direct linked vec2 fragment input
sample   = ordinary tex(unit=0, sampler=0, coord), 2D
c        = direct full vec4 fragment uniform load at base 0, range 1
out      = sample.xyzw * c.xyzw
store out.xyzw to FRAG_RESULT_COLOR/DATA0
```

The renderer has only one texturecolor startup object.  Because samplers do
not occupy the fragment constant store, `u_Color` is physical application
constant vector zero in this link.  The first implementation should record
byte offset zero from the actual `load_uniform`; a later generalized helper
may accept another compile-time 16-byte-aligned direct offset and repack that
vec4 into native c0.  It must never infer an offset from the opaque uniform
declaration id.

The semantic matcher accepts identity MOV/copy propagation and either the
linked `load_deref` or equivalent lowered input intrinsic.  It requires all
four FMUL lanes to use the corresponding sample and constant lanes.  Operand
commutation is harmless for one multiplication.  Reject scalar splats,
nonidentity swizzles, partial stores, saturation, projection, bias, explicit
LOD, offsets, discard, derivatives, coordinate arithmetic, another sample,
an indirect/dynamic constant offset, or a load shorter than one vec4.

At draw time, read the selected 16 bytes from constant buffer zero on every
draw, after applying the Gallium buffer offset and checked bounds.  Support
both user buffers and mapped PIPE_BUFFER resources; reject missing storage,
overflow, short ranges and non-finite lanes.  Copy IEEE dwords bit-for-bit to
the job-local c0 record.  Do not bake the value at shader creation and do not
quantize it through D1.

Command 0x16/capability 33 then selects the fixed native object documented in
`ioquake3-gl2-texturecolor-command16-audit.md`.  Its only program delta from
TEX0-times-primary is the first MUL source word
`30040053 -> 30080053`; the sampler, FVF and fixed VS stay unchanged.  XRGB
sampling supplies alpha one, so output alpha is c0.a.  BGRA sampling supplies
stored alpha, so output alpha is sample.a times c0.a.  Destination A52 does
not alter either rule.

## Exact reduced-lightall NIR

All six captured objects declare:

```text
u_DiffuseMap  sampler2D, texture/sampler unit 0
u_LightMap    sampler2D, texture/sampler unit 1
u_AlphaTest   direct int load, base=0, range=1
VAR0          smooth highp vec4
VAR1          smooth highp vec4
```

Their dataflow, including association, is exactly:

```text
packed  = load VAR0
primary = load VAR1
light   = tex(unit=1, coord=packed.zw)
lcol    = primary.xyz * light.xyz
diffuse = tex(unit=0, coord=packed.xy)
alpha   = diffuse.w * primary.w

if u_AlphaTest == 1: terminate if alpha == 0.0
else if u_AlphaTest == 2: terminate if alpha >= 0.5
else if u_AlphaTest == 3: terminate if alpha < 0.5

rgb = diffuse.xyz * lcol
out = vec4(rgb.x, rgb.y, rgb.z, alpha)
```

The exact instruction census is two input loads, two vec2 constructors, two
ordinary texture instructions, three FMULs, one vec4 constructor, one direct
integer uniform load, three IEQ, one FEQ, one FGE, one FLT, one IAND, three
`terminate_if`, and one full color store.  The five constants are integer
1/0/2/3 and float `0x3f000000`.  There are no projectors and no fragment
float constants.

## Current semantic-compiler boundary

This capture was rechecked against Chrome9 commit `23cbe36` after the generic
structured backend grew to 96 ALU and 32 TEX records.  Capacity is not the
first rejection.  The six captured programs use two varyings and two sampler
slots, within the currently advertised three of each, and their color DAG is
small.

The first rejection is specialization discovery.  The linked capture has one
straight-line block with three `terminate_if` intrinsics and no surviving
`nir_if`.  `chrome9_nir_find_inlinable_uniforms()` discovers uniform leaves
from `nir_if` conditions and loop exits; it does not inspect a
`terminate_if` predicate.  Consequently `u_AlphaTest` does not enter the
fragment variant key and the unspecialized three-predicate program reaches
`chrome9_compile_ps_nir()`.

That compiler rejects the first predicate with the normal configuration
because native KILL is opt-in.  Enabling the diagnostic KILL path does not
make this program admissible: the only qualified shape is one direct
`tex2D(s0).a < 0.5` predicate, while lightall tests
`tex2D(s0).a * VAR1.a` under three runtime modes.  The compiler also permits
only one ordered KILL.  This is a semantic rejection, not an instruction-
store overflow and not evidence for general native fragment control flow.

The generic closure is to collect direct UBO leaves from `terminate_if`
predicates with the same bounded walk already used for mixed loop-exit
conditions.  It is safe for that walk to over-key a variant: after inlining,
constant folding either deletes a predicate, leaves one independently
qualified predicate, or compilation continues to fail closed.  Do not key on
the uniform name, one of the six source hashes, or an ioquake program enum.
In particular, mode zero must delete all three side effects before native
lowering.

An offline NIR-builder probe of the exact mode-zero dataflow, compiled by the
same backend at `23cbe36`, produces:

```text
semantic instructions  8 = 2 TEX + 3 MUL + 3 MOV
native stores          6 ALU, 2 TAU
semantic varying mask  0x0003
semantic sampler mask  0x0003
A90..A93               90008002 91000001 92020006 93000002
TAU                     201101f0 31130bf1
ALU                     0140b6d0 50203055 0070c105 000000a0
                        0140b6d0 50202055 00700105 00000020
                        0140b6d0 d02c176d 00120176 00000120
                        01c00530 501c0053 00720005 000d0160
                        01c00530 801c0053 008204b6 000d0160
                        01c00530 301c0053 00f20505 000d0820
```

The three MOVs reconstruct RGB plus the independently computed alpha; they
must not be optimized into a four-channel texture product because lightmap
alpha is absent from the source expression.  This result proves current
compiler and store fit after specialization: 6/96 ALU, 2/32 TEX, two of
three sampler slots, two of three varying slots, and six of eight advertised
temporaries.  It does not by itself prove pixels or mutable-mode transitions
on VX900.

The bounded regression should build the captured predicate algebra with a
dynamic sampled alpha and one direct mode uniform, then prove:

1. discovery records only the mode dword, not the varying/sample leaves;
2. mode zero removes every `terminate_if` and emits the eight-record object
   above;
3. modes one through three retain exactly their specified predicate and stay
   unsupported until their multiplied-alpha compare is qualified;
4. changing modes selects a different cached variant and returning to zero
   restores the byte-identical no-KILL object;
5. missing constant storage, a short range, or an unrepresentable key fails
   before draw submission.

Only after that host regression should a guarded live ladder test mode zero,
then one discard mode, then `0 -> mode -> 0` state closure.  The first pixel
oracle uses distinct unit-zero and unit-one colors plus nonwhite primary, so
swapping a descriptor, consuming lightmap alpha, or routing VAR1 incorrectly
cannot accidentally pass.

The first bounded classifier should recognize this graph, not any of the six
hashes.  Require one common four-lane input behind both coordinate pairs,
with unit 0 taking exactly lanes xy and unit 1 exactly lanes zw.  Require a
different full four-lane input for primary color.  Require matching texture
and sampler indices 0 and 1, ordinary non-array/non-shadow 2D TEX, one coord
source apiece and no other texture operands.  Require the complete alpha-test
tree even when the current uniform value is zero; deleting it from the key
would make later uniform updates unsafe.

Normalize only identity moves and the deref-versus-lowered-input spelling.
The captured NIR print has no floating-point restriction annotation on either
RGB FMUL.  Mesa 26.2 represents the relevant restriction as
`nir_fp_no_reassoc` in each `nir_alu_instr::fp_math_ctrl`; zero is
`nir_fp_fast_math`.  The available native three-factor object associates
texture 0 and texture 1 before primary, so select it only after checking that
**both** captured-tree FMULs have `nir_alu_instr_no_reassoc()` false.  If
either forbids reassociation, compile/select a complete object matching
`diffuse * (primary * light)` instead.  Never reassociate across a saturation
or partial write.

## Compatibility fixed-path post-TNL packing contract

The packing below belongs to the older fixed-object compatibility program.
It is an important exception to both ordinary two-varying texture layout and
the GL1 projected path:

```text
linked VAR0.xy -> immediate D3.s,t -> native oT0
linked VAR0.zw -> immediate D4.s,t -> native oT1
linked VAR1    -> normalized packed D1 -> native oD0 / PS primary
D2             -> canonical zero
```

All four VAR0 lanes are consumed coordinates.  VAR0.z/w are **not** R/Q for
unit zero, and there are no source Q values to validate or discard.  Applying
the GL1 `TEX0.q == TEX1.q == 1.0` gate to this linked form would reject every
valid lightall vertex or, worse, confuse lightmap S/T with projectors.  The
hardware operation is ordinary TEXLD for both units.

Require all four coordinate lanes finite.  Preserve values outside [0,1] so
the separately validated CLAMP/REPEAT/MIRROR state continues to work.  Pack
primary only after requiring finite lanes in [0,1].  For a strictly exact GL2
lowering, initially require each lane to round-trip through the selected
UNORM8 byte (`float(byte)/255 == value`); the common all-white world primary
passes this gate.  Broader arbitrary-float primary varyings need a float
varying linkage rather than unreported D1 quantization.

Use the established two-coordinate fixed VS and ten-dword vertex record:

```text
x y z rhw diffuse specular s0 t0 s1 t1
```

The exact FE invariants are common A01 `01ce1540/4c/48`, FVF A02
`020041d1`, D3/D4 declarations, A60 `60800005`, two-coordinate A08, and the
ordinary two-stage TAU linkage.  The old C0 A01 form makes D1 unnormalized
and is not a legal shortcut.

Do not impose this D1 quantization contract on the generic structured
program.  The current semantic compiler represents the linked declarations
literally: VAR0 is float texture input T0 and VAR1 is float texture input T1,
with `semantic_varying_mask=0x3` and no primary-input bit.  The generic draw
path therefore emits two `EMIT_4F` varying records.  This preserves arbitrary
finite primary color and avoids the fixed path's UNORM8 round-trip gate.  A
future linker rewrite to COL0 would be a distinct interface and must update
the semantic mask and vertex layout together.

## Compatibility fixed-object program and format gate

Capability 25 selects the complete three-factor object on commands 0x0a,
0x0d and 0x13.  The state-complete form is:

```text
A90..A93  90018000 91000003 92020003 93000002
TAU        301001b0 311211b1
ALU        0140b6d0 30202053 00f20005 0000007f
           0140b6d0 30040053 00f20105 0000007f
           01c00000 301c0000 00f20105 000d083f
```

Use the fully scheduled object as a unit; do not mix the virtual Windows TAU
words or an individual partial-alpha ALU into it.  Body sizes are 266 dwords
for command 0x0a, 270 for 0x0d, and 274 for the command-0x13 destination/
blend/alpha envelope.  Exact indices are in
`three-factor-two-texture-runtime.md`.

The captured shader ignores lightmap alpha.  Therefore the three-ALU
full-vector object is exact only when unit 1 translates to XRGB A30 format 98
and supplies alpha one.  Unit 0 may be XRGB or BGRA: its translated alpha
then gives exactly `primary.a` or `diffuse.a*primary.a`.  If unit 1 is BGRA,
reject this key or select the separately recovered four-ALU
texture-0-alpha-times-primary object.  The older primary-alpha-only bypass
object is insufficient when unit 0 has stored alpha.

For `u_AlphaTest=0`, commands 0x0d or 0x13 may use canonical disabled A33.
Nonzero modes require the state-complete command 0x13 and draw-time CPU
specialization:

| uniform | kept fragments | fixed state |
|---:|---|---|
| 0 | all | A00 alpha-test bit clear, A33 `33007000` |
| 1 | alpha > 0 | A00 bit set, A33 `33004000` |
| 2 | alpha < 0.5 | A00 bit set, A33 `33001202` |
| 3 | alpha >= 0.5 | A00 bit set, A33 `33006202` |

Only expose each row once its compare function/reference and combined
two-texture+D16 state have their independent live capability.  Any other
integer rejects before BO lookup.  The uniform must be reread every draw;
shader binding does not freeze it.

## Guarded live oracle sequence

Every positive case uses distinct guarded target, texture0, texture1 and D16
objects, exact submitted/rejected deltas, retirement cookie, full guard
checks, idle/error registers, and a following old-program draw.

1. **Packed-lane routing.** Use NEAREST 2x2 textures.  Put logical RGBA
   `(128,64,255,128)` in a BGRA texture at texture0's `(0.25,0.25)` sample
   and poison its other texels.  Put logical RGB `(128,255,64)` in an XRGB
   texture at texture1's `(0.75,0.25)` sample and
   poison its other texels.  Emit packed VAR0 `(0.25,0.25,0.75,0.25)` and
   primary bytes `(255,128,128,128)`.  Expected BGRA result is approximately
   `0x40402020`.  Swapping xy/zw or using VAR1 as texture1 coordinates must
   select poison and fail visibly.
2. **No-projector discriminator.** Change only packed z/w across all four
   texture1 quadrants and require four different legal results.  A path which
   treats w as Q or insists it equal one rejects or mis-samples.  NaN in any
   of the four packed lanes must reject before ioctl.
3. **Primary program discriminator.** Alternate white and exact UNORM8
   nonwhite primary.  White may use the live two-factor optimization;
   nonwhite must select cap-25 three-factor and produce the lane-scaled
   result.  Run three-factor -> legacy two-factor -> three-factor without an
   engine reset.
4. **Lightmap alpha poison.** Store alpha zero in the unit-1 allocation but
   sample through a legal XRGB view/A30=98; output alpha must remain
   `texture0.a*primary.a`.  Rebind the same storage through BGRA/A30=99 and
   require rejection unless the complete texture-0-alpha object is selected.
5. **Runtime alpha tree.** Use four strips whose texture0 alpha is
   `00,40,80,c0`, unit1 XRGB and primary alpha ff.  Mode 1 keeps the last
   three; mode 2 keeps `00,40`; mode 3 keeps `80,c0`.  On killed strips both
   color and D16 must remain byte-identical.
6. **Texturecolor constant freshness.** BGRA texel `80402010`, then update c0
   after shader/view binding.  c0 `(1,0,1,1)` expects `80400010`; c0
   `(1,0.5,0.25,0.5)` expects approximately `40401004`.  Repeat the first
   value to catch stale persistent c0.

Negative tests cover a second coord base, missing packed lane, swapped unit
indices, unit-1 alpha in the NIR result, missing/repeated RGB leaf, modified
alpha tree, nonzero alpha mode outside 1..3, non-finite/unrepresentable
primary, short or indirect texturecolor constant loads, source/destination
aliasing, malformed guards and every unknown flag.

## Immediate implementation order

1. Land the exact texturecolor matcher metadata and command-0x16 draw-time c0
   fetch; it is one startup object and does not interact with lightall
   packing.
2. Add a distinct packed-lightall layout enum carrying coordinate semantic,
   primary semantic and alpha-uniform byte offset.  Do not reuse the existing
   packed-gray layout.
3. Emit D3 from packed xy, D4 from packed zw and D1 from the separate primary;
   validate the consumed components only and apply no Q gate.
4. Initially admit unit1 XRGB, alpha mode zero, D16 LEQUAL/write, blend
   disabled.  Prove the nonwhite oracle before relying on cap 25.
5. Add already-live alpha compare rows and blend/depth combinations one at a
   time.  Keep default multi-sampler lightall, mipmapped views and arbitrary
   fragment programs outside this key.

## Confidence boundary

The six lightall hashes, declarations, constant offset, NIR dataflow and live
runtime identity are capture exact.  Texturecolor source, startup position,
constant-vector role and program-18 classification are exact, but its linked
NIR hash was not printed.  Native program words and packet indices are
compiler/static-driver exact.  The underlying two-texture, D16, blend and
alpha state has separate live evidence.  General nonwhite cap-25 execution,
native c0 under command 0x16, the packed GL2 coordinate split and their
combined state remain provisional until the guarded oracles above retire on
VX900.
