# Chrome9 native shader verifier policy

## Result

The proprietary Linux and Windows compilers are translation oracles, not a
safety boundary.  They silently alias invalid registers and modifiers,
truncate oversized pixel programs, lose instruction flags, wrap VS activation
counts, and accept some flow instructions without emitting code.  The old
kernel parser does not validate shader payloads either.

The first Mesa verifier must therefore be deliberately less general than the
decoded ISA.  Level 0, now implemented in the offline corpus, accepts only a
complete canonical object regenerated from a closed semantic key and compares
every control, resource field, instruction, inactive tail, and zero slack
byte.  There is no raw-native input entry point.  A future Level 1 verifier may
accept newly generated straight-line instructions only after applying every
range and field rule below; it must not weaken Level 0 for existing keys.

## Evidence for the missing vendor boundary

- Linux `PVSCP_VALIDATE_VS20_H5` at `via_chrome9_dri.so:0x18fc80` remaps
  inputs and returns zero without checking opcodes, flow, targets, or lengths.
- Linux `stmPPSValidatePS20_invi` at `0x1b5870` allocates registers and
  returns one without validating native instructions.
- The Linux KMD H3 parser skips four-dword groups until a header-like word;
  it does not validate the explicit program count or instruction contents.
- Windows/Linux PS appenders stop at 96 ALU or 32 TAU records but compilation
  still succeeds.  Linux normalized PS record 129 crashes its fixed array.
- Linux/Windows VS A60 masks `active_count-1` to nine bits.  An offline
  516-record result therefore wraps intended last index 513 to one.
- D3D instruction predication/coissue bits produce byte-identical native code,
  and BREAK/BREAKC/IFC can parse successfully without a native operation.

Compiler success is consequently never a verifier result.

## Hard ranges versus initial policy

Architectural or proprietary-store observations are upper bounds, not a
promise that every value has run on silicon.  The stricter right column is the
only range initially eligible for emission.

| resource | recovered bound | initial verified policy |
|---|---:|---:|
| VS normalized source records | fixed array 256, unchecked | closed templates only |
| VS uploaded/active native records | nine-bit A60: at most 512; Linux native area 295 | at most 256, current maximum 13/11 |
| VS temporaries | native destination masks to 16 | allocate `r0..r11`; closed keys use only compiler-fixed r0 |
| VS inputs | source id masks to 32 | `v0..v15`; closed keys use v0..v3 |
| VS float constants | `c0..c255` | identity c0..c3 only |
| VS integer / Boolean constants | `i0..i15`, `b0..b15` | none |
| VS texture-coordinate outputs | `oT0..oT7` | oT0/oT1 only |
| VS other outputs | oPos, oD0..oD3, oFog, oPts | full oPos; optional full oD0 only |
| PS normalized source records | 128; record 129 crashes | closed expression keys only |
| PS ALU records | store 96; record 97 silently dropped | current maximum 2 |
| PS TAU records | store 32; record 33 silently dropped | current maximum 2 |
| PS temporaries | allocator searches `r0..r11` | compiler-fixed allocation only |
| PS constants | `c0..c31` | c0/c1 as declared by the closed key |
| PS color varyings | native v0..v31 field | only exact v0/v1 closed records; advertise only live-linked v0 |
| PS texture varyings | remapped t0..tN | only coordinates consumed by TAU0/TAU1 closed records |
| samplers | native `s0..s15` | s0/s1 only, each with matching validated texture state |
| PS outputs | oC0..oC3 plus depth alias | full/known partial oC0 only; no depth |

Before any Level 1 upload require:

```text
1 <= VS active <= VS upload <= 256
A60[8:0] == VS active - 1
PS ALU <= 96 and PS TAU <= 32
H3 VS/PS-ALU dword count is exactly records * 4
H3 PS-TAU dword count is exactly records
```

Count after lowering, register allocation, finalization, and inactive VS tail
insertion.  Never validate an IR count and assume native expansion fits.

## Level 0 canonical-object verifier

`chrome9_verify_fs_offline()` and `chrome9_verify_vs_offline()` regenerate the
expected object with the offline compiler, then check in this order:

1. non-null pointers and a recognized semantic key;
2. exact active/upload or ALU/TAU counts and hard maxima;
3. exact constant/input/varying/sampler masks and output metadata;
4. exact A25/A26/A60/A62 or A90..A9b controls;
5. byte-exact used VS/ALU/TAU records;
6. byte-exact complete object, which also requires all unused arrays, inactive
   storage, structure padding, and future-reserved fields to remain zero.

The returned reject classes are `NULL`, `KEY`, `COUNT`, `CONTROL`, `RESOURCE`,
`INSTRUCTION`, and `SLACK`.  They are diagnostics only; callers must treat all
as the same fail-closed outcome.  Tests independently mutate each class,
including an unused last store slot.  This last test matters because a later
packet-count bug must not turn stale process bytes into native instructions.

Canonical comparison also binds finalized scheduling as one object.  For PS,
A90..A9b transition maps, TAU dependency words, ALU dependency fields, and
instruction order cannot be combined across templates.  For VS, compiler
A62.low9, active count, upload count, both inactive sentinels, output metadata,
and native records cannot be recomputed or substituted independently.

## Level 1 native instruction rules

These rules describe the minimum future structural verifier.  They do not
authorize Level 1 execution yet.

### Vertex sources

Each native source is one dword:

```text
bit 0       negate
bits 13:2   four 3-bit swizzle/literal selectors
bits 22:14  nine-bit unified register index
```

