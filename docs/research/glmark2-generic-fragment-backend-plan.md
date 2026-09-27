<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 generic glmark2 fragment-backend plan

Status: evidence and implementation plan, not an execution allowlist.  This
note covers the fragment programs captured in
`/tmp/chrome9-glmark-nir-v4/nir` and the glmark2 2023.01 fragment sources in
`/tmp/glmark2-src.ps2OQY/usr/share/glmark2/shaders`.  It deliberately defines
a reusable NIR compiler.  No scene name, shader hash, GLSL text, or glmark2
option is a compilation key.

The important result is that Chrome9 pixel execution is a scheduled sequence
of separate ALU and TAU stores, not a fixed collection of glmark programs.
The compiler evidence is sufficient to implement a general straight-line SSA
backend, including repeated and dependent texture reads.  It is **not** yet
sufficient to advertise arbitrary pixel control flow or exceptional IEEE-754
behavior.  Every opcode below still needs the stated silicon numerical gate
before becoming executable.

## Evidence boundary

The exact records below come from the production Linux 5.75 compiler and the
Windows 7 D3D compiler unless a row says otherwise.  Primary corpora are:

- `chrome9-next-alu-one-record-corpus.tsv` for ADD/SUB/MAD/DP3/DP4/MIN/MAX;
- `chrome9-rcp-rsq-scalar-corpus.tsv` and
  `chrome9-exp-log-scalar-corpus.tsv` for scalar selectors and dependencies;
- `chrome9-frc-one-record-corpus.tsv` for FRC;
- `chrome9-cmp-abs-sat-corpus.tsv` for CMP/ABS/SAT;
- `chrome9-nrm-pow-expansion-corpus.tsv` for production POW and NRM;
- `chrome9-ps-minmax-temp-producer-corpus.tsv` and the lifetime corpora for
  temporary destinations and final-use fields;
- `chrome9-two-texture-callback-provenance-corpus.tsv` and `shader-isa.md`
  for independent/dependent texture scheduling;
- `chrome9-ps-misc-input-relocation-corpus.tsv` for the vPos candidate used
  by `gl_FragCoord`.

Compiler agreement proves encoding and scheduling.  It does not prove native
NaN, infinity, denormal, signed-zero, rounding, comparison, saturation, or
texture-derivative behavior on VX900 silicon.

## Native ALU capability matrix

Every ALU record is four dwords.  The ordinary datapath is `B + A*C`.
Source-language order must be assigned to physical A/B/C slots explicitly;
commutative NIR canonicalization must not silently exchange MIN/MAX/CMP
operands because signed zero and NaN behavior may be order dependent.

| NIR semantic form | native form | exact representative final record | evidence / remaining gate |
|---|---|---|---|
| `mov v0` | subop `d`, A=v0 | `01c00530 301c0053 00f04005 000d0820` | encoding closed; already used by bounded keys |
| `fmul(v0,c0)` | subop `0`, A=v0, C=c0, B=ZERO | `0140b6d0 30080053 00f04005 00000820` | encoding closed; bounded forms live |
| `fadd(v0,c0)` | subop `0`, A=v0, B=c0, C=ONE | `00800530 30140924 00f04005 00000820` | cross-driver exact; numerical ADD gate |
| `fsub(v0,c0)` | subop `0`, B=v0, A=c0, C=NEG_ONE | `80400530 30140924 00f08005 00000820` | cross-driver exact; SUB/NEG gate |
| `ffma(a,c,b)` | subop `0`, physical A/C/B | for `(v0,c0,c0)`: `00800530 30080053 00f04005 00000820` | exact MAD encoding; only select when NIR contraction semantics permit it |
| `fmin(v0,c0)` | subop `c`, A=v0, C=c0 | `01c00530 30080053 00f04005 000c0820` | exact, including temp producer; finite/order/NaN silicon gate |
| `fmax(v0,c0)` | subop `b`, A=v0, C=c0 | `01c00530 30080053 00f04005 000b0820` | exact, including temp producer; finite/order/NaN silicon gate |
| replicated `fdot3(v0,c0)` | subop `2`, A=`xyz0`, C=`xyz0` | `01c00530 50080055 00f04005 00020820` | exact; DP3 numerical and scalar-broadcast gate |
| replicated `fdot4(v0,c0)` | subop `3`, A/C=`xyzw` | `01c00530 30080053 00f04005 00030820` | exact; DP4 numerical and scalar-broadcast gate |
| `fexp2(c0.w)` | subop `4`, A=`000w` | `01c00530 b01c0053 00f080b6 00040820` | exact; first live domain `[-4,0]` |
| `flog2(c0.w)` | subop `5`, A=`000w` | `01c00530 b01c0053 00f080b6 00050820` | exact; first live domain `[1,2]` |
| `frcp(c0.w)` | subop `6`, A=`000w` | `01c00530 b01c0053 00f080b6 00060820` | exact; first live domain `[1,16]` |
| `frsq(c0.w)` | subop `7`, A=`000w` | `01c00530 b01c0053 00f080b6 00070820` | exact; first live domain `[1,256]` |
| `fabs(v0)` | subop `9`, A=v0, B=-v0, C=v0 | `00400538 30040053 00f04005 00090820` | exact; numerical/sign-zero gate |
| `cmp_ge0(c0,v0,c0)` | subop `9`, A=condition, C=true, B=false | `00800530 30040053 00f08005 00090820` | exact three-source record; predicate/zero/NaN gate |
| `ffract(v0)` | subop `e`, A=v0 | `01c00530 301c0053 00f04005 000e0820` | exact; negative/end-point/precision gate |
| destination `fsat` | producer dword 3 bit 12 | e.g. MOV becomes `... 000d1820` | exact bit; lower/upper clamp and ordering gate |

