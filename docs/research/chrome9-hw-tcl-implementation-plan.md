<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 hardware TCL: implementation plan

Status: design, 2026-09-24.  Read-only; nothing here was run on the target.
It turns [chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md) (ISA, finalizer,
constants, streams, corpus [chrome9-hw-tcl-vs-corpus.json](chrome9-hw-tcl-vs-corpus.json))
into a kernel UAPI, a kernel validator/compiler, a Mesa lowering and a
silicon ladder.  Paths are relative to the kernel tree
`src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2` (`K/`) and the
Mesa driver `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/chrome9`
(`M/`).  Kernel line numbers refer to the working tree of today, which
another lane is still editing.

## Silicon baseline (today)

Passed on VX900 through `DRM_CHROME9_FIXED_QUAD` (`K/.../chrome9_engine.c:689`
`chrome9_fixed_vs_oracle`, `K/.../chrome9_drv.c:755`
`chrome9_emit_3d_quad_h6_internal`, `struct chrome9_quad_vs` in `chrome9_drv.h`):

* vendor `mov oPos,v0; mov oD0,v1; mov oD1,v2` plus the 5-record finalizer,
  rows 256..258 at `32030400` (sx=16, sy=-16, sz=1, ox=oy=15.5+786432),
  clip-space vertices with W=1 and W=2, one-triangle orientation;
* `m4x4 r0,v0,c0; mov oPos,r0` with user c0..c3 at `32030000`.

In progress: clip engine (`VS_CLIP_WIDE`, `VS_CLIP_NEAR`, `VS_CLIP_NEAR_TAIL`)
and a first memory-fetch quad (`VS_FETCH`, working tree only).

**Open issue in the clip oracle.**  `chrome9_drv.c:893` sets bit 11 in word 1
of the `fe020000` (FE) block, `01002521 -> 01002d21`.  Every other note
places the clipping-engine enable in the *type-01* A01 word (`01c01540`/
`01ce1540`, the word that also carries cull bit 7 and depth bits 3:2):
[native-clip-cull-list-fastpath.md](native-clip-cull-list-fastpath.md)
("complete A01 `01ce1d40`"), [raster-front-end-oracles.md](raster-front-end-oracles.md),
[fixed-depth-cull-stencil-contract.md](fixed-depth-cull-stencil-contract.md).
FE A01 is the texture-coordinate stage configuration
([chrome9-vs-varying-fvf.md](chrome9-vs-varying-fvf.md)), so bit 11 there is
most likely a texture-stage field, not clipping (inference).  Also,
`CLIP_WIDE` cannot tell the two apart: a ±4 quad covering the whole target
renders identically with or without clipping when the guard band holds.
Only `CLIP_NEAR` (one vertex with W<0) discriminates.  Re-run the clip rungs
with `state[]` word `01c01540 -> 01c01d40` before trusting either outcome.

## Key decisions

1. New ioctl `DRAW_VERTEX_PROGRAM` (`0x1e`, caps bit 58 immediate and 59
   buffers) that *embeds* the complete `struct drm_chrome9_draw_fragment_program`.
   cmd22 (`DRAW_FRAGMENT_PROGRAM`, `0x1d`) stays unchanged.  Update the
   stale ledger [chrome9-uapi-allocation.md](chrome9-uapi-allocation.md)
   (it still says `0x1d` is next).
2. Userspace sends a semantic, SSA, straight-line vertex IR modelled on
   `drm_chrome9_fragment_instruction`.  The kernel builds every native record,
   the finalizer, A60/A61/A62, declarations, stream words, constant headers,
   viewport-derived rows and A40..A48.
3. The output side stays byte-identical to the proven cmd22 linkage: oD0
   and oD1 are always written (kernel fills them when not consumed), and oT
   slots are the fragment program's dense packed varyings with the same
   widths.  A25/A26/A28/A29 and FE A01..A03/A08 therefore come unchanged from
   `chrome9_fragment_raster_state()` (`chrome9_engine.c:1943`) and
   `chrome9_fragment_fe_config()` (`:2143`).
4. No kernel lowering uses the scratch register: position always ends in
   one `MOV 0xa0.xyzw, rP` directly before the finalizer (the silicon-proven
   M4x4 form).  Matrices are four DP4s into temp lanes, not op 26.
5. No flow control, no MOVA/relative addressing, no LIT, no SINCOS in ABI
   v1.  Mesa unrolls or falls back.
6. Clip engine mandatory for the new ioctl: once positions are computed on
   the GPU the kernel can no longer run `chrome9_coordinate_valid()` /
   `chrome9_triangle_nondegenerate()` (`chrome9_engine.c:5452/5460`).  The
   capability stays behind an opt-in module parameter (`hw_tcl=1`) until the
   adversarial rung L12 passes.
