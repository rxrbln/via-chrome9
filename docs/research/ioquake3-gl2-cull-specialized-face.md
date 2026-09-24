# ioquake3 GL2 alpha-mode and culled-face boundary

Status: current-source and exact linked-NIR audit, 2026-09-24.  This is an
offline compiler design.  No target draw was issued and no production source
was changed.

## Current first compiler stop

The historical one-vec4 fragment-uniform link failure is no longer the
current boundary.  Capability `FRAGMENT_CONSTANTS_32` exposes 32 vec4s or 128
components; the largest audited default ioquake declaration has 71
components.  Extended linkage exposes eight fragment inputs and eight
samplers.  The default renderer needs six smooth vec4 inputs and at most six
simultaneous samplers with logical indices through six, so those two resource
limits now fit.

The first unmodified default-lightall backend stop is its runtime integer
`u_AlphaTest` tree:

```text
mode == 1: terminate if alpha == 0
mode == 2: terminate if alpha >= 0.5
mode == 3: terminate if alpha < 0.5
```

The linked NIR is a straight-line block with three `terminate_if`
intrinsics.  The current generic inlinable-uniform walker finds uniform leaves
in ordinary control flow but not in those predicates.  The unspecialized
program therefore reaches semantic lowering with all three side effects and
rejects before FACE or arithmetic allocation.  Existing ioquake packed
generic and reduced-lightmap classifiers handle this exact tree only for
their closed one- and two-texture shapes; they do not make arbitrary
five-sampler lightall executable.

The generic closure is to include direct constant-buffer-zero leaves reached
from `terminate_if` predicates in the fragment variant key.  Over-keying is
safe: after inlining and normal constant folding, mode zero must remove all
three side effects; any remaining predicate still passes through the normal
fail-closed compiler.  Missing/short/indirect storage and unknown integer
values reject before draw.  Modes one through three remain separate alpha-
test qualification work; mode zero is the first useful default-world gate.

## Next exact stop: FACE

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

The implementation must clone the private fragment variant at draw time,
constant-fold only the recognized FACE value, and key/cache it by the
semantic cull result.  It must not mutate canonical NIR or the application's
rasterizer object.  Cull-none stays unsupported until the COLOR/BCOLOR or a
native BFC route is qualified.  An arbitrary FACE use may use the same
constant only under the same cull proof; without that proof it still rejects.

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
unscheduled live-set peak is 15 vectors at record 67.  Six active coordinate
inputs select a hardware resource bucket limited to six physical temporary
vectors.  Culling-specialized FACE removes the semantic-system-value stop but
does not prove this program allocatable.

The next major generic backend slice is therefore dependency-aware scheduling
plus channel-granular allocation/source rewriting.  The proprietary Linux
compiler has explicit `TryAllocChn_invi`, `RenameSrcChannel_invi` and
`stmPPS_ShrinkSrcChn_invi` passes, which is positive evidence for channel
rather than whole-vector allocation.  Promotion requires a final object with
at most 96 ALU, exactly five TEX, correct port classes, no undefined lanes,
and physical high-water at most six.  Default lightall must remain rejected
until that object and a guarded distinct-color pixel oracle both pass.

## Evidence

- Exact default lightall sources:
  `/tmp/ioq_default_lightall.{vert,frag}`; fragment SHA-256
  `7913798a3f3299f5df065c2523cb15f4ad01511241c516df70e4c7b2bc9cc2d6`.
- Mode-zero linked NIR BLAKE3:
  `b74fa228 0463e837 19ff85e8 95d43a83 eeab1fd8 4f324902 fb56f997 33ce7ba0`.
- Exact resource replay:
  `/tmp/chrome9-lightall-resource-probe` and
  `ioquake3-gl2-eight-unit-linkage.md`.
- Draw ordering and two-sided marker proof:
  `chrome9-draw-twoside-face-sign.md`.
- Native vFace negative differential:
  `chrome9-vface-normal-sign-boundary.md`.
