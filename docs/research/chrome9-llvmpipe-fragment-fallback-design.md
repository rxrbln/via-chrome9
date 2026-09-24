# Chrome9 llvmpipe fragment fallback: code-level design

Status: design only.  No target access, no driver source changes.  All
statements are from the local Mesa 26.2.2 tree, the local chrome9 kernel
tree and the T2 sysroot.  Estimates are marked as such.

**Implementation status (2026-09-24 evening, `gallium/chrome9` `c49c7b6`,
Mesa `41073a9`).**  Built in `objs-chrome9-llvm` with
`cross-t2-chrome9-llvm.ini` (adds `--sysroot` link arguments; only the
host-side chrome9 test executables still fail to link against the sysroot
libLLVM, the driver library links).  Differences from this design:

* Resource aliases are per draw sequence: created on demand, released with
  all llvmpipe bindings in `chrome9_sw_finish()`, so no alias outlives its
  chrome9 BO and no `content_seq` tracking is needed.  Mipmapped textures are
  copied on every fallback draw.
* CSO/shader twins live in a pointer hash table in the llvmpipe context and
  are dropped from each chrome9 delete hook (`chrome9_sw_forget`).
* R8 materialized: gallivm's `no_integers` path crashed (`cast_type`) on the
  chrome9-finalized FS, which keeps native 1-bit booleans.  Shader clones
  are compiled under llvmpipe's own `nir_options`; the chrome9 lowering only
  float-encodes integer uniforms, which stays correct.
* Selection: unsupported FS (as designed) **and** vertex shaders that sample
  textures (the Gallium draw path interprets them per vertex; 25k-triangle
  terrain mesh took about 20 s per frame).

Result: glmark2 terrain renders at about 0.56 s per frame (noise pass
90 ms warm, 0.78 s with the first LLVM compile), matching llvmpipe
(`artifacts/2026-09-24/tcl/glmark2-terrain-swfb-vs-llvmpipe.png`).

Path abbreviations used in citations:

| Prefix | Path |
|---|---|
| `M/` | `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/` |
| `C/` | `M/src/gallium/drivers/chrome9/` |
| `LP/` | `M/src/gallium/drivers/llvmpipe/` |
| `GV/` | `M/src/gallium/auxiliary/gallivm/` |
| `K/` | `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/chrome9/` |
| `S/` | `/srv/t2/build/x86-64-desktop-26.9.15-desktop-x86-64-nocona-linux/` |

Background: `docs/research/chrome9-fragment-capacity-strategies.md` (the
terrain noise program needs at least 248, realistically about 360 native
ALU records against a store of 96 ALU + 32 TEX records).

## 0. Summary of decisions

1. **Build**: `-Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true
   -Dgallium-drivers=chrome9,llvmpipe` with `cross-t2-llvm.ini` (sysroot
   `llvm-config`, LLVM 23.1.1).  The m9x build in the same tree already
   proves this cross configuration.  One ordering change in
   `M/src/gallium/meson.build` and a few lines in `C/meson.build`.
2. **Ownership**: one llvmpipe `pipe_screen` per chrome9 screen, created
   lazily through a small chrome9-owned `sw_winsys`; one llvmpipe
   `pipe_context` per chrome9 context, also created lazily.
3. **Resource aliasing**: level-0 surfaces are wrapped **zero-copy** as
   llvmpipe display targets through `resource_from_handle` +
   `displaytarget_from_handle`, which accepts the chrome9 stride as is.
   `resource_from_user_memory` is **not** used for images, because it
   ignores the caller's stride.  Mipmapped textures sampled with mip
   filtering, and render/depth targets whose height is not a multiple of 4
   (unless page padding covers the 4-row overhang), use an llvmpipe-owned
   staging copy.
4. **State**: all bound chrome9 state is mirrored into llvmpipe per
   fallback draw.  llvmpipe CSOs are cached inside the chrome9 CSO objects,
   and a bind is skipped when the llvmpipe CSO is unchanged.  Viewport,
   clip, blend color, sample mask and stipple state must newly be recorded
   by chrome9, because today they are dropped or only forwarded to `draw`.
5. **Synchronization**: GPU to CPU uses the existing
   `chrome9_draw_flush_batch()`.  Every chrome9 ioctl is synchronous, and
   the kernel flushes CPU caches after completion.  CPU to GPU uses a lazy
   `chrome9_sw_finish()` (llvmpipe flush + `fence_finish`), called from
   `chrome9_draw_flush_batch()` and before every hardware draw.  The kernel
   flushes CPU caches for each object before GPU use.
6. **Selection**: fall back when `chrome9_select_fs_variant()` fails, and
   when a guarded-domain batch is rejected.  Optionally also on per-draw
   unsupported hardware state.  `CHROME9_SW_FALLBACK=off|auto|state|all`
   overrides this for testing.
7. **Performance (estimate)**: terrain noise pass 256×256 takes about 25–35
   ms of single-thread CPU time, or 13–20 ms with two rasterizer threads.
   It runs once per frame.  The first use has a one-time LLVM compile of
   0.2–2 s, which llvmpipe's disk cache removes on later runs.

## 1. Owning llvmpipe inside chrome9

### 1.1 Build system

Current chrome9 build (`M/objs-chrome9-full/meson-logs/meson-log.txt:3`):
`-Dllvm=disabled -Ddraw-use-llvm=false -Dshared-llvm=disabled
-Dgallium-drivers=chrome9 --cross-file=cross-t2-chrome9.ini`.

Existing precedent in the same tree: `M/objs-m9x-llvm` was configured with
`llvm=enabled, shared-llvm=enabled, draw-use-llvm=true` and found
`S/usr/bin/llvm-config` version 23.1.1 (`meson-log.txt:3820-3843`).  The
resulting `libgallium-26.2.2.so` has `NEEDED libLLVM.so.23.1`.  The sysroot
has `S/usr/lib64/libLLVM.so.23.1` (shared mode, `--has-rtti YES`, X86 target
built) and also the static component archives.

