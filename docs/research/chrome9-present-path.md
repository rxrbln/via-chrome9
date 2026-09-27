<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 present path: shadows, guards and a zero-copy plan

Status: research note, 2026-09-24.  No kernel or Mesa source was changed.
Constraint: stock X.Org 1.21 (`xorg-server-21.1.24`) and its modesetting
driver stay unmodified; `-debug dmabuf_capable` is not available.  Only the
Chrome9 Mesa backend (loader/DRI code only if strictly needed) and the
Chrome9 kernel driver may change.

Paths are abbreviated: `K/` = `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/chrome9/`,
`M/` = `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/chrome9/`,
`D/` = `.../mesa-26.2.2/src/gallium/frontends/dri/`, `X/` = the
xorg-server-21.1.24 tree.  Evidence labels: **S** static reading of the
cited code, **D** measured on the target (cited run), **E** estimate,
**U** unverified hardware assumption (a test is specified).

## 1. The present chain today

ioquake3 (DRI3/Present client) and Xorg/glamor both run Chrome9 Mesa.  At
800x600 one frame is 1,920,000 bytes (stride 3200).

| # | process | step | code | CPU work per frame |
|---:|---|---|---|---|
| 1 | client | draws go to a private guarded *render shadow* of the DRI3 back buffer | `chrome9_set_framebuffer_state` `M/chrome9_context.c:6356`, `chrome9_render_shadow` `:6313` | none |
| 2 | client | SwapBuffers: `chrome9_flush_resource` (`:7028`) -> `chrome9_flush_render_target` (`:148`) -> `chrome9_copy_surface_rect` (`:67`) copies the dirty rectangle shadow -> back | `GEM_CPU_ACCESS` PREPARE_READ on the shadow waits for GPU completion and invalidates the rows (`chrome9_bo_cpu_sync_locked`, `K/chrome9_engine.c:4640`) | 1.92 MB memcpy, 1.92 MB clflush, a blocking GPU wait |
| 3 | Xorg | glamor samples the imported pixmap; the import is unguarded, so the sampler view gets a guarded *sampler shadow* (`chrome9_create_sampler_view` `:6174`) that is refreshed by a full-surface CPU copy before **every** Gallium draw (`chrome9_refresh_sampler_shadows` `:6832`, called at `:6907`) | the shadow's announced write range is clflushed by the kernel at the next job (`K/chrome9_engine.c:6627..6650`) | 1.92 MB memcpy, 1.92 MB clflush |
| 4a | Xorg | glamor renders into the render shadow of the unguarded front BO, then `chrome9_flush_render_target` copies the dirty rectangle to the front | PREPARE_READ invalidates 600 rows of the 4096-byte-stride shadow | 1.92 MB memcpy, 2.46 MB clflush |
| 4b | Xorg | alternative when glamor's copy is recognised as a plain 2-triangle REPLACE/NEAREST blit (`chrome9_textured_batch_copy_rect` `M/chrome9_draw.c:1731`): a synchronous `COPY_RECT` from the sampler shadow straight into the front (`:1991..2012`) | ioctl blocks for the whole 2D copy | the step-3 copy remains |
| 5 | kernel | the front BO *is* the scanout (direct SF scanout, `chrome9_gem_fb_sf_base` `K/chrome9_gem.c:328`, default `staging_scanout=0`); DIRTYFB is only a `wmb()` (`chrome9_fb_dirty` `:143`) | | none |

Which of 4a or 4b ran in the profiled session cannot be decided statically;
the perf call graph distinguishes them (caller of `memmove`:
`chrome9_refresh_sampler_shadows` versus `chrome9_flush_render_target`).
With 4a the chain carries 5.76 MB of memcpy and about 6.3 MB of clflush per
frame (**S**), in two processes that wait for each other.

Hazard in 4b (**S**): `chrome9_context_render_target_direct_written`
(`M/chrome9_context.c:232`) marks the front's render shadow stale, so the
next non-copy glamor draw to the root copies the whole 1024x768 front
(3 MiB) back into the shadow (`chrome9_context_prepare_render_target`
`:239`).

## 2. Why the shadows exist

### 2.1 The kernel contract: guards inside the object

Every draw ioctl validates a *canonical guarded object*: pixel rows start at
byte `PAGE_SIZE`, the object is exactly `P + ALIGN(pitch*height, P) + P`
bytes, and the first and last page are guard pages the kernel fills with
`0x13579bdf` before the job and compares after retirement (**S**):

