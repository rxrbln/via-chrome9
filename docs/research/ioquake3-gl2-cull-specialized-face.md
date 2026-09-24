# ioquake3 GL2 alpha-mode and culled-face boundary

Status: current-source and exact linked-NIR audit, 2026-09-24.  Direct-UBO
terminate-leaf and constant-FACE specialization are implemented and
host-tested.  No target draw of default lightall was issued.

## Alpha mode zero is closed offline

The historical one-vec4 fragment-uniform link failure is no longer the
current boundary.  Capability `FRAGMENT_CONSTANTS_32` exposes 32 vec4s or 128
components; the largest audited default ioquake declaration has 71
components.  Extended linkage exposes eight fragment inputs and eight
samplers.  The default renderer needs six smooth vec4 inputs and at most six
simultaneous samplers with logical indices through six, so those two resource
limits now fit.

The first unmodified default-lightall backend stop was its runtime integer
`u_AlphaTest` tree:

```text
mode == 1: terminate if alpha == 0
mode == 2: terminate if alpha >= 0.5
mode == 3: terminate if alpha < 0.5
```

The linked NIR is a straight-line block with three `terminate_if`
intrinsics.  Mesa commit `04a35c3` extends the generic inlinable-uniform
walker to collect only direct constant-buffer-zero leaves reached from those
predicates.  Its standalone exact-shape test proves that mode zero folds all
three side effects away, while modes one, two, and three retain exactly one
dynamic terminate each.  Indirect addressing, a nonzero UBO, and unrelated
dynamic values do not enter the variant key.

This remains fail closed at integration boundaries: after inlining and normal
constant folding, any remaining predicate still passes through the normal
unsupported-operation path.  Missing/short storage and unknown integer
values reject before draw.  Modes one through three remain separate alpha-
test qualification work; only mode zero is closed by this slice.

## Constant FACE is closed offline

After exact mode-zero specialization, the linked program loads FACE for:

```glsl
vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
```

The semantic backend has no FACE source.  The proprietary compiler's
out-of-profile vFace object aliases vPos byte for byte, so POSITION/MISC is
not a face implementation.  The full software two-sided COLOR/BCOLOR marker
described in `chrome9-draw-twoside-face-sign.md` remains the general fallback
for no-cull draws.

There is, however, a smaller application-independent and conforming first
rung.  Gallium's rasterizer cull state gives a compile-time face constant for
every fragment which can survive a draw:

| `pipe_rasterizer_state::cull_face` | surviving FACE value |
|---|---|
| `PIPE_FACE_BACK` | true |
| `PIPE_FACE_FRONT` | false |
| `PIPE_FACE_NONE` | dynamic, reject this specialization |
| `PIPE_FACE_FRONT_AND_BACK` | no fragments; skip the draw |

This is not “forcing triangles front-facing.”  Chrome9 always requests the
draw pipeline because its kernel accepts triangles only.  Mesa draw performs
clipping, determinant classification and culling before the vbuf callback,
using the same `front_ccw` rasterizer convention which defines FACE.  When
one semantic face is culled, every published fragment has the opposite FACE
value even for clipped triangles.  `front_ccw` changes which physical winding
belongs to the semantic class, but not the table above.

Mesa commits `e5617b5` and `954a7ce` implement the target-independent NIR
part.  The helper replaces `load_front_face`, `load_front_face_fsign`, and the
boolean FACE input in either deref or IO-lowered `load_input` form, then runs
the same stable specialization fold used for uniform variants.  The
IO-lowered form matters for the exact linked lightall NIR.  Its standalone
test proves all four representations become `+1.0` for a surviving front
face and `-1.0` for a surviving back face.

Draw integration must still clone the private fragment variant, key/cache it
by the three-state semantic face mode, and call the helper only for FRONT or
BACK.  It must not mutate canonical NIR or the application's rasterizer
object.  Cull-none stays unsupported until the COLOR/BCOLOR or a native BFC
route is qualified.  An arbitrary FACE use may use the same constant only
under the same cull proof; without that proof it still rejects.

The host regression uses opposite-winding triangles, toggles `front_ccw`, and
includes one clipped triangle.  With BACK culling, every emitted fragment
variant must contain the true branch; with FRONT culling, the false branch;
with NONE, no constant-FACE variant may be selected.  The draw module's
published triangle count and surviving winding provide the proof without a
GPU submission.

## FACE is not the final lightall blocker

The exact mode-zero default permutation has five dense samplers, six smooth
varyings (23 live lanes), and three vec4 float-uniform groups.  Replacing FACE
with a separately supplied exact sign and applying the current qualified
SQRT lowering produces:

```text
semantic records  100 = 95 ALU + 5 TEX
varying mask       0x003f
sampler mask       0x1f
uniform mask       0x00000007
constant mask      0xe0000007
```

The current whole-vector physical allocator fails at semantic record 34; its
unscheduled live-set peak is 15 vectors at record 67.  The earlier six-temp
claim for six coordinate inputs was incorrect: the valid zero resource bucket
handles the six-input/ten-temp production compiler differential.  The
current kernel still limits allocation to twelve physical vectors.
Culling-specialized FACE removes the semantic-system-value stop but does not
prove this program allocatable.

