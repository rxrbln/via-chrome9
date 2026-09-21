# Xorg modesetting/glamor ScreenInit failure

## Measured failure

The target log `/root/tmp/chrome9/Xorg-chrome9-gl21.log` from Xorg
21.1.24 reaches all of the following without a kernel error:

```text
glamor: Using OpenGL 2.1 context.
glamor X acceleration enabled on VIA Chrome9 HD ...
glamor initialized
...
Depth 24 pixmap format is 32 bpp
AddScreen/ScreenInit failed for driver 0
```

The depth line is not evidence that a framebuffer ioctl failed.  T2's
`restore-xaa.patch` adds it to `InitOutput()` before DIX calls `AddScreen()`.
The next operation is the modesetting driver's `ScreenInit()`.

The first deterministic failure is front-buffer allocation, before AddFB,
atomic validation, or a dumb-buffer ioctl.  This is a same-device GBM/Gallium
contract failure; PRIME offload and a second GPU are not involved.

## Exact current failure

The relevant Xorg 21.1.24 call chain is:

```text
ScreenInit                                     driver.c:1809
  drmmode_create_initial_bos                   drmmode_display.c:4159
    drmmode_create_bo(1024, 768, 32)            drmmode_display.c:1082
      gbm_bo_create_with_modifiers(ARGB8888)    first attempt
      gbm_bo_create(ARGB8888,
        RENDERING | SCANOUT)                    fallback
```

For depth 24, modesetting deliberately asks GBM for
`GBM_FORMAT_ARGB8888`, not XRGB8888.  Gallium's DRI image frontend maps that
to `PIPE_FORMAT_B8G8R8A8_UNORM`.  It adds the resource bindings derived from
the request:

```text
PIPE_BIND_RENDER_TARGET
PIPE_BIND_SAMPLER_VIEW
PIPE_BIND_SCANOUT
PIPE_BIND_SHARED
```

The first modifier-aware attempt is expected because the Chrome9 primary
plane exposes XRGB8888 plus `DRM_FORMAT_MOD_LINEAR` in `IN_FORMATS`.  It fails
because the current Chrome9 `pipe_screen` does not install
`resource_create_with_modifiers`; `dri_create_image()` rejects a nonempty
modifier list when that callback is absent.  Xorg intentionally falls back to
ordinary `gbm_bo_create()`.

That fallback also fails.  The current `chrome9_format_supported()` keeps
BGRA8888 internal-only: its allowed bindings omit `PIPE_BIND_SCANOUT` and
`PIPE_BIND_SHARED`.  Consequently `chrome9_resource_create()` rejects the DRI
template, GBM returns `NULL`, `drmmode_create_initial_bos()` silently returns
`FALSE`, and modesetting `ScreenInit()` returns `FALSE`.  This exactly matches
the absence of a modesetting-specific or kernel error after the depth line.

The required allocation contract is not a request to scan alpha.  Xorg later
passes the BO through `get_opaque_format()`, so the KMS framebuffer is
`DRM_FORMAT_XRGB8888`; ARGB is the GBM/root-pixmap storage convention for a
depth-24 screen.

## The immediately following same-device handle requirement

After BGRA scanout/shared allocation is accepted, Mesa GBM queries the DRI
image for `__DRI_IMAGE_ATTRIB_HANDLE`, `STRIDE`, and later `OFFSET`.  Gallium
maps these attributes to a `winsys_handle` with
`WINSYS_HANDLE_TYPE_KMS`.  The handle is the GEM handle already owned by the
same DRM file; exporting and reimporting a dma-buf is neither required nor
desirable.

The captured driver accepted only `WINSYS_HANDLE_TYPE_FD`.  GBM does not fail
the BO when its initial HANDLE/STRIDE queries fail; it leaves the calloc-zeroed
handle and stride.  Xorg would therefore proceed with `displayWidth == 0`.
This is a real next-stage defect, although it is not the first failure in the
captured log.  Also, zero `displayWidth` alone does not make `fbScreenInit()`
return false: `miScreenInit()` accepts a zero-width backing and defers the
screen pixmap to `CreateScreenResources()`.

The bounded same-device result must be:

```text
type      = WINSYS_HANDLE_TYPE_KMS
handle    = resource->bo.handle       # no ownership transfer
stride    = 4096                      # 1024 * 4, 32-byte aligned
offset    = 4096                      # current leading guard page
modifier  = DRM_FORMAT_MOD_LINEAR
```

The same query must accept the ARGB/BGRA storage resource used by Xorg as well
as the XRGB/BGRX application surface.  The FD path remains useful for optional
same-Chrome9 sharing, but it is not on this front-buffer path.

## Modifier and AddFB2 requirement

Merely allowing BGRA and returning the KMS handle is enough to get beyond the
observed `ScreenInit()` failure, but it is not enough for correct scanout.
Without `resource_create_with_modifiers`, Xorg marks the fallback BO
`used_modifiers = FALSE`.  `drmmode_bo_import()` then uses legacy
`drmModeAddFB()`, which cannot carry the resource's 4096-byte data offset.
The framebuffer would describe offset zero, i.e. the leading guard rather
than the image rendered by Chrome9.