7. Mesa lowers GL clip-space Z to the D3D `0 <= z <= w` convention with
   `nir_lower_clip_halfz`, because the clip engine's near/far planes are the
   D3D ones (fast-path note step 2).  Viewport rows use `sz=f-n, oz=n`.

## 1. Kernel semantic vertex program

### 1.1 What the kernel owns (security boundary)

| item | why kernel-owned |
|---|---|
| native records, finalizer, A60/A61/A62 | arbitrary words could address other register spaces |
| A00..A10, A12, A20, A23, A24, A2C..A2E, `3003` stream words | encode GPU addresses and fetch bounds |
| `32030000` header offset/count, rows 256..258, A40..A48 | rows 224..255 are shared internal state; A40..A48 feeds clipping |
| type-01 A01 bits 11/7, A23 bit 8 | clip/cull state must not leak between clients |
| index copy, min/max, per-element range check | the only guard against out-of-object vertex reads |
| constant read window | constants, temps and inputs persist in hardware across clients; any read of a row, temp lane or input not written in this draw is a cross-client read and is rejected |

### 1.2 UAPI (`include/uapi/drm/chrome9_drm.h`)

```c
#define DRM_CHROME9_DRAW_VERTEX_PROGRAM		0x1e
#define DRM_CHROME9_CAP_VERTEX_PROGRAM		(1ULL << 58) /* stage A */
#define DRM_CHROME9_CAP_VERTEX_BUFFERS		(1ULL << 59) /* stage B */
#define DRM_CHROME9_VERTEX_ABI_VERSION		1U
#define DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS	128U
#define DRM_CHROME9_VERTEX_MAX_RECORDS		128U  /* native, incl. finalizer */
#define DRM_CHROME9_VERTEX_MAX_CONSTANTS	256U
#define DRM_CHROME9_VERTEX_MAX_INPUTS		16U
#define DRM_CHROME9_VERTEX_MAX_VERTICES		384U  /* stage A, 128 triangles */
#define DRM_CHROME9_VERTEX_MAX_INDICES		12288U /* stage B, per ioctl */

enum drm_chrome9_vertex_op {
	DRM_CHROME9_VERTEX_OP_MOV, DRM_CHROME9_VERTEX_OP_ADD,
	DRM_CHROME9_VERTEX_OP_SUB, DRM_CHROME9_VERTEX_OP_MUL,
	DRM_CHROME9_VERTEX_OP_MAD, DRM_CHROME9_VERTEX_OP_DP3,
	DRM_CHROME9_VERTEX_OP_DP4, DRM_CHROME9_VERTEX_OP_DPH,
	DRM_CHROME9_VERTEX_OP_MIN, DRM_CHROME9_VERTEX_OP_MAX,
	DRM_CHROME9_VERTEX_OP_ABS, DRM_CHROME9_VERTEX_OP_SGE,
	DRM_CHROME9_VERTEX_OP_SLT, DRM_CHROME9_VERTEX_OP_SGN,
	DRM_CHROME9_VERTEX_OP_FRC, DRM_CHROME9_VERTEX_OP_FLR,
	DRM_CHROME9_VERTEX_OP_RCP, DRM_CHROME9_VERTEX_OP_RSQ,
	DRM_CHROME9_VERTEX_OP_EXP2, DRM_CHROME9_VERTEX_OP_LOG2,
	/* LIT, MOVA and relative sources reserved for ABI v2. */
};
enum drm_chrome9_vertex_file {		/* sources */
	DRM_CHROME9_VERTEX_FILE_TEMP = 0,	/* virtual 0..127, SSA */
	DRM_CHROME9_VERTEX_FILE_INPUT = 1,	/* dense v0..v(n-1) = element n */
	DRM_CHROME9_VERTEX_FILE_CONSTANT = 2,	/* c0..c255 */
};
enum drm_chrome9_vertex_dest {
	DRM_CHROME9_VERTEX_DEST_TEMP = 0,
	DRM_CHROME9_VERTEX_DEST_POSITION = 1,	/* index 0, clip space */
	DRM_CHROME9_VERTEX_DEST_PRIMARY = 2,	/* index 0 -> oD0, saturated */
	DRM_CHROME9_VERTEX_DEST_VARYING = 3,	/* logical T index 0..7 */
};
/* Swizzle values and NEG/SAT reuse DRM_CHROME9_FRAGMENT_SWIZZLE_* etc. */
struct drm_chrome9_vertex_source { __u8 file, index, swizzle[4], modifier, reserved; };
struct drm_chrome9_vertex_destination { __u8 file, index, writemask, modifier; };
struct drm_chrome9_vertex_instruction {		/* 32 bytes */
	__u8 op; __u8 reserved0; __u16 reserved1;
	struct drm_chrome9_vertex_destination dst;
	struct drm_chrome9_vertex_source src[3];
};

#define DRM_CHROME9_VERTEX_FORMAT_FLOAT1	0	/* 02 */
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT2	1	/* 42 */
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT3	2	/* 82 */
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT4	3	/* c2 */
#define DRM_CHROME9_VERTEX_FORMAT_BGRA8_UNORM	4	/* d1, live-proven */
/* c7, 49/c9, 4a/ca, 4b/cb, 44/c4 are added one oracle at a time. */
struct drm_chrome9_vertex_element { __u8 format, buffer; __u16 reserved; __u32 offset; };
struct drm_chrome9_vertex_buffer { __u32 handle, stride; __u64 offset; };

#define DRM_CHROME9_VERTEX_CULL_CW	(1U << 0)	/* screen-space winding */
#define DRM_CHROME9_VERTEX_CULL_CCW	(1U << 1)
#define DRM_CHROME9_VERTEX_FLAT		(1U << 2)	/* after rung L13 */

struct drm_chrome9_draw_vertex_program {
	struct drm_chrome9_draw_fragment_program fragment; /* vertices_ptr = 0,
							      triangle_count = 0 */
	__u32 abi_version, flags;
	__u32 instruction_count, element_count;
	__u32 constant_first, constant_count;
	__u32 vertex_count;		/* stage A; 0 for stage B */
	__u32 index_count, index_size;	/* stage B: 2 or 4 */
	__u32 buffer_count;
	__s32 base_vertex;
	__u32 pad;
	__u32 viewport[8];		/* sx sy sz ox oy oz, 0, 0: final float bits */
	__u64 instructions_ptr, constants_ptr, elements_ptr;
	__u64 vertices_ptr, indices_ptr, buffers_ptr;
	__u64 reserved[4];
};
```

