# Chrome9 draw throughput: current cost profile and vendor recipes

Date: 2026-09-24.  Scope: static profile of the current T2 kernel and Mesa
draw paths, plus reverse engineering of how the VIA Linux 5.75 and Windows 7
stacks reach throughput.  No live GPU access was used for this note; every
"live" number below is quoted from earlier notes and marked as such.

Confidence tags: **P** proved by source or disassembly, **M** stated by the
VX900 programming manual (not yet silicon-tested), **S** strong static
inference, **U** unknown / hypothesis to test.  Static and dynamic evidence
are kept in separate columns or paragraphs.

Inputs and identities:

| input | identity |
|---|---|
| kernel Chrome9 repo | `linux-7.2/drivers/gpu/drm/chrome9`, HEAD `561cc56` |
| Mesa Chrome9 repo | `mesa-26.2.2/src/gallium/drivers/chrome9`, HEAD `d56f61f` |
| Linux 5.75 UMD `via_chrome9_dri.so` | sha256 `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` |
| Linux 5.75 KMD source | `binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/` |
| Windows D3D UMD `VTGUModeDX32A.dll` | sha256 `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| Windows KMD `VTGKModeDX32A.sys` | sha256 `618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56` |
| VX900 3D manual | `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf` (Rev 1.0, 2011-01-19) |

Kernel paths below are relative to `linux-7.2/drivers/gpu/drm/chrome9/`,
Mesa paths to `src/gallium/drivers/chrome9/`.

## 1. Static profile of the current draw path

### 1.1 Userspace (Mesa) per-draw and per-triangle work

| # | cost | where | kind |
|---|---|---|---|
| U1 | every vertex runs the Gallium draw module with `draw_create_no_llvm()`: the vertex shader is interpreted by TGSI exec, then clipped, culled and decomposed on the CPU | `chrome9_context.c:5891`; `chrome9_vbuf_need_pipeline()` always true, `chrome9_draw.c:243-253` | P |
| U2 | every sampler view with a CPU shadow causes a batch flush plus a full CPU `memcpy` of the texture on **every** `draw_vbo` | `chrome9_refresh_sampler_shadows()` `chrome9_context.c:5504-5519`, called at `:5577` | P |
| U3 | `num_draws > 1` is split by `util_draw_multi()` into single draws | `chrome9_context.c:5579-5581` | P |
| U4 | index and vertex buffers are consumed only through CPU mappings (`bo.map`) by the draw module; no GPU vertex or index fetch exists | `chrome9_context.c:5598-5620` | P |
| U5 | cmd22 path: per triangle the fragment instruction list, constants and all texture descriptors are rebuilt and compared with four `memcmp()` calls (up to 184 B + 4 KiB + 512 B + 8*176 B) | `chrome9_queue_generic_triangle()` `chrome9_draw.c:1287`, rebuild `:1330-1334`, compare `:1529-1538` | P |
| U6 | cmd22 batch capacity is 16 triangles (`CHROME9_FRAGMENT_BATCH_TRIANGLES`), below the kernel UAPI maximum of 128 | `chrome9_draw.c:31`, flush `:1555-1556` | P |
| U7 | cmd22 batch is flushed at the end of every draw-module callback, so a Gallium draw that the draw module splits produces at least one ioctl per callback | `chrome9_draw.c:3125-3126`, `:3194-3195` | P |
| U8 | vertex record copied to the kernel is the fixed 152-byte `drm_chrome9_fragment_vertex` (6 fixed dwords + 8 x vec4) even when the program uses one vec4 varying (40 B) | UAPI `chrome9_drm.h:1065-1073` | P |
| U9 | `glFlush`/present copies the complete guarded shadow render target to the external drawable with a CPU row `memcpy` | `chrome9_flush_render_target()` `chrome9_context.c:50-62`, `chrome9_copy_surface()` `:33-48` | P |
| U10 | Gallium fences are `NULL`; every submission is synchronous | `chrome9_flush()` `chrome9_context.c:5654-5665` | P |

Other untextured and textured paths (RHW batch/run/chain, textured run, mip,
two-texture) use the older structured ioctls with their own caps (34, 544,
24 triangles; the 16,384-triangle chain and the textured run are module-
parameter gated, `chrome9_engine.c:74-88`).  Their kernel costs follow the
same pattern as cmd22 below; see
[chrome9-submit-bottleneck.md](chrome9-submit-bottleneck.md).

### 1.2 Kernel per-ioctl work for cmd22 (`DRAW_FRAGMENT_PROGRAM`)

Entry `chrome9_draw_fragment_program_ioctl()` `chrome9_engine.c:7407`, submit
`chrome9_submit_fragment_program()` `:4798`, packet builder
`chrome9_build_fragment_program()` `:2079`.  All steps are P (source).

| # | step | citation | scaling |
|---|---|---|---|
| K1 | scalar validation of the 184-byte header, scissor and blend | `:7447-7488` | per ioctl |
| K2 | `memdup_array_user` of instructions, constants and texture descriptors | `:7511-7529` | per ioctl |
| K3 | `vmemdup_array_user` of `3*T` 152-byte vertices, then per-vertex coordinate, Z, RHW and 32 finite-float checks, then a non-degeneracy check per triangle | `:7558-7599` | per vertex |
| K4 | `kvzalloc()` of a 2,276-byte `chrome9_fragment_program` and a **full fragment compile** (validate, allocate, encode) of the same program on every ioctl | `:7600-7616`; `chrome9_fragment.c:1167-1208` | per ioctl |
| K5 | GEM lookup for target, depth and every texture; `drm_gem_shmem_get_pages_sgt()` per object | `:4831-4891` | per object |
| K6 | job allocation, `dma_fence_init64()`, `drm_exec` reservation of all objects, `dma_resv_wait_timeout()` per object | `:4899-4921` | per object |
| K7 | `drm_gem_shmem_vmap_locked()` of every object (full kernel mapping) | `:4922` | per object |
| K8 | `submit_lock`; SF mapping-cache lookup (hit: no PTE work; miss: PTE build, `drm_mm` insert, VGA acquire, PTE MMIO writes, SR6F flush) | `:4933-4946`; `chrome9_draw_mapping_acquire_locked()` `:3071-3207` | per object; miss only |
| K9 | write two 4-KiB guard pages per object | `:4949-4952`; `chrome9_prepare_map_guards()` `:3541` | per object |
| K10 | **`clflush_cache_range()` over every complete object** plus `dma_sync_sgtable_for_device()` | `:4953-4956`; `chrome9_cache_flush()` `:389-398` | per object byte |
| K11 | packet build: `memset(0xcc)` of the complete 64-KiB run staging, full state image re-emitted (destination, textures, common, FE config, raster, constants, VS constants, clip, VS program, PS controls, ALU, TAU), vertex payload, then `chrome9_finish_ring_sized()` fills the rest of the 64 KiB with dummy pairs | `:2079-2296`, memset `:2218`; `:487-529` | per ioctl + per vertex |
| K12 | ring append: `memcpy` of `STOP + 0x220` bytes into the 64-KiB ring, `clflush` of that range, A64/A65 MMIO publish; on overflow a PID10 STOP, full 64-KiB dummy fill + clflush and A60-A68 reinstall | `chrome9_append_segment()` `:2708-2802`; wrap `:2609-2699` | per ioctl; per wrap |
| K13 | **synchronous retirement** inside `submit_lock`: poll `DST_POS` cookie, CR PAUSE, engine work mask (`readl_poll_timeout`, 10 us) | `chrome9_wait_job()` `:3679-3703` | per ioctl |
| K14 | `dma_sync_sgtable_for_cpu()` and a **second whole-object `clflush`** for every object | `:4996-4999` | per object byte |
| K15 | read back and compare both guard pages of every object; `chrome9_sf_resync_object_locked()` issues a scanout `sync_for_device` | `:5002-5012`; `:3428-3441` | per object |
| K16 | release mappings, vunmap every object, `drm_exec_fini`, fence signal, object puts | `:5013-5064` | per object |

The completion cookie is the 2D `DST_POS` register alternating between two
values (`:4966-4967`, `chrome9_wait_job()` `:3690`), so at most one job can be
in flight by construction.  The engine therefore never overlaps CPU vertex
processing with GPU execution.

### 1.3 Quantified per-ioctl cost (static model)

`tools/re/chrome9_cmd22_cost_model.c` mirrors K1-K16 emission sizes from the
UAPI header and builder source.  Build and run:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -D__user= \
  -I src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/include/uapi/drm \
  tools/re/chrome9_cmd22_cost_model.c -o ~/tmp/re-perf/chrome9_cmd22_cost_model
~/tmp/re-perf/chrome9_cmd22_cost_model
```