The scalar operations use `A=000W`: XYZ are literal zero and W selects the
scalar input component.  X/Y/Z/W selectors are independently exact.  A full
XYZW destination in compiler output is not by itself proof that all four
lanes receive the scalar.  The first live scalar oracle must test broadcast
and a temporary consumer, not only terminal UNORM color.

The generic encoder may use source classes TEMP=0, INPUT=1, CONSTANT=2, and
TEXTURE/MISC=3; destination TEMP=0 and COLOR=2.  Source swizzles 0..3 are
X/Y/Z/W, 4 is literal one, and 5 is literal zero.  Reject 6/7.  Initially use
physical r0..r11, oC0 only, NONE/NEG source modifiers only, and NONE/SAT
destination modifiers only.  Whole-register last-use markers are allocated
after scheduling: A is dword2 bit17, C is dword1 bit21, and B is dword0
bit25.  If A and C consume the same temporary on its final instruction, A
wins.  They are release markers, not arbitrary dependency flags.

### Checked ALU packing specification

The encoder can be implemented without native templates escaping its module.
The exact field map is:

| operand/control | native field |
|---|---|
| B modifier | dword0 `[3:0]` |
| B swizzle X/Y/Z/W | dword0 `[15:13]`, `[12:10]`, `[9:7]`, `[6:4]` |
| B id/class/final-use | dword0 `[21:16]`, `[24:22]`, bit 25 |
| C modifier | dword0 `[31:28]`; Windows also mirrors nonzero C modifier in dword3 bit20 |
| C swizzle X/Y/Z/W | dword1 `[11:9]`, `[8:6]`, `[5:3]`, `[2:0]` |
| C id/class/final-use | dword1 `[17:12]`, `[20:18]`, bit 21 |
| A modifier/W | dword1 `[27:24]`, `[30:28]` |
| A Z | dword1 bit31 plus dword2 `[1:0]` |
| A Y/X | dword2 `[4:2]`, `[7:5]` |
| A id/class/final-use | dword2 `[13:8]`, `[16:14]`, bit17 |
| destination writemask | dword2 `[23:20]` |
| destination id/class | dword3 `[9:6]`, `[11:10]` |
| SAT / result scale / subop | dword3 bit12, `[15:13]`, `[19:16]` |

The C-modifier mirror is a cross-driver discrepancy: Linux does not set it in
all equivalent records.  The first generic backend must reject modified C
sources rather than choosing one driver's convention.  NONE is exact.

Destination mask is native XYZW bit order (`1,2,4,8`).  Native register ids
are six-bit fields, but frontend wraparound is documented for out-of-range
ids.  Validate logical and physical limits before packing; masking is not
validation.