The handler reuses the whole scalar/format/blend/stencil/texture/GEM
validation of `chrome9_draw_fragment_program_ioctl()` (`chrome9_engine.c:7764`),
refactored into a helper that stops before the post-viewport vertex checks.

### 1.3 Native encoding (cross + dynamic, corpus-backed)

```text
source word = unified<<14 | sx<<11 | sy<<8 | sz<<5 | sw<<2 | neg
  unified: INPUT 32+i, TEMP 64+phys, CONSTANT 256+c, literal-only 129,
           scratch 128 (finalizer only), rows 224..226 (finalizer only)
  selectors 0..3 = xyzw, 4 = 1.0, 5 = 0.0   (same values as FRAGMENT_SWIZZLE_*)
  unused source = 00000000
dest word   = op<<17 | sat<<14 | dest<<4 | mask
  dest: TEMP 0x10+phys, oPos 0xa0, oD0 0x22, oD1 0x23, oT(n) 0x28+n
```

Checks against the corpus: `v0` = `0008014c`, `c0` = `0040014c`, `r0` =
`0010014c`, literal one `00206490`, literal zero `00206db4`, `MOV r0` =
`0008010f`, `MOV_SAT oD0` = `0008422f`, `MOV oPos` = `00080a0f`.

| semantic | native (op) | corpus case / record |
|---|---|---|
| MOV d,a | 4 `[a,0,0]` | `mov_r0_v0` `0008014c 0 0 0008010f` |
| ADD d,a,b | 2 `[a,one,b]` | `add_r0_v0_c0` `0008014c 00206490 0040014c 0004010f` |
| SUB d,a,b | 2 `[a,one,-b]` | `sub_r0_v0_c0` `... 0040014d ...` |
| MUL d,a,b | 2 `[a,b,zero]` | `mul_r0_v0_c0` `0008014c 0040014c 00206db4 0004010f` |
| MAD d,a,b,c | 2 `[a,b,c]` | `mad_r0_v0_c0_c1` |
| DP4 | 3 `[a,b,0]` | `dp4_r0_v0_c0`, lane form `dp4_r0x_v0_c0` `...00060101` |
| DP3 | 3, both W selectors = 5 | `dp3_r0_v0_c0` `00080154 00400154 0 0006010f` |
| DPH | 3, src0 W selector = 4 | GL `PVS` form (static), no corpus case: oracle L5 |
| MIN / MAX | 9 / 8 `[a,b,0]` | `min_*` `0012010f`, `max_*` `0010010f` |
| ABS | 8 `[a,-a,0]` | `abs_r0_v0` `0008014c 0008014d 0 0010010f` |
| SGE / SLT | 10 / 11 | `0014010f` / `0016010f` |
| SGN | 12 `[a,0,0]` | `sgn_r0_v0` `0018010f` |
| FRC / FLR | 7 / 6 | `frc_r0_v0` `000e010f`; FLR `000c010f` (GL static) |
| RCP / RSQ | 20 / 21, one replicated lane | `rcp_r0_v0w` `00081b6c 0 0 0028010f`, `rsq_r0_v0x` `00080000 ... 002a010f` |
| EXP2 / LOG2 | 16 / 18, one replicated lane | `exp_r0_v0x` `0020010f`, `log_r0_v0x` `0024010f` |
| saturate | dest bit 14 on any op | `sat_mov` `0008410f`, `sat_mad` `0004410f` |

