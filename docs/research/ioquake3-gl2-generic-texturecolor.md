# Minimal native GL2 path for ioquake3 generic and texturecolor

Status: patch-ready design from source-exact ioquake semantics, current
kernel/Mesa packet layout, three-driver static reverse engineering, and Wine
compiler differentials, 2026-09-21. The existing one-texture MODULATE,
blend, alpha-test and D16 components have live evidence. The native c0 MUL
and nonzero c0 payload have not yet run on VX900 silicon and remain gated by
the readback sequence below.

## Result

The first two ioquake3 GL2 fragment families need only two closed native
programs. They do not need a general shader instruction UAPI:

```text
GENERIC       out.rgba = texture0.rgba * primary.rgba
TEXTURECOLOR  out.rgba = texture0.rgba * c0.rgba
```

GENERIC is the already-live one-texture MODULATE object. Its runtime integer
`u_AlphaTest` is CPU-specialized to disabled alpha test for value zero and
fixed A33 GREATER/zero for value one. Values two and three remain rejected
until the exact `0x200` half-reference discriminator has run.

TEXTURECOLOR changes only the first native MUL's second source from normalized
diffuse `v0` to native float constant `c0`, and fills the existing four-dword
c0 store. The TAU, scheduler, output MOV, sampler state, VS, FVF and body
length are unchanged. This is the smallest new hardware delta that preserves
the arbitrary float precision of ioquake's `u_Color`.

The kernel continues to own all native words. Userspace supplies a semantic
program id, structured draw state and IEEE constant values.

## Exact program objects

Both keys retain the current one-texture control tuple and sampler operation:

```text
A90..A9a  90018000 91000001 92010002 93000001
           94000000 95000000 96000000 97000000
           98000000 99000000 9a000000
tail       cccccccc dddddddd
TAU        301001b0
```

GENERIC uses the live MODULATE ALUs:

```text
MUL tex0*v0  0140b6d0 30040053 00f20005 0000007f
MOV oC0      01c00000 301c0000 00f20105 000d083f
```

TEXTURECOLOR uses the Xorg/Linux-closed native-c0 form:

```text
MUL tex0*c0  0140b6d0 30080053 00f20005 0000007f
MOV oC0      01c00000 301c0000 00f20105 000d083f
```

Thus the only ALU differential is dword one of the first record,
`30040053 -> 30080053`. The exact scheduled c0 object comes from proprietary
Xorg `via_drv.so:0x32670`, with controls at `0x3272e..0x32795` and ALUs at
`0x32900..0x32b7e`. The independently hashed Windows D3D compiler produces
the same semantic `TEX0_C0` resource mapping; Linux configuration two closes
the dependency spelling used above. See
[ioquake-projected-texture-constant.md](ioquake-projected-texture-constant.md)
and [windows-closed-key-shader-corpus.tsv](windows-closed-key-shader-corpus.tsv).

For the first live implementation, keep the current H2 c0 packet rather than
introducing H3 at the same time:

```text
FE020440 0000043c 22030000 cccccccc <c0.x c0.y c0.z c0.w>
```

The current kernel emits this exact header with four zero payload dwords at
`chrome9_engine.c:982..983`. Replacing the payload preserves every packet
start and total body length. The full H3 c0..c31 upload remains the separately
gated `FRAGMENT_CONSTANTS_32` path documented in
[ioquake3-gl2-fragment-constants.md](ioquake3-gl2-fragment-constants.md).

## Version-one structured UAPI

Reserve private command `0x16` and capability bit 33 as recorded in
[chrome9-uapi-allocation.md](chrome9-uapi-allocation.md). Capability bit 34
separately means that Mesa may advertise all 32 fragment constant vectors.
Do not infer either capability from the presence of the other.

The new command is fixed-size and versioned. It unifies the existing
one-texture blend and D16-blend envelopes so the semantic program and its
constants are one atomic job:

