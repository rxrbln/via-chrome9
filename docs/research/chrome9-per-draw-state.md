<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 per-draw state inside one command segment

Date: 2026-09-24.  Scope: the command words, waits and host work spent per
draw inside one `DRAW_VERTEX_PROGRAMS` staging segment, which of them repeat
between consecutive draws with the same destination and depth, and a state
cache that removes the repeats.  No target access was used; every hardware
claim below that is not already proven by an earlier rung is marked
**needs rung** and has a test in the last section.

Source snapshot: kernel `drivers/gpu/drm/chrome9` at `35875c8` plus the
uncommitted `async_retire` working-tree change (line numbers below refer to
that `chrome9_engine.c`); Mesa `src/gallium/drivers/chrome9` at `fc54b52`.

Starting point (main session profile): ioquake3 q3dm1 800x600, GL1 about
32 fps, GL2 about 22 fps; about 104 draws per frame, about 25 draws per
`DRAW_VERTEX_PROGRAMS` ioctl, about 3.5 ms GPU wait per ioctl, i.e. about
140 us of GPU time per draw including fill.

## 1. What a segment contains

`chrome9_submit_fragment_programs()` (`chrome9_engine.c:6365`) packs the
ioctl's draws into 64-KiB staging segments (`CHROME9_RUN_STAGING_SIZE`,
limit `chrome9_fragment_segment_limit()` `:3351`) and retires each segment
before building the next (`:6726-6737`; only the last segment retires
asynchronously).

Per segment, not per draw:

| position | words | source |
|---|---|---|
| start | H0 `006c=00800000` (3D wait) | `chrome9_fragment_segment_begin()` `:3341` |
| end | FE33 `fe330004` system write with `WAIT_3D`, H0 3D wait, H2 A1C/A1D status, H2 `0x420/00300000` dummy, H0 `DST_POS` cookie | `chrome9_emit_3d_tail()` `:893` |
| end | PAUSE/STOP trailer and read-ahead fill | `chrome9_finish_ring_sized()` `:914` |
| host | memcpy of `STOP + 0x220` bytes to the ring, clflush, A64/A65 publish; on overflow PID10 STOP plus full 64-KiB dummy fill, clflush and A60-A68 reinstall | `chrome9_append_segment()` `:3761`, `chrome9_install_prime_ring()` `:3662` |
| host | FE33 fence, cookie, PAUSE and work-mask polls | `chrome9_wait_job()` `:4912` |

**There is no flush, fence or wait between two draws of one segment.**  The
only per-draw work the GPU sees is the state that
`chrome9_emit_fragment_draw()` (`:3051`) re-emits for every draw, followed
by the primitive packet.

## 2. Per-draw emission

`chrome9_emit_fragment_draw()` writes the complete state image for every
draw, in this order (`:3215-3290`):

| # | block | header / paratype | line | depends on |
|---|---|---|---|---|
| 1 | destination and blend (A50-A52, A38-A3F or A33-A37) | H2 `00010000` | 3215 | target, pitch, format, blend |
| 2 | per texture unit: image, mip table, sampler | H2 `u<<24\|00020000`, H3 `1003xxxx`, H2 `u<<24\|20020000` | 3218 (`chrome9_emit_fragment_texture()` `:2995`) | texture BO base, descriptor |
| 3 | common: A86/A87, A53, A00, A01 (cull, clip engine), depth A10-A13, bias, pixel-Z clip, stencil, A33 | H2 `00010000` | 3221 | depth/stencil/blend/cull |
| 4 | FE config A00-A0D | H2 `fe020000` | 3223 | PS varyings/textures, UCP |
| 5 | raster/FVF: A00 declaration, A01..A0n element words, A12, A61, A62, A20, A23, **A24**, A25-A28 linkage, A40-A52 viewport | H2 `00040000` | 3225 (`chrome9_tcl_raster_state()` `:2370`) | VS program, element layout and **element address bits 9:2** (`:2387`), index size, cull/flat, per-draw count |
| 6 | PS constants | H3 `22030000`, `22030080` | 3230 | PS constants |
| 7 | VS user constants | H3 `32030000+4*first` | 3238 | uniforms (MVP) |
| 8 | VS rows 256.. (viewport, snap bias) | H3 `32030400` | 3241 | viewport |
| 9 | VS integer constants (always zero) | H2 `33030000` | 3246 | none |
| 10 | clip/scissor A80-A83 | H2 `00010000` | 3248 | size, scissor |
| 11 | VS program | H2 `31030000` | 3251 | VS |
| 12 | A60 | H2 `00040000` | 3254 | VS |
| 13 | HPS location (only if `controls[0]` bit 19) | H2 `00010000` | 3262 | PS |
| 14 | PS controls A90-A9B | H2 `00010000` | 3265 | PS |
| 15 | PS ALU, TAU | H3 `20030000`, `21030000` | 3268 | PS |
| 16 | stream slots | H3 `30030000` | 3282 | vertex BO base |
| 17 | A2C/A2D/A2E/A24 | H2 `00040000` | 3287 | index range and count |
| 18 | indices | H3 `00000000` | 3290 | draw |

Blocks 1-15 are state; 16-18 are the draw.

### 2.1 Measured size

`~/tmp/agent-state/harness.c` runs the kernel's own
`chrome9_emit_fragment_draw()`, `chrome9_fragment_compile()` and
`chrome9_vertex_compile()` in userspace (extracted verbatim from the
snapshot) for an ioquake3-like stage B draw: two textures (mipmapped
diffuse, lightmap), `TEX; TEX; MUL`, VS = 4 x DP4 + three MOVs, X8Z24
depth, 20 triangles.  Dwords per draw, headers and alignment included:

| block | dw/draw |
|---|---:|
| destination/blend | 16 |
| texture image + mip + sampler, 2 units | 52 |
| common/depth | 32 |
| FE config | 16 |
| raster/FVF | 32 |
| VS constants (4 rows) | 20 |
| VS rows 256+ | 16 |
| VS integer constants | 8 |
| clip/scissor | 8 |
| VS program (14 records) | 60 |
| A60 | 8 |
| PS controls, ALU, TAU | 32 |
| **state total** | **300** |
| stream slots, A2C-A24, 60 indices | 50 |
| **draw total** | **350 (1400 B)** |

25 such draws are about 35 KiB per segment.  The ring is 64 KiB
(`CHROME9_RING_PAGES` 16), so every second segment takes the wrap path
(PID10 STOP, `chrome9_fill_ring_dummy()` `:3750` over 64 KiB,
`chrome9_cache_flush()` over 64 KiB `:3689`, A60-A68 reinstall and two MMIO
poll sequences).

### 2.2 What repeats between consecutive draws

Within one segment (same destination and depth by construction of the
ioctl, `:6425-6433`):

* **always identical**: VS integer constants (block 9, constant zero); VS
  rows (block 8) unless the viewport changes; destination (block 1) unless
  blend changes; clip/scissor unless the scissor changes; common (block 3)
  unless depth/stencil/alpha/cull state changes.