Every row is one native record, so native count = semantic count + fills +
1 (position MOV) + 5 (finalizer).  RCP/RSQ/EXP2/LOG2 require all four source
selectors equal (the validator rejects mixed swizzles; Mesa scalarizes).

### 1.4 Validation (fail closed)

Mirror `chrome9_fragment_validate()` (`K/.../chrome9_fragment.c:188`):

* counts: 1..128 instructions, all reserved/pad fields zero, op in range,
  source count per op exact, unused sources all-zero, modifiers only NEG on
  sources and SAT on destinations.
* SSA: a virtual temp is defined component-wise by disjoint writes and is
  never read before every read lane is defined (reuse the fragment
  `defined_mask`/`used_mask` scheme).  Reads of undefined lanes are rejected,
  which also prevents reading another client's stale physical temps.
* INPUT index < `element_count`; CONSTANT index inside
  `[constant_first, constant_first+constant_count)`; all constants finite.
* POSITION: written exactly once per lane, all four lanes.  PRIMARY: all
  four lanes when the fragment program reads PRIMARY, otherwise rejected.
  VARYING n: must be a logical varying the fragment program consumes; lanes
  `[0, varying_width[packed(n)])` defined exactly once; extra lanes rejected
  (Mesa removes dead outputs).
* Three distinct constant registers in one record: rejected, like the
  fragment rule at `chrome9_fragment.c:224`.  Three distinct inputs: rejected
  until rung L6.  Mesa legalizes both with an extra MOV (same helper as
  `chrome9_fragment_legalize_constant_ports()`).
* total native records <= `DRM_CHROME9_VERTEX_MAX_RECORDS`; physical temps <= 16.

### 1.5 Temp allocation

16 native temps r0..r15 (`temps_16_live`, object `+0 = 0x10`).  Linear scan
in program order, lowest free physical register at first definition,
released only after the instruction that last reads it (the vendor does not
reuse a register freed in the same record: `temp_reuse` gives `r5->r0`,
`r7->r1`).  This reproduces `temps_12_live`, `temps_16_live` and
`temp_reuse` byte-for-byte, which the self-test checks.  The finalizer uses
r0 after all user records, so no register is reserved.  The scratch
register 128 is written only by the final position MOV.

### 1.6 Program image, finalizer and activation words

```text
records = user records (outputs written directly by their producers)
        + oD0 fill  00206db4 00000000 00000000 0008422f   (if PRIMARY unused)
        + oD1 fill  00206db4 00000000 00000000 0008423f   (always)
        + MOV oPos  <rP.xyzw> 00000000 00000000 00080a0f
        + 00201b6c 00000000 00000000 00280108   RCP  r0.w = 1/src128.w
          00200150 00101b6c 00201b6c 001c0107   op14 r0.xyz
          0010014c 00380150 00384154 0004021f   MAD  pos33 = r0*224.xyz1 + 225.xyz0
          0020014c 00000000 00000000 0008020f   inactive tail
          0020014c 00000000 00000000 0008020f   inactive tail
active = records - 2      (+1 if rung L0c shows the clip engine needs output 32)
A60 = 60800000 | (active - 1)
A61 = 61000000 | (phys_temps <= 8) << 23          # 61800000 as proven for <= 8
A62 = 62008000 | (inputs - 1) << 20 | low9        # bit 15: 1106:7122
low9 = 1ff (Xorg value) unless rung L1 shows the compiler value is required
```

The fills use literal-only sources; they are built from proven fields but
have no corpus case (the vendor never writes a literal to oD), so rung L4
covers them.  `out_d0_d1` is the kernel self-test for the no-fill form.

### 1.7 Constants, internal rows and viewport

```text
user:     fe03043c <4*K> 32030000+4*first cc000000 <c[first..first+K-1]>
internal: fe03043c 0000000c 32030400 cc000000
          <sx sy sz 0> <ox oy oz 0> <49400000 49400000 0 0>
ints:     existing 33030000 zero store (unchanged)
```