Required changes:

| Item | Change | Why |
|---|---|---|
| cross file | use `M/cross-t2-llvm.ini`, not `cross-t2-chrome9.ini` | the latter sets `llvm-config = 'llvm-config'`, which would pick the host LLVM; the former points at `S/usr/bin/llvm-config` |
| options | `-Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true -Dgallium-drivers=chrome9,llvmpipe` | gallivm is compiled into libgallium only `if draw_with_llvm` (`M/src/gallium/auxiliary/meson.build:294`); `M/meson.build:246,251-255` derive and enforce it; `M/meson.build:2026` rejects llvmpipe without LLVM draw |
| `M/src/gallium/meson.build` | move the `if with_gallium_chrome9 … subdir('drivers/chrome9')` block (`:40-44`) below the llvmpipe/swrast block (`:71-86`) | `libllvmpipe` must be defined before chrome9's `meson.build` references it |
| `C/meson.build` | add `chrome9_swfb.c`; when `with_gallium_llvmpipe`: `c_args += '-DCHROME9_HAVE_LLVMPIPE'`, include `inc_gallium_drivers` (for `llvmpipe/lp_public.h`), `driver_chrome9 = declare_dependency(link_with : [libchrome9, libllvmpipe], dependencies : dep_llvm, …)`; the smoke/test executables that link `libchrome9` need the same `libllvmpipe` + `dep_llvm` | explicit link instead of relying on archive grouping |

Consequences of listing `llvmpipe` in `gallium-drivers`:

* `-DHAVE_LLVMPIPE` and `GALLIUM_LLVMPIPE` are set, and `driver_swrast`
  (`M/src/gallium/meson.build:80-81`) is linked into the DRI megadriver
  (`M/src/gallium/targets/dri/meson.build:59-60`).  `swrast`/`kms_swrast`
  become available.  This is useful here: `LIBGL_ALWAYS_SOFTWARE=1` on the
  target produces pure llvmpipe reference images for comparison with the
  fallback.
* The chrome9 hardware path is unchanged.  Chrome9 creates its draw module
  with `draw_create_no_llvm()` (`C/chrome9_context.c:6689`), and its vertex
  shader caps are the interpreter caps (`C/chrome9_screen.c:314-315`).  The
  comment there already anticipates LLVM-enabled builds.  Enabling
  `draw-use-llvm` therefore does not change the vertex stage or the
  `tgsi_sampler` vertex texture callback.
* An alternative that avoids registering swrast is a private meson option
  (for example `chrome9-sw-fallback`) that sets `with_any_llvmpipe` without
  adding `llvmpipe` to `gallium_drivers`.  It needs more meson edits and
  loses the reference driver, so it is not recommended.

`-Dshared-llvm` trade-off:

* `enabled` (recommended, matches m9x): libgallium stays about the current
  size.  Every GL process maps `libLLVM.so.23.1` (165 MB file, shared pages)
  and pays its dynamic relocation at load time.  An LLVM soname bump in T2
  requires a Mesa rebuild.
* `disabled`: links the needed LLVM components statically (archives present
  in `S/usr/lib64`).  libgallium grows by several tens of MB (estimate) and
  has no runtime LLVM dependency.

RTTI: LLVM reports `has-rtti YES`, and Mesa's default `cpp_rtti=true`
matches (`M/meson.build` RTTI check after `:2029`).

On x86-64, MCJIT is used (`llvm_has_mcjit`).  Target CPU features come from
`util_get_cpu_caps()` at run time (`GV/lp_bld_misc.cpp:350-357`), not from
the `nocona` build target.  If the Nano reports SSE4.1, `roundps` is used
for floor/fract automatically.

### 1.2 Private sw_winsys

`null_sw_create()` cannot be used, because its `displaytarget_from_handle`
returns NULL (`M/src/gallium/winsys/sw/null/null_sw_winsys.c:109`).  Chrome9
provides a minimal winsys in `chrome9_swfb.c`:

```c
struct chrome9_sw_dt {
   uint8_t *map;
   unsigned stride;
};

static struct sw_displaytarget *
chrome9_sw_dt_from_handle(struct sw_winsys *ws,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle, unsigned *stride)
{
   struct chrome9_resource *res = whandle->com_obj;
   struct chrome9_sw_dt *dt = CALLOC_STRUCT(chrome9_sw_dt);

   if (!dt)
      return NULL;
   dt->map = (uint8_t *)res->bo.map + res->level_offset[0];
   dt->stride = res->level_stride[0];
   *stride = dt->stride;
   return (struct sw_displaytarget *)dt;
}
```

Other callbacks: `displaytarget_map` returns `dt->map`, `unmap` does
nothing, `destroy` frees the wrapper (never the BO),
`is_displaytarget_format_supported` returns false, `displaytarget_create`
returns NULL, `get_fd` is NULL.  **`displaytarget_create_mapped` must be
NULL**: otherwise `llvmpipe_resource_from_handle` tries to interpret
`whandle->handle` as a dma-buf fd (`LP/lp_texture.c:781-797`).  With it
NULL, the path goes straight to `displaytarget_from_handle`
(`LP/lp_texture.c:799-803`).

### 1.3 Lifetime

```c
/* chrome9_screen.c: struct chrome9_screen */
simple_mtx_t sw_lock;
struct pipe_screen *sw_screen;     /* llvmpipe, lazily created */
struct sw_winsys sw_winsys;

/* chrome9_context.h: struct chrome9_context */
struct chrome9_sw_context *sw;     /* NULL until first fallback */
```