```c
#define DRM_CHROME9_DRAW_TEXTURED_PROGRAM_TRIANGLE 0x16

#define DRM_CHROME9_CAP_DRAW_TEXTURED_PROGRAM (1ULL << 33)
#define DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32 (1ULL << 34)

#define DRM_CHROME9_PROGRAM_DRAW_DEPTH       (1U << 0)

#define DRM_CHROME9_FRAGMENT_PROGRAM_TEX0_PRIMARY 0
#define DRM_CHROME9_FRAGMENT_PROGRAM_TEX0_C0      1

#define DRM_CHROME9_FRAGMENT_CONSTANT_COUNT 32

struct drm_chrome9_fragment_constants_v1 {
        __u32 used_mask;
        __u32 reserved0;
        __u32 value[DRM_CHROME9_FRAGMENT_CONSTANT_COUNT][4];
        __u64 reserved[2];
};

struct drm_chrome9_draw_textured_program_triangle {
        __u32 version;              /* must be 1 */
        __u32 flags;
        __u32 program;
        __u32 destination_handle;
        __u32 depth_handle;         /* zero without DEPTH */
        __u32 width;
        __u32 height;
        __u32 destination_pitch;
        __u32 depth_pitch;          /* zero without DEPTH */
        __u32 destination_format;
        __u32 reserved0;
        __u32 reserved1;
        struct drm_chrome9_texture_state texture;
        struct drm_chrome9_blend_state blend;
        struct drm_chrome9_textured_vertex vertices[3];
        struct drm_chrome9_scissor_state scissor;
        struct drm_chrome9_fragment_constants_v1 constants;
        __u64 reserved[4];
};
```

The ABI offsets and sizes are fixed:

| offset | bytes | field |
|---:|---:|---|
| 0 | 4 | `version` |
| 4 | 4 | `flags` |
| 8 | 4 | `program` |
| 12 | 36 | destination/depth geometry and two reserved dwords |
| 48 | 36 | `texture` |
| 84 | 40 | `blend` |
| 124 | 96 | `vertices` |
| 220 | 12 | `scissor` |
| 232 | 536 | `constants` |
| 768 | 32 | final reserved array |

The request is exactly 800 bytes on all supported ABIs. On x86 its write-only
ioctl value is `0x43206456`. Add static assertions for the 536-byte constant
record and 800-byte request in both kernel and Mesa builds.

The first implementation accepts only these exact masks:

| program | `used_mask` | sampler count | interpolant |
|---|---:|---:|---|
| `TEX0_PRIMARY` | `00000000` | 1 | normalized primary plus S/T |
| `TEX0_C0` | `00000001` | 1 | S/T only; D1 is ignored |

Every unused constant dword, `constants.reserved0`, every constant-record
reserved qword, both header reserved dwords and every final reserved qword
must be zero. `TEX0_C0` initially rejects any c0 lane with exponent `0xff`.
`TEX0_PRIMARY` requires all 32 vectors' 128 payload dwords to be zero. No native
instruction, scheduler word, TAU word or selector enters UAPI.

`flags` accepts only `PROGRAM_DRAW_DEPTH`, the existing depth-function field
at bits 8..10 and `DEPTH_READ_ONLY` at bit 11. Without DEPTH, the depth handle,
pitch and all depth bits must be zero. With DEPTH, require an explicit LESS or
LEQUAL value, a nonzero distinct guarded D16 handle, canonical pitch and the
existing depth capabilities. Do not retain the old implicit-zero-means-LESS
compatibility in this new command.

The embedded blend state uses the existing validator. Its canonical disabled
form is flags zero, RGB/alpha factors ONE,ZERO, both equations ADD, requested
color mask, and alpha function/reference zero. For GENERIC, alpha test is
initially either disabled or exactly GREATER/ref zero. TEXTURECOLOR may use
the already-supported fixed alpha-test combinations because its shader has no
internal discard tree.

## Kernel packet mapping

The handler should lower the request to the existing internal one-texture
draw request, retain the current `drm_exec`/reservation/fence/quarantine
lifetime, and select one of the two kernel arrays above. Copy c0 into a local
four-dword array before ring publication. Validation failure must occur before
BO binding or publication.

The first implementation retains the current body sizes exactly. Let `D=4`
when D16 is enabled and zero otherwise. The state-complete blend envelope has
the current fixed `B=4`, so both programs are:

```text
body length = 228 + B + D = 232 + D dwords
```

The important invariant indices are unchanged:

```text
common A01             body[48]          no depth
common A01             body[48]          depth also; D words follow common
PS c0 H2 header        body[96 + D]
PS c0 selector         body[98 + D]      22030000
PS c0 payload          body[100+D .. 103+D]
fragment ALU H2        body[192 + D]
first MUL              body[196+D .. 199+D]
output MOV             body[200+D .. 203+D]
immediate vertex H2    body[204 + D]
```

No-depth keeps `A01=01ce1540`. D16 write uses `01ce154c`; D16 read-only uses
`01ce1548`. Both programs retain normalized diffuse FVF
`A02=020041d1`, texture-coordinate FVF `A03=030052d0`, texture image state
`A18=18001101`, and the current A20/A21/A22/A30 plus A31/A32 sampler lowering.
TEX0_C0 does not consume D1, but keeping the complete vendor-exact FVF and VS
closure avoids a second simultaneous state transition.

For `TEX0_C0`, require each submitted vertex color to be `ffffffff` and
specular to be zero in version one. Those values are semantically unused, but
canonicalizing them prevents stale or accidentally selected D1/D2 data from
masquerading as a successful c0 result. GENERIC accepts its actual primary
color and requires specular zero.

The constant record belongs to the job, just like vertex data. A
set-constants-then-draw ioctl is unsafe because another context could replace
persistent c0 between those operations.

## Mesa classifier and draw-time values

The current classifier hard-codes constant index zero and converts it through
UNORM8 D1. The native path needs two small pieces of shader metadata:

```c
struct chrome9_fs_key {
        __u8 program;
        __u8 sampler_unit;       /* must be zero initially */
        __u8 texcoord_semantic;  /* linked GENERIC/TEXCOORD index */
        __u8 color_semantic;     /* GENERIC/COLOR index or ff if unused */
        __u32 c0_byte_offset;    /* TEXTURECOLOR only */
        __u32 alpha_byte_offset; /* GENERIC only */
};
```

The exact C spelling is not ABI; the recorded facts are. Do not assume that a
GLSL variable named `var_Color`, `var_DiffuseTex` or `var_Tex1` becomes a
particular `VARn`. Inspect each NIR input's `io_semantics.location`, record the
linked index, and require the vertex shader to export that exact semantic.
The software draw module then repacks linked outputs into the fixed hardware
vertex:

```text
D0 = screen x, y, z, reciprocal-W
D1 = GENERIC/COLOR selected as primary, packed AARRGGBB and normalized by d1
D2 = 00000000
D3 = selected texture coordinate S, T
```

GENERIC requires both a full four-lane primary varying and a distinct
two-component texture coordinate. TEXTURECOLOR requires only the coordinate;
Mesa writes canonical D1=`ffffffff`, D2=`00000000` after post-TNL packing.
Both require finite positive RHW and finite S/T. The current VS program and
`A01=01ce...` provide perspective interpolation; normalized 2-D coordinates
go to ordinary TAU TEXLD. Projectors, offsets, bias, explicit LOD, shadow and
array sampling do not alias either key.

For TEXTURECOLOR, accept one direct full vec4 load from fragment constant
buffer zero at a compile-time byte offset aligned to 16. The NIR helper must
record the real byte offset rather than insisting that the application placed
the uniform in physical slot zero. At draw time:

1. validate `buffer_offset + c0_byte_offset + 16` without overflow against
   `buffer_size` and the backing resource width;
2. map either `user_buffer` or the existing CPU-mapped PIPE_BUFFER;
3. copy the four IEEE dwords bit-exactly into hardware `value[0]`;
4. set `used_mask=1` and zero the rest of the record; and
5. reject non-finite lanes during the first-live phase.

This repacks the selected GLSL uniform to native c0; it does not quantize it
through D1. Samplers occupy Gallium's sampler namespace and are not part of
the float constant mask. Require one ordinary sampler/view at unit zero and
the existing BGRX/BGRA level-zero texture constraints.