The kernel has no FPU, so userspace sends the final float bits (Mesa
computes `ox = tx - center + 786432`).  The kernel validates by bit pattern:
all six finite; `|sx|,|sy| <= 2048.0`; `|sz|,|oz| <= 1.0`; `ox`,`oy`
positive with exponent field exactly `0x92` (value in `[2^19, 2^20)`, the
snap domain of the `786432` bias).  It then derives A40..A48 exactly as
`chrome9_drv.c:880-891` (low 24 bits per word, exponent bytes in
A42/A45/A48) and always emits them, clip enabled or not.  Row 258 is
kernel-constant for triangles.

### 1.8 Files

* `K/.../chrome9_vertex.[ch]`: `chrome9_vertex_compile(insn, count,
  const struct chrome9_fragment_program *fp, element_count, constant window,
  struct chrome9_vertex_program *out)` and `chrome9_vertex_selftest()`
  comparing against corpus cases `mov_opos_v0`, `out_d0_d1`, every row of
  the table in 1.3, `dp4_r0x_v0_c0` lanes, `swz_*`, `neg_*`, `mask_*`,
  `temps_12_live`, `temps_16_live`, `temp_reuse`, `out_t0_xy`,
  `out_t0_t1_xy`, `out_t7_xyzw`.  Run from the existing selftest hook
  (`chrome9_engine.c`, next to `chrome9_fragment_fe_config_selftest()`).
* `chrome9_engine.c`: split `chrome9_build_fragment_program()` (`:2371`)
  into a common state emitter plus two tails (post-TNL passthrough, TCL).
* `chrome9_drv.c`: ioctl table, caps 58/59 behind `hw_tcl`.

## 2. Vertex input path

### 2.1 Stage A: immediate raw vertices

Only the vertex side of the proven cmd22 stream changes.  Order, from
`chrome9_build_fragment_program()` and the passing fixed-quad oracle:

```text
FE00 sync
type-01: destination/blend state
per texture: image, [1003 mip], sampler
type-01: common, A01 = <depth word> | 0x800 (clip) | 0x80 (cull, flags)
fe020000: FE config A00..A0d (chrome9_fragment_fe_config, unchanged)
type-04:
  A00 = (elements-1)<<8 | stride_dw          # [16:15] = 00 (inline)
  A(1+i) = (1+i)<<24 | dw_offset_i<<12 | i<<8 | fmt_i   # element i -> v(i)
  A12 = 12000000 | GENMASK(elements-1, 0)
  A61, A62 (1.6)
  A20 = 20000000, A23 = 23004f02 | cull winding bit 8, A24 = 24000000 | n
  A25 A26 A28 A29 (chrome9_fragment_raster_state, unchanged)
  A40..A48 (from rows), A49=49000000, A50=50000000, A51=51000000, A52=52147fff
22030000/22030080: PS constants
32030000+4*first: user constants
32030400: rows 256..258 (12 dwords)
33030000: integer store
type-01: A80..A83 window/scissor
fe020440 0000043c 31030000 cc000000 <records>
type-04: A60
[type-01 8a020000] type-01 A90..A9b; 20030000 ALU; 21030000 TAU
fe020440 0000043c 00000000 cc000000 <n * stride_dw raw dwords>
tail (chrome9_emit_3d_tail)
```

Differences from cmd22 (`chrome9_fragment_raster_state()` today): A00 counts
raw elements, not `D0,D1,D2,T...`; A12 is the input mask, not
`GENMASK(2+N,0)`; A62[23:20] is `inputs-1`, not `2+N`; A40..A48 and rows
256..258 come from the viewport instead of the identity `(1, 786432)` image;
payload is raw application attributes, not Q16 post-viewport data.  Element
dword offsets are `sum(size of earlier elements)`; `stride_dw <= 127`
(A00[6:0]); `elements <= 16`.  The staging bound
`stop + 0x220 <= CHROME9_RUN_STAGING_SIZE` is checked on the final cursor,
not on estimates.  Float attributes are rejected if not finite until L12.

### 2.2 Stage B: SF GEM vertex buffers and inline indices

```text
type-04 (same block as 2.1, except):
  A00 = 00008000 | (elements-1)<<8 | sum_element_dw    # [16:15] = 01, SF
  A(1+i) = (1+i)<<24 | ((addr_i >> 2) & 0xff)<<12 | i<<8 | fmt_i
  A20 = 20000001 (16-bit) | 20000003 (32-bit)
  A2C = 2c000000 | (max & 00ffffff)                    # inclusive (Linux)
  A2D = 2d000000 | (min & 00ffffff)
  A2E = 2e000000 | (max >> 24)<<8 | (min >> 24)
fe03043c <elements> 30030000 cc000000 <stream[0..elements-1]> <cc pad>
  addr_i   = sf_base(buffer) + buffer.offset + element.offset + base_vertex*stride
  stream_i = (addr_i & fffffc00) | (stride >> 2)
... constants, program, A60, PS as in 2.1 ...
per chunk (whole primitives, <= segment capacity):
  fe020440 0000043c 00040000 cc000000 24<count> cc000000 cc000000 cc000000
  fe03043c <ceil(count*size/4)> 00000000 cc000000 <indices, unused lane 0>
```