* `chrome9_sw_screen_get()` creates the llvmpipe screen under `sw_lock` on
  first use with `llvmpipe_create_screen(&cscreen->sw_winsys)`.  It is
  destroyed in `chrome9_screen_destroy()` (`C/chrome9_screen.c:228`) after
  all contexts.  llvmpipe owns the rasterizer threads per screen
  (`LP/lp_screen.c:983`), and their count is `nr_cpus` (two on the Nano X2),
  adjustable with `LP_NUM_THREADS` (`LP/lp_screen.c:1068-1072`).
* `chrome9_sw_context_get(ctx)` creates
  `sw_screen->context_create(sw_screen, NULL, 0)` on the first fallback
  draw.  `chrome9_context_destroy()` (`C/chrome9_context.c:6502`) calls
  `chrome9_sw_finish()`, unbinds all llvmpipe state, deletes the cached
  llvmpipe CSOs/shaders, drops mirror references and destroys the llvmpipe
  context.  This happens before `draw_destroy()` and before the resource
  references are released.
* Lazy creation keeps the cost of LLVM initialisation (`lp_build_init`,
  disk-cache setup `LP/lp_screen.c:892-909`) away from applications that
  never fall back.

### 1.4 Softpipe comparison

`softpipe_create_screen(struct sw_winsys *)` has the same interface and also
supports display-target imports.  The mirror code below is written against
the generic `pipe_context` API only, so a `-Dllvm=disabled` build could use
softpipe with an `#ifdef` around screen creation.  Softpipe runs fragment
shaders through the TGSI interpreter.  The capacity-strategies document
estimates 0.5–1.5 s per terrain frame, which misses the target, so softpipe
is a correctness-only option.

## 2. Aliasing chrome9 BOs as llvmpipe resources

### 2.1 Chrome9 layout facts

* All BOs are cached shmem with a persistent CPU mapping `bo.map`
  (`C/chrome9_drm.h:13-21`, capability check `C/chrome9_screen.c` requires
  `DRM_CHROME9_CAP_GEM_SHMEM_CACHED`).  CPU reads are therefore fast (not
  write-combined).
* Surface row stride is `ALIGN(width * cpp, 32)` per level
  (`C/chrome9_resource.c:108`).
* Private (guarded) surfaces: level 0 at `page_size`, page-rounded, followed
  by a guard page.  Mip levels start after the internal guard at 256-byte
  alignment, and a final guard page ends the BO (`C/chrome9_resource.c:132-160`).
* Shared (imported/exported) surfaces: level 0 at offset 0, unguarded,
  level-0 only (`C/chrome9_resource.c:111-130`).  They are rendered through
  the guarded shadow `ctx->render_target` (`C/chrome9_context.c:5980-5995`)
  and sampled through `chrome9_sampler_view::shadow`
  (`C/chrome9_context.c:5827-5840`).
* The kernel fixes the object size as `ALIGN(pitch*height, PAGE) + 2 pages`
  (`K/chrome9_engine.c:5634-5638`), and fixes mip offsets likewise
  (`K/chrome9_engine.c:6347-6380`).  Padding rows **cannot** be added to
  chrome9 surfaces without a UAPI change.

### 2.2 llvmpipe constraints

`resource_from_user_memory` (`LP/lp_texture.c:893-925`) takes no stride and
no pointer checks.  It runs `llvmpipe_texture_layout()`, which imposes
llvmpipe's own layout:

* width and height aligned to `LP_RASTER_BLOCK_SIZE` (4)
  (`LP/lp_texture.c:167-170`);
* `row_stride = align(nblocksx * cpp, cacheline)`, with cacheline 64 on the
  Nano (`LP/lp_texture.c:190-191`);
* mip levels packed consecutively with 64-byte alignment
  (`LP/lp_texture.c:113`, loop to `:220`).

Chrome9's stride matches only when `align(w,4)*cpp` is a multiple of 64 and
equals `ALIGN(w*cpp,32)`, for example 32-bpp widths that are multiples of
16.  Mip offsets never match, because of the guard page between L0 and L1.
`resource_from_user_memory` is therefore unsuitable for images in general.

`resource_from_handle` with a display target (`LP/lp_texture.c:724-812`)
takes the stride from the winsys (`row_stride[0]`).  Its limits are
`last_level == 0` and single sample (`LP/lp_texture.c:734-736`).  llvmpipe
maps display targets for rendering (`LP/lp_texture.c:653-676`) and for
sampling (`LP/lp_jit.c:485-486`).

Render-block overhang: llvmpipe shades every 4×4 block that intersects the
clipped tile (`LP/lp_rast.c:118-121`, `:331-332`).  The fragment function
loads and stores whole unswizzled blocks (`LP/lp_state_fs.c:3146-3149`).
Uncovered pixels are written back with their loaded value.  A color or depth
target is therefore **read and rewritten up to `align(h,4)` rows and
`align(w,4)` columns**.

* Columns: `ALIGN(w*cpp,32) >= align(w,4)*cpp` for cpp 1, 2 and 4, so the
  overhang stays inside each row.
* Rows: bytes `[level_size, stride*align(h,4))` past level 0.  For a guarded
  L0 this lands in the page padding, then in the kernel-owned guard page,
  and for wide targets beyond it (mip level 1, or past the BO end).  An
  X-channel format could also be normalized on write-back.

### 2.3 Alias policy

`struct chrome9_resource` (`C/chrome9_resource.h:15-26`) gains:

```c
struct pipe_resource *sw_alias;   /* llvmpipe resource, owned */
uint64_t content_seq;             /* bumped by every chrome9-side write */
uint64_t sw_alias_seq;            /* content_seq copied into a staging alias */
bool sw_alias_staged;             /* alias is a copy, not a view of bo.map */
```

