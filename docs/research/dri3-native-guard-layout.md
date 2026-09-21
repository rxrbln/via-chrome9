# Native DRI3 sharing and guarded Chrome9 resources

This note audits the current same-device DRI3 path from a Chrome9 client
through Xorg/glamor and back into Chrome9 Gallium.  It distinguishes three
independent properties which must not be conflated:

1. dma-buf/GEM object identity and lifetime;
2. the byte layout needed by the bounded render UAPI; and
3. the SF PTEs which make those bytes reachable by the GPU.

The intended architecture remains native and zero-copy: a Chrome9 GEM object
is rendered by Chrome9, shared with another Chrome9 DRM file when DRI3 needs
it, and either sampled by Chrome9 or scanned out by Chrome9.  No second GPU or
PRIME-offload copy is involved.

## Same-device PRIME identity

The Linux 7.2 core makes same-device sharing stronger than the word
"imported" suggests.  `drm_gem_shmem_prime_import_no_map()` at
`drivers/gpu/drm/drm_gem_shmem_helper.c:934..972` checks
`drm_gem_is_prime_exported_dma_buf(dev, dma_buf)`.  If the dma-buf came from
the same `drm_device`, it returns `dma_buf->priv`, the original GEM object,
after taking an object reference.  It does not make a proxy object or set
`obj->import_attach`.

Consequently, in the intended DRI3 flow:

```text
client drm_file -- PRIME fd --> Xorg drm_file
                         same Chrome9 drm_device
                                  |
                                  +-- new Xorg handle, original GEM object
                                      original dma_resv, import_attach=NULL
```

`drm_gem_is_imported(obj)` means that the object is a dma-buf attachment from
another device.  It does **not** mean that this drm_file obtained its handle
through `PRIME_FD_TO_HANDLE`.  The Chrome9 kernel therefore correctly accepts
a same-Chrome9 round trip while rejecting an actual external-device proxy:

* `chrome9_sf_pin()` rejects `obj->dev != &cdev->drm` and
  `drm_gem_is_imported(obj)`;
* framebuffer creation applies the same test; and
* all current draw submission paths apply the same test before converting the
  object to `drm_gem_shmem_object`.

The dma-buf fd holds the exported object alive.  The handle created in the
receiving drm_file holds another GEM reference.  Both views share the original
reservation object, so producer fences remain visible to
`drm_gem_plane_helper_prepare_fb()` and the render ioctls' `dma_resv` waits.
This is the required synchronization relationship for native DRI3; it is not
provided by copying bytes through a userspace shadow.

## Current Mesa handle-lifetime defect

`chrome9_bo_import_fd()` currently sets every imported wrapper's
`handle_owned=false` and says that imported handles remain until the screen fd
closes.  This avoids one real hazard, but creates a leak and does not solve all
alias lifetimes.

The core PRIME cache is per `drm_file`, not per process-wide object.
`drm_gem_prime_handle_to_dmabuf()` records `(dma_buf, handle)` when exporting.
`drm_gem_prime_fd_to_handle()` first looks up that pair and, on a hit, returns
the existing handle without creating or incrementing a handle reference.
The installed libdrm 2.4.134 does not add any userspace ownership layer:
`drmPrimeHandleToFD` at `libdrm.so.2+0xcc90` issues ioctl `c00c642d`, and
`drmPrimeFDToHandle` at `+0xccf0` issues ioctl `c00c642e` and copies the
returned handle.  Its installed `xf86drm.h:815..833` explicitly warns that GEM
handles are not reference counted, guarantees that multiple dma-buf fds for
one object return the same handle, and requires the caller to provide a single
userspace reference-counting implementation.  The current Chrome9 wrappers do
not yet provide it.
Thus this sequence on one Gallium screen is unsafe:

```text
native resource A owns handle H
export A; import fd on the same screen -> alias B also names H
destroy A -> GEM_CLOSE(H)
B remains alive but its submission handle no longer exists
```

Marking B non-owning prevents B from closing A's handle, but it cannot prevent
A from closing B's handle.  Conversely, an import which was not already in
that drm_file's cache creates a new handle reference; never closing that
handle leaks it and its GEM object until the DRM fd closes.  Repeated Xorg
pixmap imports can therefore grow the file's handle table for the lifetime of
the server.

This defect is scoped per drm_file.  An application and Xorg normally use
different DRM files, so the receiving Xorg file gets its own handle and the
client closing its handle does not invalidate Xorg's handle.  The current
window-copy diagnostic (`num_sampler_views=0`) occurs before a texture handle
is submitted and is not explained by this defect.  The defect is nevertheless
a production blocker for same-screen export/re-import, repeated aliases, and
long-lived DRI3 consumers.