* **identical per material/shader**: texture units (block 2), PS
  controls/ALU/TAU (13-15), FE config (4), VS program and A60 (11-12).
* **identical per object transform**: VS constants (7).  The ioquake3
  world uses one MVP for all world surfaces.
* **raster/FVF (5)** is not identical even for equal programs: A24 carries
  the per-draw count, and for stage B the element words carry element
  address bits 9:2 (`:2387`).  The A24 in this block is redundant for
  stage B because block 17 writes A24 again immediately before the index
  payload.  The address bits change whenever the vertex data does not start
  at the same offset modulo 1 KiB.  Mesa uploads client arrays with 4-byte
  alignment (`chrome9_tcl.c:1465`), so for the ioquake3 GL1 client arrays
  they are expected to change on almost every draw (not yet counted).

Model results (all numbers are dwords actually emitted, prototype of
section 4, `~/tmp/agent-state/verify.c`):

| sequence of 25 draws | full | cached | removed |
|---|---:|---:|---:|
| one program, 8 materials (textures change every draw) | 8798 | 2846 | 68 % |
| identical state | 8798 | 1598 | 82 % |
| world plus entities (VS constants change every 5th draw) | 8798 | 1778 | 80 % |
| two programs interleaved | 8606 | 5022 | 42 % |
| two programs, vertex starts 1-KiB aligned | 8606 | 5022 | 42 % |
| two programs, unaligned vertex starts, scissor toggling | 8606 | 6206 | 28 % |
| one program, 8 materials, unaligned vertex starts | 8798 | 6398 | 27 % |

The last two rows are the current Mesa GL1 upload pattern: the unaligned
element words force the link group (section 4) out on every draw.  Aligning
client-array uploads to 1 KiB, or splitting the raster block (rung R4),
moves them to the 68 % row.

## 3. Host cost per draw

### 3.1 Kernel

Per draw, before the submit lock (`chrome9_draw_vertex_programs_ioctl()`
`:9840`):

* `chrome9_tcl_prepare()` `:9632`: `kvzalloc` of a 4,680-byte
  `chrome9_tcl_draw` (`:9692`, contains the 4-KiB compiled VS), `memdup` of
  buffers, elements, instructions, constants, `vmemdup` of indices, a
  min/max pass and a repack into a second `kvcalloc` (`chrome9_tcl_indices()`
  `:9557`, called `:9739`), finite checks of all constants.
* `chrome9_fragment_draw_prepare()` `:9185`: `memdup` of instructions,
  constants, texture descriptors, per-texture validation, `kvzalloc` of the
  2,276-byte `chrome9_fragment_program` (`:9391`), a full fragment compile
  and a full vertex compile (`:9408`) although most draws of a batch repeat
  the same programs.

That is about ten allocations and 7 KiB of zeroed memory per draw.  The
compilers themselves are small: on the build host the fragment plus vertex
compile of the model draw takes 0.33 us and the emission 0.07 us
(`~/tmp/agent-state/timing.c`).  The target profile at one draw per job
reported compile 18 us and build 43 us; the difference to the host figure is
not explained by instruction count alone and has to be attributed on the
target (allocation, cache misses, or phase boundaries of the profile) before
compile caching is ranked above allocation removal.

Per ioctl: `kvzalloc` of `chrome9_fragment_submit` (about 14 KiB, `:6396`),
GEM lookup only for new handles (`chrome9_fragment_submit_object()`
`:6224`, linear de-duplication, bounded by 64), `drm_exec` reservation,
`dma_resv_wait_timeout()` and `vmap` per object (`:6535-6560`), SF mapping
cache lookup per object (`:6585`), guard and flush work per object, the
stage B finite scan of every fetched float element of every draw
(`chrome9_tcl_buffers_finite()` `:2609`, called `:6567`).

Note for the finite scan: it reads the vertex BO through the kernel map
before the device flush; the BO stays mappable by userspace, so the check
does not by itself guarantee what the fetcher reads.  Whether a non-finite
fetched element can stop the engine is not established.

### 3.2 Mesa

Per draw in `chrome9_tcl_draw()` (`chrome9_tcl.c:1803`): VS variant lookup
is cached (`tcl_program()` `:1617`); the fragment state is rebuilt from the
semantic IR every draw (`chrome9_draw_tcl_fragment()` `chrome9_draw.c:3526`
-> `chrome9_generic_fragment_state()` `:1391`,
`chrome9_fragment_build_instructions()` `:1415`); indices are scanned for
min/max (`tcl_index_range()` `:1370`) and rewritten to 32 bit
(`tcl_draw_buffers()` `:1490`), which the kernel scans and repacks again;
`tcl_batch_append()` (`chrome9_draw.c:3722`) `MALLOC`s one block per draw
(`:3764`) and copies all arrays once more.  None of this touches the GPU;
it is small next to the kernel and GPU cost but is paid on the same CPU
while the GPU is idle.

The batch ends at a destination/depth change, 64 objects
(`:3756-3761`), `DRM_CHROME9_VERTEX_MAX_BATCH`, or any
`chrome9_draw_flush_batch()` caller (clears, framebuffer changes, sampler
shadows, VS textures, mapped buffers, fallback triangles).  At about 25
draws per ioctl ioquake3 still pays about four segment tails and
synchronous waits per frame; the flush reasons are not counted yet.

## 4. Segment-local state cache

### 4.1 Design

The cache lives for exactly one staging segment: it is created empty in the
submit loop next to `chrome9_fragment_segment_begin()` and discarded at
`chrome9_fragment_segment_end()`.  It is therefore independent of other
ioctls, other files, ring wraps, timeouts, resets, 2D/KMS paths and
suspend, which is what made cross-ioctl state omission unsafe in
[vendor-state-reuse-performance-audit.md](vendor-state-reuse-performance-audit.md).
Every segment starts with the complete state image, exactly as today.

State is partitioned into groups.  A group is omitted only if all its words
equal the words of its most recent emission in the same segment:

| group | blocks | compare |
|---|---|---|
| DESTINATION | 1 | words in staging |
| TEXTURE + unit | 2 (image, mip, sampler of one unit) | words in staging |
| COMMON | 3 | words in staging |
| PS_CONSTANTS | 6 (both banks) | words in staging |
| VS_CONSTANTS | 7 | words in staging, whole group (range and data) |
| VS_ROWS | 8 | words in staging |
| VS_INTEGERS | 9 | words in staging |
| CLIP | 10 | words in staging |
| LINK | 4, 5, 11, 12, 13, 14, 15 | inputs: FE config words, raster words with A24 masked, PS controls/ALU/TAU, VS records and A60 |
| (always) | 16, 17, 18 | none |

Rules:

1. The comparison is against the emitted words, not against pointers or
   userspace keys, so any difference in any input produces an emission.
2. The shadow of a word group is its previous emission in the staging
   buffer itself (pointer and length).  No copy and no fixed-size shadow
   array is needed; the staging buffer is not rewritten before the segment
   is published.