## Composite operations and comparisons

| NIR or compiler pattern | exact production lowering | policy |
|---|---|---|
| `fpow(x,y)` | LOG5 to temp, MUL0 by scalar exponent, EXP4; Linux controls `A90=90058000,A92=92000003` | exact three-record Linux allocation; positive-domain and compound-precision silicon gate |
| normalize | DP3(2), special subop9 record, RSQ(7), MUL(0) in vendor NRM | exact four-record vendor object; generic `DP3;RSQ;MUL` is preferable only after each independent temp/broadcast gate |
| `ffloor` | FRC(e), then subtract through special r12/r0 policy | exact production expansion, but numerical semantics and allocation policy not live |
| `flrp` | Linux two ordinary records; Windows one subop11 record | do not use the Windows-only instruction; lower algebraically when NIR fast-math/exactness permits |
| `bcsel(fge(x,y),t,f)` | `d=x-y`; `CMP(d,t,f)` | semantic straight-line if-conversion candidate; requires SUB and CMP zero/NaN proof |
| `bcsel(flt(x,y),t,f)` | `d=y-x`; `CMP(d,t,f)`, with equality arm corrected as required | not executable until strict-vs-nonstrict and signed-zero behavior is tested |
| `bcsel(feq(x,y),t,f)` | candidate `d=x-y; a=ABS(d); CMP(-a,t,f)` | generic finite-value lowering, but the complete ABS-temp/NEG/CMP object has no production/live proof yet |
| materialized `fge` | proprietary SGE is two records, subops `0,9` | compiler exact; prefer fused compare-select above until SGE numerical behavior is live |
| `fpow(x, positive integer constant)` | exponentiation by squaring using MUL | generic optimization, not a scene key; only legal under the NIR/GLSL precision and exceptional-value contract |

The exact Linux POW stream for `POW oC0,c0,c1` is:

```text
01c00530 b01c0053 00f080b6 00050020
0140b6d0 30081b6b 00f00005 00000020
01c00530 301c0053 00f20005 00040820
```

Zero is important to glmark2: lighting computes `pow(max(dot(...),0),N)`.
The currently proposed positive-only LOG domain is therefore insufficient for
correct glmark2.  Either characterize native POW at base zero, or use the
generic positive-integer exponentiation lowering when the exponent really is
a compile-time integer.  Do not clamp an arbitrary base away from zero.

## Texture ISA and repeated sampling

The hardware supports alternation between separately uploaded TAU and ALU
stores.  This is enough for repeated samples and ALU-generated coordinates;
there is no reason to make a two-texture scene enum.

Raw semantic TAU examples are:

```text
texld  r0,t0,s0: 201001b0
texldp r0,t0,s0: 207001b0
texldb r0,t0,s0: 203001b0
```

Sampler id is duplicated in bits 27:24 and 3:0.  TEXLD/TEXLDB/TEXLDP have
operation nibbles 1/3/7 in bits 23:20.  Destination and allocated coordinate
ids occupy the overlapping fields documented in `shader-isa.md`; they must
be packed through a checked allocator rather than ORed from unchecked API
ids.  TEXLDL and TEXLDD do not produce a usable TAU in either compiler and
remain rejected.

Production finalized examples:

```text
# independent tex0,tex1, then MUL
TAU 301001b0 311111b1
ALU 0140b6d0 30201053 00f20005 00000820
A90..A95 90018000 91000003 92020001 93000002 94000000 95000000

# tex0, ALU coordinate dependency, tex1, then MUL
TAU 301001b0 311111b1
ALU 01c00530 301c0053 00f00005 000d0460
    0140b6d0 30201053 00f20005 00000820
A90..A95 90018000 91000003 92020002 93000003 94000000 95000001
```

`A90.bit18` selects ALU first.  Appending ALU after TAU marks bit
`tau_index` in the T2A bitmap A93/A94.  Appending TAU after ALU marks bit
`alu_index` in A95..A9a.  A92 holds `(tau_count << 16) | alu_count`.  A91
includes the finalized sampler/resource mask.  The generic scheduler must
build these maps from the final ordered instruction list; it must not assume
all TAU instructions precede all ALU instructions.  The production finalizer
also owns TAU high lifetime bits and ALU source last-use fields.