`chrome9_sw_alias(ctx, res, use)`:

| Resource / use | Alias |
|---|---|
| `PIPE_BUFFER` vertex/index | none: passed as user pointers `bo.map + offset`; llvmpipe consumes them synchronously inside `draw_vbo` (`LP/lp_draw_arrays.c:83-104`) |
| `PIPE_BUFFER` constants (VS/FS) | none: copied per draw with `u_upload_data()` into the llvmpipe `const_uploader` (`LP/lp_context.c:361-365`).  llvmpipe asserts non-user constant buffers (`LP/lp_state_fs.c:4311`) and references, not copies, them until rasterization (`LP/lp_setup.c:1116-1125`) |
| 2D, sampled, `last_level == 0` or view/sampler restricted to level 0 (`min_mip_filter == NONE`) | display-target alias, zero copy |
| 2D, render/depth target, `h % 4 == 0` or `stride*align(h,4) <= ALIGN(level_size[0], page)` | display-target alias, zero copy |
| 2D, render/depth target otherwise | llvmpipe staging resource of the same format; copy in at the first fallback draw of a pending sequence, copy out in `chrome9_sw_finish()` |
| 2D mipmapped, sampled with mip filtering | llvmpipe `resource_create` (own layout, `last_level` preserved); per-level `util_copy_rect` when `content_seq != sw_alias_seq` |
| unguarded shared surface, sampled | alias the shared BO directly (`level_offset[0] == 0`), not the chrome9 shadow; this saves the per-draw shadow copy (`C/chrome9_context.c:6293-6309`) |

Pointer and stride alignment: the display-target base is `bo.map +
level_offset[0]`.  It is page aligned for guarded resources and offset 0 for
shared ones, so it is at least 16-byte aligned, as the SSE block loads need.
Strides are multiples of 32.  If the stride is not a multiple of 64, tile
columns may share cache lines between rasterizer threads.  That costs
performance only, because block stores never overlap bytes.

Formats: every chrome9 format is passed unchanged, and llvmpipe supports all
of them: `B8G8R8X8_UNORM`, `B8G8R8A8_UNORM`, `B5G6R5_UNORM`, `A8_UNORM`,
`L8_UNORM`, `L8A8_UNORM` (sampling), `Z16_UNORM`, `X8Z24_UNORM`,
`S8_UINT_Z24_UNORM` (depth/stencil and sampling).  At alias creation,
`sw_screen->is_format_supported()` is checked with the intended bind flag.
On failure the draw falls back to today's rejection.

Terrain case: the height map is 256×256 at 4 bytes per pixel, stride 1024,
`h % 4 == 0`, so it is a zero-copy alias.  Common window sizes
(800×600, 1024×768, 1366×768) also have `h % 4 == 0`.

`content_seq` is bumped in `chrome9_transfer_map()` with `PIPE_MAP_WRITE`
(`C/chrome9_resource.c:519`), in `chrome9_context_render_target_written()`
and `_direct_written()` (`C/chrome9_context.c:81,108`, which also covers the
external resource that the shadow is copied to), in the clear paths
(`C/chrome9_context.c:6194-6290`), in `chrome9_generate_mipmap()`
(`C/chrome9_resource.c:755`), and in the fallback draw itself.

Destruction: `chrome9_resource_destroy()` (`C/chrome9_resource.c:484`)
calls `pipe_resource_reference(&res->sw_alias, NULL)` before destroying the
BO.  This is safe because the fallback mirror holds a **chrome9** reference
on every resource it has bound into llvmpipe (section 3.3).  A chrome9
resource that is still bound in llvmpipe therefore cannot reach its destroy
function.

### 2.4 Guarded shadow render target

The fallback renders into the same object the hardware path uses:

* color: `ctx->render_target`, the guarded resource itself or its guarded
  shadow.  Before the draw, call `chrome9_context_prepare_render_target(ctx)`
  (`C/chrome9_context.c:115-127`) to copy external to shadow when stale.
  After the draw, call `chrome9_context_render_target_written(ctx)` (`:81`)
  so that `chrome9_flush_render_target()` (`:54-65`) publishes the shadow
  to the external image at flush or present.
* depth/stencil: `ctx->framebuffer.zsbuf.texture` (always a guarded private
  resource).
* depth-only framebuffers: the llvmpipe framebuffer gets `nr_cbufs = 0`.
  The hardware-only `depth_only_scratch` (`C/chrome9_context.c:6000-6030`)
  is not used.

## 3. State mirroring

### 3.1 State chrome9 must start recording

Today these entry points only forward to `draw` or drop the state.  They
must store a copy in `struct chrome9_context`:

| Entry point | Today | Add |
|---|---|---|
| `chrome9_set_viewport_states` (`C/chrome9_context.c:6055`) | forwards to draw | `struct pipe_viewport_state viewport[PIPE_MAX_VIEWPORTS]` |
| `chrome9_set_clip_state` (`:6077`) | forwards to draw | `struct pipe_clip_state clip` |
| `chrome9_set_blend_color` (`:6472`) | dropped | `struct pipe_blend_color blend_color` |
| `chrome9_set_sample_mask` (`:6487`) | dropped | `unsigned sample_mask` |
| `chrome9_set_polygon_stipple` (`:6494`) | dropped | `struct pipe_poly_stipple stipple` |
| `chrome9_create_vs_state` (`:374-389`) | NIR handed to draw only | `nir_shader *sw_nir = nir_shader_clone()` when the fallback is enabled |
| scissor (`:6064-6074`) | stored (index 0) | unchanged |
| stencil ref (`:6480`) | stored | unchanged |

A `uint32_t sw_dirty` bit mask is set by each setter, so the mirror
replays only what changed.

### 3.2 CSO caching