Kernel validation, before any GEM reservation is published:

1. `index_size` 2 or 4; `index_count` 1..MAX and a multiple of 3 (lists only).
2. Copy indices with `vmemdup_array_user()`; compute min/max from the data.
3. Buffers: exact handles, pinned with `chrome9_sf_pin()` for the job; offset
   and stride multiples of 4; `4 <= stride <= 2044` (2048 encodes pitch 0,
   vendor bug; stride 0 only after rung L9).
4. Per element, checked 64-bit: `offset + element.offset + (min+base)*stride
   >= 0` and `... + (max+base)*stride + fmt_bytes <= bo_size`.  The stream
   base cannot precede the object because SF objects start page-aligned.
5. Exact packet end, alignment, PAUSE/STOP positions and read-ahead limit.

The concurrent `VS_FETCH` fixed-quad oracle (`chrome9_drv.c:931`) uses this
exact form with one shared base for three elements and a 4-vertex fan
(`A20 = 201c5c01`); keep it as rung L8a.  A00 is one global memory domain,
so user arrays never mix with stage B in one draw: Mesa uses stage A for
them.  8-bit indices, primitive restart and non-list topologies are
converted in Mesa (`u_indices`).

## 3. Mesa side

### 3.1 Shader creation

`chrome9_create_vs_state()` (`M/chrome9_context.c:341`) keeps the Gallium
draw shader (fallback) and additionally clones the NIR.  A VS-specific
`nir_shader_compiler_options` (`M/chrome9_screen.c:268` today serves both
stages) adds `lower_fpow`, `lower_flrp32`, `lower_fsqrt`, `lower_fdiv`,
`lower_sincos`, `lower_ffloor`, `lower_fdph = false`.  Lazy compile on first
bypass attempt:

1. `nir_lower_clip_halfz`, `nir_lower_io` to vec4 slots, `nir_opt_loop_unroll`
   (existing `max_unroll_iterations = 31`), `nir_opt_if`,
   `nir_opt_peephole_select` with a large limit so all `if`s become `bcsel`;
   any remaining control flow, loop, `load_ubo` with non-constant offset,
   texture instruction, `load_vertex_id`/`load_instance_id` -> unsupported.
2. `nir_lower_alu_width` for `frcp frsq fexp2 flog2` only, then algebraic,
   copy-prop, DCE.  Keep other ALU vectorized.
3. Walk the single block with a builder patterned on the PS builder
   (`chrome9_compile_ps_nir`, `M/chrome9_context.c:4654`) into
   `struct chrome9_vs_ir`, then `chrome9_vertex_build_instructions()` into
   UAPI records.  New files `M/chrome9_vs.c`, `M/chrome9_tcl.c`; the offline
   template compiler `chrome9_compile_vs_offline()`
   (`M/chrome9_shader_offline.c:1053`) is superseded.

NIR op mapping: `mov` MOV; `fneg` source NEG; `fabs` ABS; `fsat` SAT on the
producer when it is the only user, else MOV_SAT; `fadd` ADD; `fsub` SUB;
`fmul` MUL; `ffma` MAD; `fdot2` DP3 with Z/W literal zero; `fdot3` DP3;
`fdot4` DP4; `fdph` DPH; `fmin`/`fmax` MIN/MAX; `slt`/`sge` SLT/SGE;
`seq` = `sge(a,b)*sge(b,a)`, `sne` = `1 - seq`; `fsign` SGN; `ffract` FRC;
`frcp` RCP; `frsq` RSQ; `fexp2` EXP2; `flog2` LOG2 (D3D LOG uses |x|; GL
leaves log of negatives undefined); `vecN` lane-assembly MOVs;
`fcsel c,a,b` = `MAD(abs(sgn(c)), a-b, b)` (SLT/SGE results skip the
`abs(sgn())`).  `load_const` values go to literal selectors when 0/1, else
to Mesa-allocated constant rows above the uniform range.

### 3.2 Inputs, constants and outputs

* Inputs: dense by `driver_location`; vertex element k feeds input k.
  Stage A converts every attribute to FLOAT1..4 or BGRA8_UNORM on the CPU
  (vendor also converts BYTE/INT/DOUBLE).  Until rung L9, positions and
  other attributes with fewer than four components are expanded to float4
  with the GL default `(0,0,0,1)`, because the hardware fill rule is unmeasured.