Output (static model; triangle totals per frame for ioquake3 and glxgears are
illustrative inputs, the cat total 8,043 is the live-measured one-triangle
call count from [chrome9-generic-cmd22-batching.md](chrome9-generic-cmd22-batching.md)):

| scenario | guarded objects | packet state / body | ring copy | clflush per ioctl | ioctls/frame | clflush/frame |
|---|---:|---:|---:|---:|---:|---:|
| glmark2 cat 320x240 XRGB+D16, 16 tri | 117 pages | 220 / 730 dw | 3,600 B | 2 x 468 KiB | 503 | 461.5 MiB |
| ioquake3-like 1024x768 XRGB+X8Z24, 2 textures, 16 tri | 1,646 pages | 274 / 786 dw | 3,856 B | 2 x 6.43 MiB | 375 | 4.82 GiB |
| same at UAPI max 128 tri | 1,646 pages | 274 / 4,146 dw | 17,296 B | 2 x 6.43 MiB | 47 | 605 MiB |
| glxgears-like 1024x768 XRGB+D16, 16 tri | 1,156 pages | 212 / 722 dw | 3,600 B | 2 x 4.52 MiB | 82 | 741 MiB |

The live 16-triangle cat run made exactly `ceil(8043/16) = 503` calls, so in
that scene callback-boundary flushes (U7) added no extra ioctls; larger
per-ioctl capacity would therefore be filled.  Conclusions:

1. **Whole-object cache maintenance (K10/K14) is the dominant CPU cost that
   scales with render-target size and is invariant with the amount of useful
   work.**  A 1024x768 color+depth target costs about 9 MiB of `clflush`
   per ioctl.  (S: clflush throughput on the target CPU was not measured.)
2. Fixed 64-KiB `memset` plus 64-KiB trailer fill (K11) writes 128 KiB per
   ioctl although only 3.6-17 KiB is copied to the ring. (P)
3. The copied packet is 24-28% state at 16 triangles and 6% at 128; the
   remaining bytes are the inline vertex payload.  State re-emission is not
   the primary cost once batches are large. (P, model)
4. A 64-KiB ring holds only 16-17 cmd22 appends before a full reinstall,
   and three at 128 triangles. (P, model)
5. User copy-in inflates vertex bytes 3.8x (152 vs 40 B) and repeats the
   program compile per ioctl (K3/K4). (P)
6. Synchronous retirement (K13) serializes the CPU software vertex pipeline
   (U1) with GPU execution; neither can hide the other. (P)

### 1.4 Live measurements already on record (dynamic)

| workload | result | source |
|---|---|---|
| glmark2 Gouraud cat, D16 RHW batch 34 | 237 ioctls/frame, 65.6% of frame in ioctl | [chrome9-submit-bottleneck.md](chrome9-submit-bottleneck.md) |
| glmark2 cat, cmd22 1 vs 16 triangles | 8,043 -> 503 calls, 4.49 s -> 0.466 s/frame | [chrome9-generic-cmd22-batching.md](chrome9-generic-cmd22-batching.md) |
| second target: nearest cube / linear cube / Gouraud cat | 39 / 38 / 9 FPS | [persistent-batched-submit-performance-plan.md](persistent-batched-submit-performance-plan.md) |
| glxgears direct-SF | about 20.2 FPS | [chrome9-submit-bottleneck.md](chrome9-submit-bottleneck.md) |

No phase timing (clflush vs wait vs VS) has been measured yet; the ranking in
section 4 therefore uses the static model for magnitude and the live call
counts for scaling only.

## 2. VX900 hardware facts from the programming manual (M)