Chrome9 CSOs are raw copies (`C/chrome9_context.c:172-222`).  They become
wrapper structs with the Gallium state as the first member.  Existing code
that stores `struct pipe_blend_state *` etc. therefore stays valid:

```c
struct chrome9_blend_state { struct pipe_blend_state base; void *sw_cso; };
struct chrome9_dsa_state   { struct pipe_depth_stencil_alpha_state base; void *sw_cso; };
struct chrome9_rast_state  { struct pipe_rasterizer_state base; void *sw_cso; };
/* existing */ struct chrome9_sampler_state { ...; void *sw_cso; };
/* existing */ struct chrome9_vertex_elements { ...; void *sw_cso; };
/* existing */ struct chrome9_shader { ...; void *sw_cso; nir_shader *sw_nir; };
/* existing */ struct chrome9_sampler_view { ...; struct pipe_sampler_view *sw_view;
                                             struct pipe_resource *sw_view_alias; };
```

`sw_cso` is created on the first fallback use with the llvmpipe context's
`create_*_state(&base)`.  It is deleted from the matching chrome9 `delete_*`
hook if `ctx->sw` exists (`chrome9_delete_state` `:360`,
`chrome9_delete_sampler_state` `:323`, `chrome9_delete_vs_state` `:402`,
`chrome9_delete_fs_state` `:5705`, vertex elements `:5757`).  If it is
currently bound in llvmpipe, it is unbound first.  The mirror remembers the
last bound llvmpipe pointer per slot and skips identical binds, so
llvmpipe's dirty tracking does not re-derive state on every draw.

Shaders:

* FS: `sw_cso = lp->create_fs_state()` with `{.type = PIPE_SHADER_IR_NIR,
  .ir.nir = nir_shader_clone(NULL, shader->nir)}`.  The **canonical** NIR is
  used (`C/chrome9_context.c:5647`), not a chrome9 variant.  Inlinable
  uniforms are still read from the constant buffer, and front-face is
  dynamic in llvmpipe.  One llvmpipe compile per program therefore suffices,
  instead of one per chrome9 variant (`C/chrome9_context.c:5482-5575`).
  llvmpipe takes ownership of the clone.
* VS: `sw_cso = lp->create_vs_state()` from `sw_nir` (clone again, keep
  `sw_nir` for context re-creation).
* The NIR has already been through `chrome9_finalize_nir`
  (`C/chrome9_screen.c:290-300`): UBO index shifts rewritten as float
  multiplies, `nir_lower_int_to_float`, and `nir->options ==
  &chrome9_nir_options` with `no_integers` (`:268-283`).  gallivm handles
  this through its `no_integers` prepass branch
  (`GV/lp_bld_nir_soa.c:6219-6227`, `lp_nir_lower_ubo_vec4` at `:6217`,
  local commit `0ed5ce5 gallivm: implement legacy float set comparisons`).
  That branch has so far been exercised by the LLVM draw path, not by the
  llvmpipe **fragment** path; see risk R8.

### 3.3 Per-draw replay list

`chrome9_sw_mirror(ctx)` binds, in this order (skipping unchanged slots):

| llvmpipe call | Source in chrome9 | Notes |
|---|---|---|
| `set_framebuffer_state` | `ctx->framebuffer` (`C/chrome9_context.h:159`) with `cbufs[0].texture = alias(ctx->render_target)`, `zsbuf.texture = alias(zsbuf)` | only when a framebuffer serial changed or an alias was re-created |
| `bind_rasterizer_state` | `ctx->rasterizer` → `sw_cso` | keeps `half_pixel_center`, `scissor`, cull, `flatshade`, `clamp_vertex_color` |
| `bind_depth_stencil_alpha_state` | `ctx->dsa` → `sw_cso` | depth, stencil front/back, alpha test (chrome9 may advertise `alpha_test`, `C/chrome9_screen.c:488-492`) |
| `set_stencil_ref` | `ctx->stencil_ref` | |
| `bind_blend_state`, `set_blend_color` | `ctx->blend`, `ctx->blend_color` | |
| `set_sample_mask`, `set_min_samples(1)` | `ctx->sample_mask` | single sample only |
| `set_polygon_stipple` | `ctx->stipple` | |
| `set_viewport_states` | `ctx->viewport[]` | |
| `set_scissor_states` | `ctx->scissor` if `scissor_valid` | |
| `set_clip_state` | `ctx->clip` | |
| `bind_vs_state`, `bind_fs_state` | `ctx->vs->sw_cso`, `ctx->fs->sw_cso` | |
| `bind_vertex_elements_state` | `ctx->vertex_elements->sw_cso` | `src_stride` lives in the element in this Gallium |
| `set_vertex_buffers` | `ctx->vertex_buffers[]` rewritten to `is_user_buffer = true`, `buffer.user = bo.map` (resource) or the original user pointer | no alias; same mapping chrome9 uses (`C/chrome9_context.c:6390-6399`) |
| `set_constant_buffer` VS/FS, index 0..N | `ctx->vs_constants[]`, `ctx->fs_constants[]` | `u_upload_data(lp->const_uploader, …)`, `take_ownership = true` |
| `bind_sampler_states` FS/VS | `ctx->sampler_states[]` / `vs_sampler_states[]` → `sw_cso` | from `chrome9_sampler_state::base`; the chrome9 `supported` flag is ignored (llvmpipe supports all filters, wraps, LOD and compare) |
| `set_sampler_views` FS/VS | `ctx->sampler_views[]` / `vs_sampler_views[]` → `sw_view` | template copied from the chrome9 view (format, swizzle, level range) with `texture = alias` |
| index buffer | `info->has_user_indices = true`, `index.user = bo.map` | copy of `pipe_draw_info`; `take_index_buffer_ownership = false` |

