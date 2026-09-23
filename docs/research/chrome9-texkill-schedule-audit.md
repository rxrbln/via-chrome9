# Chrome9 TEXKILL schedule and discard boundary

Fresh production differentials close the compiler schedule for isolated,
output-followed, and initialized TEXKILL programs.  TEXKILL is not a lone TAU
word: the compiler first moves its logical temporary into texture/address
destination 0, then issues TAU operation 4.  Linux configuration 2 adds the
ALU-to-TAU dependency bits and changes the control base.  A guarded live test
now proves finite sign comparison and color/D16 suppression; broader discard
semantics still require separate execution evidence.

## Exact records and finalization

The common pre-finalizer kill pair is:

```
ALU 01c00530 301c0053 00f00005 000d0420
TAU 214001b1
```

Linux configuration 2 rewrites it to:

```
ALU 01c00530 301c0053 00f20005 000d0420
TAU 314001b1
```

The ALU is MOV-family subop d into texture/address destination 0.  The TAU has
operation nibble 4.  Linux adds dword-2 bit `00020000` to the ALU source-A
dependency and bit `10000000` to the TAU.  These are required schedule fields,
not alternate TEXKILL semantics.

For initialized `mov r0,c0; texkill r0; mov oC0,v0`, Linux emits:

```
01c00530 301c0053 00f08005 000d0020
01c00530 301c0053 00f20005 000d0420
01c00530 301c0053 00f04005 000d0820
TAU 314001b1
A90..A95 90058000 91000001 92010003 93000001 94000000 95000002
```

Windows has the same three arithmetic records except kill-transfer dword 2 is
`00f00005`, uses TAU `214001b1`, and controls
`90040000 91000000 92010003 93000001 94000000 95000002`.
The v0-driven initialized form changes only the first/last MOV source classes.

The isolated kill has phase 2 and one ALU/one TAU.  Adding an output makes
phase 1 and adds A93 bit 0 for TAU-to-ALU.  Adding the leading initializer
changes A95 from bit 0 to bit 1, so the transition maps must be carried from
the finalized program rather than reconstructed merely from counts.  Every
case sets TEXKILL-present metadata.  Linux reports temporary mask 1; this
Windows wrapper reports zero, another reason not to transplant raw metadata
between compiler families.

## TEXKILL metadata is not a context sideband

Public instance `+0x7c` is a compiler/rebuild flag, not a separately emitted
hardware-context enable.  `stmPPS_TEXKILL_PS20_invi:0x167903` sets it to one;
initialization clears it at `stmInitializePixelShaderInstance_inv:0x1b81d7`.
`INVPS_HwRebuildInstBuffDesc:0x198f21..0x198f3d` reconstructs the same flag
from TAU operation 4 (its object-relative `+0x6c` is public-instance
`+0x7c`).

Neither production draw updater consumes that flag.  Linux
`stmUpdatePS_inv:0x17f710..0x180c00` reads public `+0x78` at `0x17f77d` to
update the fragment-depth context bit, but contains no public `+0x7c` read.
The Windows D3D programmable update at
`VTGUModeDX32A:0x1015b000..0x1015c600` likewise reads `+0x78` at
`0x1015b057` and never reads `+0x7c`.  It uploads the TEXKILL ALU/TAU records
and A90--A9A transition image through the ordinary program path.

The resource counter provides an independent consistency check.  Pre-final
`texkill r0; mov oC0,c0` reports `(1,1,0,0)`: the kill-transfer ALU defines
all four components of internal texture/address register zero, and the kill
TAU consumes that internally defined register.  Its external texture map is
empty, so the A00-producing third output remains zero.  TEXKILL must
therefore neither set a separate context bit nor increment A00 merely because
it uses the texture/address path.  Its required per-program state is the
ordered transfer ALU, operation-4 TAU, finalized transition controls, and the
normal replacement/clear of that complete program on the next draw.

## Exact emitter boundary and operand contract