For GENERIC, recognize the complete texture-times-primary expression plus the
exact ioquake alpha-test tree. Record the direct constant-buffer-zero integer
load's aligned byte offset. At each draw, bounds-check and `memcpy` that dword:

- value 0: clear fixed alpha-test enable;
- value 1: enable GREATER with reference zero, yielding `A33=33004000`;
- values 2 and 3: reject until exact-half A33 is live-proven;
- every other value, indirect load, alternate compare or extra discard:
  reject before the ioctl.

The integer alpha-mode is CPU specialization metadata, not a PS float
constant. GENERIC therefore submits `used_mask=0` and an all-zero constant
record. Shader state records offsets; mutable values are always read at draw
time.

## Closed validation matrix

The kernel must additionally require:

- version one and no unknown flags or semantic program id;
- canonical destination, texture and optional D16 pitches and guarded native
  shmem layouts;
- distinct target, source and depth BOs with existing reservation ordering;
- one BGRX8888 or BGRA8888 source, one BGRX/BGRA destination, level zero,
  current filter/wrap enum bounds and finite normalized coordinates;
- nondegenerate triangle, in-range scissor, finite positive RHW and existing
  fixed-point coordinate/depth bounds;
- the selected program's exact used mask and canonical unused values; and
- all failures before publication, with post-publication timeout retaining
  the existing wedge/quarantine behavior.

Capability bit 33 alone must not change Gallium's reported fragment constant
size. Only bit 34 permits `max_const_buffer0_size=512` and 128 fragment
uniform components. Conversely, bit 34 alone does not authorize command
`0x16`; Mesa needs both for native TEXTURECOLOR execution.

## Guarded promotion sequence

Every positive test uses guarded BGRA target/source BOs, known outside prefill,
readback, guard verification, fence retirement and MMIO error checks. Run a
known-good current MODULATE draw after each new case.

1. **Unchanged GENERIC mode zero:** distinct BGRA texture and primary colors;
   exact product proves command `0x16` did not disturb the live object.
2. **GENERIC mode one:** source alpha strip `00,01,7f,ff`; alpha-zero samples
   preserve the prefill and all nonzero samples draw. Repeat off/on/off to
   prove A00/A33 closure.
3. **Native c0 transport:** texture `804080ff`, c0 lanes containing only zero
   or one, with expected componentwise product. This isolates source selection
   from float rounding.
4. **Precision discriminator:** c0 `(0.2,0.6,0.5,1.0)` over a distinct texture.
   Compare against one final UNORM render quantization. Choose texels for which
   the old D1 byte-grid path differs by at least one channel.
5. **State transitions:** GENERIC/c0/GENERIC with different c0 payloads, then
   c0/no-depth, c0/D16-write, c0/D16-read-only and c0/no-depth. Verify color
   and depth after every draw.
6. **Blend:** native c0 alpha one-half, BGRA destination and already-proven
   source-over factors. This proves that shader alpha reaches the independent
   blend-alpha path rather than stale D1 alpha.

Negative tests cover every mask mismatch, nonzero reserved/unused field,
NaN/Inf c0, short or overflowed constant buffer, absent/misaligned semantic,
wrong sampler unit, extra texture/ALU/discard, noncanonical TEX0_C0 D1/D2,
unknown program, depth state without DEPTH and aliased BO handles. Snapshot
the generated segment once: only selector `22030000` may carry c0, and the
first ALU must be exactly one of the two rows above.

Passing this sequence promotes only these two keys. It does not validate
general PS constants, arbitrary NIR ALU, native flow control, projected
coordinates, mip LOD or additional samplers.

## Evidence and confidence

The exact hashes and backend offsets are collected in
[ioquake3-gl2-fragment-constants.md](ioquake3-gl2-fragment-constants.md) and
[ioquake3-gl2-ps-next-rungs.md](ioquake3-gl2-ps-next-rungs.md). Program words,
the c0 selector, current packet indices and body sizes are **code/compiler
exact**. The ioquake expressions and uniform roles are **source exact**. The
semantic-index recording and UAPI are an **implementation design**. Nonzero
native c0 payload, the `30080053` scheduled object on VX900, and its combined
D16/blend transitions remain **provisional until measured**.
