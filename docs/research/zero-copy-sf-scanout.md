# VX900 zero-copy IGA1 scanout from translated SF

This note defines the smallest path by which Chrome9 3D and IGA1 can use the
same GEM shmem backing without PRIME or a CPU copy into BAR2.  The public VX900
manual is complete enough to construct the static display address and the
existing render path already proves the required RTSF translation.  Direct
IGA1 fetch from SF is **not yet live-proven**, so the first implementation must
remain a guarded, fixed-mode oracle before it replaces the BAR2 staging path.

## Exact display address

The VX900 Part I manual, printed page 66, defines CR48 exactly:

```text
CR48[7:6]  IGA1 channel: 00=SL, 01=SF, 1x=reserved
CR48[5]    reserved/RO
CR48[4:0]  starting-address bits [28:24]
```

CR0D, CR0C, and CR34 contain starting-address bits `[7:0]`, `[15:8]`, and
`[23:16]`.  IGA1 uses a two-byte address unit.  This unit is independently
confirmed by both proprietary drivers:

* Xorg `VIASetStartingAddress:0x4cd7a..0x4ce2b` shifts the byte address right
  once before packing CR0D/CR0C/CR34/CR48;
* Windows `S3gSetVidPnSourceAddress` IGA1 helper
  `VTGKModeDX32A.sys:0x79683..0x796de` performs the same shift and puts the
  resulting bits `[28:0]` into packed MMIO alias `0214` while preserving
  `[31:29]`.

For an SF virtual byte address `B`, the indexed-register image is therefore:

```text
A = B >> 1
CR0D       = A[7:0]
CR0C       = A[15:8]
CR34       = A[23:16]
CR48[4:0]  = A[28:24]
CR48[7:6]  = 01b                       # translated SF
```

`B` is an offset in the driver's internal 64-MiB SF virtual aperture.  It is
not a CPU virtual address, a DMA address, a PFN, or a BAR2/local-framebuffer
offset.  Preserve CR48 bit 5 and set only masks `c0` and `1f`.  The existing
fixed KMS path at `chrome9_kms.c:222..251` already implements the same packing
for SL; its sole domain change is CR48 `[7:6]=01` and its base source changes
from `scanout_offset` to the pinned framebuffer's SF VA.

The current 1024x768 mode's pitch remains 4096 bytes.  Viafb
`via_set_primary_pitch()` proves the general encoding:

```text
P = pitch_bytes >> 3
CR13      = P[7:0]
CR35[7:5] = P[10:8]
```

Thus the location change does not alter format, timing, FIFO, fetch-count, or
PLL programming.  XRGB8888 stays the live-proven SR15 depth-24 image and the
linear modifier remains mandatory.

MMIO `0214` is a packed address alias, not a location selector.  When it is
used for a page flip, first establish and retain CR48's SF channel; then apply
the recovered read/modify/write pack:

```text
new_0214 = (old_0214 & e0000000) | ((B >> 1) & 1fffffff)
```

The Windows helper does not program CR48 in this path because channel
selection belongs to mode state.  Its direct-base sequence first streams T11
A12=`12000000` to disarm automatic swapping, selects by-frame operation with
CR32 bit 0 clear, and only then updates `0214`.  Do not mix an armed T11 swap
with a direct base write.

## Exact RTSF mapping used by display

Part I printed pages 46--48 define the shared graphics translation interface:

```text
SR6A       table base bits [19:12]
SR6B       table base bits [27:20]
SR6C[0]    table base bit  [28]
SR6C[7]    GART table write protection
SR6F[7]    translation-cache flush; software sets, GTI clears
```

The old Linux Chrome9 KMD and Windows KMD both construct one 4-KiB-page RTSF
table from page-frame numbers.  The live driver has proved the VX900 form for
ordinary WB GEM shmem:

```text
pte = (dma_address >> PAGE_SHIFT) & 3fffffff | 02000000
```

Bit 25 is the vendor cached/snooped attribute for WB shmem.  Plain PFNs are
valid only for the measured coherent/noncached and WC cases.  The manual's
SR79/SR7A descriptions explicitly call their arbitration clients “SL/SF” and
say the small kill number services display promptly; together with CR48's SF
channel this establishes that IGA1 is a client of the same translated address
space, not a second display-only page table.

Every scanout page must be DMA-mapped below the current 32-bit device boundary,
represented by a complete PTE, and remain pinned.  Publishing a new mapping is
the existing bounded transaction:

1. wait for `SR6F[7]` clear;
2. clear SR6C bit 7 without changing SR6C bit 0;
3. write every PTE and drain the final write;
4. execute a write barrier, set SR6F bit 7, and wait for hardware to clear it;
5. restore SR6C bit 7.

An H0 engine-idle word and a render fence do not flush GTI translations.
Conversely, the GTI flush does not prove rendering complete or a display base
latched.

## Smallest object and pinning design

The least invasive first implementation is one private framebuffer wrapper,
not a new GEM memory manager:

```text
struct chrome9_framebuffer {
        struct drm_framebuffer base;
        struct drm_mm_node sf_node;
        struct sg_table *sgt;
        u32 *ptes;
        unsigned int page_count;
        bool sf_bound;
};
```

`chrome9_gem_fb_create()` validates the GEM object, allocates this wrapper,
gets the shmem SG table, allocates one page-aligned node from the engine's
existing `sf_mm`, installs cached PTEs, and records:

```text
scanout_base = sf_node.start + fb->offsets[0]
```