Both proprietary emitters implement the same two paths.  The Linux PS 2.0
emitter is `stmPPS_TEXKILL_PS20_invi:0x167860..0x1679c6`.  Its register-class
test is at `0x1678b2..0x1678c3`.  An ordinary temporary takes the bridge path:
`stmPPS_GetSrcSetting_invi` at `0x16792e`, texture/address scratch allocation
through callback `+0x1388` at `0x167933..0x167939`, ALU construction at
`0x16793f..0x167983`, `INVPS_AddAInst` at `0x167986..0x16798d`, TAU
construction/addition at `0x167992..0x1679aa`, and scratch release through
callback `+0x1390` at `0x1679af..0x1679bc`.  A texture/address-class operand
instead goes directly through `0x1678c3..0x167903` without a bridge ALU.

Windows `VTGUModeDX32A.dll:0x1021c1d0..0x1021c2ec` independently has the
same split at `0x1021c1f1..0x1021c209`, bridge allocation and ALU at
`0x1021c20f..0x1021c270`, TAU at `0x1021c275..0x1021c288`, and direct path at
`0x1021c2ad..0x1021c2dd`.  Both start the TAU from literal `214001b1` and OR
the allocated texture/address id into bits 17:12.  Configuration 2 then adds
the proven dependency bit, producing `314001b1 | (id << 12)` for a sole/final
use.  The bridge destination is native texture/address class 1 with full
XYZW write mask; for scratch id `p` its destination word is
`000d0420 | (p << 6)`.

The D3D PS 2.0 `TEXKILL` operand is destination-like, not a general ALU
source.  Source swizzle and modifier bits are therefore not part of the
semantic instruction.  The bridge copies the selected temporary as identity
XYZW, and the TAU word has one fixed identity image.  A new semantic ABI must
not infer a swizzled or negated KILL from unused record fields.  The bounded
first revision should accept exactly one initialized TEMP operand with
identity XYZW, no modifier, no ordinary destination, zero sampler/reserved
fields, and zero unused sources.  Mesa must construct the signed four-lane
predicate explicitly before KILL.

The manuals add one state invariant which is easy to miss in the opcode
corpus.  HParaType 01 A90 bit 20 is `HPSDPNTXkill_N`: zero enables a dependent
TXKILL, while one promises that none exists.  The manual example says a
sample/ALU/TXKILL chain affects both `CVALID` and `STVALID`.  Every recovered
TEXKILL control image keeps A90 bit 20 zero.  Do not apply the otherwise
legal independent-program performance value to a kill program.  This is a
normal A90 program-control field, not the compiler object's `+0x7c` flag.

## Exact alpha-threshold lowering

The closed proprietary alpha row is:

```text
SUB     r1, sampled.aaaa, c0.aaaa
TEXKILL r1
MOV     oC0, sampled
```

The Linux configuration-2 object has two TAUs (sample then kill), three ALUs
(SUB, kill bridge, color output), A2T bit 1 and T2A bits 0/1.  Its compact
retained form is `alpha_texture_ge_ref` in
[`fog-alpha-shader-corpus.tsv`](fog-alpha-shader-corpus.tsv).  Under the PS
semantic, TEXKILL rejects a negative component.  Replicating alpha and the
reference into all four SUB lanes therefore implements the strict predicate
`sampled_alpha < reference`; equality produces zero and survives.  It also
makes the unresolved question of whether silicon examines XYZ or XYZW
irrelevant for this lowering.

This is the source-backed path for glmark2 `ideas`, whose logo shadow uses
`if (texture.a < 0.5) discard`.  Its A8 UNORM sample and literal half are
finite, so no unordered comparison is involved.  It is not permission to
lower arbitrary `terminate_if`: signed zero, infinity, NaN, and a Boolean
0/1-to-negative conversion still need their own execution proof.  In
particular, do not use a swizzle on KILL itself; emit a replicated SUB result
and kill the identity temporary.

## Implementation-ready bounded slice

The following division keeps policy in Mesa and native safety in the kernel.

