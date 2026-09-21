# Chrome9 fragment constants for the ioquake3 GL2 gate

Status: static three-driver packet closure plus fresh offline Wine compiler
differentials, 2026-09-21. No new packet in this note has run on VX900
silicon. The implementation and smoke sections are deliberately bounded.

## Result

Chrome9 has 32 API-visible PS 2.0 float constants, not one. Each constant is
a raw four-dword vector. The store is split around two hardware-owned ranges:

```text
API c0..c15   -> native ids  0..15 -> dword addresses 00..3f
API c16..c31  -> native ids 32..47 -> dword addresses 80..bf
```

The exact variable-length packet is:

```text
FE03043C <dword-count> <selector> CC000000 <payload...>

selector(cN), N < 16  = 22030000 | (4*N)
selector(cN), N >= 16 = 22030080 | (4*(N-16))
count(K vec4)          = 4*K
```

A run crossing c15/c16 must be split. For example, c15 and c16 are two
four-dword packets under `2203003c` and `22030080`; one eight-dword packet
under `2203003c` would overwrite the first internal vector.

The strongest safe implementation is therefore two complete user-bank
packets, with unused API constants explicitly zeroed:

```text
FE03043C 00000040 22030000 CC000000 <c0..c15, 64 dwords>
FE03043C 00000040 22030080 CC000000 <c16..c31, 64 dwords>
```

Never write user data through `22030040`, `2203005c`, `220300c0`, or
`220300dc`. Those selectors are proprietary-driver internal stores. Never
accept c32+: the compiler maps c32..c47 into native ids 48..63, colliding
with the second internal/gap range, and wraps c48..c63 onto native ids 0..15.

The current four-dword c0 packet is consequently a bring-up restriction, not
a hardware limit. Mesa's `max_const_buffer0_size = 16` becomes exactly four
fragment uniform components in `st_extensions.c`. The native user capacity is
`32 * 4 * sizeof(float) = 512` bytes, which becomes 128 components.

This storage result does not make ioquake3 GL2 executable by itself. It
removes the current link-time resource lie. The first runtime programs still
need their own verified ALU/TEX/control-flow keys, more samplers, and the
integer alpha-test specialization described below.

## Exact provenance

The artifacts used here are:

| artifact | SHA-256 |
|---|---|
| Windows D3D UMD `VTGUModeDX32A.dll` | `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| Windows OpenGL ICD `VTGOGL32A.dll` | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` |
| Linux proprietary `via_chrome9_dri.so` | `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` |
| Wine D3D compiler wrapper | `a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c` |

The D3D PS 2.0 updater at
`VTGUModeDX32A.dll:0x1015c163..0x1015c21a` emits count `0x40`, selectors
`22030000/22030080`, and copies 64 dwords from software stores separated by
`0x100` bytes. Its fallback copy at `0x1015c286..0x1015c33a` emits the same
two user banks. It then emits the internal 28-dword stores at
`0x1015c363..0x1015c41a` under `22030040/220300c0`.

The OpenGL ICD is field-for-field identical at
`0x10206883..0x1020693a`, `0x102069a6..0x10206a5a`, and
`0x10206a83..0x10206b3a`. This is an independent executable, not a shared
symbol in the D3D DLL.

Linux `stmUpdatePS_inv` independently constructs sparse stores. At
`via_chrome9_dri.so:0x0017ffc5..0x0018009d`, it writes
`count = vector_count * 4`, selects `22030000 | (native_start * 4)`, and
advances four payload dwords per vector. The PS 1.x branch calls
`INV_FloatToC1_Float`; the PS 2.0 branch at
`0x00181165..0x0018117d` copies the original 16-byte vectors. Full-bank and
internal-store paths at `0x00180222`, `0x00180472`, `0x001807aa`, and
`0x00180c55` reproduce the same selector/count layout.

The current kernel's `chrome9_emit_header2(..., 22030000, ..., 4)` is a
live-used fixed c0 spelling, but it carries no explicit payload count. Use
the proprietary H3 form above for a variable or full-bank successor. H3
syntax is already present in the working stream for VS constants; only the
PS selector/count combination remains to be qualified live.

## Offline compiler boundary differential

`tools/wine_shader_probe.c` was run under Wine 11.10 against the exact D3D
UMD. Each probe was `ps_2_0; mov oC0,cN; end`, followed by the vendor
scheduler/finalizer. The decisive source field is:

| API source | final native ALU | native id |
|---:|---|---:|
| c0 | `01c00530 301c0053 00f08005 000d0820` | 0 |
| c15 | `01c00530 301c0053 00f08f05 000d0820` | 15 |
| c16 | `01c00530 301c0053 00f0a005 000d0820` | 32 |
| c31 | `01c00530 301c0053 00f0af05 000d0820` | 47 |
| c32, invalid | `01c00530 301c0053 00f0b005 000d0820` | 48 |
| c47, invalid | `01c00530 301c0053 00f0bf05 000d0820` | 63 |
| c48, invalid | `01c00530 301c0053 00f08005 000d0820` | 0 |
| c63, invalid | `01c00530 301c0053 00f08f05 000d0820` | 15 |

Three two-source probes prove that the mapping applies independently to both
operands and closes the discontinuity:

```text
mul oC0,c0,c1    0140b6d0 30081053 00f08005 00000820
mul oC0,c15,c16  0140b6d0 300a0053 00f08f05 00000820
mul oC0,c16,c31  0140b6d0 300af053 00f0a005 00000820
```

The exact tokens and SHA-256 values are retained in
`windows-ps-constant-index-corpus.tsv`. The compiler object itself has no
constant-use mask differential: apart from process pointers, c0 and c31
objects are identical. A new driver must derive the used mask from its
verified semantic key/NIR; it cannot trust vendor-object metadata to provide
one.

These compiler results are not silicon execution evidence. In particular,
the fact that malformed PS 2.0 tokens compile at c32 does not make c32 legal.

## What ioquake3 GL2 actually declares

The audited tree is ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`. Its live startup log
`/root/tmp/chrome9/ioquake3-gl1-probe-v1.log` reaches
`GLSL_InitGPUShaders()` and reports:

```text
error: Too many fragment shader default uniform block components
error: Too many fragment shader uniform components
ERROR: shaders failed to link
```

The source-declared non-sampler fragment components below are before linker
packing and dead-uniform elimination. Samplers are separate from the native
PS float store.

| fragment source | non-sampler components | important form |
|---|---:|---|
| `generic_fp`, `dlight_fp` | 1 | runtime integer `u_AlphaTest` |
| `texturecolor_fp`, `fogpass_fp` | 4 | one vec4 color |
| `down4x_fp` | 2 | inverse texture resolution |
| `bokeh_fp`, `calclevels4x_fp` | 6 | vec4 color plus vec2 resolution |
| `tonemap_fp` | 9 | vec4 color, vec2 exposure, vec3 tone levels |
| `shadowfill_fp` | 5 | vec4 origin plus float radius |
| `pshadow_fp` | 14 | three vec3 directions, vec4 origin, float radius |
| `ssao_fp`, `depthblur_fp` | 4 | one vec4 view tuple |
| `lightall_fp` | 1..23 | alpha-test plus feature-dependent lighting vec3/vec4 values |
| `shadowmask_fp` | 23 or 71 | one or four mat4 values, vec3 origin, vec4 view tuple |

Default `r_shadowCascadeZFar=1024` selects the 71-component shadowmask form.
Default normal/specular mapping also selects lightall forms well beyond one
vec4. All are within the recovered 128-component user constant store. They
are not within the current four-component cap.

The exact failing program is not printed in the retained log, so do not label
the first error as a particular source file. The source audit is sufficient
to prove that raising the cap only to c0/c1 would still be false for startup.

`u_AlphaTest` needs special treatment. A D3D PS float constant packet carries
raw dwords, but the recovered native constant source is a floating-point
operand; it is not proof of integer compare/control-flow semantics. The safe
ioquake lowering is to recognize the complete alpha-test branch shape, read
the CPU-visible uniform value, and specialize values 0..3 into disabled or
fixed A33 alpha test. Unknown values and non-exact branch shapes reject.
Merely uploading the integer bits to cN does not implement the shader.

## Versioned structured UAPI plan

Do not add a standalone `SET_FRAGMENT_CONSTANTS` ioctl. Constant state is
persistent hardware state, and a set-then-draw interface can be interleaved
with another file/context or overwritten by a later program template. Keep
constants in the same validated job as the program and draw.

Define one reusable fixed-size record for new program-draw successors:

```c
#define DRM_CHROME9_FRAGMENT_CONSTANT_COUNT 32