The mirror owns a **chrome9** reference for every resource currently bound
into llvmpipe: the framebuffer targets, sampler view textures and
vertex/index buffers of the pending draw.  These references are updated
together with the llvmpipe binds.  This keeps every aliased BO mapped until
llvmpipe has finished with it.

Not mirrored, because chrome9 does not expose it: stream output, queries,
render condition, MSAA, images and SSBOs.

## 4. Synchronization

### 4.1 Kernel facts

* Every draw/copy ioctl is synchronous.  It waits on the destination's
  reservation (`K/chrome9_engine.c:4458`), flushes CPU caches for the whole
  object before publishing to the GPU (`chrome9_cache_flush`, `:408-418`,
  used at `:4509`; fragment programs `:5384-5391` for every object in the
  job), waits for retirement (`chrome9_wait_job`, `:4545`, `:5423`) and
  flushes again after completion (`:4550-4552`, `:5433-5436`) before
  returning.
* `chrome9_flush()` returns a NULL fence and `fence_finish` is a no-op
  (`C/chrome9_context.c:6444-6456`, `C/chrome9_screen.c:255-265`).  The
  contract is "flush means complete".

Consequences:

* **GPU to CPU**: after `chrome9_draw_flush_batch()`
  (`C/chrome9_draw.c:3387-3397`) returns, all retained cmd22 triangle
  batches have been submitted and retired, and the kernel has written back
  and invalidated CPU cache lines of every object involved.  No separate
  idle wait is needed.
* **CPU to GPU**: llvmpipe stores reach cached shmem pages.  The next
  submission's pre-publication `clflush` over each whole object makes them
  visible to the GPU.  The fallback must only guarantee that llvmpipe's
  rasterizer threads have **finished** before that submission starts.

### 4.2 Pending state and the finish hook

```c
void
chrome9_sw_finish(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct pipe_fence_handle *fence = NULL;

   if (!sw || !sw->pending)
      return;
   sw->lp->flush(sw->lp, &fence, 0);
   sw->screen->fence_finish(sw->screen, NULL, fence, OS_TIMEOUT_INFINITE);
   sw->screen->fence_reference(sw->screen, &fence, NULL);
   chrome9_sw_copy_out_staged(ctx);
   sw->pending = false;
}
```

Call sites:

1. At the start of `chrome9_draw_flush_batch()` (`C/chrome9_draw.c:3387`).
   Every existing CPU accessor and ordering point already goes through it:
   `chrome9_context_sync_resource()` for synchronized texture maps
   (`C/chrome9_context.c:90-106`, reached from `chrome9_transfer_map`
   `C/chrome9_resource.c:532-535`), `chrome9_clear` (`:6204`),
   `set_framebuffer_state` (`:5972`), the sampler shadow refresh (`:6305`),
   VS texture refresh (`:6323`), `chrome9_flush` and `flush_resource`
   (`:6449`, `:6467`), and guarded-domain begin (`C/chrome9_draw.c:3406`).
2. At the start of the hardware path in `chrome9_draw_vbo`, before
   `chrome9_refresh_sampler_shadows()` (`C/chrome9_context.c:6368`), so that
   no GPU submission starts while llvmpipe still writes or reads a BO.
3. `chrome9_context_destroy()` (section 1.3).

Buffers: `chrome9_transfer_map()` skips synchronization for `PIPE_BUFFER`
(`C/chrome9_resource.c:532-535`).  This remains correct, because the
fallback never lets llvmpipe read a chrome9 buffer asynchronously: vertex
and index data are consumed inside `lp->draw_vbo`, and constants are copied.

Finishing lazily lets consecutive fallback draws bin into one llvmpipe
scene.  Every switch between the hardware and fallback paths costs one
llvmpipe flush/finish (estimate 0.1–0.5 ms including thread wake-up) plus
the existing synchronous batch submission.  `CHROME9_SW_SYNC=1` forces a
finish after every fallback draw for debugging.

Cross-process sharing: CPU access by llvmpipe does not participate in
`dma_resv`.  This is the same policy as the existing CPU shadow copies of
imported images, and no change is proposed here.

## 5. Selection rule

### 5.1 Decision points in `chrome9_draw_vbo`

```c
enum chrome9_sw_reason {
   CHROME9_SW_NONE,
   CHROME9_SW_FS,        /* no native program after all lowering */
   CHROME9_SW_STATE,     /* draw-level hardware state unsupported */
   CHROME9_SW_DOMAIN,    /* guarded value-domain proof failed */
   CHROME9_SW_FORCED,    /* CHROME9_SW_FALLBACK=all or FS index override */
};
```

1. After the `num_draws`/indirect handling (`C/chrome9_context.c:6368-6376`)
   `if (!chrome9_select_fs_variant(ctx)) reason = CHROME9_SW_FS`.  The
   sampler shadow refresh, today the first statement of `chrome9_draw_vbo`
   (`C/chrome9_context.c:6366`), moves below this decision into the
   hardware path; the fallback samples the original images (section 2.3).
   This replaces today's rejection at
   `C/chrome9_context.c:6378-6383`.  The FS is unsupported when
   `chrome9_compile_fs_nir()` leaves `CHROME9_FS_UNSUPPORTED`
   (`C/chrome9_context.c:5362-5387`): non-straight-line control flow,
   operations without a native form, or programs that exceed 96 ALU / 32
   TEX / 12 temps.  The terrain noise program falls here.