This directly covers desktop blur's generic topology:

```text
coord_i = T0.xy + constant_offset_i
sample_i = TEX(s0, coord_i)
accum = accum + sample_i * weight_i
```

The default radius-five pass uses eleven samples of one sampler.  It needs an
ALU-to-TAU transition for every computed coordinate and a TAU-to-ALU
transition for every weighted accumulation.  Hardware/compiler store limits
are 32 TAU and 96 ALU records, so the topology fits.  What is still missing is
an exact production corpus for ADD-generated coordinates with repeated s0,
plus a live derivative/LOD and transition stress test.  Reusing the dependent
MOV example without that whole-program proof is not sufficient.

### Store framing and termination

There is no executable pixel-shader END instruction.  D3D `0000ffff` is a
frontend token and is absent from the native stores.  The native transaction
is a complete type-01 A90..A9b control object plus counted H3 stores:

```text
FE03043C payload_dword_count selector CC000000
payload...
```

ALU and TAU upload order is independent of execution order.  The declared
counts and A92/transition maps terminate execution.  `CC000000`/`DD000000`
are command padding/terminators, not shader opcodes.  Payload after the count
must be zero slack, not an invented END record.  A9b is kernel-owned runtime
state.

## Full production buffer-wireframe object

The byte-exact input is retained in
`chrome9-glmark-buffer-production-corpus.tsv`.  Its token SHA-256 is
`7e37878edfc6bb21f84c77a7a3827d4b1f2021b3ba0d55050424cbf992fcd71f`.
It uses one vector MUL and a fresh logical destination for every later value:

```text
mul r0.xyz,t0.xyz,t0.wwww
min r1.x,r0.y,r0.z
min r2.x,r0.x,r1.x
mul r3.x,r2.x,r2.x
mul r4.x,r3.x,c0.x
exp r5.x,r4.x
lrp oC0,r5.x,c1,c2
```

Linux production configuration 2 allocates internal r6 for the LRP expansion,
reports active tuple `(1,7,1,0)`, and emits:

```text
0140b6d0 502c06dd 0070c005 00000020
01c00530 d000056d 00100036 000c0060
01c00530 d020116d 00120016 000c00a0
0140b6d0 d000216d 00120216 000000e0
0140b6d0 d008016d 00120316 00000120
01c00530 801c0053 001204b6 00040160
80810530 30140924 00f08205 000001a0
00820530 00206053 00f20500 00000820
```

Controls are:

```text
90040001 91000000 92000008 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

The complete Linux textual result SHA-256 is
`34139e757c40a998f89438b18917817d3d1c5e0bc1c96f92baff02fb8a2ce1c5`.
There is no in-place or partial WAW: r0.xyz is one vector definition and every
scalar value after it is fresh.  Seven physical temporaries remain below the
initial r0..r11 limit.

The Windows wrapper result is deliberately retained in the TSV as a negative
test.  Its first MIN reads X/X rather than Y/Z, its records omit production
dependency fields, and it finishes with LRP11.  A backend test must match the
Linux stream and must not accept the Windows stream merely because it has
fewer records.

This is the only buffer whole-object oracle admitted by this plan.  An earlier
ten-record experiment used repeated partial writes to r0/r1.  Although the
Linux frontend accepted it, it is noncanonical for an SSA backend and is not
retained as an implementation target.  In particular, do not infer a
three-temporary A90 resource tuple from that discarded schedule.  The exact
eight-record controls and seven-temporary allocation above are authoritative.

## A90 resource derivation

A90 is derived after physical allocation, not selected from an opcode or
scene.  Production configuration two uses the first two active-resource
counts and the separate MISC auxiliary predicate:

```text
adjusted0 = active0 + (misc_aux != 0)