* Constants: `lower_uniforms_to_ubo` gives `load_ubo(0, const)`; row =
  offset/16, lane via swizzle.  The draw sends exactly the used row range.
* Outputs: `VARYING_SLOT_POS` -> POSITION; `COL0` -> PRIMARY when the bound
  FS reads PRIMARY (`semantic_input_mask` bit 0); `VAR0+n`/`TEX0+n` ->
  VARYING n, the same lookup `chrome9_vbuf_get_vertex_info()` uses
  (`M/chrome9_draw.c:257`), so the kernel's packed order and widths match the
  draw path exactly.  Outputs the FS does not read are dropped per bound
  FS (link-time variant keyed by the FS varying mask).

### 3.3 Bypass and fallback

Insert at the top of `chrome9_draw_vbo()` (`M/chrome9_context.c:6237`) after
`chrome9_select_fs_variant()`: `if (chrome9_tcl_draw(ctx, info, draws)) return;`.
`chrome9_tcl_draw()` first calls `chrome9_draw_flush_batch()`
(`M/chrome9_draw.c:3336`) so queued cmd22 triangles keep their order.  It
returns false (Gallium draw path) for any of:

| condition | reason |
|---|---|
| VS compile unsupported, > 16 temps, > 128 records, > 256 rows | ISA/limits |
| FS guarded-domain masks (`semantic_equal_zero_input_mask`, `..._identity_projector_...`, `..._unit_interval_...`) | need CPU proof of post-VS values |
| FS reads FACE with `face_mode == 0` | face computed on the CPU |
| `cull_face != NONE` before rung L7 | hardware cull unproven |
| `light_twoside` / BFC outputs | oBFD (A23 bit 7) and dest 36 unproven |
| `clip_plane_enable`, clip/cull distances | vendor `STM_SetClipPlane_H5` is empty |
| points, lines, `fill_front/back != FILL`, edge flags, stipple | only triangle lists |
| point sprites, `PSIZ` consumed | points not in v1 |
| `flatshade` before rung L13 | provoking vertex unmeasured |
| `depth_clip_near/far == false`, `rasterizer_discard`, stream output | not modelled |
| `instance_count > 1`, divisors, indirect | not modelled |
| VS texture sampling (`M/chrome9_vs_texture.c`) | no VS texture unit |

Stencil: the native `cw`/`ccw` blocks are screen-space (proven by
`--faces`); derive them from `front_ccw` and the sign of the viewport Y
scale instead of the per-triangle CPU resolution in
`chrome9_triangle_stencil()` (`M/chrome9_draw.c:1307`).  Culling after L7
uses the same mapping.  Once L13 passes, GL provoking-last is handled by
rotating each list triangle to `(v2,v0,v1)` (same winding, provoking vertex
first) during de-indexing or index translation.

Viewport words: `sx = scale[0]`, `sy = scale[1]`, `sz = 2*scale[2]`,
`ox = translate[0] - center + 786432`, `oy = translate[1] - center + 786432`,
`oz = translate[2] - scale[2]`, with `center = 0.5` when
`half_pixel_center`, matching the `pixel_center_bias` of the post-TNL path
(`M/chrome9_draw.c:2344`).  The `sz`/`oz` form follows from the halfz lowering.

Stage A submission: gather `[start, start+count)` (or de-index), convert
topology to lists with `u_indices`, pack the declared elements, split at 128
triangles or the kernel's `-E2BIG`, and reuse the existing batch key
(identical state and identical VS constant upload) to merge consecutive
draws.  Stage B: only GEM-backed vertex buffers; index data comes from a
user pointer or the mapped index BO.

### 3.4 Fixed function and targets

Mesa's `ffvertex_prog` already emits straight-line NIR (lights unrolled,
`fpow` for specular, `frsq` normalize), so no LIT is needed.  glxgears
needs: flat shading, back-face cull, one directional light, normalize ->
rungs L7, L10, L13.  ioquake3 GL1 (unlit, T0/T1, `BGRA8` colors, culling,
client arrays) needs stage A + L7.  An offline check runs each compiled
`chrome9_vs_ir` through a small CPU evaluator against the NIR interpreter
on random inputs, extending `M/chrome9_shader_nir_test.c`.

## 4. Silicon oracle ladder

One variable per rung.  Each rung uses guarded targets, full-page
comparison against a reference made with the proven post-TNL path, cookie,
PAUSE, non-CR idle, `040c = 0`, guards intact, and a legacy 2D/cmd22
closure draw.  Stop on the first timeout, engine error or guard change.