Source: `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, text extracted with
`pdftotext -layout`; page numbers are the manual's own.  None of these
has been silicon-tested by this project unless stated.

| fact | manual | consequence |
|---|---|---|
| H2/H3 type 00 carries either inline vertices ("Vertex Command Mode") or inline indices ("Vertex Buffer Index Mode"); CR fires primitives automatically and ends the list after A24 vertices | p.73 | one A24 group = one primitive list; many lists may follow in one command stream |
| A20 bit0 = command (0) / index (1) mode, bit1 = 16/32-bit index; A20[15:8]/[21:16] vertex-reuse cycles for strips/fans | p.160 | **there is no non-indexed draw from a vertex buffer**; a VB draw must carry inline indices |
| A00[16:15] vertex-buffer location: 00 SL (local frame buffer), 01 SF; [11:8] last element; [6:0] vertex size in dwords | p.154 | VBs may live in SF GEM objects |
| H3 subtype 30: 16 stream words, base `[31:10]` in 1-KiB units, pitch `[8:0]` in dwords | p.153 | stream base 1-KiB aligned, pitch <= 2044 B |
| A01..A10 element offset `[19:12]` in dwords (max 1020 B) | p.154 | low 10 address bits carried per element |
| A2C/A2D/A2E max/min index; "if index > max or < min, ignore this and the followed indices" | p.169 | **hardware index-range clamp**: a kernel can bound fetches by programming max from the BO size instead of trusting indices |
| type-01 A01 ("Enable Setting 2") bit12 vertex cache, bit13 coarse-Z test, bit9 coarse-Z write; coarse-Z buffer base/pitch/location in its own setting words | p.87 area, pp.~180 | the current `01ce1540` word enables the vertex cache and leaves coarse Z off |
| HParaType 08h PPC counters: vertices, vertex-cache hits, VS instr, primitives, pixels, PS ALU/TAU, SL/SF/VB/Z/C/texture read/write traffic, occlusion; written back to SL/SF memory by a fire bit | pp.174-175 | GPU-side profiling without guessing |
| T11 A05..A07 traditional fence: mode (record / IRQ / system write, optionally after all engines idle), interrupt-wait pauses CR until `0204[5]` is cleared, queue-full skip | pp.82-83 | a kernel fence exists without the 2D `DST_POS` cookie |
| T11 A68..A6B traditional branch: location SL/SF, size in 16-byte units, "nested branch forbidden" | p.87 | matches the FE4x first-level branch used by both vendor KMDs |

PPC write-back recipe (M, derived from the table on pp.174-175; not
silicon-tested): H2 `fe020440 0000043c 00080000 cc000000`, then
`01000000|type`, `02000000|((addr>>2)&00ffffff)`,
`03000000|(loc<<22)|0x100|((addr>>2)>>24)` with `loc` 01 for SF.

## 3. Linux 5.75 stack (VX900 = UMD chip family 8)

Evidence is static disassembly of `via_chrome9_dri.so` plus the KMD source.
Checker: `python3 tools/re/chrome9_umd_vertex_path_audit.py` (pins the
UMD hash; 154 instruction invariants, 4 tables, 13 relocations, 7 stubs;
current output `RESULT: PASS (0 failures)`).

### 3.1 Which path is VX900's

`hwmContextCreate` stores family 8 for `0x7122` (`0xaafee`, `0xab04c`) (P).
`__glS3InvCreateContext` gives family 8 `__glS3H5UpdateDrawPrimitivePath`
(`0x1d878f`), i.e. **hardware vertex shading** (`ctx+0x3ac8=0` at
`0x230d32`) and `drawPrimitiveFuncTable_H5`; families 7/0xe take the
software-VS H6H7 path (`0x230729`) (P; the family-number identity between
the two lookups is S).  Correction: `__glS3InvDrawPrimitiveTriangleList`
(`0x209d10`) and the `_H6` draw functions cited by
[vendor-state-reuse-performance-audit.md](vendor-state-reuse-performance-audit.md)
and [raster-front-end-oracles.md](raster-front-end-oracles.md) are the
pre-transformed (software VS) emitters, not the VX900 hardware-VS draw.  The
VX900 triangle-list draw is table entry 44, `__glS3H5DrawPrimitiveTriangleList`
`0x1e7490` (P).

### 3.2 Vertex and index placement

- GL buffer objects and the per-context client-array ring are allocated in
  **VRAM (SL)** on VX900 (`__glS3InvInitializeLocalMemoryFlag` sets
  `0x22b0=1` at `0x1d74ca`/`0x1d74ff`; heap table `0x3f91c4`;
  `sfmVertexBuffer_CreateInternal` `0xc6518`; `__glDpMmAlloc` ioctl
  `0xc0346447` at `0x1ea503`) (P).  CPU writes go through the framebuffer
  mapping.  The SF/AGP heap is only a fallback (location 4), and only then
  is the KMD `FLUSH_CACHE` clflush ioctl used
  (`__glS3InvFlushVertexBufferPcieCache` `0x2314f0`) (P).
- Client arrays: one 0x800400-byte ring per context (`0x23a301`), bump
  allocation (`__glDpVBGetSpace`/`ReleaseSpace`, `0x236526`); at wrap the
  UMD sends an event, flushes and **spins until the GPU drains**
  (`0x23a199..0x23a1e1`) (P).
- Buffer objects: one allocation each, 4-KiB aligned (`0xc658a`); every
  `BufferData`, `BufferSubData`, `UnmapBuffer` and busy `MapBuffer` performs
  a full `FlushDMABufferAndWait` (`0x22b1af`, `0x22b1e7`, `0x22af89`,
  `0x22b001`, `0x22b264`) (P).  There is no per-buffer GPU fence: the
  per-buffer stamp is a CPU counter never emitted to the GPU (`0x231f7f`,
  `0x2328cc`; no reader of the `hwctx+0x3acc` list) (S).
- Unsupported attribute types and strides above 0x800 are repacked on the
  CPU into float streams (`0x231d28`, `0x232b78`) (P).

### 3.3 Draw recipe (hardware VS, VB in one location)

`__glim_DrawArrays` (`0x2c35d0`) -> `__glS3InvBegin` ->
`__glS3InvSetupDrawParameters` -> `__glS3InvConfigVertexStream`
(`0x231bb0`) -> VS/PS update -> `stmSetCurrentState` -> table entry 44.
The draw ORs the stream location codes (`0x1e7526`); mixed or system
locations use the `_VM` fallback that gathers vertices inline (`0x1e7586`,
`0x1e759f`) (P).  Otherwise:

```text
per element e (stmUpdateVertexInput_H5 0x17cbb0):
  G      = vb_base + stream_off + elem_off + stride*first        (0x17cd72)
  palette[s] = (G & ~0x3ff) | ((stride>>2) & 0x1ff)              (0x17cdac/0x17cdd3)
  A01+e[19:12] = (G >> 2) & 0xff                                 (0x17cd8b)
  A00[16:15] = 01 only for location 4 (SF), else 00 (SL)         (0x17d291)