The implementation-ready repair is a `chrome9_screen` BO table protected by a
mutex and keyed by the returned GEM handle:

1. register every locally created handle in the table with one handle
   reference and one resource reference;
2. after `drmPrimeFDToHandle`, look up the returned handle;
3. on a hit, share/increment the table entry instead of inventing ownership;
4. on a miss, create an owning table entry for the handle reference just
   returned by the kernel; and
5. issue exactly one `GEM_CLOSE` when the entry's last resource reference is
   dropped.

The lookup and insertion must be serialized because concurrent imports of one
dma-buf are intentionally collapsed by the kernel.  Mapping ownership can be
kept per resource initially, or moved into the same entry with a separate map
reference.  Keying only by a dma-buf fd number is wrong because fd numbers are
reused; the kernel-returned handle is the relevant per-drm_file identity.

The cache also defines an important ownership boundary.  Every handle created
or imported through that screen fd must pass through the same table.  If EGL,
GBM, or another library independently creates/imports handles on the identical
DRM file description, neither implementation can know whether it owns the one
kernel handle returned for an object.  This is the exact shared-fd case which
libdrm's header forbids.  Chrome9 should use its loader-owned screen fd through
one screen-wide BO manager and must not put a second private manager around a
duplicated reference to the same file description.

### Hardware-free lifetime tests

The cache can be validated without submitting any GPU work:

1. Create A, export dma-buf F, import F as B on the same screen, and assert
   `A.handle == B.handle`.
2. Destroy A first.  Exporting B with `drmPrimeHandleToFD` must still succeed.
   This fails with the present code because destroying A closes the shared
   handle.
3. Repeat with B destroyed first.  A must remain exportable and exactly one
   `GEM_CLOSE` must occur when A is finally destroyed.
4. Import two independently duplicated dma-buf fds for the same object and
   destroy aliases in every permutation.  Every wrapper must share one cache
   entry and the handle must remain exportable until the last wrapper drops.
5. On a second DRM file, import F, close the original file's handle, and prove
   the second file's handle remains exportable.  This distinguishes the
   intended per-file table from an incorrect process-global handle table.
6. Loop import/destroy of one held dma-buf thousands of times and instrument
   the BO table: its live-entry count must return to baseline and its
   `GEM_CLOSE` count must match newly created import entries, not wrapper
   count.

Close every temporary dma-buf fd in all cases.  These tests exercise only GEM
handle and dma-buf lifetime; they need no SF mapping, MMIO write, draw ioctl,
or active display.

## Canonical level-zero guard proof

For a level-zero linear surface, `chrome9_resource_layout()` constructs this
exact object:

```text
P = system page size (4096 on the target)
C = bytes per pixel (4 for BGRX/BGRA, 2 for Z16)
S = ALIGN(width * C, 32)
D = S * height
O = P
Z = P + ALIGN(D, P) + P

byte [0, P)                         leading mapped guard
byte [P, P + D)                     useful rows, stride S
byte [P + D, P + ALIGN(D,P))        row-tail/allocation padding
byte [P + ALIGN(D,P), Z)            trailing mapped guard
```

For the measured 1024x768 XRGB front buffer this gives:

```text
stride       = 4096
pixel offset = 4096
payload      = 4096 * 768 = 3,145,728
object size  = 4096 + 3,145,728 + 4096 = 3,153,920 bytes
page count   = 770
```

Mesa calls an imported level-zero BGRX resource guarded only when all three
external values exactly match the recomputed layout:

```text
whandle.stride == S
whandle.offset == P
dma_buf_size   == Z
```

The surrounding import checks also require a linear/invalid modifier,
level zero, a 2D/RECT layout accepted by `chrome9_resource_layout()`, BGRX,
32-byte-aligned stride, and bounded dimensions.  This tuple proves that the
object contains enough bytes at the exact locations required by the bounded
UAPI.  It does **not** prove who allocated the object.  An unrelated exporter
can coincidentally provide the same size, stride, and offset.

The kernel remains the authoritative provenance and bounds check.  For a
level-zero destination it independently derives
`P + ALIGN(pitch*height,P) + P` and requires `obj->size` to equal it.  Texture
sources may be larger to admit a complete guarded mip allocation, but the
kernel prepares and verifies both the guard at the submitted level-zero end
and the guard at the complete object end.  It also rejects external import
proxies before binding any PTE.  Therefore a coincidentally canonical foreign
tuple cannot escape into hardware, although current Mesa will misclassify it
as direct and report a failed draw rather than selecting its shadow fallback.
A future driver-info ioctl can expose native-object status explicitly if
cross-device imports are to degrade gracefully.