struct drm_chrome9_fragment_constants_v1 {
        __u32 used_mask;       /* bit N means API cN is consumed */
        __u32 reserved0;
        __u32 value[DRM_CHROME9_FRAGMENT_CONSTANT_COUNT][4];
        __u64 reserved[2];
};
```

Add new untextured and one-texture program-draw ioctls rather than changing
the sizes of existing ioctls or reinterpreting their inconsistent reserved
tails. Each new request embeds its existing blend/scissor/destination/
texture/vertex state plus the record above. Its `program` remains a semantic
enum selected from canonical kernel templates; it is never native ALU bytes.
The first enum set should be only:

```text
C0_REPLACE              expected mask 00000001
C0_C1_MODULATE          expected mask 00000003
TEXTURE0_C0_MODULATE    expected mask 00000001
```

Later ioquake-specific keys may reuse the same record. For every request the
kernel must:

1. require all reserved fields to be zero;
2. require `used_mask` to equal the selected key's exact mask, not merely be
   a superset;
3. require every unused value dword to be zero;
4. initially reject exponent `0xff` in every consumed floating lane;
5. regenerate the exact canonical program and its constant mask;
6. copy constants once before publication and keep them in the job-owned
   packet; and
7. emit constants after common/sampler state but before PS program stores and
   immediate vertices.

The finite-only rule is a first-live safety gate, not an architectural
conversion. Both proprietary PS 2.0 paths copy IEEE dwords unchanged. It can
be widened only after NaN/Inf numerical behavior is tested; never apply the
PS 1.x C1 clamp.

Use two capability stages so Mesa does not overstate what a partially updated
kernel accepts:

```text
FRAGMENT_CONSTANTS_2   c0/c1 packet plus the three closed keys
FRAGMENT_CONSTANTS_32  full two-bank upload and 512-byte binding contract
```

With only the first capability, Mesa may execute c0/c1 keys but must retain
its current advertised size. With the second, set
`fs->max_const_buffer0_size = 32 * 4 * sizeof(float)` and retain
`max_const_buffers = 1`. Unsupported NIR remains an unsupported shader/draw;
the larger resource cap must not turn the closed classifier into a wildcard.

For the first c0/c1 implementation emit exactly:

```text
FE03043C 00000008 22030000 CC000000 <c0, c1>
```

This is 12 dwords and remains 16-byte aligned, so all following packet starts
move by exactly four dwords relative to the current four-dword c0 payload.
For the 32-constant capability, emit both complete 68-dword bank packets
shown at the start of this note. This deterministic full upload costs 136
dwords total (128 payload plus eight header dwords), or 544 bytes. It prevents
dirty-range and stale-state mistakes. The current page-sized generated bodies
still have ample space, but every builder must retain its explicit
end/read-ahead bound check.

Mesa must copy the bound constant buffer bit-exactly after checking buffer
zero, `buffer_offset` 16-byte alignment, overflow, and sufficient
`buffer_size`/resource width for the key's highest vec4. Do not CPU-pack a
native-uniform key through D1. The existing D1 route remains valid only for
its already documented byte-grid subset.

## Guarded smoke sequence

Use a guarded BGRA8888 target, nearest/no-texture state as appropriate, one
triangle covering a known interior sample, a known outside prefill, and full
cookie/guard/MMIO retirement checks after every draw.

1. **c0 MOV:** upload c0 `(1,0,1,1)` and run `C0_REPLACE`. Interior must be
   `ffff00ff` (magenta).
2. **c0/c1 simultaneous:** c0 `(1,1,0,1)`, c1 `(1,0,1,1)` under
   `C0_C1_MODULATE`. Interior must be `ffff0000` (red). This distinguishes a
   real eight-dword upload from either stale vector.
3. **texture*c0:** use a BGRA texel with distinct lanes and c0 containing only
   zero/one lanes. Compare against the exact componentwise product; do not use
   0.5 in the first oracle because output rounding is a separate question.
4. **bank boundary diagnostic:** an oracle-only `mul oC0,c15,c16` uses exact
   compiler words `0140b6d0 300a0053 00f08f05 00000820`. Put yellow in c15
   and magenta in c16; result must be red. This must run only after c0/c1 is
   clean and with the two-bank packet generated by the kernel.
5. **last legal vector:** `mov oC0,c31` with blue produces `ff0000ff`.
6. **transition closure:** run c0-only, c0/c1, no-constant texture, c31, then
   c0-only again with different values. Every constant-consuming job uploads
   both complete user banks in the full-capability path.

Negative tests reject before ring publication: nonzero reserved fields,
mask/key mismatch, any nonzero unused value, short or offset-overflowed
constant buffer, NaN/Inf during the finite-only phase, unknown program enum,
and any NIR constant index above c31. Snapshot the generated segment once to
confirm both selectors/counts and that no `22030040/5c/c0/dc` packet exists.

Passing these vectors proves constant transport and bank selection only. It
does not prove ioquake's branches, discard, shadow samplers, normalize/dot
ALU, or default five-plus-texture lightall variants.