The next major generic backend slice is dependency-aware semantic scheduling
followed by a whole-vector allocation replay.  The isolated Mesa scheduler
now reorders the same 100 records without cloning or deleting an operation:
95 ALU, five TEX, and an allocate-before-release peak of nine live vectors.
Its fail-closed domain is one terminal full-mask COLOR write, no KILL, no dead
record, disjoint partial TEMP definitions, and a complete TEMP dependency
DAG.  It therefore does not move side effects or silently discard work.
Channel-granular allocation/source rewriting is not yet shown necessary for
this permutation.
The proprietary Linux compiler has `TryAllocChn_invi`,
`RenameSrcChannel_invi` and `stmPPS_ShrinkSrcChn_invi` passes for broader
packing.  Promotion requires a final object with at most 96 ALU, exactly
five TEX, correct port classes, no undefined lanes, and physical high-water
at most twelve.  An end-to-end offline replay now runs the scheduled semantic
object through the current Mesa native builder and current kernel compiler:
95 ALU, five TAU, physical `p0` through `p8`, and one final-use marker for
every physical TEMP (`0x1ff`).  Its resource controls include
`A92=9205005f` and `A93=9300001f`, matching six varying fields and five
samplers.  No allocator, port, or upload-capacity blocker remains in that
replay.  Mesa commit `fde983a` invokes a bounded deterministic scheduler only
after the original order exceeds twelve live TEMP vectors.  The exact
100-record corpus remains 95 ALU plus five TEX and schedules from peak 15 to
peak nine without deleting or cloning an instruction.  This is committed host
evidence, not a silicon result.  Default lightall must remain unqualified
until the complete integrated object and a guarded distinct-color pixel
oracle both pass.

## Six-texture sunlight/shadow object

The default sunlight/shadow lightall permutation is a distinct, tighter
object.  Its linked NIR contains ordinary `fmad`, seven live source negations,
and nine saturations.  Retaining separate MUL and MOV.SAT instructions exceeds
the 96-ALU upload envelope.  Mesa commit `cecf7f6` implements four generic
compactions required by the exact object:

- map NIR `fmad` to the already qualified native MAD operation;
- fold `fneg` only into native source slots with a NEG encoding, retaining the
  explicit MUL(-1) fallback for the C slot and POSITION;
- fuse a single-use `fsat` into its non-TEX producer destination;
- rewrite the exact shared form
  `sat(max(x,.25))` to `sat(max(sat(x),.25))`, allowing both producer SATs to
  fuse.  This identity is valid for the finite, infinity and NaN cases of the
  same NIR min/max/saturate operations.

The resulting exact semantic corpus contains:

```text
records             101 = 95 ALU + 6 TEX
source TEMP peak    15
scheduled peak      12
SAT destinations    9
NEG sources         9
varying count       7
active TEX resource 7
native result       96 ALU + 6 TAU
```

The scheduler regression preserves the instruction multiset and terminal
COLOR write and produces the same output on repeated runs.  Replaying that
scheduled object through the current kernel compiler succeeds with physical
TEMP high-water 12, releases `p0..p11` (`0x00000fff`), and controls:

```text
9008007e 91800081 92060060 9300003f 94000000 95000840
96000000 97800000 98000082 99000000 9a000000 9b000000
```

The focused host tests also prove that three-distinct-constant MAD still
legalizes separately, multi-use saturation is not fused, and a negated native
C operand keeps the safe two-instruction fallback.  Native debug tests and the
matching cross Gallium build pass.  No six-texture sunlight/shadow packet has
been submitted to VX900 silicon yet; this section proves compiler capacity and
byte construction only.

## Evidence

- Exact default lightall sources:
  `/tmp/ioq_default_lightall.{vert,frag}`; fragment SHA-256
  `7913798a3f3299f5df065c2523cb15f4ad01511241c516df70e4c7b2bc9cc2d6`.
- Mode-zero linked NIR BLAKE3:
  `b74fa228 0463e837 19ff85e8 95d43a83 eeab1fd8 4f324902 fb56f997 33ce7ba0`.
- Exact resource replay:
  `/tmp/chrome9-lightall-resource-probe` and
  `ioquake3-gl2-eight-unit-linkage.md`.
- Exact scheduler corpus and result:
  `chrome9_ps_schedule_test.c`, mode-zero 100 records peak 15 to nine and
  sunlight/shadow 101 records peak 15 to twelve; cross Meson test
  `chrome9-ps-schedule` passes.  This is host evidence, not a GPU result.
- Exact six-texture scheduled semantic object:
  `/tmp/chrome9-shadow-scheduled-sem.txt`, SHA-256
  `69745d5e179574747b90e0998bf291f738c072ec5f9ef19e6036f74eb69e69ff`;
  native replay summary `/tmp/chrome9-shadow-native-replay.txt`, SHA-256
  `cb48d369318c3319325e5d4bc400c17145be1bfe53e422f53a4b7d1ae5cead45`.
- Native/kernel replay: `/tmp/chrome9-kernel-replay/replay.c`, SHA-256
  `16da371648c6146c20a74ddab9d99611d32c8e2c478a639e827da2be9c098df6`;
  replay binary SHA-256
  `afb966a9e3ee57c8e0e4d0f75e09d6113eda20626cf3da93dadd2512a7569a79`.
- Generic specialization tests: Mesa commits `04a35c3`, `e5617b5`, and
  `954a7ce`;
  `chrome9-specialise-test` passes under the matching T2 sysroot loader.
- Draw ordering and two-sided marker proof:
  `chrome9-draw-twoside-face-sign.md`.
- Native vFace negative differential:
  `chrome9-vface-normal-sign-boundary.md`.
