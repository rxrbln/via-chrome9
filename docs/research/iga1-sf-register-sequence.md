# VX900 IGA1 SF register sequence and presentation oracle

This note is the code-ready register companion to
`zero-copy-kms-lifetime-design.md`.  It rechecks the supplied Windows 7 KMD,
the proprietary Xorg driver, the old Linux VIA sources, and the VX900 manual
against the current Chrome9 KMS staging driver.  It covers only same-device
Chrome9 GEM to Chrome9 IGA1 scanout.  PRIME/dma-buf is optional DRI3 sharing,
not part of the presentation architecture.

The inspected binaries are:

```text
VTGKModeDX32A.sys sha256 618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56
via_drv.so       sha256 5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787
```

## Independent executable evidence

The normal PCI-7122 IGA1 path in `VTGKModeDX32A.sys` is exact:

* `S3gSetVidPnSourceAddress` at `0x69e10` configures display interrupts through
  `0x767e0`, then dispatches source 0 to `0x79080`.
* `0x79580..0x795bc` writes `00110000` to MMIO `041c`, followed by
  `12000000` to MMIO `0420`.  This is T11 A12 with a zero payload: disable
  IGA1 automatic framebuffer swapping before a direct base update.
* `0x795e4..0x79633` reads CR32 and selects bit 0 according to the requested
  mode.  Bit 0 clear is flip by frame; bit 0 set is flip by scan line.
* The PCI-7122 normal branch at `0x79683..0x796de` shifts the byte address
  right once, saves that word address in the KMD shadow, reads `0214`, retains
  `0214 & e0000000`, inserts `(byte_address >> 1) & 1fffffff`, and writes the
  result to `0214`.
* The exceptional `0x79641..0x79681` branch which ORs `c0000000` into the
  address is gated on PCI 1122 plus KMD mode 4.  It is not a VX900 rule.

The VX900 manual names `0214[31]` IGA1 down-scaling flip,
`0214[30]` down-scaling line-flip enable, `0214[29]` reserved/RO, and
`0214[28:0]` the address shadow equivalent to CR48[4:0], CR34, CR0C, and CR0D.
Therefore preserving `[31:29]` is required and those bits must never be
derived from an SF byte address.  The source domain is not present in `0214`.
It remains selected separately in CR48[7:6].

The proprietary Xorg `VIASetStartingAddress` at
`via_drv.so:0x4cc10` independently shifts the IGA1 byte address right by one
at `0x4cd7a`.  Its direct indexed-register branch writes CR0C, CR0D, CR48, and
CR34 at `0x4cdec..0x4ce2b`.  That branch selects legacy SL; it corroborates the
address unit but is not an SF-domain example.  The manual supplies the domain
decode:

```text
CR48[7:6] = 00  physical SL
CR48[7:6] = 01  translated SF
CR48[7:6] = 1x  reserved
CR48[5]          reserved/RO, preserve
CR48[4:0]        IGA1 word-address bits [28:24]
```

The old `viafb` `via_set_primary_address()` uses the same low-register order
and updates only CR48[4:0].  The proprietary Linux DRI's T11 flip path clears
CR32 bit 0 for by-frame operation and emits the captured automatic-swap
packet documented in `display-flip-vblank.md`.  None of these sources performs
a second hidden address addition: Mesa's AddFB2 page offset must be added
exactly once to the persistent SF node.

## Exact static-enable image

Let `S` be the byte start of the persistent SF mapping and `O` the AddFB2
plane offset.  For the guarded Mesa front buffer, `O=0x1000`.  Validate
`B=S+O` as even and wholly inside the 64-MiB SF aperture, then define
`A=B>>1`.

While IGA1 is completely blanked/disabled, program:

```c
vga_io_wcrt(0x0d, A);
vga_io_wcrt(0x0c, A >> 8);
vga_io_wcrt(0x34, A >> 16);
chrome9_cr_mask(0x48, GENMASK(7, 6) | GENMASK(4, 0),
                BIT(6) | ((A >> 24) & GENMASK(4, 0)));
```

Writing CR48 last publishes the high address and switches SL to SF in one
indexed byte while the pipe cannot fetch.  `chrome9_cr_mask()` preserves the
reserved/RO bit 5.  Do not first clear CR48[7:6] as the current BAR2 staging
code does; that explicitly selects SL.  Do not write `0214` as a second copy
during initial mode setup.  Read it afterward only as a consistency snapshot:

```text
(readl(mmio + 0214) & 1fffffff) == (A & 1fffffff)
(vga_io_rcrt(48) & df) == (BIT(6) | ((A >> 24) & 1f)) modulo preserved bit5
```