2. Optional (`CHROME9_SW_FALLBACK=state`): a new per-draw predicate
   `chrome9_draw_hw_state_ok(ctx)` evaluates `chrome9_draw_state_supported()`
   (`C/chrome9_draw.c:807`) once per draw, with the texture count computed as
   in `chrome9_vbuf_get_vertex_info()` (`C/chrome9_draw.c:287`).  Today that
   function runs per triangle and drops each triangle
   (`C/chrome9_draw.c:2148-2153`).  Its reasons include blend factors,
   depth/stencil modes without kernel capability, sampler filters and wraps,
   textures bound as render target, and scissor.  Per-triangle geometric
   rejections (coordinate range, degenerate triangles, batch failure) stay
   as they are.
3. Guarded domain: `chrome9_draw_end_guarded_domain()`
   (`C/chrome9_draw.c:3417`) is changed to return `failed`.  On failure, the
   deferred batches were never submitted (`C/chrome9_draw.c:3445`).  The
   whole draw is replayed through the fallback, and the rejected-triangle
   counter is corrected by the deferred count.
4. Everything else takes the hardware path.

Draws with `indirect` stay unsupported on both paths (llvmpipe could handle
them, but chrome9 has no indirect buffer mapping contract).

### 5.2 Environment overrides

| Variable | Values | Effect |
|---|---|---|
| `CHROME9_SW_FALLBACK` | `off` / `0` | old behaviour: reject |
| | `auto` / `1` (default when built with `CHROME9_HAVE_LLVMPIPE`) | reasons FS and DOMAIN |
| | `state` | additionally STATE |
| | `all` | every draw through llvmpipe on the chrome9 framebuffer (end-to-end reference with chrome9 resources, clears and present) |
| `CHROME9_SW_FALLBACK_FS` | `lo[-hi]` | force FS creation indices in range, same numbering as `CHROME9_DEBUG_FS_MARK` (`C/chrome9_context.c:5579-5600`) |
| `CHROME9_SW_DEBUG` | `1` | one line per fallback draw: reason, FS index, vertex count, alias kinds (zero-copy/staged), elapsed time of mirror + draw, and time spent in finish |
| `CHROME9_SW_SYNC` | `1` | finish after every fallback draw |
| `LP_NUM_THREADS` | n | llvmpipe rasterizer threads (`LP/lp_screen.c:1070`) |

Counters `ctx->sw_draws`, `ctx->sw_vertices` and per-reason totals are
printed with the existing reject report (`chrome9_report_rejects`,
`C/chrome9_context.c:147`).

### 5.3 Fallback draw body

```c
static void
chrome9_sw_draw_vbo(struct chrome9_context *ctx,
                    const struct pipe_draw_info *info, unsigned drawid_offset,
                    const struct pipe_draw_start_count_bias *draws,
                    unsigned num_draws)
{
   struct pipe_draw_info lp_info = *info;

   chrome9_draw_flush_batch(ctx);
   chrome9_context_prepare_render_target(ctx);
   if (!chrome9_sw_context_get(ctx) || !chrome9_sw_mirror(ctx)) {
      CHROME9_REJECT(ctx, draws[0].count);
      return;
   }
   if (info->index_size && !info->has_user_indices) {
      lp_info.has_user_indices = true;
      lp_info.index.user = chrome9_resource(info->index.resource)->bo.map;
      lp_info.take_index_buffer_ownership = false;
   }
   ctx->sw->lp->draw_vbo(ctx->sw->lp, &lp_info, drawid_offset, NULL,
                         draws, num_draws);
   ctx->sw->pending = true;
   chrome9_sw_mark_written(ctx);
}
```

`chrome9_draw_flush_batch()` at the top is required even though
`chrome9_sw_finish()` is also called from it.  It retires GPU batches that
wrote the framebuffer or the textures this draw reads.  Because `pending` is
checked first, consecutive fallback draws do not finish each other.  The
order inside `chrome9_draw_flush_batch()` must be "submit GPU batch, then
finish llvmpipe", or the reverse; the two domains are never active at the
same time, so either order is correct.

## 6. Performance and correctness

### 6.1 Terrain noise pass (estimate)

Workload (from the capacity-strategies document): a 256×256 target (65,536
fragments), two triangles, no depth, no textures, and about 1,700 scalar
lane-operations per fragment after chrome9's NIR lowering.

* llvmpipe executes fragment shaders in SoA form, with one 128-bit vector
  holding one scalar for four fragments.  That gives about 425 SSE
  instructions per fragment.  Without SSE4.1, the 152 floor/fract lanes cost
  about 5 instructions each instead of 1, so the total is about 550–650
  instructions per fragment, or 36–43 M instructions per frame.
* At 1.2–1.6 GHz and an effective IPC of about 1, that is 25–35 ms on one
  core.  With two rasterizer threads and the main thread doing binning, it
  is about 13–20 ms.  Setup and binning for two triangles are negligible.
  Framebuffer access is zero-copy: the 256 KiB height map is read and
  written in place, and the next GPU submission flushes 256 KiB of cache
  lines, which the kernel already does today for every object.
* One hardware/fallback transition per frame: synchronous batch submission
  (existing) plus llvmpipe flush and finish (estimate 0.1–0.5 ms).
* One-time: LLVM compile of the noise FS variant.  The estimate is 0.2–2 s
  on the Nano at llvmpipe's optimization level for a few thousand IR
  instructions.  llvmpipe stores variants in its disk cache
  (`LP/lp_screen.c:892-909`, `MESA_SHADER_CACHE_DIR`), so later runs skip
  it.  The VS compile is small.

The target of less than 1 s per frame is met with a wide margin.  The
"tens of ms for 256×256" goal is met if the Nano delivers at least about 1
SSE instruction per clock.  Measure with `CHROME9_SW_DEBUG=1` and
`LP_NUM_THREADS=1/2`.

### 6.2 Correctness risks