| rung | change against the previous passing rung | pass criterion |
|---|---|---|
| L0a | `VS_CLIP_NEAR` with bit 11 moved to type-01 A01 (`01c01d40`), FE A01 back to `01002521` | coverage equals homogeneous-clipped reference |
| L0b | same with FE-A01 bit 11 only (the current commit) | documents which word is the clip enable |
| L0c | `VS_CLIP_NEAR_TAIL` (first tail MOV active) | decides whether `active` gains the tail |
| L1 | fixed quad, A62[8:0] `002 -> 1ff` | image unchanged -> kernel uses `1ff` constant |
| L2 | fixed quad with a 9-temp vendor program, A61 bit 23 clear | image equals the 8-temp variant |
| L3 | new ioctl, stage A, kernel-compiled `mov oPos,v0; mov oD0,v1; mov oD1,v2`, zero varyings | records byte-identical to `out_d0_d1`; image equals cmd22 |
| L4 | PS reads no PRIMARY: oD0/oD1 literal fills | image unchanged |
| L5 | one varying `oT0 = v2` float2, then float4, then the 8-varying mixed-width set from [chrome9-vs-varying-fvf-corpus.json](chrome9-vs-varying-fvf-corpus.json) | equals cmd22 with the same varyings |
| L6 | per op (ADD..LOG2, DPH): result into oT0, PS outputs it; then MAD with 3 distinct constants and 3 distinct inputs | 1-LSB match per op; decides port rules |
| L7 | type-01 A01 bit 7 + A23 bit 8, one triangle and its reversal | exactly one image changes per selector |
| L8a | `VS_FETCH` fixed quad (one stream per element, 16-bit indices) | equals the inline quad |
| L8b | stage B ioctl: two BOs, then 32-bit indices, then `base_vertex`, then two chunks | equal images each |
| L9 | FLOAT3 position without W; then stride-0 constant attribute; then `c7`/`49`/`4a`/`44` formats | defines fill and format rules |
| L10 | vendor glxgears FF program with its 31-row constant image, then the Mesa-compiled glxgears VS | per-pixel color within 1 LSB of the draw path |
| L11 | 16 live temps; 128 then 256 native records | capacity limits |
| L12 | adversarial: NaN/Inf/1e30 positions, W=0, all-outside, zero area after transform, sliver triangles | clean retirement, guards intact, nothing outside the window |
| L13 | A23 bit 14 clear (flat), distinct color per vertex | provoking vertex identified |
| L14 | two VS programs in one segment (program switch between draws) | both images correct |

L3 is the first rung through the new ioctl; before it, the kernel
self-test must reproduce all corpus rows listed in 1.8.  `hw_tcl` becomes
default-on only after L12.

## 5. Risks and unknowns

* **Clip enable register** (L0a/L0b): see the open issue above.  All other
  TCL work assumes clipping works; without it L12 cannot pass.
* **Safety without per-vertex validation**: the destination window A80..A83
  and the pixel-Z clip are the only remaining bounds; guard-band behaviour,
  huge or NaN raster coordinates and zero-area primitives after transform
  are unmeasured.  This is the main blocker for a default-on capability.
* op 14 semantics, the `0xa0` write reaching output 32, and whether the
  inactive tails matter (L0c).
* A62[8:0]: the vendor value is a pressure-like number that is
  nondeterministic without `v#` reads; the design avoids reproducing it
  (L1).  A61 bit 23 for more than 8 temps (L2), A61 bits 18/16.
* Numerics: EXP2/LOG2 precision and D3D `|x|` LOG, RCP/RSQ of 0 and Inf,
  SGE/SLT with NaN, FRC of negatives, MAD rounding (L6).  Vertex-color
  saturation always applies to oD0.
* Constant/input port conflicts with three distinct registers (L6); the
  fragment unit is known to alias.
* Depth convention: correctness depends on the halfz lowering; the vendor
  Linux GL driver probably folds the remap into its MVP rows c8..c11
  (inference, not checked).
* Vertex fetch: prefetch or overrun beyond the last indexed element, stride
  2048, stride 0, unaligned data (vendor drops address bits 1:0), A2C
  inclusive versus exclusive, element bits 23:20, Windows A00 `0x540000`.
* Performance: stage B still copies indices inline and every ioctl still
  pays the whole-object flushes described in
  [chrome9-throughput-re.md](chrome9-throughput-re.md); static vertex
  buffers need ownership tracking before stage B beats stage A.
* Flat shading and provoking vertex (L13), hardware cull winding against
  framebuffer orientation (L7), per-varying `flat` (no evidence; fallback).
* VS store capacity beyond 128 records and program switches within one
  segment (L11, L14).