The existing pitch programming is unchanged for the fixed oracle:
CR13=`00`, CR35[7:5]=`010b`, representing 4096 bytes in eight-byte units.

Before any base is published, the full BO PTE image must already be visible
and the SR6F GTI translation flush must have completed.  The display base is
the SF virtual byte address `B`; it is never a PFN, DMA address, CPU pointer,
BAR2 offset, or the GEM offset alone.

## Serialization with the command reader

MMIO `041c/0420` is the same transfer port used by the Chrome9 command reader,
not independent display storage.  The live observation that `0420` changes as
3D runs is consistent with this alias.  The T11 disarm must consequently be
serialized by `engine->submit_lock` after the incoming render fence has
retired and while the persistent reader is known to be safely at PAUSE:

```c
mutex_lock(&engine->submit_lock);
if (engine->stopping || engine->wedged || !engine->ready)
        fail;
writel(0x00110000, mmio + 0x041c); /* select T11 A12 */
writel(0x12000000, mmio + 0x0420); /* auto-swap enable=0 */
readl(mmio + 0x0400);              /* post both writes; value is diagnostic */
mutex_unlock(&engine->submit_lock);
```

Do not depend on `readl(0420)` returning `12000000`; its read semantics expose
changing transfer/reader state.  The read of another register in the same BAR
is a posting barrier, not a value assertion.  This serialization also prevents
a ring submit from changing transfer-space state between disarm and the base
write.  For an atomic direct flip, keep `submit_lock` through CR32 selection
and the `0214` write, but never acquire it in hard IRQ context.

## Producer and cache order

The Windows source-address function contains no 3D cache packet, CPU cache
flush, or SR6F operation between T11 disarm and its `0214` write.  Its caller
relies on WDDM scheduling/resource residency before invoking the display
callback.  This is evidence against inventing an undocumented display-cache
packet; it is not evidence that Linux may ignore its DMA ownership rules.

For a new Linux framebuffer the required order is:

1. install all bit-25 cached PTEs and complete the SR6F translation flush;
2. let `drm_gem_plane_helper_prepare_fb()` capture the incoming reservation
   fence, then let atomic helpers wait for it with no engine/display lock held;
3. call `dma_sync_sgtable_for_device(..., DMA_BIDIRECTIONAL)` because the
   current synchronous render path returns the shmem SG table to CPU ownership
   after retirement;
4. use a normal DMA/write barrier before MMIO publication;
5. serialize and disarm T11, select by-frame mode, then publish the base.

The GTI flush proves translations, the render fence proves producer
retirement, the DMA sync transfers memory ownership, and the display latch
proves presentation.  None substitutes for another.  A framebuffer whose
base is unchanged has no atomic update at which to perform step 3; direct
front-buffer rendering remains outside the first contract.  Render into the
inactive BO and then flip it.

## Exact direct-flip sequence

CR48 remains `01b` (SF) for the entire modeset.  One serialized flip from old
mapping to new mapping is:

```c
/* prepare_fb already pinned new; atomic helpers already waited its fence */
dma_sync_sgtable_for_device(dev, new->sgt, DMA_BIDIRECTIONAL);
dma_wmb();

mutex_lock(&engine->submit_lock);
/* reject stopping/wedged/not-ready and an existing pending flip */
writel(0x00110000, mmio + 0x041c);
writel(0x12000000, mmio + 0x0420);
readl(mmio + 0x0400);

/* VGA legacy-I/O lock is already held by the sleepable update path. */
chrome9_cr_mask(0x32, BIT(0), 0);       /* flip by frame */

old = readl(mmio + 0x0214);
packed = (old & 0xe0000000) |
         ((new_sf_byte_address >> 1) & 0x1fffffff);

/* Publish pending ownership/event before the write so IRQ cannot miss it. */
writel(packed, mmio + 0x0214);
readl(mmio + 0x0214);                  /* posting read, masked value check */
mutex_unlock(&engine->submit_lock);
```

Before the write, reject an odd address, an address outside 64 MiB, a mapping
which is not fully bound, a stale/absent producer fence result, or
`readl(0204)&BIT(1)` from a preceding update.  The old and new mappings both
retain display references.  Reading back the new `0214` value is not latch
completion.

The original conservative completion predicate was:

```text
a newly handled post-arm 0200.bit3 IGA1-vsync interrupt
and (readl(0204) & BIT(1)) == 0
and (readl(0214) & 1fffffff) == (packed & 1fffffff)
```