The production-compatible linear path is therefore:

1. Mesa accepts only a one-element `DRM_FORMAT_MOD_LINEAR` modifier list (and
   rejects every unknown modifier), using the same guarded linear allocator.
2. GBM records `used_modifiers = TRUE`.
3. modesetting obtains the one plane's KMS handle, 4096-byte pitch,
   4096-byte offset, and LINEAR modifier.
4. modesetting calls `drmModeAddFB2WithModifiers(...,
   DRM_MODE_FB_MODIFIERS)` and converts ARGB to XRGB for scanout.
5. KMS accepts only a page-aligned offset whose overflow-checked
   `offset + pitch * height` fits the GEM object.

Linux 7.2 reports `DRM_CAP_ADDFB2_MODIFIERS` unless
`mode_config.fb_modifiers_not_supported` is set.  Chrome9 passes a LINEAR
modifier list to `drm_simple_display_pipe_init()`, so DRM core creates the
plane `IN_FORMATS` property and Xorg will take the modifier path once Mesa
implements it.

Current KMS explicitly rejects this correct request in two places:

* `chrome9_scanout_layout_validate()` rejects every nonzero offset;
* `chrome9_fixed_state_valid()` requires `fb->offsets[0] == 0`.

Those checks must become the page-aligned, object-bounded validation above.
Accepting arbitrary offsets would be unsafe; accepting only zero would make
the screen display the guard page.

## Screen-front lifecycle after allocation succeeds

Xorg's sequence is important when checking the next log:

```text
ScreenInit
  allocate front GBM BO
  initialize fb/glamor/RandR/colormap/present

CreateScreenResources
  create root screen pixmap
  record desired mode (the initial call uses set_hw = FALSE)
  import the same front GBM BO as the root pixmap EGL image

first modesetting block handler
  drmModeAddFB2WithModifiers for that same BO
  glamor_finish()
  atomic test/commit, then the real atomic commit
```

Thus AddFB2 and atomic plane validation are later stages and cannot explain
the captured immediate ScreenInit failure.  Once reached, failures should be
visible as `failed to add fb`, `failed to set mode`, or an atomic ioctl error.

The front BO remains owned by `drmmode.front_bo` for the screen lifetime, and
the KMS framebuffer holds the GEM reference while active.  The Chrome9 KMS
implementation must attach a persistent SF mapping to that framebuffer (or
to the GEM object), keep every PTE and DMA mapping pinned while IGA1 can fetch
it, and release the old mapping only after disable or a completed display
latch.

The current `chrome9_pipe_enable()` and `chrome9_pipe_update()` instead call
`chrome9_gem_copy_fb_to_scanout()` and program a private BAR2 carveout.  That
is incompatible with the required architecture.  The final front buffer must
be the one Chrome9 GEM object rendered through its visible offset and scanned
out from its persistent translated-SF address; no BAR2 copy is part of the
path.

## Dirty, rendering, and flip synchronization

`CreateScreenResources()` probes `drmModeDirtyFB()`.  A return of `-EINVAL` or
`-ENOSYS` disables Xorg damage dispatch.  That is acceptable for coherent
direct scanout and must not be used as a hidden copy trigger.

Before an initial mode change, modesetting calls `glamor_finish()`.  Present
flips call the glamor block handler before importing and flipping the new BO.
The current Chrome9 draw ioctls retire synchronously, so completion at ioctl
return is the bounded first implementation's producer fence.  If submission
becomes asynchronous, the GEM reservation object or an atomic `IN_FENCE_FD`
must carry the render dependency; a page-table flush alone is not render
completion.

Chrome9 currently has no vblank IRQ support.  Static initial scanout can still
be validated, but page flips with events must remain disabled/quarantined
until the driver can deliver the real vblank completion.  A successful MMIO
base write is not permission to signal a flip event or unpin the prior SF
mapping.

## Smallest discriminators

The safest staged checks, without submitting any 3D work, are:

1. Create the exact 1024x768 GBM ARGB8888 BO with
   `GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT`.  Require non-NULL, KMS handle
   nonzero, stride 4096, offset 4096, and LINEAR modifier.
2. Require the modifier-aware create path, not its legacy fallback.
3. Submit AddFB2 only and require an XRGB8888 framebuffer with offset 4096;
   do not enable the CRTC yet.
4. Run an atomic `TEST_ONLY` commit with the fixed plane geometry.  This checks
   format, pitch, offset, and plane state without programming IGA1.
5. Only after the persistent SF mapping exists, run the already specified
   fixed-mode direct-SF scanout oracle.

The first check distinguishes the captured Mesa allocation/handle defects.
The third and fourth distinguish framebuffer-layout and atomic validation
from the later physical display-address path.