Accept selector 0..3 for XYZW and 4/5 for literal one/zero.  Reject 6/7.
Accept only NONE or NEG; the proprietary frontend maps every other D3D source
modifier to one of those by enum parity.  Range-check the logical register
before unified mapping: unchecked `r32`/`v32` aliases id zero, `c256` wraps to
unified zero, `i16` aliases b0, and `b16` aliases i0.

Relative constant addressing is initially rejected.  The native relative
selector is instruction-wide, and malformed instructions with two different
relative selectors silently keep the first.  MOVA/address registers therefore
remain outside the first GL2 subset as well.

### Vertex destinations and operations

The control dword contains XYZW mask in bits 3:0, destination in 9:4,
saturate in bit 14, operation selector in 21:17, and relative selector in
27:24.  Accept only destination modifier NONE/SAT; other proprietary values
again parity-alias.  Require all reserved/unknown bits to equal the canonical
emitter output for the selected operation.

The first generated straight-line allow-list may contain MOV, ADD, SUB, MUL,
MAD, DP3, DP4, MIN, MAX, RCP, RSQ, FRC, ABS, and CMP only after each exact
record and numerical behavior has an offline and live corpus entry.  Current
Level 0 uses only compiler-exact M4x4/MOV plus the indivisible three-record
reciprocal-W/viewport finalizer.  It does not decode an arbitrary matching
opcode number and accept it.

Require one dominating full-XYZW oPos write.  Only full oD0 and XY T0/T1
writes are initially valid.  Reject partial position, multiple position
writes, output readback, fog, point size, oD1+, oT2+, and any output alias.

### Pixel ALU and TAU

Accept only finalized configuration-two records produced by a recognized
key.  Initial ALU semantics are MOV/MUL and the exact partial RGBX pair;
initial TAU semantics are ordinary 2-D TEX for sampler 0/1.  Every operand
class, temporary allocation, writemask, swizzle, source modifier, destination
modifier, and dependency field must equal the canonical record.

For a future general ALU verifier, source swizzle selectors 0..5 are the only
defined values; source modifier is NONE/NEG and destination is NONE/SAT.
Result-shift encodings 0,1,2,3,13,14,15 are the only proprietary values with
defined behavior, but Level 0 accepts identity zero only.  Reject codes 4..12,
which silently become identity.  Validate sampler IDs before encoding: s16,
s32, and s255 corrupt or alias fixed TAU fields.  A TAU record is accepted
only if the matching texture image, sampler, coordinate varying, cache pulse,
and PS resource mask are all present in the same state object.

Reject PS relative constants, direct tN ALU reads without an exact linkage
map, projection, bias/LOD/gradient/offset/shadow/array sampling, discard,
depth output, dual source, and dependent reads outside the one already
captured compiler schedule.

### Flow, flags, and termination

Reject all VS CALL/CALLNZ/RET, IF/ELSE/ENDIF, LOOP/ENDLOOP, REP/ENDREP,
BREAK/BREAKC, labels, address-relative branches, and all PS flow.  Although
some VS records and nine-bit fixups are decoded, no branch stack depth,
latency, reconvergence, or silicon behavior is proven.  Vendor fixup tables
also hold only 20 entries per construct without bounds checks.

Reject D3D/native predication, coissue, unsupported instruction flags, and
unknown bits even when the proprietary compiler ignores them.  Require the
two exact inactive VS tail sentinels and the PS CC/DD store terminators where
the selected H2 builder uses them; neither is executable slack.

## NIR admission map

The NIR-facing classifier runs before native encoding.  Any unmatched item
returns to the existing Gallium `draw` or software fragment fallback; it is
not a draw error and never becomes a partially emitted native program.

### Initially representable VS NIR

- exactly one entry point, one acyclic block, no calls, loops, phis, or
  divergent control;
- `load_input` of position vec4 and optional COL0 vec4/T0.xy/T1.xy at fixed
  dense locations;
- identity `mat4 * position` recognized as the exact four-row M4x4 template;
- one full position store and optional full COL0 / XY T0 / XY T1 stores;
- no other live instruction after copy propagation and DCE.

Until a general emitter is live-gated, even an algebraically equivalent set
of four `fdot4` instructions is rejected unless canonicalization produces the
exact M4x4 key.  User matrices, UBO/SSBO loads, indirect constants, vertex
texture fetch, integer ALU, bit operations, conversions, system values,
clip/cull distances, point size, transform feedback, and stream output are
unsupported.

### Initially representable FS NIR

- one entry point and one acyclic block;
- one oC0 store matching a closed MOV, MUL, or RGBX-alpha-force expression;
- leaves limited to linked COL0/COL1, aligned c0/c1, and ordinary 2-D sample
  results from linked T0/S0 or T1/S1;
- only the exact full masks or exact RGBX partial-write pair in the corpus;
- no unused instruction or resource after DCE.

Reject add/sub/mad/dot/min/max/cmp until their finalized PS keys are added,
despite decoded raw encodings.  Also reject scalarized component shuffles not
represented by the alpha-replicate keys, dynamic sampler selection, uniforms
beyond c0/c1, interpolation qualifiers not matched by fixed state, front-face
and sample system values, derivatives, discard/demote, framebuffer fetch,
memory/image/atomic operations, multiple render targets, depth, and control
flow.

## Capability rule

Passing this verifier proves only that Mesa reconstructed an already audited
native object.  It does not prove silicon execution, render-target safety, or
GLSL completeness.  Native keys remain behind their address-free and guarded
live gates.  `PIPE_CAP_GLSL_FEATURE_LEVEL`, ES versions, and shader-related
extensions must remain unchanged until ordinary application-created NIR,
negative tests, resource hazards, and fallback transitions pass independently.