* size: `chrome9_fragment_destination_size_valid` `K/chrome9_engine.c:6188`,
  object checks `:6470..6476` (also `drm_gem_is_imported` rejection), the
  COPY_RECT size formula `:8598..8606`, the older path `:5670..5700`;
* GPU address: `sub->bases[0] + PAGE_SIZE` `:6692..6694`, `target +=
  PAGE_SIZE` `:5937..5944`;
* guards: `chrome9_prepare_map_guards`/`chrome9_verify_map_guards`
  `:4604..4627`, the verified-guard cache `:6604..6656`, post-job verify in
  `chrome9_fragment_job_finish_locked` `:6249..6308` (`-EIO` and a fence
  error on corruption).

The guards are containment, not a hardware requirement:
[dri3-native-guard-layout.md](dri3-native-guard-layout.md#what-the-guard-ptes-are)
records that they are ordinary valid PTEs so that a texture prefetch or a bad
edge calculation reaches mapped poison instead of an uncharacterised GTI
translation fault, and that destination overruns are detected after
retirement.  Invalid-PTE guards are ruled out (no recovered fault/restart
path).  The requirement is therefore "valid, verifiable pages adjacent to the
surface in the SF aperture", not "guards inside the GEM object".

### 2.2 Why DRI3 and the X front cannot carry an in-object leading guard

Mesa already has a guarded shared layout: a LINEAR-modifier allocation gets
`CHROME9_RESOURCE_FLAG_CANONICAL_SHARED` (`M/chrome9_resource.c:910..926`)
and exports `offset = PAGE` (`:521..523`).  Stock X never uses it (**S**):

* DRI3 client backs: `dri3_get_supported_modifiers` returns no modifiers,
  because glamor reports no formats without `dmabuf_capable`
  (`X/glamor/glamor_egl.c:674`, `X/dri3/dri3_screen.c:179..183, 246..247`;
  the status is ignored at `X/dri3/dri3_request.c:369`).  The loader then
  calls `dri_create_image_with_modifiers(..., NULL, 0)`
  (`D/loader_dri3_helper.c:1528`), i.e. plain `resource_create`.  Even when
  the client sends `PixmapFromBuffers` with an offset
  (`D/loader_dri3_helper.c:1653..1665`, taken because Chrome9 reports
  `DRM_FORMAT_MOD_LINEAR`), `glamor_pixmap_from_fds` without
  `dmabuf_capable` imports through `glamor_back_pixmap_from_fd` with the
  stride only (`X/glamor/glamor_egl.c:599..625`, `gbm_bo_import(...,
  GBM_BO_IMPORT_FD)` at `:570`).  **The offset is discarded; pixels must be
  at byte 0.**  An offset-`PAGE` back buffer would be displayed shifted by
  one page.
* X front: `drmmode_create_bo` asks for modifiers only if the primary plane
  exposes `IN_FORMATS` (`X/hw/xfree86/drivers/modesetting/drmmode_display.c:1108..1127`).
  The primary plane is only enumerated with universal planes, which
  modesetting never requests, and the kernel refuses `DRM_CLIENT_CAP_ATOMIC`
  for any process whose name starts with `X`
  (`drivers/gpu/drm/drm_ioctl.c:342..346`).  The front therefore comes from
  `gbm_bo_create(..., RENDERING|SCANOUT)` -> offset 0, unguarded,
  `DMA32_SCANOUT` (`M/chrome9_resource.c:115..133, 357..364`).

The shared, offset-0 layout (`M/chrome9_resource.c:123..133`) is thus forced
by the protocol, and the kernel rejects it for rendering, which is the whole
reason for the render shadow, the sampler shadow and the publish copy.  Pitch
is not a problem: `ALIGN(width*4, 32)` satisfies the 3D (32 B), 2D (16 B) and
IGA1 (`chrome9_state_valid`, `K/chrome9_kms.c:585..590`) rules.  Size padding
at allocation time can add a trailing guard, but no allocation-time choice in
Mesa can add the leading one.

### 2.3 Other facts that matter for the design

* Same-device PRIME import yields the original GEM object and reservation
  (`drm_gem_is_imported()` is false), so Xorg's handle to the client's back
  buffer shares `bo->explicit_access`, `cpu_dirty` and the dma_resv fences
  ([dri3-native-guard-layout.md](dri3-native-guard-layout.md#same-device-prime-identity)).
  Implicit synchronisation between client and Xorg already works for GPU
  writes.
* Draw mappings are cached per object (`chrome9_draw_mapping_acquire_locked`
  `K/chrome9_engine.c:4124`), so a mapping-level change costs nothing per
  draw.  A pinned KMS framebuffer short-circuits to its scanout mapping
  (`:4145..4158`), which uses **snooped** PTEs (`chrome9_sf_pin`,
  `chrome9_collect_sgt_ptes(..., true)` `:4359`); draw mappings use
  non-snooped PTEs by default (`gart_nosnoop`, `:137`).  The measured fill
  rate difference is 141 versus 343 Mpixel/s (**D**,
  [chrome9-performance-20260924.md](chrome9-performance-20260924.md)).
* Neither the sampler nor the render shadow can be avoided by Mesa-side
  bookkeeping alone in Xorg: the DRI3 pixmap is written by another process,
  so Xorg cannot know whether its sampler shadow is current.

## 3. Why the GPU publish (`CHROME9_GPU_PUBLISH=1`) was slower

`chrome9_copy_surface_gpu` (`M/chrome9_context.c:111..145`) replaced a
dirty-rectangle memcpy with a kernel 2D copy.  Static causes (**S**), ranked:

1. **Fully synchronous ioctl.**  `chrome9_copy_rect_ioctl` (`:8532`) runs
   `chrome9_submit_depth_triangle` (`:5592`), which waits on both
   reservations (`dma_resv_wait_timeout`, `:5774..5802`), then takes the
   submit lock, which first retires the in-flight job
   (`chrome9_submit_lock_interruptible` `:597` ->
   `chrome9_engine_retire_locked` `:6312`), appends, and waits for its own
   completion inline (`chrome9_wait_job` at `:6025`) before verifying guards
   and returning.  Xorg's main loop is blocked for render-completion plus copy
   time; nothing overlaps.  Only DRAW_FRAGMENT_PROGRAM has the asynchronous
   last segment (`:6670..6745`).
2. **Whole surface instead of the dirty rectangle.**  `copy.width/height`
   are `source->width0/height0` (`M/chrome9_context.c:133..141`), i.e. the
   full 1024x768 root (3 MiB read + 3 MiB write) instead of the 800x600
   dirty rectangle the CPU path copies (`ctx->render_dirty`, `:161`).
3. **Snooped destination.**  The front is a pinned framebuffer, so the copy
   destination resolves to the snooped scanout mapping (`:4145..4158`).  The
   2.7x snoop penalty was measured for 3D fill; for 2D writes it is **U**.
4. **Per-copy guard and cache work.**  The source guards are rewritten and
   flushed on every call (`:5878..5881`, `chrome9_copy_rect_cache_flush`
   `:4734..4786`) and verified after retirement (`:6061..6068`); the
   verified-guard cache of the fragment path is not used.  If the front is
   `cpu_dirty` (after any Xorg CPU access, e.g. a software fallback), every
   destination row is clflushed.
5. **Polling waits.**  `chrome9_wait_job` (`:4912`) polls FE33, cookie, PAUSE
   and engine idle with `read_poll_timeout(..., 10, ...)`, i.e. about 10 us
   sleeps; the retire worker (`chrome9_retire_work` `:6350`) holds
   `submit_lock` while it polls, so the next submitter from any process waits
   on the mutex (**S**).

A memcpy of 1.92 MB is roughly 1.5 ms of an otherwise idle CPU (**E**,
section 6), whereas the GPU publish serialised a blocking 3 MiB snooped copy
behind the composite job.  In a serial chain that is a net loss.

## 4. Design options

### A. External (mapping-level) guards for native offset-0 objects — recommended

Move the guards from the GEM object into the SF mapping.  For a flagged
object the cached draw mapping becomes

```text
SF node:  [guard A][object page 0 .. n-1][guard B]
           node      node+P                node+P+size
```

where guard A/B are two private kernel pages per mapping (valid PTEs,
non-snooped like the object).  The GPU sees a *virtual canonical object* of
`obj->size + 2P` bytes with pixels at `+P`, so all existing address formulas
(`bases[i] + PAGE_SIZE`, `texture_level_offsets[0] = PAGE_SIZE`) and all
draw UAPI structures stay unchanged.  Only these places change (**S**):

* object-size checks compare the request's canonical size with
  `obj->size + 2P` when the object is flagged (`:6470..6476`, `:5675..5700`,
  COPY_RECT `:8598..8606`);
* guard prepare/flush/verify and the verified-guard cache operate on the
  mapping's private pages instead of the object's first/last page
  (`:4604..4633`, `:6249..6308`, `:6604..6656`, `:5878..5898`, `:6061..6080`);
  the cache moves from `bo->guard_object_size` into the mapping;
* `chrome9_draw_mapping_acquire_locked` builds `page_count + 2` PTEs and
  `chrome9_sf_mapping_free` releases the guard pages;
* the scanout short-cut (`:4145`) is skipped for flagged objects so draws
  use a non-snooped wrapped draw mapping (see H2), or `chrome9_sf_pin` wraps
  the pin the same way (`chrome9_gem_fb_sf_base` then returns
  `node.start + P + offsets[0]`).

Flag and discovery:

* `DRM_CHROME9_GEM_CREATE_EXTERNAL_GUARDS` sets `bo->external_guards` at
  creation.  Mesa sets it for every `PIPE_BIND_SHARED` offset-0 surface
  (DRI3 backs, the gbm front, glamor-exported pixmaps).
* `DRM_CHROME9_GEM_INFO {handle -> size, flags}` lets an importer (Xorg
  importing the client's back) learn the flag.  A per-object creation flag
  is unambiguous; inferring the layout from sizes per request is not, because
  one BO can be submitted with different dimensions.
* The 64-bit capability word is full (bit 63 used, `chrome9_drm.h:247`);
  add a `CAPS2` GETPARAM index with `DRM_CHROME9_CAP2_EXTERNAL_GUARDS`.

Mesa changes:

* Split `chrome9_resource.guarded` into *hardware-direct* (kernel accepts the
  object) and the CPU `data_offset`.  Offset-0 shared surfaces become
  hardware-direct when CAP2 is present (`chrome9_resource_layout`
  `M/chrome9_resource.c:123..133`, `chrome9_resource_create` `:334`), and
  `chrome9_resource_from_handle` (`:396`) marks an import direct when
  GEM_INFO reports the flag, `offset == 0`, stride `== ALIGN(w*cpp, 32)` and
  size `>= ALIGN(stride*h, P)`.
* `chrome9_set_framebuffer_state` (`M/chrome9_context.c:6374`) and
  `chrome9_create_sampler_view` (`:6174`) create no shadow for direct
  resources; the draw-path gates use the new predicate
  (`M/chrome9_draw.c:890, 989, 1110, 1513`, `chrome9_tcl.c`).  Shadows remain
  the fallback for foreign or non-canonical imports.
* The existing glamor copy -> COPY_RECT fast path (`M/chrome9_draw.c:1981`)
  then takes the DRI3 pixmap itself as source.

Result: the client renders directly into its DRI3 back buffer, glamor
samples it directly and renders directly into the front, which is already
the scanout.  All three CPU copies and their clflushes disappear, and the
client no longer waits for GPU completion at SwapBuffers; its next frame
overlaps the GPU and Xorg (bounded by the loader's back-buffer count and
throttle).  Attribution is preserved because the guard pages are private per
mapping ([dri3-native-guard-layout.md](dri3-native-guard-layout.md#can-synthetic-mapped-guards-remove-sampler-shadows)
raised this for shared scratch pages).  Cost: 8 KiB and two PTEs per cached
mapping (at most `CHROME9_DRAW_CACHE_MAX_ENTRIES` = 64).

### B. Asynchronous, minimal COPY_RECT — for the remaining copy paths

Needed while shadows remain (foreign imports, fallback, step ordering) and
for the glamor copy -> 2D fast path:

1. Mesa: copy only `ctx->render_dirty` in `chrome9_copy_surface_gpu`
   (`M/chrome9_context.c:133..141`).
2. Kernel: give COPY_RECT the fragment path's deferred retirement
   (`engine->inflight` + `retire_work`) instead of the inline
   `chrome9_wait_job`; attach WRITE/READ fences and return.  Post-job guard
   verification moves into the retire step as for fragment jobs.
3. Kernel: reuse the verified-guard cache for the source; do not rewrite
   guards of an unchanged, explicit, verified object.
4. Kernel: use the non-snooped draw mapping for GPU writes to scanout
   objects (H2) or measure the snooped 2D rate first (H3).

This alone does not remove CPU copies: with shadows, the render shadow still
has to reach the front and the sampler shadow still has to be refreshed.  Its
value is taking the publish off Xorg's critical path.

### C. Engine-level serialisation (general, larger)

* Keep more than one job in flight: `engine->inflight` is a single pointer
  and every submit-lock holder retires it first (`:587..603`).  A queue with
  ring-space accounting lets CPU preparation of job N+1 overlap GPU job N.
* Skip the CPU `dma_resv_wait_timeout` (`:6546`) for fences of the engine's
  own `fence_context`; in-order ring execution plus an in-ring barrier
  (`006c` wait, texture force-miss for render-to-texture) replaces it (H4).
* Signal fences from the FE33 interrupt instead of 10 us polling while
  holding `submit_lock` (interrupt acknowledgement is not yet proven, see
  [sf-2d-copy-present.md](sf-2d-copy-present.md#completion-ownership-and-present)).

### D. Local frame buffer placement of the front (future)

SL colour writes are 2.7x faster than snooped SF and need no GART
([chrome9-local-fb-placement.md](chrome9-local-fb-placement.md)); IGA1
scanout from SL is proven.  CPU access through BAR2 is slow and uncached, and
glamor software fallbacks would suffer.  Only after A and the SL ladder.

### Rejected alternatives

* **Offset-`PAGE` DRI3 backs** (the existing CANONICAL_SHARED layout): X
  discards the offset without `dmabuf_capable` (section 2.2); the image would
  be shifted.
* **Mesa enabling `DRM_CLIENT_CAP_UNIVERSAL_PLANES` on Xorg's fd** from
  `chrome9_screen_create`: `glamor_egl_init` (`X/.../driver.c:1211`) creates
  the gbm screen before `drmmode_pre_init` (`:1289`), so modesetting would
  see `IN_FORMATS` and allocate a guarded front with modifiers and
  `AddFB2WithModifiers(offset = PAGE)`, which the kernel accepts
  (`chrome9_scanout_layout_validate` `K/chrome9_gem.c:182`).  It changes the
  X server's DRM file state behind its back, fixes only the front, and is
  unnecessary once A exists.
* **Invalid-PTE guards**: no fault recovery path is known.
* **Kernel guard inference from sizes**: ambiguous per request (see A).

## 5. Ranked implementation plan

| rank | step | where | removes | risk |
|---:|---|---|---|---|
| 1 | CAP2 word, `GEM_CREATE_EXTERNAL_GUARDS`, `GEM_INFO`, wrapped draw mapping with private guard pages, virtual-size checks, guard prepare/verify/cache on mapping pages; flagged objects bypass the snooped scanout short-cut | kernel | enables 2-4 | low: all PTEs valid, guards still verified |
| 2 | Mesa: hardware-direct predicate; flag shared offset-0 surfaces; GEM_INFO on import; no render shadow for direct DRI3 backs | Mesa | client memcpy + clflush + swap-time GPU wait | low |
| 3 | Mesa in Xorg: no render shadow for the direct front | Mesa | publish memcpy + shadow invalidate; the 3 MiB stale copy-in hazard | medium: snoop/coherence of GPU writes to a scanout BO (H2) |
| 4 | Mesa in Xorg: no sampler shadow for direct imports; COPY_RECT fast path takes the pixmap as source | Mesa | per-draw full-surface memcpy + clflush | low |
| 5 | COPY_RECT: dirty rectangle only, deferred retirement, guard cache, non-snooped destination | Mesa + kernel | Xorg blocking in the 2D path; residual fallbacks | low-medium |
| 6 | multi-job in-flight queue, own-context fence skip, IRQ fence signalling | kernel | CPU/GPU serialisation of all draws | medium-high (ring ordering, IRQ ack) |
| 7 | SL placement of the root/front | kernel + Mesa | snooped/SF fill cost | medium (SL ladder first) |

Steps 1+2 are useful on their own (client side); 3 and 4 depend on 1.  Keep
the shadow code as fallback, controlled by an environment switch
(`CHROME9_EXTERNAL_GUARDS=0`) for A/B comparison.

## 6. Expected savings

Per frame at 800x600, shadow path 4a (**S** byte counts):

| process | memcpy | clflush | other |
|---|---:|---:|---|
| client | 1.92 MB | 1.92 MB | waits for GPU completion before Present |
| Xorg | 3.84 MB | 4.38 MB | none |
| total removed by steps 1-4 | 5.76 MB | ~6.3 MB | client/GPU overlap |

Time estimate (**E**): if the reported Xorg shares (memmove ~5 %,
clflush ~6.5 %) are of all samples on two cores at about 32 fps, they
correspond to about 3.1 ms memcpy (0.8 ms/MB) and 4.1 ms clflush
(0.95 ms/MB) per frame.  Applying the same rates to the whole chain gives
about 4.6 ms memcpy + 6.0 ms clflush = about 10 ms of CPU per frame, most of
it on the serial path.  If the shares are of Xorg samples only, the absolute
figure is proportionally lower.  Direct rendering adds GPU time only where
the target is snooped: a glamor composite of 0.48 Mpixel is about 1.4 ms
non-snooped and about 3.4 ms through the snooped scanout mapping (**E** from
the 3D fill measurements), against the shadow composite (about 1.4 ms) plus
the CPU publish it replaces.

Expected effect on ioquake3 q3dm1 800x600 (**E**, **U**): the per-draw job
overhead (about 200 us x ~104 draws) remains the dominant cost, so the
present-path fix is estimated at +15..30 % (GL1 32 -> 37..42 fps), with the
system idle share dropping because the client and Xorg stop waiting for each
other's copies.  glxgears (small window, many frames) should gain more in
relative terms.  These numbers are estimates and must be measured.

## 7. Unverified hardware assumptions and tests

All target runs follow the usual rules: short runs (about 7 s), GPU lock,
hash-gated module hand-off, NanoKVM power cycle as recovery.

* **H1** (low risk) GPU accesses to a surface whose neighbouring PTEs point
  at private kernel pages behave like the in-object guard case (all PTEs
  valid).  *Test:* `chrome9_fill_bench` into an offset-0 flagged BO: full
  readback equals the fill, both private guard pages and the GART table hash
  are intact, and Mpixel/s equals the canonical-object result.  Repeat with a
  scissor touching the last row and column.
* **H2** Non-snooped GPU writes into the scanout BO (via a second,
  non-snooped draw mapping) become visible to the snooped IGA1 fetch without
  CPU cache maintenance, including when the CPU holds clean lines of the
  front from an earlier read.  *Test:* map and read the whole front
  (populate the cache), GPU-fill a new colour through the draw mapping,
  compare the displayed image (existing screenshot capture) against the
  expected colour for ten frames; repeat with the snooped path as reference.
  If it fails, keep the snooped mapping for scanout objects (correct by
  construction) or flush the written rectangle in the DIRTYFB handler.
* **H3** 2D BitBLT throughput SF->SF, snooped versus non-snooped
  destination.  *Test:* time COPY_RECT 800x600 with `profile=N` (GPU phase)
  for both mappings.
* **H4** In-ring ordering suffices for render -> sample and render -> 2D copy
  hazards without a CPU retire in between (step 6).  *Test:* render a
  pattern, sample it in the next job without the CPU `dma_resv` wait, compare
  against a waited reference with pixel hashes; include the `006c=00c00000`
  pre-sync and the texture force-miss.
* **H5** The memcpy/clflush rates in section 6.  *Test:* `perf record -a -g`
  during a 7 s q3dm1 run before and after each step; report `memmove`,
  `clflush_cache_range` and idle shares per process, the kernel
  `profile=N` flush split, and `cg_drawFPS` from the screenshot.

Correctness gates before enabling by default:

1. Host (no hardware): Mesa unit test of the new layout and import predicate
   (flag present/absent, offset 0/PAGE, stride and size mismatch fall back to
   shadows); GEM handle lifetime tests from
   [dri3-native-guard-layout.md](dri3-native-guard-layout.md#hardware-free-lifetime-tests).
2. Target: glxgears windowed, x11perf `-copywinwin500` and a glamor text
   test; screenshot hashes equal to the shadow path
   (`CHROME9_EXTERNAL_GUARDS=0`).
3. Target: ioquake3 GL1/GL2 q3dm1 menu and map, window moved and resized
   (back-buffer reallocation), fps and perf shares recorded in
   `artifacts/<date>/`.
4. Deliberate guard check: a kernel debug knob that shifts the destination
   base by one page must make the job fail with `-EIO` and report the
   private guard, proving that verification covers the new pages.