Only then would the driver send the DRM event, make `new` active, and defer
release of the old display reference.  Live VX900 testing subsequently found
that `0214` accepted the new shadow while RO `0204.bit1` stayed set over the
measured frame boundary and 100-ms follow-up.  This predicate is therefore
not qualified for successful completion.  Timeout, address mismatch, or
stopped IRQ cadence still quarantines both mappings until a confirmed full
CRTC disable.  The exact contradiction and next discriminators are in
[iga1-flip-pending-contradiction.md](iga1-flip-pending-contradiction.md).

## Exact IRQ enable and acknowledgement

`VTGKModeDX32A.sys:0x767e0..0x7691c` reads `0200`, sets global bit 31, and for
an enabled IGA1 ORs both bit 19 (enable) and bit 3 (clear stale RW1C status)
before writing `0200`.  Its ISR at `0x772c0..0x774ea` requires sampled bit 3,
then writes `(sample & ffff6e56) | 80000008` before it reads/reports the packed
IGA1 base.  The old upstream VIA driver independently names bit 19 enable and
bit 3 pending, but unsafely echoes the whole sample to acknowledge it.

Linux should narrow this to a control-only shadow:

```c
#define CHROME9_IRQ_RW_ENABLE_MASK  0xf7ff0800
#define CHROME9_IRQ_GLOBAL          BIT(31)
#define CHROME9_IRQ_IGA1_ENABLE     BIT(19)
#define CHROME9_IRQ_IGA1_PENDING    BIT(3)

spin_lock_irqsave(&irq_lock, flags);
irq_enable_shadow = readl(mmio + 0x0200) & CHROME9_IRQ_RW_ENABLE_MASK;
irq_enable_shadow |= CHROME9_IRQ_GLOBAL | CHROME9_IRQ_IGA1_ENABLE;
writel(irq_enable_shadow | CHROME9_IRQ_IGA1_PENDING, mmio + 0x0200);
readl(mmio + 0x0200);
spin_unlock_irqrestore(&irq_lock, flags);
```

The hard IRQ first samples `0200` once.  `ffffffff` or global clear is not a
device interrupt.  If bit 3 is set, acknowledge early with
`writel(irq_enable_shadow | BIT(3), 0200)`, flush-read `0200`, then call
`drm_crtc_handle_vblank()` once and test the pending flip.  Never copy sampled
RW1C status bits into the write and never take the sleepable engine/VGA locks
there.

On disable, clear bit 19 in the shadow, write `shadow | BIT(3)` to clear the
final pending edge, flush-read, and `synchronize_irq()`.  Preserve global bit
31 while any CR/DMA/display source still needs it.

## Xorg/GBM/AddFB2 path

For Xorg server 21.1.24, falling back from
`gbm_bo_create_with_modifiers()` to `gbm_bo_create()` does **not** by itself
select legacy AddFB.  `drmmode_bo_import()` chooses
`drmModeAddFB2WithModifiers()` whenever:

```text
DRM_CAP_ADDFB2_MODIFIERS != 0
and gbm_bo_get_modifier(front_bo) != DRM_FORMAT_MOD_INVALID
```

It does not consult `bo->used_modifiers` for that decision.  Chrome9 currently
advertises the capability because `mode_config.fb_modifiers_not_supported` is
unset.  Mesa's DRI image query asks Chrome9 `resource_get_handle()` for a KMS
handle, and the driver returns `modifier=LINEAR`, `stride=resource->stride`,
and `offset=resource->data_offset`.  The authoritative 2026-09-21 live Xorg
capture below proves that this path exported the guarded allocation and its
page offset; it did not fall back to a three-MiB offset-zero dumb buffer.

Legacy `drmModeAddFB()` cannot describe the page offset and must not be the
final direct-scanout path.  During a live Xorg run the quickest authoritative,
read-only discriminator is:

```sh
cat /sys/kernel/debug/dri/0/framebuffer
cat /sys/kernel/debug/dri/0/state
```

The kernel's `drm_framebuffer_print_info()` prints the exact active FB id,
format, modifier, pitch, offset, GEM size, and import status.  `GETFB2` is an
equally authoritative alternative after discovering the FB id.  Attaching
`strace` after Xorg startup cannot recover the already completed AddFB ioctl;
it is useful only when starting the server under tracing.  A capture made
after an earlier Xorg run had already exited showed Chrome9 with no active
framebuffer and only vkms fbcon FB 54 at offset zero; that was not evidence
about Xorg's earlier front buffer.  The later live capture, taken two seconds
after starting Xorg while `glxgears` was active, is authoritative:

```text
framebuffer[40]:
        allocated by = Xorg
        format=XR24 little-endian
        modifier=0x0
        size=1024x768
        pitch[0]=4096
        offset[0]=4096
        obj[0]: size=3153920 imported=no
```

The corresponding atomic state had plane 36 on CRTC 38 with FB 40 and both
`enable=1` and `active=1`.  Thus current Xorg uses one native Chrome9 GEM
object with exactly 770 pages; there is no second GPU and no PRIME/offload
scanout.  The direct path must map the complete object and add offset
`PAGE_SIZE` exactly once.

## Current kernel audit and patch boundary

The current header already has correct definitions for `0214`, the
`e0000000` keep mask, the `1fffffff` address mask, T11 A12 disarm, and CR32
by-frame selection.  Commit `433ee37` also permits AddFB2 offset `PAGE_SIZE`
and makes the BAR2 staging copy read pixels from that offset.  The remaining
direct path is not an isolated substitution in `chrome9_kms.c`:

* `struct chrome9_engine`, `sf_mm`, both locks, GART helpers, and quarantine
  ownership are private to `chrome9_engine.c`;
* the generic framebuffer has nowhere to retain a persistent SF node;
* the simple pipe has no custom `prepare_fb`/`cleanup_fb` hooks;
* current enable/update still CPU-copy to BAR2 and program CR48 as SL;
* no vblank IRQ/latch implementation exists.

The safe patch boundary is therefore:

```c
/* chrome9_engine.c, implemented beside the existing GART code */
int chrome9_scanout_mapping_get(struct chrome9_device *,
                                struct drm_gem_object *,
                                struct chrome9_sf_mapping **);
void chrome9_scanout_mapping_put_prepare(struct chrome9_sf_mapping *);
void chrome9_scanout_mapping_get_display(struct chrome9_sf_mapping *);
void chrome9_scanout_mapping_put_display_deferred(struct chrome9_sf_mapping *);
u32 chrome9_scanout_mapping_base(const struct chrome9_sf_mapping *);

/* chrome9_gem.c */
struct chrome9_framebuffer { struct drm_framebuffer base; ...mapping...; };

/* chrome9_kms.c */
chrome9_pipe_prepare_fb();
chrome9_pipe_cleanup_fb();
chrome9_iga1_program_sf(cdev, mapping_base + fb->offsets[0]);
```

First land only persistent bind/unbind and static SF enable; keep FB-changing
atomic commits rejected.  This is why no source edit accompanies this note:
placing ad-hoc GART access in KMS would bypass the proven lock/quarantine
contract, while replacing the BAR2 base without persistent PTE ownership could
make IGA1 fetch freed system pages.

## Bounded live oracle

Run each gate on a fresh boot and stop at the first mismatch:

1. With display untouched, prepare and abort one guarded XRGB8888 FB.  Require
   770 bit-25 PTEs, successful SR6F bind/invalidate flushes, read-back invalid
   entries, intact ring entries 0--15, and all render/2D regressions passing.
2. Keep one mapping bound but IGA disabled.  Render the high-visibility fixed
   pattern at object offset `0x1000`, wait its reservation fence, perform the
   DMA device sync, and capture both guards.
3. Save IGA state, fully blank/disable fetch, program CR13/35 and the exact
   indexed SF address image above, then unblank.  Capture CR0C/0D/34/48,
   `0200`, `0204`, `0214`, SR6A/B/C/F/7B, first/last PTE, and the monitor.
   Do not issue a live flip.
4. Require the complete expected image with no partial/stale rows.  On black or
   stale output, restore/disable IGA while retaining the mapping; do not change
   render packets or PTE attributes speculatively.
5. After confirmed restore, invalidate the mapping, verify both guards and
   invalid PTE readback, then rerun 3D, 2D, D16, texture, and blend tests.
6. Only after static SF scanout passes, enable read-only vblank counting and
   prove bit-3 cadence.  Then bind two conspicuous BOs, alternate direct
   `0214` writes while retaining both for the full test, and record arm
   sequence/`0200`/`0204`/`0214`/event for every flip.
7. Enable post-latch old-node release only after repeated success of the exact
   completion predicate.  A software-suppressed completion must leave both
   mappings pinned and must not send a successful event.

The intended manual behavior was `0204.bit1` transiently one, then zero by a
bounded post-arm vblank.  That did not occur in the first live direct-flip
test, although ordinary CR PAUSE/retired producer state and full restore were
clean.  No display step may leave `0400` newly busy, set `040c`, alter the
command ring, or modify guard pages.