A20[1:0] = 01 (count < 0xffff) or 11                            (0x1bc3c7/0x1bc28e)
A2C = 2c000000|(n-1), A2D = 2d000000, A2E = high bytes
per chunk:
  fe020440 0000043c 00040000 cc000000                            (0x1e7809)
  24000000|n                                                     (0x1e782a)
  <cc/cd/ce/cf000000 pad to 16 B>
  fe03043c <ceil(n/2) | n> 00000000 cc000000                     (0x1e7851)
  generated indices 0..n-1: ((i+1)<<16)|i or 32-bit i            (0x1e7b1b/0x1e7890)
chunk limit: (cmGetDMAInfo(6) - 0x600) / index_size; reserve ndw+0x100 (0x1e77b4)
```

There is no base-vertex register; the start vertex is folded into every
stream/element address (P).  `DrawElements`: indices are scanned on the CPU
for min/max (`__glGetIndexRange` `0x2c3d69`) and copied verbatim inline with
A2D/A2C = min/max (`0x1e36a2`); an element-array buffer object is memcpy'd
from its VRAM mapping to system memory on every draw (`0x23186d`) and then
copied inline; the GPU never fetches indices from memory in this driver
(P).  `GL_UNSIGNED_BYTE` indices take the `_VM` path (`0x1e3281`) (P).

### 3.4 State, batching, kickoff, waits

- Vertex input (A00..An, A12, palette) and A2C-A2E are marked dirty on
  **every** draw without comparison (`0x17d0ff..0x17d1cc`,
  `0x1bc362..0x1bc3ae`); other state follows the dirty-range serializer of
  [vendor-state-reuse-performance-audit.md](vendor-state-reuse-performance-audit.md)
  (P for the marking; S that the H5 updater at `0x179e88` is always
  installed).
- `MultiDrawArrays/Elements` loop over single draws (`0x2c1c1e`,
  `0x2c1b95`) (P).
- GL does **not** kick the ring from userspace: `__glDpFlushDMABuffer`
  issues `DRM_VIA_CHROME9_FLUSH` (ioctl `0xc01c644e`, `0x1eb183`); the KMD
  (`via_chrome9_ioctl_flush()` `via_chrome9_dma.c:1378-1470`) copies the user
  buffer into the AGP ring, runs its verifier and kicks.  Correction to
  [via-5.75-dri-state.md](via-5.75-dri-state.md): `cmKickoffRingAGP_inv` is
  not the GL path (P).
- Command buffer: 8 MiB `osAllocMem` (`0xa9136`), capacity `0xffffe` dwords
  (`0xaf666`); `cmGetDMAInfo(,6)` is total capacity, not free space (P).
  Kickoff triggers: buffer full (`0xb0031`), glFlush/glFinish (`0x1d838e`),
  SwapBuffers, ring wrap, queries, blits (P).  Typical result: one flush
  ioctl per frame of up to a few MiB (S).
- Every flush clears the previous-state pointer (`0x1eb1d7`), so the first
  draw after a flush replays the full `0x5f0`-dword state (P).
- All waits are unbounded busy spins (`hwmWaitForEvent`, `hwmWaitChipIdle`
  on BAR1+`0400`), at SwapBuffers (one frame in flight, global event
  `gDp+0x1aac`), per ring wrap, and per buffer update (P).  NV_fence entry
  points are empty stubs (P).

### 3.5 Texture upload

`glTexImage` only records the image; `__glDpTextureMakeResident` uploads at
draw validation (`0x2359d4`) into VRAM first (`0x2012c2`), through a 2D
host-data BitBLT: texels are memcpy'd into the command buffer behind
`fe010000` / GECMD `cc002041` in about 0x806-dword chunks (`0xea49a`,
`0xea835`), without a wait (P).  CPU-direct writes (SF heap, compressed and
some tiled cases) first wait for chip idle (P).

### 3.6 What makes the Linux 5.75 stack fast

It is not a sophisticated synchronization design; it is **one large command
buffer per frame, hardware vertex shading, VRAM-resident vertex/index/texture
data and no CPU cache maintenance on the hot path**.  Each draw costs a few
dozen state dwords plus 2 bytes per index.  Its waits are coarse (per frame,
per buffer update) and unbounded, and its KMD copies and weakly verifies user
commands: none of that is acceptable for a secure kernel, but it shows which
costs the hardware does not require.

## 4. Windows 7 WDDM stack (PCI 7122)

Evidence: static disassembly.  Checker:
`python3 tools/re/chrome9_win_draw_submit_audit.py` (pins both hashes; all
checks pass).  Scratch listings are in `~/tmp/re-perf/windows/`.

### 4.1 Corrections to earlier notes

- The KMD selector at `VTGKModeDX32A.sys:0x77fd0` maps PCI `0x7122` to chip
  **row 4** (`0x7806b`, `0x7809e`); row 4 at `.data 0x108468+4*0x34` is
  `{b66d0 submit, d3d80 paging, add10, c88a0, b5a90 patch, c7c00 CR install,
  a17a0, b88d0 rewind, b8be0 kickoff, b8ee0 ring space, a1f50, c8360 ISR
  ack, c2b60}` (P).  The tuple containing `0xc8190` belongs to row 1
  (PCI 1122/5122).  The VX900 kickoff `0xb8be0` pads to **16 bytes**
  (`0xb8c37 and edx,0xf`) and writes `041c=00100000`,
  `0420=64000000|lo24(cursor-0x10)`, `0420=65000000|hi8` (`0xb8d31`,
  `0xb8d40`) (P).  Claims that `0xc8190` with 128-byte padding is the VX900
  kickoff in [monotonic-command-ring.md](monotonic-command-ring.md),
  [cr-pause-stop-lifecycle.md](cr-pause-stop-lifecycle.md) and
  [persistent-batched-submit-performance-plan.md](persistent-batched-submit-performance-plan.md)
  are therefore for the wrong chip row.  The live-proven 128-byte closure
  remains valid as a conservative choice; it is not the vendor VX900 rule.
- For 7122 the UMD draw entries are `0x101b0460` (DP) and `0x101b05f0` (DIP)
  in family table `0x10331f18`; `0x10156b20` is the class-4 dirty-state
  emitter (entry `+0x20` of `0x103309c8+4*0xf8`), called with a 0/1 flag
  (P for the tables, S for the flag meaning).

### 4.2 DrawPrimitive / DrawIndexedPrimitive

- VB in VRAM or aperture pool (codes 2/4) -> `0x101b1b60`: hardware stream
  fetch with **generated inline 16/32-bit indices**, A20 `01`/`11`
  (`0x101f5e82`/`0x101f5e7d`), A2C = count (`0x101f5ee4`), A2D = 0, A24 via
  `0x101b0a20`, payload `i|(i+1)<<16` (`0x101b1ebf`); `StartVertex*stride` is
  folded into the element/stream address (`0x101b1c61..6e`,
  `0x10159700..98`) (P).  This independently matches the Linux recipe of
  section 3.3.
- VB in system memory: vertices copied inline, A20 `00` (`0x101b1150`,
  memcpy at `0x101b1393`) (P).
- DIP (`0x101b15e0`): indices copied inline from the IB lock pointer
  (`+0x48c8`, `0x101b1827..2d`, memcpy `0x101b199b`); A2D = MinIndex,
  A2C = MinIndex+NumVertices (`0x101b1659`/`0x101b1665`, one past the
  highest legal index); `BaseVertexIndex*stride` folded into the stream
  offset (`0x101b16fc`) (P).
- A hardware index-fetch form exists in code: `fe430020` (`|1` for
  aperture), `count*isize`, `0`, `0` with relocation `0x3c` patching address
  and length (`0x101b1430`, `0x101b155c`; KMD `0xb5e45`) (P for the code).
  Whether it is reachable on 7122 is U; the ring-level HW-IB split is read
  only for PCI 3230/3371/1122 (`0x11f6a5..e9`).

### 4.3 Security and relocation model

- KMD `DxgkDdiRender` (`0x84780`) checks only that the first dword's top
  byte is `0xFE` (`0x84bf4`, else `STATUS_ILLEGAL_INSTRUCTION`), keys a
  checksum (`0x848ba`), and **memcpy's the user command buffer into the DMA
  buffer** (`0x84cac` -> `0x5dec0`).  There is no packet parser (P).
- Addresses are applied by the KMD patch routine `0xb5a90` from the
  allocation/patch lists (P):
  - `0x39` (`0xb5dc5`): `w = (w & 0x3ff) | (A & 0xfffffc00)` stream palette;
  - `0x3a` (`0xb5ded`): `w = (w & 0xfff00fff) | ((A & 0x3ff) >> 2) << 12`
    element word;
  - `0x3b` (`0xb5e1b`): `w = (w & 0xfffe7fff) | L << 15`, A00 location,
    01 for the aperture segment, 00 for local (`0x7ef90`);
  - `0x3c` (`0xb5e45`): FE43 index-fetch address/length.
- DMA buffers per context: 512 KiB / 128 KiB / 64 KiB by context type,
  allocation list `0xc00` entries (`0x853f2`, `0x85425`, `0x8544c`,
  `0x851ee`) (P).
- Submit (`0xb66d0`) appends a first-level branch to the DMA buffer:
  engine-class header `fe430000` (3D), `fe400000`, `fe450000`, `fe470000`
  or `fe4a0000` (`0xb7380..0xb73bc`), ORed with `0x11` (and `0x80` for the
  video flag), then `size_dw<<2`, `addr & ~0xf`, `0` (`0xb73f1..0xb7479`);
  a generic `fe400001`/`fe400081` form follows the other branch
  (`0xb74c1..0xb74e4`), then the FE3x fence already described in
  [via-5.75-kmd.md](via-5.75-kmd.md) (P for the words; the meaning of the
  `0x10` bit on VX900 is S: the Linux header names `d0[5:4]` the branch
  hierarchy).
- Paging (texture/resource moves) uses FE07 descriptor DMA on MMIO
  `0x0e00..0x0e3c` (`0xd4239`, `0xd42c9`, `0xd43aa`) (P).

The WDDM design is **trusted DMA buffers + KMD relocation patching**.  That
matches Windows' threat model, not Linux DRM's: an unprivileged render node
cannot be given the same raw command access.

## 5. Recovered recipes usable by a secure kernel

All words below are kernel-generated from a structured UAPI; userspace
supplies handles, offsets, counts and index arrays only.

### 5.1 Vertex-buffer draw with inline indices (P vendor, M manual, not silicon-tested)

```text
# once per run (stream setup), stream s = 0
fe03043c 00000001 30030000 cc000000
(G & 0xfffffc00) | (stride >> 2)   cc000000 cc000000 cc000000
fe020440 0000043c 00040000 cc000000
A00 = (loc << 15) | (last_element << 8) | vertex_dwords     # loc 01 = SF
A01+e = ((1+e) << 24) | (s << 20) | (((G_e >> 2) & 0xff) << 12) | (ir << 8) | fmt
A12 = 12000000 | input_register_mask
A20 = 20000000 | 1 (16-bit) or 3 (32-bit)                   # triangle list
A23 = 23004f02
A2C = 2c000000 | (max & 00ffffff)
A2D = 2d000000 | (min & 00ffffff)
A2E = 2e000000 | ((max >> 16) & ff00) | (min >> 24)
# per draw in the same run
fe020440 0000043c 00040000 cc000000
24000000|n  cc000000 cc000000 cc000000
fe03043c <ceil(n/2) or n> 00000000 cc000000
<indices, final 16-bit lane zero>
```

Kernel obligations (from [indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md)
plus the manual clamp): derive `max` from the VB object size,
`(size - elem_off - elem_width - G_off) / stride`, and program A2C/A2D from
that bound rather than from user indices; the manual says out-of-range
indices terminate the list, so the kernel need not scan every index if a
silicon oracle confirms that clamp.  Until then, validate indices while
copying (the copy is required anyway because indices must live in command
memory).

### 5.2 Multi-draw in one list (P vendor)

Several A24 + index groups may follow one state/stream setup; both vendor
stacks re-emit only A24 (+A20/A2C-A2E when changed) and the index payload
between draws of the same state.  State changes between draws need only the
dirty H2/H3 groups; the manual defines no cross-list state reset.

### 5.3 Completion without `DST_POS`

Use FE33 system-write fences with a monotonically increasing ID into a
kernel-owned DMA page ([fe33-persistent-ring-production.md](fe33-persistent-ring-production.md),
manual T11 A05-A07); IRQ `0204[5]` only after a polled oracle.  This is the
prerequisite for more than one job in flight.

### 5.4 Branching to kernel-validated lists

FE40/FE43 first-level branch `(hdr|0x01|hier), size_dw<<2, addr&~0xf, 0`
into an SF command page lets the kernel build each job's list in its own
persistent pages and publish only a 16-byte branch plus fence into the main
ring, removing the 64-KiB ring-wrap reinstall from the hot path.  Only
first-level, kernel-written branch targets are acceptable: the target pages
must never be user-writable after validation.

## 6. Ranked throughput changes

Ranked by expected effect on glxgears / ioquake3 frame time, with the
evidence class of the magnitude.  "Security" states whether the change keeps
the kernel-built-command boundary.

| rank | change | expected impact | evidence | security |
|---:|---|---|---|---|
| 1 | Stop whole-object `clflush` + guard + DMA sync on every ioctl (K9, K10, K14, K15): track per-BO CPU/GPU ownership; transition only on explicit CPU access (a `CPU_PREP/CPU_FINI` ioctl around Mesa `bo.map` use and readback), and flush only CPU-dirty ranges.  Alternatively allocate GPU-only render targets/depth as WC/UC pages with plain PTEs, the pairing used by the old Linux KMD (needs a silicon oracle: an earlier WC-shmem test failed before later coherency fixes) | removes ~9 MiB clflush per ioctl at 1024x768 (0.6-4.8 GiB/frame in the model); likely the single largest CPU cost for large targets | static model (magnitude), vendor never flushes RT/VRAM (P) | unchanged |
| 2 | Asynchronous retirement: FE33 system-write fence IDs, pending-job list, return from ioctl after publish; Mesa waits only on map/readback/present | lets CPU vertex work overlap GPU raster; removes 3 sleeping polls per ioctl | vendor: one frame in flight (P); FE33 not silicon-tested | unchanged |
| 3 | One ioctl per flush: a multi-draw submit that carries many state-delta + draw records (kernel validates each record, emits dirty groups only), filled until glFlush/present/capacity; raise per-run capacity beyond 16/128 triangles with a pre-write bound | cmd22 cat 503 -> tens of ioctls/frame; vendor emits one command buffer per frame | P vendor, live counts | unchanged (structured records) |
| 4 | Persistent per-job command pages + FE40/FE43 branch instead of copying into the 64-KiB ring; or a larger ring | removes wraps every 3-17 appends and 64-KiB fill+clflush per wrap | model (P), vendor rings 4 MiB (P) | unchanged |
| 5 | Remove per-ioctl fixed work: 64-KiB `memset` + trailer fill (K11), in-kernel fragment recompile (K4: cache the compiled program by content hash per file), 152-byte vertex struct (U8: pack to the program's varying width), per-triangle Mesa rebuild/memcmp (U5: key by bound-state generation) | tens of microseconds per ioctl and 3.8x less copy-in | P (source) | unchanged |
| 6 | Mesa: flush the cmd22 batch at state change/flush boundaries, not per draw-module callback (U7); remove per-draw sampler-shadow copies (U2) | ioctl count; per-draw full texture memcpy | P | unchanged |
| 7 | Vertex-buffer draws with inline indices (section 5.1) using SF GEM vertex buffers; indices 2-4 B each instead of 40-152 B vertices through the ioctl | bytes per draw drop ~10x for indexed meshes | P vendor, M manual; not silicon-tested | kernel bounds A2C from BO size |
| 8 | Hardware vertex shading (native VS already partly decoded) to replace TGSI-exec software VS (U1); interim: LLVM draw when no VS textures are bound | removes per-vertex CPU interpreter cost, prerequisite for 7 to pay off | vendor VX900 uses HW VS (P); magnitude U (measure) | VS code must be kernel-validated like PS |
| 9 | Place render targets/depth in SL (VRAM carve-out) instead of snooped SF | unknown; removes GART translation and snoop traffic | vendor places RT/VB/textures in VRAM (P); benefit U | unchanged |
| 10 | Enable coarse Z (type-01 A01 bit13/bit9 + buffer setup) | GPU fill-rate for depth-heavy scenes | M only | kernel-owned buffer |

Measurement first: add the debugfs phase counters already specified in
[persistent-batched-submit-performance-plan.md](persistent-batched-submit-performance-plan.md)
(clflush bytes/time, wait time, compile time, copy bytes) and the manual's
PPC counters (section 2) before and after ranks 1-3, on the
`perf_stats=1` / disabled pair.

## 7. Open unknowns

- Whether RTSF PTE bit 25 actually snoops CPU caches on VX900 (would allow
  dropping explicit clflush for WB objects entirely); must be a guarded
  silicon oracle with CPU pre-write, no flush, GPU read (U).
- WC/UC shmem objects with plain PTEs as GPU-only targets (U; one early
  WC-shmem failure predates the A51/PTE25 fixes in
  [sf-render-target-coherency.md](sf-render-target-coherency.md)).
- Hardware index clamp behavior of A2C/A2D (M, untested).
- Semantics of branch header bits `0x10` and the FE43 vs FE40 class on VX900 (S).
- Linux UMD: flush type field `0x4f4` on VX900 (BCI vs ring), the H5
  vertex-input updater condition at `0x179e88`, stream-ID bit placement in
  A01+e `[23:20]` (U).
- Windows: reachability of the FE43 index-fetch form on 7122 (U).
- CPU cost split between TGSI VS, clflush and GPU wait on the target: needs
  the counters above (U).

## New files

- `tools/re/chrome9_cmd22_cost_model.c`: per-ioctl static cost model.
- `tools/re/chrome9_umd_vertex_path_audit.py`: Linux UMD invariant checker.
- `tools/re/chrome9_win_draw_submit_audit.py`: Windows UMD/KMD invariant checker.