if adjusted0 <= 4 and active1 <= 3: bucket = 0x18000
else if adjusted0 <= 5 and active1 <= 4: bucket = 0x10000
else if adjusted0 <= 8 and active1 <= 6: bucket = 0x08000
else:                                      bucket = 0
```

For an ALU-first object, add bit `0x40000`.  A90 also ORs the compiler-owned
input-location usage mask.  Direct full-float T0 contributes usage bit0;
therefore fresh full-T0 objects produce:

| active tuple | bucket | complete A90 |
|---|---:|---:|
| `(1,5,1,0)` | `08000` | `90048001` |
| `(1,7,1,0)` | `00000` | `90040001` |
| `(1,8,1,0)` | `00000` | `90040001` |

`active1` is the allocated physical-temp high-water count, not ALU count.
Run liveness/allocation before this calculation.  The final A91 sampler mask
is written later and never participates in the bucket decision.

MISC vPos/vFace is different: it sets the auxiliary predicate, is relocated
after bucket selection, and contributes a relocation-dependent usage bit (the
first bucket's location 3 gives A90 low bit `0x8`).  Until vPos linkage is
live-proven, reject MISC rather than generalizing direct-T0 bit0 to it.

## Fresh-temp lighting and integer-power objects

`chrome9-glmark-lighting-production-corpus.tsv` retains four authoritative
Linux production objects plus hashes and controls.  The reusable lighting
core is:

```text
r0.x   = dot3(T0.xyz, T0.xyz)
r1.x   = rsq(r0.x)
r2.xyz = T0.xyz * r1.xxx
r3.x   = dot3(r2.xyz, c0.xyz)
r4.x   = max(r3.x, c1.x)       # c1.x = 0 for lighting
oC0    = r4.xxxx
```

It reports `(1,5,1,0)`, six ALUs, controls
`90048001 91000000 92000006 93000000` followed by zero A94..A9b, and exact
records:

```text
01c00530 500c0055 0010c005 00020020
01c00530 801c0053 001200b6 00070060
0140b6d0 50201005 0072c005 000000a0
01c00530 50080055 00120205 000200e0
01c00530 d008116d 00120316 000b0120
01c00530 001c0053 00f20400 000d0820
```

This closes production temp swizzles, partial scalar masks, DP3-to-RSQ and
RSQ-to-vector-MUL consumption, MAX, direct T0 usage, and the five-temp A90
bucket as one object.  It does not close numerical silicon behavior.

The same corpus contains generic multiplication chains for the constant
positive exponents present in the captures:

| exponent | addition chain | temp count | ALU count including terminal MOV | A90 |
|---:|---|---:|---:|---:|
| 20 | `x2,x4,x5,x10,x20` | 5 | 6 | `90048001` |
| 30 | `x2,x3,x6,x7,x14,x15,x30` | 7 | 8 | `90040001` |
| 100 | `x2,x4,x5,x10,x20,x25,x50,x100` | 8 | 9 | `90040001` |

This is a compiler optimization for any legal constant positive integer
`fpow`, not a lighting-program key.  It gives the defined base-zero result
without LOG and stays below the initial r0..r11 policy.  Apply it only when
NIR exact/fast-math rules and the GLSL `pow` domain allow strength reduction;
otherwise retain the separately gated LOG-MUL-EXP lowering or fall back.

## Pixel control flow

No fragment branch instruction is proven.  The proprietary pixel compiler's
supported output is straight-line ALU/TAU; the recovered CALL/IF/LOOP/REP
records belong to the vertex compiler.  PS 2.0 BREAK/BREAKC/IFC paths either
have no dispatcher or emitted no useful native record in the recovered
harness.  Therefore:

- side-effect-free acyclic fragment `if` regions must be if-converted to CMP
  selects before backend scheduling;
- compile-time-count loops must be unrolled in NIR before SSA lowering;
- the glmark2 `loop` shader has a **uniform** integer trip count and cannot be
  compiled once into straight-line native code.  A correct generic route is
  draw-time uniform specialization: key the native variant on the exact loop
  uniform value, unroll it with a checked bound, and invalidate/recompile when
  that uniform changes.  Counts which exceed the ALU-store bound require a
  real software fallback, not truncation;
- discard is a side effect, not ordinary control flow.  TEXKILL has a separate
  exact schedule and program metadata; fixed alpha test is usable only when
  it exactly implements the NIR compare/discard ordering.

The specialization rule is based on NIR loop analysis and constant-buffer
dependencies, never on a shader hash or uniform name.

## `gl_FragCoord` boundary

The function, loop, conditionals, and ideas-shadow programs consume
`gl_FragCoord`.  The production compiler maps D3D `vPos` through the
miscellaneous-input relocator.  A terminal example finalizes as:

```text
MOV oC0,vPos
01c00530 301c0053 00f2c305 000d0820
A90..A93 90058008 91000000 92000001 93000000
```

However, `vPos` and `vFace` compile byte-identically and the external raster
linkage which distinguishes/provides them is not yet proven.  The encoding is
closed; the semantic source is not.  A live oracle must check pixel-center X
and Y, origin convention, Y transform, viewport offset, W, multisample
behavior, and the `gl_FbWposYTransform` uniform before these scenes can be
correct.  Treating an ordinary varying as fragment position would be wrong.

## Captured glmark2 fragment dependency matrix

This table describes the optimized NIR actually captured, not guesses from
scene names.

| capture | relevant optimized fragment shape | generic backend requirements | first unresolved item |
|---|---|---|---|
| `jellyfish` | two TEX, six ADD/MUL ALUs; `pow(x,2)` already folded to MUL | straight-line SSA, two samplers/coordinates, full-float T2/T3 ALU inputs, constants | live generic multi-record/two-TAU submission and linkage |
| `buffer` | vector edge MUL, 2 MIN, square/scale, EXP2, final two-record LRP | MIN, scalar EXP2/broadcast, multiple constants, full-float varying | silicon MIN/EXP2 and full-T0 linkage; exact eight-record object is closed above |
| `bump` | normalize=`DP3+RSQ+MUL`, two more DP3, MAX, POW, color MUL/ADD | dot/scalar broadcast, RSQ, MAX, POW or integer exponent lowering | DP3/RSQ/MAX numerics and zero-base POW |
| `conditionals` | vPos arithmetic, three FRC, `fge`, one `bcsel` | vPos, FRC, SUB+CMP if-conversion | vPos linkage and CMP strict/NaN behavior |
| `function` | vPos arithmetic and two FRC | vPos, MUL/ADD/FRC | vPos linkage, then FRC |
| `loop` | same prefix plus uniform-count loop of MUL+FRC | vPos/FRC plus draw-time uniform specialization and unroll | bounded variant cache/fallback |
| `desktop` base | one ordinary TEX | generic one-sample path | already a bounded key; migrate without regression |
| `desktop` blur | eleven repeated TEX, coordinate ADD, weight MUL, accumulation ADD, alpha=1 | general alternating scheduler, repeated sampler, partial result assembly | repeated-s0 ADD-coordinate production/live corpus |
| `ideas` simple draws | varying MOV, constant MOV, scalar/vector assembly | generic MOV and partial vector construction | varying/linkage breadth |
| `ideas` logo/lamp lighting | fixed trip-count 3 loop, FEQ/boolean selects, RCP/RSQ/DP3/MAX/POW, many temporaries | static unroll, equality-select lowering, general ALU allocator/constants | pressure plus FEQ/CMP semantics |
| `ideas` shadow | vPos-derived coordinate, TEX, alpha compare and terminate | vPos, coordinate ALU, TEX, exact discard/alpha-test lowering | vPos and discard ordering |

The captured `ideas` fixed three-light loop should be unrolled by ordinary NIR
loop optimization.  It must not reach the native backend as `phi`, `iadd`, or
branch instructions.  Integer `ilt` selectors of the induction variable then
constant-fold.  In contrast, the `loop` capture compares its induction value
with a uniform, so it needs the generic specialization rule above.

## Full glmark2 source implications

The captured set is not every fragment shader shipped by glmark2.  The source
inventory adds these reusable requirements:

- bilinear texture shaders: four repeated samples, FRC, and LRP lowering;
- bump-height: three dependent height samples before the same lighting core;
- cel/shadow: comparisons, if-conversion, and discard;
- refract: three dependent samples, RCP/RSQ/dot/arithmetic and several
  coordinate chains;
- terrain: at least five fragment samplers, long ALU programs, FBO passes,
  plus vertex texture fetch outside this fragment backend;
- terrain-noise: FLOOR/FRC, MOD lowering, MIN/MAX, comparisons, large dot and
  polynomial networks likely near or beyond the 96-ALU/12-temp envelope.

Consequently a complete generic fragment compiler is necessary but not
sufficient for every glmark2 scene.  Terrain also needs truthful vertex
texture, framebuffer, resource-transition, format, and sampler capabilities.
Programs exceeding native stores or register pressure need a real Gallium
software fallback if the API continues to advertise GLSL 1.20.

## Concrete implementation order

1. **Replace expression classification with a real compiler object.**  Clone
   and optimize NIR, require one color output, lower vars/IO, scalarize only
   where native vector semantics demand it, run copy propagation/DCE, and
   retain NIR exact/fast-math metadata.
2. **Build semantic SSA IR.**  Represent vector writemasks, swizzles, constants,
   input linkage, TAU operations, and three ordered ALU sources.  Remove the
   current six-ALU/two-TAU topology checks; retain hard 96/32 bounds.
3. **Implement liveness and allocation independently of encoding.**  Linear
   scan full vec4 temporaries, compact holes, reserve at most r0..r11 first,
   reject partial-WAW aliasing, then mark one final source occurrence for each
   physical temp/texture result.
4. **Encode and offline-byte-test MOV/MUL/ADD/SUB first.**  Reproduce all
   lifetime corpora and the full jellyfish object from semantic IR.  This is
   the architecture gate for later ops, not a jellyfish-specific path.
5. **Add MIN/MAX and DP3/DP4.**  Byte-test terminal and temporary-producer
   objects, then run finite silicon grids with asymmetric operands and scalar
   broadcast consumers.  This unlocks buffer/bump arithmetic structure.
6. **Add FRC and scalar RCP/RSQ/EXP2/LOG2.**  Test all source components,
   temporary consumers, negative FRC, endpoints, and conservative finite
   domains.  Do not infer broadcast from terminal color.
7. **Add POW as a compiler lowering.**  First add generic positive integer
   exponentiation by squaring where semantically legal; separately gate the
   production LOG-MUL-EXP sequence including base zero and error tolerance.
8. **Generalize TAU scheduling.**  Encode repeated sampler ids, checked
   coordinate/destination allocation, and transition bitmaps from the ordered
   schedule.  Byte-test independent, dependent-MOV, dependent-ADD, repeated-s0,
   unused-result, and last-use variants before an eleven-tap blur live test.
9. **Add vector assembly and partial masks.**  Preserve untouched components
   through SSA rather than in-place WAW; use fresh temps and one final oC0
   definition.  This covers RGB+constant-alpha outputs without shader keys.
10. **Add vPos only after its raster-linkage oracle.**  Then enable FRC-based
    function shaders.
11. **Add CMP-based if-conversion.**  Validate nonnegative/negative, +0/-0,
    strict comparisons, unordered operands, nested selects, and equality
    expansions.  Only then accept conditionals and statically unrolled ideas
    lighting.
12. **Add loop specialization and bounded variant caching.**  Track uniform
    dependencies, recompile on value changes, cap generated records, and use
    software fallback beyond the native envelope.
13. **Add TEXKILL/depth/MRT only as separate side-effect contracts.**  Their
    program metadata and ordering cannot be inferred from ordinary ALU.

At every rung the kernel should receive semantic instructions, BO handles,
constant ranges, linkage, and sampler descriptors, then rebuild and verify
native records.  Passing arbitrary native words from Mesa would turn a
compiler bug into a privileged GPU-wedge primitive.

## Required tests before capability promotion

- offline byte equality against every relevant Linux production-finalized
  corpus, including controls and zero slack;
- unit tests for invalid ids, undefined reads, partial-WAW, excessive pressure,
  missing output lanes, transition overflow, sampler aliasing, and programs
  which compile to zero useful records;
- guarded 32x32 silicon tests for each opcode with known-good draws before and
  after, target/depth guard pages, ordered completion cookies, reset/error
  counters, and repeated execution;
- numerical grids which include asymmetric lanes, component swizzles, exact
  zeros and signed zeros, operation-specific finite boundaries, then separate
  NaN/Inf/denormal characterization;
- NIR differential tests comparing llvmpipe/reference pixels for each generic
  lowering, including uniform changes for specialized loops;
- full-scene correctness images only after the primitive opcode tests pass.
  A glmark2 FPS number is not an ISA conformance test.