| # | Risk | Details | Mitigation |
|---|---|---|---|
| R1 | 4-row block overhang | section 2.2; writes past level 0 into guard, mip or unmapped memory | alias rule in 2.3 (stage when `h % 4 != 0` and padding is insufficient); host test with `mprotect(PROT_NONE)` guard pages |
| R2 | X8Z24 / S8Z24 bit layout | Gallium `X8Z24_UNORM` has depth in bits 31:8.  This matches the hardware layout (`docs/research/chrome9-d24-r2t-transition-oracle.md:55-56`, `chrome9-depth-r2t-vtf-contract.md:55-56,158-163`).  llvmpipe masks with `0xffffff00` (`LP/lp_bld_depth.c:938-949`) and stores stencil in bits 7:0 for S8Z24 | verify in the host test that llvmpipe writes the X byte as 0, as the D24 contract requires for later S8Z24 view transitions |
| R3 | depth value mismatch across paths | the hardware path sends vertex depth as 16.16 fixed point (`C/chrome9_draw.c:590-595`), so GPU-written D24 values carry at most 16 fractional bits at the vertices.  llvmpipe converts FP32 z with `lp_build_clamped_float_to_unsigned_norm` (`LP/lp_bld_depth.c:1079`).  Multi-pass `GL_EQUAL` / `GL_LEQUAL` passes split across paths will not match | "depth-coherent" rule: once the fallback has written a depth buffer, route later draws that test it with `EQUAL`, or `LEQUAL` with depth writes disabled, to the fallback until that buffer is cleared.  Terrain noise uses no depth |
| R4 | coverage / fill rule | llvmpipe uses 8 sub-pixel bits (`LP/lp_rast.h:53`) and the GL top-left rule; chrome9 uses 16.16 coordinates clamped to `[-0.5, extent-0.5]` (`C/chrome9_draw.c:575-587`) and the hardware rule | seams only when neighbouring primitives in one image take different paths; full-target passes (terrain) are unaffected; test with a split-material scene |
| R5 | float semantics | llvmpipe is IEEE FP32 with denormals flushed in rasterizer threads (`LP/lp_rast.c:1119-1124`).  Hardware precision and `RCP`/`RSQ`/`EXP`/`LOG` approximations differ.  Both execute the same chrome9-lowered NIR (`lower_fsat`, `lower_ffloor`, int-to-float), so the results are GL-conformant but not bit-identical to hardware | compare against pure llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`), not against hardware output |
| R6 | vertex stage difference | chrome9 runs the VS in the TGSI interpreter (`draw_create_no_llvm`); the fallback runs llvmpipe's LLVM VS.  Positions may differ by ULPs | same as R3/R4; acceptable for full-pass fallbacks |
| R7 | sampling and blending precision | llvmpipe AoS unorm8 filtering uses 8-bit weights, and its LOD is approximated; hardware blend rounding is unknown | image-difference tolerance in tests |
| R8 | `no_integers` NIR in the llvmpipe FS path | the gallivm branch at `GV/lp_bld_nir_soa.c:6219-6227` is local and has been exercised by LLVM draw only.  Correct handling of the fragment-only intrinsics (`load_frag_coord` with chrome9's `fs_coord_pixel_center_integer`, `fs_position_is_sysval`, `C/chrome9_screen.c:480-487`; `front_face` as float; discard) is not yet verified | host test first: chrome9-finalized NIR through llvmpipe versus stock llvmpipe on the original GLSL, bit-exact expected |
| R9 | NIR form | llvmpipe must accept the IO lowering the state tracker produced for chrome9's options.  Unverified | same host test |
| R10 | resource types | the fallback does not extend resource support: chrome9 still rejects cube/3D/array textures and non-2D views (`C/chrome9_context.c:5814-5824`, `C/chrome9_resource.c:84-96`) | out of scope |
| R11 | memory and startup | every GL process maps libLLVM (shared mode) and pays dynamic relocation; llvmpipe context and scene memory add a few MB once the fallback is used | lazy creation (1.3); static LLVM as the alternative |

## 7. Implementation order and tests

1. Build: meson changes (1.1), `chrome9_swfb.c` with winsys, screen and
   context creation, finish; `CHROME9_SW_FALLBACK=off` default at first.
2. Host unit test `chrome9-swfb-test`, no DRM.  Fake chrome9 resources in
   `mmap` memory with the exact guarded layout and `PROT_NONE` guard pages.
   Render a triangle pair with (a) a trivial FS, (b) a >96-record FS built
   through `chrome9_finalize_nir`, (c) X8Z24 depth.  Compare bit-exactly with
   the same draw into llvmpipe-native resources.  Include `h = 255` to
   exercise staging.  Register it in `C/meson.build` like the other
   `chrome9-*` tests.
3. State mirror and CSO caching, FS reason only, `auto` default.
4. Target: glmark2 terrain with `CHROME9_SW_DEBUG=1`; compare the frame with
   `LIBGL_ALWAYS_SOFTWARE=1`; record the timings of the noise pass and of
   the transition.
5. DOMAIN reason, then the optional STATE reason and the depth-coherent rule
   (R3).

## 8. Blockers and open questions

* No hard blocker was found.  The LLVM cross build is proven by
  `M/objs-m9x-llvm`, and `S/usr/lib64/libLLVM.so.23.1` exists.
* Open: whether the T2 `mesa` package configuration may take
  `llvmpipe` + LLVM for the chrome9 target.  This decides the shared versus
  static LLVM choice and the target image size.
* Open: exact CPU clock and SSE4.1 availability of the VIA Nano X2 L4350;
  only the performance estimate depends on them.
* The kernel ABI fixes the BO size (`K/chrome9_engine.c:5634-5638`), so the
  4-row overhang for odd heights needs staging rather than padding.
* R8/R9 (llvmpipe with chrome9-finalized NIR) are the main technical
  unknowns.  They can be settled on the host before any target run.

Copyright (C) 2026 René Rebe <rene@exactco.de>