1. Append a semantic KILL opcode without changing the fixed-width record.
   Define it as one TEMP source and no destination.  Kernel validation must
   require identity XYZW, no source modifier, zero `dst`, `sampler`, reserved,
   and unused-source bytes, an initialized source, and initially at most one
   KILL.  KILL is an ordered side effect and must count as a use of its TEMP;
   it cannot be DCE'd, moved past color/depth output, or represented by CMP.
2. Account one bridge ALU and one TAU in the limits.  Allocate one internal
   texture/address scratch id after the compacted producer coordinates.  A
   sampled T0 plus KILL consequently uses scratch T1.  Emit an identity MOV
   from the physical TEMP to that scratch, then TAU operation 4.  Mark the
   bridge's ALU-to-TAU bit, the kill TAU's TAU-to-ALU bit when an output
   follows, A91 scratch usage, and A90 bit 18 when the stream starts in ALU.
   Preserve A90 bit 20 clear.
3. Keep sampler count and texture descriptors unchanged: KILL has a TAU but
   no sampler.  Resource map B/A00 also remains unchanged when the bridge
   defines the scratch before the TAU consumes it.  The scratch contributes
   to the A90 resource bucket and A91 coordinate mask, but not to the A08
   count of VS-produced coordinates.
4. Extend the Mesa ordered IR with destination-free KILL.  Teach its NIR walk
   to retain `terminate_if` as a side effect.  For the first conformant
   finite-domain lowering, form four copies of `sampled_alpha - reference`,
   append KILL at the intrinsic's program position, and leave the color store
   after it.  Reject demote/helper semantics, arbitrary Boolean conditions,
   multiple kills, fragment-depth output, and non-finite/unproven comparison
   domains until their separate gates pass.
5. Upload the complete replacement program transaction.  Switching back to
   an ordinary shader must write TAU count zero and fresh A90--A9A controls;
   there is no `+0x7c` context bit to clear.

For the minimal initialized shape `MOV r0,c0; KILL r0; MOV oC0,v0`, the exact
Linux configuration-2 tuple remains:

```text
ALU 01c00530 301c0053 00f08005 000d0020
    01c00530 301c0053 00f20005 000d0420
    01c00530 301c0053 00f04005 000d0820
TAU 314001b1
A90..A95 90058000 91000001 92010003 93000001 94000000 95000002
```

The direct `v0` output makes the active external texture-resource high water
one.  Replacing it with a constant output is the focused resource selftest:
the kill bridge is internal and the active count must be zero.

## Validator and regression matrix

Host tests should establish the packet contract before target execution:

* exact no-sampler constant-init/KILL/constant-output ALU, TAU, A90--A95 and
  active-resource count zero;
* exact sampled-alpha SUB/KILL/output ordering, scratch id one, sample TAU
  before kill TAU, transition maps, and unchanged sampler count one;
* validation failures for non-TEMP or undefined source, non-identity
  swizzle, modifier, nonzero destination/sampler/unused source, a second
  KILL, missing full oC0, and ALU/TAU expansion overflow;
* Mesa NIR proof that `terminate_if(flt(alpha, 0.5))` survives optimization
  and becomes TEX, replicated SUB, ordered KILL, then color output; and that
  unsupported discard/demote shapes remain rejected rather than disappear;
* `ordinary -> kill -> ordinary` object tests proving TAU/control state is
  replaced, not accumulated.

The first guarded silicon gate must use finite constants and fresh guarded
color plus D16 surfaces.  Test all-positive, then one negative X/Y/Z/W lane,
then replicated below/equal/above-threshold values.  A near killed draw
followed by a farther accepted draw distinguishes correct suppression of both
color and depth from color-only kill.  Read back both planes.  Only after that
passes should `+0`, `-0`, infinities and quiet NaNs be used to decide whether
generic Boolean and unordered comparisons are conformant.  A separate
stencil readback is required before claiming the manual's `STVALID` behavior
through the new driver.