## What the guard PTEs are

The current guard pages are ordinary mapped pages, not faulting holes.
`chrome9_collect_sgt_ptes()` emits one valid
`(DMA_PFN & 3fffffff) | 02000000` entry for every page of the complete shmem
object.  The GPU base is the SF node plus `PAGE_SIZE`; therefore both the
leading and trailing guards remain valid translations adjacent to the useful
image.  Before submission the kernel fills the first and last pages with the
guard word, transfers the SG table to the device, and after proven retirement
transfers it back and compares every guard dword.  Reservation locks prevent
a correctly participating producer from racing this diagnostic interval.

This choice is deliberate safety containment.  A small texture prefetch or a
bad edge calculation reaches mapped poison rather than raising an
uncharacterized GPU translation fault.  Destination overruns are detected
after retirement.  It does cost two physical pages and causes the diagnostic
path to touch/flush/scan them.

Both vendor stacks support invalid *unused* PTEs but provide no evidence for
using invalid PTEs as per-allocation guard pages:

* old Linux initializes the whole table to `80000000`, then overwrites every
  page of an allocated SG range with a valid PFN;
* Windows likewise uses `80000000` for absent residency entries and writes a
  PFN, plus bit 25 for cached memory, for every MDL page; and
* neither stack has a recovered fault-status/acknowledgement/restart path for
  an invalid-PTE fetch.

An invalid trailing PTE may fault or wedge the command reader, rasterizer, or
display.  It must not be used as a live overfetch detector.  Invalid entries
are safe only for unreachable aperture space after all readers have retired.

## Can synthetic mapped guards remove sampler shadows?

The page table can in principle compose a GPU-virtual range from a kernel
guard page followed by pinned resource pages and another guard page.  All
three kinds of entry would be valid, so this does not require invalid-PTE
fault behavior.  Such a mapping could eliminate the CPU byte copy for a
native, linear, otherwise unguarded sampler resource.

It is not implemented or yet safe as a general import path:

* current draw ioctls require the submitted handle's exact guarded object size
  and derive all SF offsets from whole-object sizes;
* current mappings use the object's complete cached SG table, not a
  per-view PTE splice;
* a repeated scratch page has no recovered read-only permission bit, so it is
  unsafe as a shared destination guard and weakens per-object corruption
  attribution;
* external dma-buf proxies are deliberately rejected, and importing their SG
  tables would require a complete attachment/DMA-map lifetime and cache-policy
  contract; and
* source row padding, offset, pitch, format, and reservation fences still need
  independent validation.  Two synthetic edge pages do not make an arbitrary
  allocation canonical.

The bounded future extension is therefore native-source-only: retain the
original GEM/dma-buf and its reservation, pin its pages, allocate private valid
guard pages, build an immutable per-view SF mapping, and teach a new kernel
UAPI to validate `{object byte range, offset, pitch, dimensions}` rather than
pretending the physical object owns those guards.  Render targets should keep
private object guards until write permissions, fault recovery, and concurrent
guard attribution are understood.

For today's same-Chrome9 DRI3 path no synthetic mapping is needed.  The
exported object already includes the canonical guards; preserving its exact
offset/stride/size across PRIME lets the receiving Chrome9 driver sample it
directly with the original reservation object and no CPU shadow.

## Safety and performance consequences

Direct native import removes one full-image CPU seed/copy-back around glamor
sampling.  At 1024x768 XRGB that is 3 MiB per copy.  The guard overhead is
only 8 KiB (about 0.26 percent) for that surface, though it is proportionally
large for tiny pixmaps.

It does not remove the current kernel's larger synchronous costs: rebuilding
and publishing every PTE per draw, full-SG DMA ownership transitions, full-BO
cache maintenance, guard scanning, and PTE invalidation remain.  Persistent
native mappings and optional post-bring-up guard readback are separate later
optimizations.  They must preserve the shared reservation object, valid mapped
guards, retirement proof, and fail-closed quarantine behavior.

Before treating a DRI3 source as zero-copy, require all of the following:

* same Chrome9 `drm_device` provenance confirmed by the kernel;
* linear BGRX level zero and the exact canonical tuple above;
* a live handle-cache reference in the receiving drm_file;
* the producer reservation fence retired before sampling;
* valid bit-25 PTEs for every object and guard page, followed by the bounded
  SR6F translation flush; and
* source preservation plus both guards intact after the first diagnostic
  submissions.