The framebuffer reference held by an active plane then also owns the SG/DMA
pin, `drm_mm_node`, and PTE lifetime.  Its destroy callback invalidates the
complete PTE range, performs the bounded GTI flush, removes the node, releases
the page-table array, and finally runs GEM framebuffer cleanup.  Framebuffer
creation which cannot acquire a complete stable SF mapping fails; there is no
late fallback to the BAR2 address after atomic check.

For the first implementation:

* accept only same-device, non-imported shmem objects;
* accept only linear XRGB8888 and the fixed 4096-byte pitch;
* require a page-aligned framebuffer offset and an overflow-checked
  `offset + pitch * height <= obj->size`;
* require `scanout_base + pitch * height <= 64 MiB` and an even base;
* serialize every PTE/node change with engine submission in the established
  `submit_lock` then `vm_lock` order.

The current render ABI places a guard page before and after the visible image:
its render target is `job_node.start + PAGE_SIZE`, while KMS currently rejects
all nonzero framebuffer offsets (`chrome9_gem.c:26..40,96..105` and
`chrome9_kms.c:319..324`).  Zero-copy must allow the exact
`fb->offsets[0]=PAGE_SIZE` layout and include both guards in the pinned node.
Pointing IGA1 at offset zero would scan the leading guard, not the image.

This wrapper design may give one BO a persistent display SF alias and a
different transient render SF alias.  Both aliases contain the same PFNs with
the same bit-25 cache attribute, so it is genuinely zero-copy and requires no
render-UAPI change.  It is the smallest useful silicon oracle.  The test must
nevertheless check for a stale-image aliasing hazard.  If that appears, the
production follow-up is an object-level `chrome9_sf_mapping`, reference-counted
by framebuffers and render jobs, so render and display use one stable SF VA.
Do not guess a cache-flush packet to hide an aliasing failure.

The corresponding small code delta is explicit:

* replace generic `drm_gem_fb_create()` with the private framebuffer and its
  destroy callback;
* stop making dumb creation and FB creation depend on the BAR2
  `scanout_size`; bound them by the SF aperture and actual GEM size instead;
* remove the fixed-mode requirement for a BAR2 carveout;
* replace `chrome9_gem_copy_fb_to_scanout()` in pipe enable/update with the
  pinned framebuffer's `scanout_base`;
* parameterize `chrome9_iga1_program()` by that base and select CR48 SF.

No render ioctl or Mesa ABI needs to change for the alias-based oracle.

## Atomic ownership and lifetime

The safe enable transaction is:

1. Pin/map the new framebuffer in SF and finish the GTI flush.
2. Wait for its exclusive reservation fence.  If the CPU was the producer,
   complete `dma_sync_sgtable_for_device()` before display ownership.  If
   Chrome9 3D was the producer, its cookie, PID00 PAUSE, and non-CR idle proof
   must already have retired.
3. While IGA1 is blanked, program pitch, the packed SF start address, and
   CR48 `[7:6]=01`; then program the remaining already live-proven mode and
   unblank.
4. Keep the mapping and GEM reference until the CRTC is disabled or a later
   framebuffer has latched.

For a page flip, pin the new framebuffer before the commit and retain the old
one through display completion.  The direct Windows-derived baseline is:

1. wait for the render dependency;
2. stream T11 A12=`12000000` to disarm auto-swap;
3. clear CR32 bit 0 for by-frame behavior;
4. update packed `0214` to the new SF base;
5. complete the DRM flip event on the vblank which observes the new state;
6. only after that event/commit cleanup may the old framebuffer's PTEs be
   invalidated and its pages unpinned.

The first version should use the normal atomic-helper prepare/cleanup and
vblank sequence rather than inventing a display fence.  The current driver has
no vblank IRQ support, so static enable is the only safe first oracle.  Do not
send an event immediately after the MMIO write and do not release the old SF
mapping merely because the preceding render fence retired.  IGA1 continues to
fetch the old frame asynchronously until the display latch point.

After vblank support exists, a kernel-only T11 discriminator is available.
The manual's exact SF substitution into the recovered IGA1 packet is:

```text
FE020420 0000041C 00110000 CC000000
10BBBBB9 110000HH 12000008 CC000099
```

where `HHBBBBB8` is the eight-byte-aligned SF byte address and the low `01b`
in A10 selects SF.  This follows directly from documented T11 bits but is not
a shipping captured vector: the proprietary template uses SL (`10BBBBB8`).
Keep it kernel-only and test it only after the direct static SF scanout works.

## First live oracle and failure containment

Use one fresh `PAGE_SIZE + 4096 * 768 + PAGE_SIZE` GEM BO, render the existing
high-visibility 1024x768 XRGB8888 pattern between its guard pages at offset
`PAGE_SIZE`, retire the normal 3D fence, and then enable the already-proven
fixed mode with:

```text
CR48[7:6] = 01b
base      = persistent_sf_node.start + PAGE_SIZE
pitch     = 4096
```

Do not flip, reuse, unmap, or CPU-read the BO while IGA1 is active.  Capture
CR0C/CR0D/CR34/CR48, MMIO `0214`, SR6A/B/C/F/7B, the PTE range, and visible
output.  Disable and restore IGA1 before invalidating any PTE.  Then verify the
two guard pages and run the existing render/2D regression.

A black display with healthy GPU retirement is a display-domain failure, not
permission to alter render packets.  Leave the mapping pinned, blank/restore
IGA1, and only then tear it down.  Any GTI-flush timeout or inability to restore
VGA ownership keeps the allocation and table live for reboot, matching the
render engine's existing quarantine rule.

The static oracle proves only IGA1 SF fetch.  Production zero-copy presentation
still requires vblank IRQ/event handling, old/new framebuffer lifetime tests,
and repeated double-buffer flips while rendering into the non-scanned BO.