The fresh isolated/output JSON SHA-256 is
`05bf89b34993acde688ca3da640c24f29477d131ede494c946e15f498a50f035`.
The two initialized cases have SHA-256
`d738992836734b637083a28ce201644fbf09ff32e3785fdb54b5c0d3c4538649`.
All records and A90..A95 tuples are retained in
[`chrome9-texkill-schedule-corpus.tsv`](chrome9-texkill-schedule-corpus.tsv).

## Guarded live discard oracle

The constant-driven initialized program is the preferred first live shape:
job-local c0 can contain finite positive or negative values while v0 supplies
the visible output.  First probe all-positive c0, then one negative lane at a
time, including W rather than assuming which lanes participate.  Include +0
and -0 boundary cases only after ordinary positive/negative behavior works.

Use command 19's mandatory fresh D16, depth clear 1, LESS/write, and guarded
color destination.  Draw a near red candidate at z=0.25 with one negative c0
lane, followed by a farther green candidate at z=0.5 with all-positive c0.
Correct discard of both color and depth ends green.  Failure to discard ends
red; color-only suppression with an incorrect depth write leaves the original
sentinel because the green draw then fails depth.  Repeat for X/Y/Z/W and
guard surrounding pixels.

On 2026-09-23, VX900 `1106:7122` with kernel `7.2.5-t2` and module SHA-256
`d3a75c8d68d558fbb341b6c8ddb403a65184033a11e98243f3782c8f4e3035cb`
passed `tools/re/chrome9_texkill_guard_smoke.c` on the render node.  Its eight
finite cases were all-positive, one negative X/Y/Z/W lane each, and replicated
below/equal/above-zero predicates.  A killed near red draw followed by an
accepted farther green draw checked both color and D16 depth; each result
matched, buffer guards and immutable inputs survived, and no engine timeout
was logged.  This establishes all-four-lane negative rejection, zero survival,
and color/D16 suppression for the tested finite constants.  It does not yet
establish sampled-alpha execution through Mesa, stencil suppression, NaN or
infinity behavior, helper/demote semantics, or arbitrary Boolean conditions.
Mesa therefore enables only the specific sampled-alpha-less-than-half
lowering behind both a kernel capability and an explicit experimental opt-in.
On the same VX900, the guarded Gallium A8 test passed nearest `0x7f` below
half (discard) and `0x80` above half (survive), with correct color, D16,
guards, and unchanged A8 source.  A linear-filtered `0x7f`/`0x80` midpoint
read back as alpha `0x80` in BGRA8 but was discarded by TEXKILL.  The packed
8-bit readback does not reveal the internal filtered value's sign relative
to half; this is a diagnostic, not evidence for exact-equality semantics.
The VX900 3D manual, part II, printed pp. 108 and 127, says texture alpha
always uses high-bit extension to s1.10.  Its 8-to-11 conversion gives
`C11 = 4*C8 + floor(C8/64)` below 255: `0x7f` becomes 509/1024 and `0x80`
becomes 514/1024.  Their ideal halfway filter value is 511.5/1024, just
below half if the filter truncates to code 511.  The observed discard is
consistent with that pipeline, while the separately rounded BGRA8 output can
still be `0x80`; the exact filter rounding has not been measured.
A bounded precision discriminator should keep the identical filtered sample
and replicated `sample-ref` SUB/TEXKILL sequence, then use separate guarded
draws with exactly representable float references `510/1024`, `511/1024`,
and `512/1024`.  Equality survives the already-qualified finite KILL
lowering, so the survive-to-discard transition bounds the internal result in
1.10 units without treating an 8-bit render-target value as the sample.  This
is only a sampler-precision oracle: the production GL predicate remains the
strict `sampled_alpha < 0.5` and must not be biased to imitate BGRA8 readback.
The raw constant zero-predicate test above separately proves zero survival.
Broader sampled formats, stencil, and GL helper semantics remain pending.
The fixed A33
alpha-test path remains preferable for alpha-test emulation because it is
already silicon-proven and avoids this new shader side effect.