3. LINK is one unit because its blocks are interleaved with the constant
   uploads and because a VS program upload or A60 write may reset output
   configuration latched from A61
   ([chrome9-twoside-enable-re.md](chrome9-twoside-enable-re.md) rank 2);
   when any part differs, all of 4, 5, 11-15 are emitted in today's order.
   The raster comparison masks only the A24 word; raster words are all
   self-addressed registers, so this mask is exact.
4. VS constants are one group, not one per range, so an overlapping
   upload in between always forces the next upload out.
5. Stage A (inline vertices) does not use the cache: its only A24 is in the
   raster block.
6. On `-E2BIG` the segment ends and the cache is discarded with it; the
   capacity check still uses the full-state bound
   (`chrome9_fragment_state_bound()` `:3033`), so the change never lets a
   segment overflow.
7. Module parameter `state_cache` (default off) selects the path; off is
   byte-identical to today.

The hardware assumption behind all of this is that 3D registers, texture
descriptors, the PS/VS program stores and the constant stores keep their
values across an A24/index payload boundary within one command stream.  The
vendor drivers rely on it (dirty-range serialization, see
[windows-draw-path.md](windows-draw-path.md#dirty-state-serialization)),
and the current code already runs about 25 A24 payloads per segment, but a
draw with **no** state before its A24 has not run on the VX900 (hw-TCL rung
B7 is still open).  **Needs rung R1-R2.**

### 4.2 Prototype

`~/tmp/agent-state/proto/state-cache-prototype.diff` (158 added lines in
`chrome9_engine.c`) implements 4.1: `enum chrome9_state_group`,
`struct chrome9_state_cache` (with draws/emitted/omitted counters for the
profile output), `chrome9_group_begin()`/`chrome9_group_end()` around the
existing emit calls, `chrome9_link_equal()`, and the `state_cache` module
parameter.  It compiles cleanly as an out-of-tree `chrome9_engine.o`
(`make M=... W=1`).  It has not run on the target.

Verification in userspace (`~/tmp/agent-state/run.sh`): `verify.c` emits
every scenario of section 2.2 once with the unmodified and once with the
cached emitter, replays both streams through a register/upload model
(type 01, type 04 and FE02 registers by address; texture blocks and H3
uploads by paratype) and requires identical state at every index payload.
All scenarios pass.  Two injected faults (LINK always considered equal;
VS constants always omitted) are detected.  The model does not know
hardware side effects of a write; that is what R2/R3 are for.

Before landing: place the cache in `struct chrome9_fragment_submit`
instead of the stack, add the counters to `chrome9_prof_job()`, and keep
the parameter off until R1-R3 and R5 pass.

### 4.3 Intermittent lost jobs were not the cache (2026-09-25)

Symptom (module k71, libs p13/p15): piglit VS tests such as
`vs-op-mult-ivec3-ivec3` and `vs-op-mult-float-mat2x3` fail in bursts.
Probes read `0 0 0 0` (clear job and TCL draws both missing) or only the
clear colour (TCL draws missing).  A variant of the test that also probes a
cleared-only pixel showed a third form: the TCL draws present but the
clear missing.  So whole jobs were lost, not state inside a segment.

Excluded:

* `state_cache`: interleaved Y/N runs on the same module failed 18/30 and
  14/30.  The earlier 0/20 with `state_cache=N` was a quiet period.
  `async_retire=0` (15/480 vs 15/480) and `program_cache_kib=0` made no
  difference either.
* Leaked alpha test (A33 is written only with blend or depth): an alpha
  test left by another client did not reproduce the failure.

Root cause: SWIOTLB bounce buffers.  `chrome9_gem_create_ioctl()` put
objects in ZONE_DMA32 only when `chrome9_dma32_scanout_supported()` is
true, and that needs the `dma32_scanout` parameter, which is off by default
and passed as 0 by every bring-up and handoff script.  The target has
512 MiB above 4 GiB (`100000000-11fffffff`) and the device a 32-bit mask,
so pages from ZONE_NORMAL are mapped through SWIOTLB.  The SF engine
writes the bounce slots through non-snooped GART PTEs; swiotlb copies
between the slot and the shmem page with cached CPU accesses and no cache
maintenance, so `dma_sync_sgtable_for_cpu()` after retirement can copy
stale lines back into the page.  The bursts follow the free page count of
ZONE_NORMAL, which is why the failures came and went with the page cache.

Evidence (same test, `drop_caches` before every run so new pages come
from ZONE_NORMAL, `tools/re/chrome9_swiotlb_repro.sh`):

| module | fail | notes |
|---|---|---|
| k71 | 18/30, mat2x3 5/30 | all-zero and clear-only |
| k79 | 15/30 | |
| k71, `gart_nosnoop=0` | 0/30 | 14080 bounce slots in use |
| k71 + fix (`sc-fix2`) | 0/50, mat2x3 0/30 | no object bounced |

Fix (kernel `018a504`): every GEM and dumb object goes to ZONE_DMA32
whenever the DMA mask is exactly 32 bits, independent of the scanout
flag; an object that still gets bounce slots logs one warning.  This also
removes the per-job bounce copies of every render target and texture.
The segment-local state cache stays on by default.

## 5. Ranked plan

Ranking is by expected frame-time savings for ioquake3 GL1 at 800x600.
GPU-side savings are unknown until R3; the ranges below state what each
outcome of R3 would mean.

1. **Segment-local state cache (section 4).**  Cuts segment bytes by
   27-82 % (model).  Certain host effects: fewer bytes to build, copy and
   clflush into the ring, and with segments of about 10-25 KiB instead of
   35 KiB the 64-KiB ring wraps every 3-6 segments instead of every second.
   GPU effect: if R3 shows that re-writing a program store, texture
   descriptor or FE block costs a pipeline drain, a large part of the
   about 140 us of GPU time per draw (fill share not yet measured) sits in
   the omitted groups and this is the largest single item, potentially
   several ms per frame; if R3 shows only command-fetch cost, the saving is
   the fetch time of 0.4-1.0 KiB per draw.  Risk: low once R1/R2 pass;
   the path is segment-local and has an off switch.
2. **LINK hits for GL1: 1-KiB aligned client-array uploads in Mesa**
   (`u_upload_data_ref(..., 4, ...)` `chrome9_tcl.c:1465` -> 1024).  No
   hardware change: the element words are unchanged in meaning.  Moves
   ioquake3 GL1 from the 27 % row to the 68 % row.  Costs at most 1 KiB of
   uploader space per draw.  For VBO draws (GL2) the equivalent requires R4.
3. **Kernel compile and allocation reuse within one ioctl.**  When a draw's
   fragment inputs (instructions, counts, descriptor mask, ABI) equal an
   earlier draw of the same ioctl, copy that compiled program and replace
   only the constants (the compiler copies constants without using their
   values, `chrome9_fragment.c:1397`); likewise reuse the VS output when
   instructions, element layout, constant range, two-side and clip mask and
   the fragment varying layout match.  Replace the per-draw allocations with
   one arena per ioctl sized from the user counts.  Expected: up to the
   measured 18 us compile per draw (about 1.9 ms per frame at 104 draws) if
   the target profile attributes it to the compilers; first add per-draw
   profile counters (compile, allocation, emit) to confirm.  No hardware
   risk.
4. **Build the next segment while the GPU runs the previous one.**  In a
   multi-segment ioctl, emit the draws of segment k+1 into staging before
   `chrome9_wait_job()` of segment k, and only the tail (which poisons the
   single FE33 slot, `chrome9_emit_fe33()` `:860`) after it.  Needs a
   second staging buffer because the ring copy happens in
   `chrome9_append_segment()` after the wait.  Saves the build time of each
   non-final segment; smaller after item 1.  No hardware risk.
5. **Mesa batch length.**  Count `chrome9_draw_flush_batch()` reasons per
   frame; every avoided break saves one segment tail, one synchronous
   retirement and one full-state first draw.  Candidates only after the
   counts exist.
6. **Mesa CPU trimming.**  Cache `chrome9_generic_fragment_state()` output
   on CSO/sampler-view/resource identity; hand the kernel 16-bit indices
   when the range allows.  Small.
7. **Raster split (R4).**  Emit the per-draw declaration (A00, A01..A0n,
   A20, A24) as its own H2 `00040000` block after the linkage words, so
   LINK hits no longer depend on vertex placement (GL2 VBO draws).  Needs
   a silicon rung because it changes the order of type-04 writes.

Not proposed: omitting state across segments or ioctls.  That requires the
kernel owner/generation protocol of the vendor audit and gains only the
first draw of each segment once item 1 is in.

## 6. Silicon rungs

All rungs run with the usual 7-s cap, `profile=1`, one client, and compare
against the same draw sequence with `state_cache=0` (full state, the proven
path).

**R1 (hw-TCL B7): state-free second draw.**  One segment: draw A with full
state, then draw B consisting only of blocks 16-18 (same program, same
textures, different index range, different screen area).  Pass: pixels
identical to the two-draw full-state reference, no timeout, FE33 id and
cookie retire.  Then 64 such B draws in one segment.

**R2: per-group persistence and interaction.**  Two draws per segment,
second omits exactly one group g while one other group h changes value
(h is emitted).  Pairs: (g=TEXTURE, h=LINK), (LINK, TEXTURE), (LINK,
VS_CONSTANTS), (LINK, COMMON with a different depth function), (LINK,
CLIP), (COMMON, LINK), (DESTINATION with blend, LINK), (VS_ROWS, LINK),
(PS_CONSTANTS, LINK).  Pass: pixel identity with the full-state reference
for every pair.  A failure identifies a write with side effects on another
group; that group pair then has to be merged in 4.1.

**R3: where the per-draw GPU time goes.**  One segment of N = 64 tiny
draws (one triangle of about 16 pixels, depth off) so fill is negligible.
Measure the FE33 fence wait per segment:

* MIN: full state once, then 63 draws of blocks 16-18 only (needs R1);
* FULL: today's stream;
* MIN+g for each group g: g re-emitted with identical words before every
  draw;
* MIN+g' for TEXTURE, LINK and VS_CONSTANTS: g re-emitted alternating
  between two different values;
* MIN+pad(K): K dwords of `cc000000` padding per draw, K in 64, 256, 1024,
  to calibrate command-fetch cost per KiB.

Per-draw cost of g = (t(MIN+g) - t(MIN)) / 63.  Comparing MIN+g with
MIN+pad of the same size separates fetch cost from processing or drain
cost.  This rung decides how far item 1 moves the frame time and whether
LINK needs finer groups.

**R4: raster split.**  Full-state stream with the raster block split into
linkage (A12, A61, A62, A23, A25-A28, A40-A52) followed by declaration
(A00, A01..A0n, A20, A24), in both orders.  Pass: glxgears and the hw-TCL
oracle corpus pixel-identical to the unsplit stream.  Only then allow LINK
to omit the linkage block while emitting the declaration.

**R5: application run.**  `state_cache=1` against `state_cache=0` on
glxgears, glmark2 (validation set) and ioquake3 q3dm1 GL1/GL2: identical
validation results and screenshots, plus the profile lines (fence wait per
job, emitted/omitted dwords, ring wraps).

## 7. Reproduction

```sh
~/tmp/agent-state/run.sh        # model tables, host timing, equivalence check
python3 ~/tmp/agent-state/decode.py <segment.bin>   # block list of a dumped segment
```

`engine.c.orig` in that directory is the analysed kernel snapshot;
`emit_extract.c` and `emit_proto.c` are verbatim extracts of the unmodified
and the prototype emitter.

## 8. Follow-up: rebased cache, rungs R2/R3, Mesa batch breaks

Date: 2026-09-24.  Kernel base: `drivers/gpu/drm/chrome9` HEAD `85ad597`
(asynchronous retirement, `GEM_CPU_ACCESS WAIT`); its `chrome9_engine.c`
is byte-identical to the `engine.c.orig` snapshot analysed above, so
sections 1-4 still apply with unchanged line numbers.  Mesa: the working
tree of `src/gallium/drivers/chrome9` on top of `b7025cd` (it carries
uncommitted changes of the main session; the line numbers below refer to
that working tree).  No target access; nothing below has run on silicon.

### 8.1 Rebased state cache

`~/tmp/agent-state/rebase/state-cache.patch` (-p1, path
`drivers/gpu/drm/chrome9/chrome9_engine.c`, 181 added / 8 removed lines,
signed off) applies to HEAD `85ad597` (`git apply --check` in the driver
repository passes) and builds without warnings out of tree
(`make -C linux-7.2 M=~/tmp/agent-state/rebase/mod CONFIG_DRM_CHROME9=m W=1
modules`).  Relative to the prototype it changes:

* the `state_cache` parameter block is placed after `async_retire`; the
  prototype had split the `async_retire` comment from its parameter;
* the cache lives in `struct chrome9_fragment_submit` (patched file
  `:6380`), not on the stack of the submit loop;
* the parameter is sampled once per segment (`:6839`) and the cache is
  cleared there.  The prototype read it per draw; a toggle in the middle
  of a segment would have let a cached draw compare against an emission
  that an uncached draw had since overwritten in the hardware;
* omitted dwords of a draw that ends in `-E2BIG` are not counted
  (`pending` field);
* `profile=N` prints `profile state cache: draws D, emitted E omitted O
  dwords per job` (`:254`).

`state_cache=0` (default) remains byte-identical to HEAD: the only other
change on that path is the 16-byte padding before each group, which the
following H2/H3 header would have written at the same place.

### 8.2 Rungs R2 and R3

Source `~/tmp/agent-state/rungs/chrome9_state_rungs.c` (GPL-2.0-only),
host build `~/tmp/agent-state/rungs/build.sh`:

```sh
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers -D__user= \
    -I$K/include/uapi/drm -o chrome9_state_rungs chrome9_state_rungs.c
```

(`$K` = `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2`;
`-D__user=` because the tree's `usr/include` has no `chrome9_drm.h`.)

The tool needs root, the patched module and no other GPU client.  It
sets `profile=1` (R3 also `async_retire=0`, so the last segment's wait is
profiled) and `state_cache`, and restores all three on exit, SIGTERM and
SIGINT.  Profile lines are read from `/dev/kmsg`.  Destination reads use
`GEM_CPU_ACCESS` with `WAIT | PREPARE_READ`, because with asynchronous
retirement the ioctl can return before the GPU has finished; this also
applies to the older oracles in `tools/re`, which read the destination
directly after the ioctl.

All draws are stage B (`buffer_count` 1, 16-bit indices) with an MVP of
four DP4 lanes (VS constants c0..c3), primary colour and one FLOAT4
texture coordinate varying, into a 64x64 BGRA8888 target; vertex data of
draw k starts at k x 1 KiB unless the case says "packed".  The pixel
programs are `TEX*D0`, `TEX`, `TEX*TEX` and `TEX*c0` on 2x2 textures.

**R2** runs each case three times in one `DRAW_VERTEX_PROGRAMS` call per
run: `state_cache=0`, `state_cache=0` again (determinism), `state_cache=1`.
PASS requires identical colour (and depth, for depth cases) in all three,
intact guard pages, pixels drawn, and a non-zero omitted count in the
cached run's profile line.

| case | draws | groups changing between draws |
|---|---:|---|
| same | 16 | none: every draw after the first is blocks 16-18 only (R1) |
| tex | 16 | TEXTURE (unit 0 alternates) |
| link-ps | 16 | LINK via the pixel program |
| link-vs | 16 | LINK via the vertex program (swizzled texcoord) |
| vsconst | 16 | VS_CONSTANTS (translated MVP) |
| link-vsconst | 18 | LINK every 2nd, VS_CONSTANTS every 3rd draw |
| psconst | 16 | PS_CONSTANTS |
| scissor | 16 | CLIP |
| viewport | 16 | VS_ROWS |
| blend | 16 | DESTINATION and COMMON (blend on/off, 50 % alpha) |
| depth | 16 | COMMON: LESS/ALWAYS/LEQUAL, overlapping quads at two depths |
| depth-link | 16 | LINK with constant depth state (COMMON omitted) |
| unit-gap | 15 | two units, then one unit, then two: unit 1 omitted after a draw that did not use it |
| tex-packed | 16 | TEXTURE with unaligned vertex starts (LINK by element address, Mesa GL1 pattern) |
| mixed | 64 | all non-depth groups, pseudo-random |
| mixed-depth | 64 | all groups including depth function, pseudo-random |

**R3** draws one triangle of 18 pixels per draw (depth off).  For each
case it runs 1 draw and 64 draws per ioctl, with `state_cache` 0 and 1,
one warm-up plus five measured runs each, and reports the median.  The
per-draw cost is the slope `(t(64) - t(1)) / 63` of the wall time (ioctl
entry to `CPU_ACCESS WAIT` return) and of the profile `gpu` phase (sum of
the per-segment retirement waits); the job line also shows segments per
job and emitted/omitted dwords.  PASS requires identical pixels between
the full and the cached run and a valid profile with omitted state.

| case | re-emitted per draw with the cache |
|---|---|
| same | nothing (approximates MIN of section 6) |
| same-packed | LINK (element address bits change) |
| tex | TEXTURE (alternating) |
| vsconst | VS_CONSTANTS (alternating) |
| link | LINK (alternating pixel program) |
| clip | CLIP (alternating scissor) |

With the cache, `(t(case) - t(same)) / 63` is the cost of re-emitting that
group with changed words; the full-state column of `same` is today's
stream.  The padding calibration of section 6 (MIN+pad) needs a kernel
debug knob and is not part of this tool.

Target commands (copy `chrome9_state_rungs` to the target, load the patched
`chrome9.ko` through the usual hash-gated module handoff, stop X first):

```sh
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_state_rungs r2 all
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_state_rungs r3 all
# one case, e.g. after a failure or a time-budget message:
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_state_rungs r2 unit-gap
```

Each invocation stops starting new cases after 6 s and prints `FAIL time
budget exhausted` for the remaining ones.  Every case prints exactly one
`R2 <case> PASS|FAIL ...` or `R3 <case> PASS|FAIL ...` line; the exit
status is non-zero if any case failed.  `run-target.sh` in the same
directory contains the two `all` commands.

### 8.3 Why Mesa flushes a TCL batch

A batch is one `DRAW_VERTEX_PROGRAMS` call (`chrome9_tcl_batch_flush()`,
`chrome9_draw.c:3655`).  Every trigger in the current tree:

| # | trigger | site | relevant to ioquake3 GL1 |
|---|---|---|---|
| 1 | 64 queued draws (`DRM_CHROME9_VERTEX_MAX_BATCH`) | `chrome9_draw.c:3754` | yes, with about 104 draws per frame at least two batches |
| 2 | different destination handle | `chrome9_draw.c:3755` | no (single back buffer) |
| 3 | different depth handle, i.e. depth test (or stencil) switched on or off: `depth_active` (`:963`) sets `depth_handle` to 0 without a depth test (`:1506`), and the kernel requires equal `DEPTH` flags in one call | `chrome9_draw.c:3756` | yes: 2D/HUD and shaders with `GLS_DEPTHTEST_DISABLE` |
| 4 | more than 64 distinct kernel objects (2 + textures + vertex buffers, kernel `CHROME9_FRAGMENT_MAX_OBJECTS` `chrome9_engine.c:279`) | `chrome9_draw.c:3757-3759` | likely: every world surface brings a diffuse texture and a lightmap page |
| 5 | batching off (`CHROME9_TCL_BATCH=0`) or the per-draw `MALLOC` fails | `chrome9_draw.c:3829`, `:3763` -> `:3844` | no |
| 6 | draw-module (vbuf) triangles are submitted: the TCL queue goes first | `chrome9_draw.c:1909`; reached from the next TCL draw (`:3543`) and the vbuf flush sites | yes for every TCL fallback: `chrome9_tcl.c:1823-1881`, `:1964`, `:1978` (reasons printed with `CHROME9_DEBUG_TCL`) |
| 7 | `chrome9_draw_flush_batch()` (`chrome9_draw.c:3403`) from: `pipe->flush` / SwapBuffers | `chrome9_context.c:7002` | yes, once per frame |
| 8 | ... clears | `chrome9_context.c:6737`, `:6790`, `:6812` | yes: depth clear per `RE_RenderScene` view (main view, HUD models) |
| 9 | ... `set_framebuffer_state` | `chrome9_context.c:6364` | rare (cso filters identical states) |
| 10 | ... CPU mapping of a non-buffer resource (texture upload, readback) through `chrome9_context_sync_resource()` | `chrome9_resource.c:619` -> `chrome9_context.c:220` | only with texture uploads during the frame; flushes even if no queued draw uses that texture |
| 11 | ... synchronized mapping of a buffer that a queued draw references | `chrome9_resource.c:622` -> `chrome9_draw.c:3691` | no for GL1 client arrays (uploads are unsynchronized); yes for GL2 `glBufferSubData` of the tess VBO |
| 12 | ... sampler views with a CPU shadow (imported, non-guarded textures), every draw | `chrome9_context.c:6843` | no |
| 13 | ... vertex-shader textures, guarded fragment domain, llvmpipe fallback, mipmap generation, `flush_resource`, context destruction | `chrome9_context.c:6861`, `chrome9_draw.c:3434`, `chrome9_swfb.c:679`, `chrome9_resource.c:873`, `chrome9_context.c:7035`, `:7106` | rare |

How often each fires per ioquake3 frame is not known without the target.
`~/tmp/agent-state/mesa-flush-reasons.patch` (not applied; compiles with
the `objs-chrome9-full` and `objs-chrome9-llvm` commands) attributes every
submitted batch to its trigger: `chrome9_draw_flush_batch()`,
`chrome9_draw_flush_gpu_batch()` and `chrome9_tcl_batch_flush()` become
macros passing `__func__`, `tcl_batch_append()` names its four limits
(`append:max-draws`, `append:destination`, `append:depth`,
`append:objects`), and with `CHROME9_DEBUG_TCL=1` the periodic statistics
line is followed by `chrome9: TCL batch flushes (site=batches/draws): ...`.

Proposed extensions, each keeping the ordering guarantees:

1. **Depth on/off (3).**  When a compatible depth buffer is bound but the
   test is off, keep `depth_active` and submit `DEPTH_FUNC(ALWAYS) |
   DEPTH_READ_ONLY`, which `chrome9_draw_depth_flags()` already produces
   for stencil-only draws; the kernel clears the Z write enable for
   `READ_ONLY` (`chrome9_engine.c:1281`).  Pixel-neutral; possible cost is
   Z reads for HUD pixels (measure).  Alternative in the kernel: allow
   draws without `DEPTH` in a call whose depth object is present; COMMON
   is re-emitted in full for the next depth draw anyway.
2. **Object limit (4).**  Size the per-call object arrays of
   `struct chrome9_fragment_submit` from the call's counts instead of the
   fixed 64 and advertise the new limit with a capability; the real bound
   is the GART budget (`CHROME9_GART_SIZE` 64 KiB of PTEs, 64 MiB mapped,
   `CHROME9_DRAW_CACHE_MAX_ENTRIES` 64), so the kernel check has to be on
   mapped pages, and Mesa has to count pages as well as handles.
3. **Fallback draws (6).**  Count them first (`CHROME9_DEBUG_TCL`); a
   fallback that remains can join the batch as a stage A entry
   (post-transform vertices with a pass-through vertex program) instead
   of the separate vbuf submission.
4. **Mid-frame depth clears (8).**  Queue a full-target depth clear as a
   batch entry (a quad with depth `ALWAYS`, colour mask 0) when no
   scissor or partial clear is involved.
5. **Texture mappings (10).**  Flush only when the TCL or vbuf queue
   references the resource or it is the current colour or depth target,
   as the buffer path already does (`chrome9_tcl_batch_flush_resource()`).
6. **Buffer rewrites (11, GL2).**  Handle `PIPE_MAP_DISCARD_WHOLE_RESOURCE`
   by giving the resource new storage while the queued draws keep the old
   BO referenced, and small `DISCARD_RANGE` writes through the stream
   uploader plus a queued copy.
7. **Batch length (1).**  Raise `DRM_CHROME9_VERTEX_MAX_BATCH` together
   with item 2 once the other breaks are gone; per-draw kernel arrays are
   then dynamically sized as well.

Items 1 and 5 are small Mesa changes without kernel impact; 2 and 7 need
a UAPI capability; the order should follow the counts of the
flush-reasons patch.

### 8.4 1 KiB client-array alignment

`~/tmp/agent-state/mesa-align.patch` (-p1 from the Mesa root, not applied,
`git apply --check` passes, `chrome9_tcl.c` compiles) replaces the 4-byte
alignment of `u_upload_data_ref()` in `tcl_bind_buffers()`
(`chrome9_tcl.c:1465`) with `TCL_UPLOAD_ALIGNMENT` 1024.  Every client
array's rebased vertex 0 then starts on a 1-KiB boundary of a page-aligned
upload BO, so element address bits 9:2 in the FVF words depend only on the
element offsets, and consecutive GL1 draws with the same programs hit the
LINK group (section 2.2: 27 % -> 68 % removed state in the model).  Cost:
at most 1020 bytes of the 1-MiB stream uploader per array and draw, i.e.
a new uploader BO (one more kernel object) after fewer draws.  VBO draws
(GL2) still start at `min * stride` and need R4.

### 8.5 Files

| file | content |
|---|---|
| `~/tmp/agent-state/rebase/state-cache.patch` | kernel state cache on HEAD `85ad597` |
| `~/tmp/agent-state/rebase/{orig,mod}/` | exported HEAD driver and patched copy (out-of-tree build) |
| `~/tmp/agent-state/rungs/chrome9_state_rungs.c`, `build.sh`, `run-target.sh` | R2/R3 rungs |
| `~/tmp/agent-state/mesa-align.patch` | 1-KiB upload alignment |
| `~/tmp/agent-state/mesa-flush-reasons.patch` | per-site batch flush counters |
| `~/tmp/agent-state/mesa/cc.py`, `cc-llvm.py` | syntax check of a modified Mesa file with its build command |

### 8.6 Rebase onto 4f6eeda (strips/fans, cube/DXT, points/lines/polygon mode)

`~/tmp/agent-state/rebase2/state-cache.patch` (-p1 from the linux-7.2
root, 183 added / 8 removed lines, signed off) applies to driver HEAD
`4f6eeda` (`patch -p1 --dry-run` clean) and builds warning-free with
`W=1` out of tree (`~/tmp/agent-state/rebase2/mod`).  Only the emission
hunk had to be redone, because `chrome9_emit_fragment_draw()` now passes
`fragment_texture_flags` to `chrome9_emit_fragment_texture()`.

Coverage of the new state words, all of which are compared as emitted:

* topology cycle bits (strip `0x1c7800`, fan `0x1c5c00`, line strip
  `0x206000`) are ORed into A20 of the raster block, point size A2A/A2B,
  sprite enables A2F and the A25[19] point-size flag are raster words:
  all are part of the LINK comparison, which masks only A24;
* the sprite enable A01[21] and the line A01[23] clear are in COMMON;
* the cube face words A01..A05 (face closure for 2D units), the A18 mode
  word and the cube A22 are inside the texture unit's H2 image block and
  therefore in that unit's TEXTURE group;
* the vertex program compiled for point size and sprite masks is compared
  record by record (LINK).

Equivalence model (`~/tmp/agent-state/model2/run.sh`): `gen.sh` now
extracts the dependency closure of `chrome9_emit_fragment_draw()`,
`chrome9_tcl_topology()` and `chrome9_validate_fragment_texture()`
directly from the unmodified and the patched `chrome9_engine.c`
(`extract.py`), so the model follows the driver without hand-made
extracts; draw fields come from the kernel's compilers, topology and
texture validation.  The replay keys H2 register writes by (paratype,
register), so a unit's cube face words that a later block does not
rewrite stay visible.  All 16 checks pass: model self-test, the four
earlier scenarios, list/strip/fan cycling, strip every 4th draw,
triangles/lines/line strip/points, point size 1/4, point sprite on/off,
polygon mode line/fill, cube/2D with face closure, cube face-stride
change, cube followed by 2D without closure, and 40-draw random mixes
with aligned and unaligned vertex starts (28-81 % of the dwords removed).
Injected faults are detected: masking A20/A2A/A2F in the LINK comparison
fails the topology and mixed cases, comparing only the first texture
dwords fails the face-stride case.

Rung tool: built against a copy of the current UAPI header
(`rungs/chrome9_drm.h`).  New R2 cases `topology` (list/strip/fan),
`prim-class` (triangles/lines/points), `point-size`, `polygon-mode` and
`cube-2d` (unit 0 cube vs 2D); new R3 case `topology` (list/strip).
Cases whose `CAPABILITIES2` bits are missing print `SKIP`.  Texture
coordinates are now (u-0.5, v-0.5, 1, 1) so cube lookups have a defined
+Z major axis.  Run commands are unchanged (section 8.2).

While this was done the driver moved to `5749595` (draw mapping cache
bounded by GART pages).  `~/tmp/agent-state/rebase3/state-cache.patch` is
the same change on `5749595` (only the profile hunk moved next to the new
`chrome9_prof_engine()` call); it builds warning-free with `W=1` and the
model passes all 16 checks against it.  Neither patch applies to the
current uncommitted working tree (query objects in progress, 4 hunks
fail); rebase again once that work is committed.

### 8.7 Rebase onto 272da90 (occlusion queries, 3D textures)

`~/tmp/agent-state/rebase4/state-cache.patch` applies to driver HEAD
`272da90` (`patch -p1 --dry-run` clean against the committed tree) and
builds warning-free with `W=1` (`rebase4/tree`).

* Occlusion query words are per-draw actions outside every group: the
  A1C/A1D counter reset is emitted before the first cached group and the
  snapshot (optional type 0 3D sync, type 10 read-back of A1C, A21/A22
  destination) after the index payload, exactly as without the cache.
  After a query draw the cache is cleared, so the next draw re-emits its
  complete state instead of assuming that the read-back and drain keep
  every register (needs rung; costs one full-state draw per query draw).
* The 3D texture words (A18 mode 2, A21 slice count, A22 depth exponent,
  R wrap in the sampler block) are inside the unit's image and sampler
  blocks and therefore compared in its TEXTURE group.

Model (`~/tmp/agent-state/model3/run.sh`, extracted from `272da90`): 20
checks pass with query mode 0, 1 and 2 (`QUERY_MODE`), including a query
tag on every 3rd draw, query tags on consecutive draws with program
changes, 2D/3D on unit 0, and 3D depth change plus 3D/cube.  Besides the
register state at each draw, the model now requires the identical
sequence of draws, counter resets, read-backs and syncs in both streams;
moving the reset into the destination group is detected by that check.

Rung tool: header updated; new R2 cases `tex-3d` (unit 0 alternates 2D
and a 2x2x2 3D texture) and `query` (depth on, every 3rd draw tagged; the
query result of the cached run must equal both full-state runs).  With
`CAPABILITIES2` query support every R2 run is bracketed by BEGIN/END and
READ with WAIT.  Run commands are unchanged (section 8.2).

Note: after `272da90` was committed the working tree again carries
uncommitted changes (`chrome9_engine.c`, `chrome9_query.c`,
`chrome9_vertex.[ch]`); the patch applies to the committed `272da90`
but not to that working tree (4 hunks).

### 8.8 Rebase onto bde40ec (VS static flow control, occlusion write-back)

`~/tmp/agent-state/rebase5/state-cache.patch` applies to driver HEAD
`bde40ec` (`patch -p1 --dry-run` clean, no fuzz) and builds warning-free
with `W=1` (`rebase5/tree`).

* Vertex flow constants are part of the LINK group: bool constants are
  ORed into A61, which is a raster word compared by LINK; int constants
  are compared in `chrome9_link_equal()` together with `int_upload`, and
  their `33030000` upload (counted H3 for flow programs, the zero H2 form
  otherwise) is emitted exactly when LINK is emitted, in its old position
  between the VS rows and the clip block.  The separate VS_INTEGERS group
  is gone.
* Occlusion reset, optional drain/idle and the type 17h write-back stay
  outside every group (their emission is unchanged); the cache is still
  cleared after every query-tagged draw.

Model (`~/tmp/agent-state/model4/run.sh`, extracted from `bde40ec`): 23
checks pass for query modes 0-2 and occlusion RE bits 0 and 0xff
(`QUERY_MODE`, `QUERY_RE`), including new cases toggling the bool
constant (A61), toggling the REP int constant (`33030000`), and mixing
flow and plain programs with a query-tagged flow draw.  Dropping the int
constant comparison from LINK is detected.

Rung tool: header updated to the `bde40ec` UAPI and rebuilt; run commands
unchanged (section 8.2).

### 8.9 Silicon result and texture-cache fix (rebase6)

Silicon (module k62 = `38e43f1` + rebase5): R2 passed for 20 cases
including `query` (447/447/447 samples); `tex`, `mixed`, `mixed-depth`
and R3 `tex` failed.  R3: per-draw GPU time full about 0.9 us, cached
0.3-0.6 us; 64-draw job GPU 74-85 us full, 40-59 us cached; two segments
become one.  State emission therefore costs less than 1 us of GPU time
per draw; the cache is mainly a CPU and ring-traffic saving.

Cause of the failures.  In `tex` the first differing pixel (8,0) belongs
to draw 1 (unit 0 = texture 1, PS `TEX*D0`, primary `ff2f5a13`).  The
full-state value `ff06440e` is texel `ff20c0c0` of texture 1 times the
primary colour; the cached value `ff2f1702` is texel `ffff4020` of
texture 0 times the same colour.  The cached draw did emit its new
texture group (the group alternates, so it is never equal to the
previous emission), but it sampled the previous image.  Everything else
of that draw was omitted, including the FE configuration of the LINK
group, whose A00 bit 0 is the texture-cache invalidation pulse
([chrome9-desktop-r2t-ordering-audit.md](chrome9-desktop-r2t-ordering-audit.md#consumer-texture-cache-invalidation),
vendor `via3DBlit` A00 `00000011`).  New image words take effect only
with that pulse.  All passing texture cases changed LINK at the same time
(`link-ps`, `unit-gap`, `tex-packed` by element address, `cube-2d` and
`tex-3d` by the program's coordinate width), so they re-emitted A00; the
failing cases are exactly those where a texture changed and LINK did not.

Fix: a draw that writes any texture group re-emits the FE configuration
block (identical words, A00 bit 0 set) after COMMON, even when the rest
of LINK is omitted.
`~/tmp/agent-state/rebase6/state-cache.patch` is the complete change
against clean `38e43f1` (dry run clean on `git archive HEAD`, W=1
warning-free); `rebase6/state-cache-fix-over-rebase5.patch` is the
incremental fix for a tree that already carries rebase5 (dry run clean on
the current working tree).

Model: `~/tmp/agent-state/model5` adds the latch rule: the texture state a
draw samples is the texture register/mip state at the most recent FE A00
write with bit 0.  With that rule the rebase5 emitter fails exactly the
texture-change-without-LINK scenarios (8 materials, two programs, cube
face stride, 3D depth change, random mix), matching silicon, and rebase6
passes all 23 checks.

Silicon must confirm:

1. R2 `tex`, `mixed`, `mixed-depth` and R3 `tex` pass with rebase6, and
   every other R2/R3 case still passes;
2. that FE configuration alone (without the raster block, VS/PS program
   and A60) suffices, which `tex` covers directly;
3. optional narrowing: whether a single-word FE A00 write is enough
   (would save 12 dwords per texture change; not implemented);
4. R3 `tex` timing with the extra FE block, to confirm the pulse does not
   add a pipeline drain comparable to a full LINK emission.

## 9. Kernel CPU per draw: program cache and per-ioctl arena

ioquake3 GL1 at 200x150 runs CPU-bound (73 fps, 86 % of one core); 46.7 %
of that thread's CPU is kernel time.  The top kernel symbols came from
preparing each of the ~25 draws per `DRAW_VERTEX_PROGRAMS` job:
`memset_orig` 5.2 % (all from the ioctl), vertex compile with its
helpers about 5.5 %, fragment compile 1.6 %, `kfree` 1.6 %.  Every draw
zeroed a 4.7 KiB `chrome9_tcl_draw` (vertex program embedded) and a
2.3 KiB fragment program, copied each user array into its own allocation
and compiled both programs, although a frame repeats a handful of
programs.

`~/tmp/agent-kcpu/kernel-cpu.patch` (on 73e7e08; W=1 and checkpatch
clean):

* **Per-file compiled program cache** (`chrome9_progcache.c`, owned by
  the `chrome9_file` of `chrome9_query.c`, freed at postclose).  The key
  holds every input that decides the compiled output: the user
  instruction bytes plus a parameter block (fragment: instruction,
  constant, texture and varying counts, descriptor mask, sparse flag,
  STR mask; vertex: counts, constant range, input lanes, two-side, clip,
  point size, sprite mask, flow depth/dynamic, the `tcl_re` bits that
  rewrite records, and the fragment varying layout and `primary_used`).
  Lookup is a per-file seeded xxh64 into a hash table that doubles with
  the entry count, then a full byte comparison of parameters and code.
  Entries are immutable and reference counted; a bounded LRU
  (`program_cache_kib`, default 512 per file, 0 disables) drops the
  oldest, and draws still holding one keep it alive.  Failed compiles
  are never cached, so a hit has already passed validation.
* **Constants are data, not program.**  `struct chrome9_fragment_program`
  lost its `constants` array; the compiler still validates them and the
  draw emits its own copy (`request->fragment_constants`).  A hit
  re-checks the constants for finiteness (same `-EINVAL`).
* **Vertex programs are sized by their records** (`records[][4]`,
  `CHROME9_VERTEX_PROGRAM_BYTES(n)`): compile scratch has the maximum,
  a cache entry only `record_count`.  Bool constants go into a per-draw
  `tcl->a61`, so one cached program serves any `b#` state.
* **Per-ioctl arena**: all copies (draw array, instructions, constants,
  elements, buffers, textures, vertices, indices), the TCL draw, the
  request arrays and compile scratch come from one bump allocator; only
  the TCL draw (now 680 bytes) and 16-bit index packing are zeroed.
  The largest chunk up to 512 KiB is kept per file for the next ioctl.
* **Submit bookkeeping** `chrome9_fragment_submit` is sized by the draw
  count (per-draw object indices and element starts) and no longer
  zeroed: 16 KiB per job before, now 3.5 KiB plus 200 bytes per draw;
  only `object_high` needed an explicit start value.
* **State cache**: `chrome9_link_equal()` compares cached programs by
  pointer before falling back to the word comparison.
* `profile=N` now prints `profile programs: fragment hit/miss, vertex
  hit/miss` per interval.

Host measurement (`~/tmp/agent-kcpu/bench/run.sh`: the driver's
compilers, cache and arena code with host shims; 25 draws per job from
five lightmap/model/flat/tint/add program pairs, interleaved; per-draw
MVP and tint constants; 96 16-bit indices per draw; Ryzen 9950X3D):

| variant | ns/draw | us/job |
|---|---|---|
| before (73e7e08) | 490 | 12.2 |
| after, cache hits (5 fragment, 4 vertex programs) | 128 | 3.2 |
| after, 8 KiB cache (evictions) | 237 | 5.9 |
| after, cache disabled (arena only) | 410 | 10.4 |
| all misses: before / after | 513 / 523 | 12.8 / 13.1 |

The host profile of "before" matches the target profile (vertex compile,
fragment compile, memset, `chrome9_vertex_selected`, malloc/free);
"after" is dominated by xxh64 of 300 to 400 key bytes per draw (both
programs), the arena copies and the key comparison.
`./after N KIB verify` checks every cached program against a fresh
compile (512, 8, 4 and 0 KiB), a hit with a NaN constant returns
`-EINVAL`, and valgrind reports no leaks or invalid accesses.

Not changed:

* `clflush_cache_range` of vertex objects covers exactly the fetched
  range of implicit `PIPE_BUFFER` objects, which Mesa rewrites every
  frame; explicit vertex buffers would need a Mesa protocol change and a
  flushed-range state separate from the guard bookkeeping.
* Double source/selector evaluation in `chrome9_vertex_compile()` now
  runs only on a miss.
* `tools/re/chrome9_cmd22_cost_model.c` mirrors the old
  `chrome9_fragment_program` layout (with constants) for its own model;
  it does not link against the driver.
* `~/tmp/texself/run.sh` already failed on 73e7e08: it does not extract
  `chrome9_fragment_texture_3d_selftest()`; the copy in
  `~/tmp/agent-kcpu/selftests/` adds it.  kraster, vertex, texture and
  fragment host self-tests pass with the patch.

### 9.1 Silicon verification

1. Load with `profile=1000`: `profile programs` shows almost only hits
   for ioquake3 and glmark2 after warm-up; kernel share of the ioquake3
   thread and `memset_orig`/`chrome9_vertex_compile` in `perf` drop.
2. GL smokes `tools/re/chrome9_gl_*_smoke.c` and the glmark2 matrix
   (`tools/chrome9_glmark_matrix.sh`) with default parameters and again
   with `program_cache_kib=0`: printed colours, mismatch counts,
   `Validation` and `rej` columns identical to the 73e7e08 baseline, no
   dmesg faults.  `vsflow`, `twoside`, `ucp` and `points_lines_native`
   cover the vertex key fields (flow, BFC, clip mask, point size,
   sprites).
3. `program_cache_kib=8` (constant eviction) on the same smokes and a
   short ioquake3 demo: identical output, no warnings.
4. ioquake3 GL1 fps at 200x150 and 800x600 against 73e7e08.
5. Two concurrent GL clients (glxgears plus ioquake3) for one minute,
   then close both: no leak in `/proc/slabinfo` (kmalloc-2k/4k/64k) and
   no refcount warnings.
