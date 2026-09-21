# Direct SF scanout implementation review and live gate

This began as the post-implementation review for Chrome9 kernel commits
`2166397` and `6193bb5`: one native Chrome9 GEM object is rendered through SF
and IGA1 fetches it through a persistent SF alias.  Commit `bf2d4a3` now adds
the first fail-closed direct base update with a polled blank/latch proof.  It
still has no vblank IRQ or DRM vblank counter.

The reviewed `W=1` build produced `chrome9.ko` SHA-256
`06eed45e3755dd017329ab1ded2b098ff665c78bd62151173782df8e67fbd78d`.
Strict `checkpatch.pl` reported zero errors, warnings, or checks.  This artifact
was not deployed by the implementation agent.

## Implemented ownership and lock proof

The live Xorg target is framebuffer 40, native and non-imported, XR24,
1024x768, pitch 4096, offset 4096, object size 3153920.  The implementation
maps all 770 object pages and programs:

```text
node.start                         persistent SF allocation start
visible base                      node.start + 0x1000
IGA1 word address                 visible base >> 1
CR48[7:6]                         01b (translated SF)
```

The private framebuffer's `pin_count` counts successful atomic-state prepares
plus one independent active-display reference.  A mapping owns its own GEM
reference, PTE image, `drm_mm_node`, and the shmem-cached SG table lifetime.
`drm_gem_shmem_get_pages_sgt()` has no per-call matching put: shmem releases
the cached DMA mapping with the GEM object, so the mapping's GEM reference is
the required lifetime pin.

All allocator and PTE changes take locks in this order:

```text
framebuffer mapping mutex (released before an unpin)
  -> engine submit_lock
     -> engine vm_lock
        -> VGA legacy-I/O arbitration inside chrome9_gart_set()
```

Display publication takes `submit_lock`, then the KMS state mutex, then VGA
legacy-I/O arbitration.  It never holds the framebuffer mutex.  Render jobs
already take reservation locks before `submit_lock`; neither framebuffer
prepare nor cleanup holds a reservation lock while taking `submit_lock`.
There is no reverse edge in the reviewed paths.

Atomic prepare first captures the incoming explicit or reservation write fence,
then establishes the persistent mapping.  The generic commit tail waits that
fence before `pipe_enable`.  Enable performs the DMA device sync, acquires an
independent display pin and framebuffer reference, takes `submit_lock`, disarms
T11, and publishes the SF base while IGA1 is blank.  The display ownership is
transferred to `kms->active_fb` only after programming succeeds.

Confirmed disable/restore clears `active_fb`, releases `submit_lock`, and only
then drops the display pin/reference.  If command-reader serialization, VGA
ownership, or restore fails, the active reference remains.  Atomic cleanup can
then drop only its prepared-state pin; it cannot invalidate the mapping still
reachable by IGA1.  Engine teardown refuses GART restoration and `drm_mm`
takedown while any normal or quarantined scanout mapping remains.

Render retirement temporarily returns objects to CPU ownership for the
existing guard verification.  Before signaling the render fence it searches
the persistent scanout list by `drm_gem_object` identity and immediately calls
`dma_sync_sgtable_for_device()` again.  The same bridge covers destination,
depth, and second-texture objects and the multi-object prepublication-error
CPU-sync path.  Therefore an unchanged direct front buffer is not left in CPU
ownership after a Chrome9 render ioctl.

The remaining non-memory-corrupting limitation is the atomic callback API:
`enable` and `disable` are void.  A hardware/VGA failure after state swap can
leave DRM software state different from hardware.  The implementation fails
closed by retaining display ownership; recovery is a later confirmed restore
or reboot, never PTE invalidation.  The later `bf2d4a3` path adds a synchronous
direct base flip and sends the DRM event after its polled latch proof.  There
is still no IRQ-backed DRM vblank counter.

## Measured static-SF result

The first live direct gate passed on 2026-09-21 with kernel commit `6193bb5`
and `staging_scanout=N`.  Xorg's active framebuffer was exactly the expected
native object:

```text
FB 40, XR24, 1024x768, pitch 4096, offset 4096
object size 3153920, imported=no
```

The kernel logged `enabled at SF 0x11000`.  Passive MMIO readback was
`0214=00008800`, `0204=00000c04` with bit 1 clear, and engine error
`040c=00000000`.  After the bounded render client had exited and Xorg was idle,
the indexed VGA snapshot returned CR0C=`88`, CR0D=`00`, CR34=`00`, CR48=`40`.
Thus both independent reconstructions agree exactly:

```text
A = 0x00008800
B = A << 1 = 0x00011000
CR48[7:6] = 01b (SF)
0214[28:0] = A
```

A bounded `glxgears` run rendered at about 20.2 FPS without a fault or engine
error.  Clean Xorg shutdown logged the confirmed IGA1 restore and debugfs then
showed no framebuffer.  This proves the complete 770-page persistent mapping,
the single addition of the 4096-byte plane offset, translated-SF display fetch,
front-buffer render retirement/resync, and clean static ownership release on
the tested path.  It does not yet prove vblank IRQ cadence or direct flips.

## Exact first live gate without a planned reboot

Keep SSH and a text log open for the whole run.  Do not use
`staging_scanout=1`; direct SF is the default.  Stop immediately on a critical
log, a new engine error, or an address mismatch.

1. Before starting Xorg, record the loaded module hash and parameters, the
   current DRM state, and the read-only display/VGA snapshots.  Engine status
   must be retired/PAUSE with `040c=0`.
2. Start Xorg only far enough to create and activate the single fixed front
   buffer.  Require the kernel log line `output enabled at SF 0x...`, never
   `BAR2`.  Record that hexadecimal byte address as `B`.
3. While Xorg is alive, require `/sys/kernel/debug/dri/0/framebuffer` and
   `/sys/kernel/debug/dri/0/state` to show the same active native FB:
   XR24, 1024x768, pitch 4096, offset 4096, object size 3153920,
   `imported=no`, active/enable both one.
4. Quiesce every userspace render client before running
   `chrome9_vga_snapshot`, then reconstruct the programmed word address:

   ```text
   A = CR0D | (CR0C << 8) | (CR34 << 16) | ((CR48 & 0x1f) << 24)
   B_read = A << 1
   ```

   Require `B_read == B`, `(CR48 & 0xc0) == 0x40`, and preserved CR48 bit 5.
   Offset 4096 must be present exactly once: the PTE index is
   `(B - 4096) >> 12`, while IGA1 receives `B`.
5. Run `chrome9_display_snapshot` read-only.  Require
   `(0214 & 1fffffff) == (B >> 1) & 1fffffff` and `0204.bit1 == 0` once the
   static mode has latched.  `041c/0420` are transfer-port aliases and their
   readback values are not T11 payload assertions.  `0200.bit1` is only the
   instantaneous blank level; it is not a completion edge.
6. Display one conspicuous full-screen pattern, then run a bounded existing
   Gallium render into the same front buffer.  Require a complete image change,
   normal fence retirement, `040c=0`, and no partial/stale rows.  This is the
   transient-alias plus retirement-resync discriminator.
7. Terminate Xorg normally.  Require `restored pre-KMS IGA1 VGA state`, no
   `mapping retained`, no `unsafe engine teardown`, and no new engine/GART
   timeout.  Only after the restore log may the old SF PTEs disappear.
8. With IGA1 restored, run the existing Gallium/D16/texture/blend and 2D smoke
   suite.  If clean, `engine_quiesce=1` is the only supported path toward a
   module unload; never force-remove the module.

The read-only commands used for steps 3--5 are:

```sh
cat /sys/kernel/debug/dri/0/framebuffer
cat /sys/kernel/debug/dri/0/state
/root/tmp/chrome9/chrome9_vga_snapshot
/root/tmp/chrome9/chrome9_display_snapshot \
  /sys/bus/pci/devices/0000:00:01.0/resource1
```

The display snapshot is passive MMIO.  Despite its name, the VGA snapshot is
an indexed-register transaction: it writes selector ports and the attribute
enable value, then restores the selectors.  It has no vgaarb lock and must not
run concurrently with rendering, a modeset, or any GART update.  The safe
first discriminator while Xorg is active is therefore the kernel's `SF` log
plus passive `0214/0204`; capture CR48 only in a deliberately quiesced window.

If output is black/stale but SSH remains, stop Xorg once and inspect logs and
registers.  A normal confirmed restore permits continued software diagnosis
without reboot.  Any `SF scanout ... retained for reboot`, GART flush timeout,
failed final IGA restore, nonzero `040c`, or newly busy engine is a hard stop:
do not unbind, unload, restart Xorg, or run another GPU job.  Retention is the
intentional containment boundary.

## Vblank IRQ integration after the static oracle

The modern DRM integration should use the existing exact register contract,
not the old DRI1 whole-status acknowledgement.

Initialization, before `drm_dev_register()`:

1. Call `drm_vblank_init(drm, 1)` after the simple pipe exists.
2. Initialize a spinlock and save `readl(0200) & f7ff0800` as the documented
   control-only baseline.  Clear owned bit 19, acknowledge stale bit 3 with
   `shadow | BIT(3)`, and flush-read.  Do not echo sampled status bits.
3. Install `request_irq(pdev->irq, ..., IRQF_SHARED, "chrome9", cdev)`.
   Unwind it before engine/MMIO teardown.
4. `pipe_enable_vblank` sets bits 31 and 19 in the shadow and writes
   `shadow | BIT(3)`; `pipe_disable_vblank` clears bit 19, acknowledges bit 3,
   and flush-reads.  Preserve global bit 31 whenever another enabled source
   remains.  Restore the saved control baseline at final teardown.
5. Call `drm_crtc_vblank_on()` only after the fixed pipe is running and
   `drm_crtc_vblank_off()` before the confirmed hardware restore.

The shared handler samples `0200` once and returns `IRQ_NONE` for
`ffffffff`, global bit clear, owned bit 19 disabled, or bit 3 clear.  For an
owned edge it writes `irq_enable_shadow | BIT(3)`, flush-reads, calls
`drm_crtc_handle_vblank()` exactly once, and then checks any pending direct
flip.  It never takes `submit_lock`, the VGA lock, or `vm_lock`.

## Page-flip implementation boundary and `bf2d4a3`

The simple-pipe helper has no atomic-flush callback and does not consume a
driver's page-flip event automatically.  A Chrome9 direct flip therefore needs
an explicit pending-flip state and must detach `crtc_state->event` in
`pipe_update`; `drm_atomic_helper_fake_vblank()` must never see that event and
`drm_atomic_helper_commit_hw_done()` must not find it still attached.

The 7.2 atomic helper already supplies the required one-flip sequencing when
the driver retains that event until the hardware latch proof.  Every active
commit gets an event in `drm_atomic_helper_setup_commit()`, including an
internal completion-only event when userspace did not request one.  State swap
inserts its `drm_crtc_commit` in the CRTC commit list before another caller can
swap.  A second nonblocking commit sees incomplete `flip_done` in
`stall_checks()` and returns `-EBUSY`; a blocking commit reaches
`drm_atomic_helper_wait_for_dependencies()` and waits for the previous event.
Therefore no custom `atomic_commit` wrapper or separate pre-swap reservation
is needed.  Keep a defensive runtime `pending != NULL` rejection, because
reaching it would reveal a helper/lifetime bug rather than authorize a second
hardware update.

One pending record owns:

```text
old active framebuffer/display pin
new framebuffer/display pin
expected packed 0214 low 29 bits
arm DRM-vblank sequence
optional drm_pending_vblank_event
one vblank reference
bounded post-arm IRQ count
```

Before enabling flips, add CR32 to the saved/restored CRTC register set and
clear CR32 bit 0 while IGA1 is still blank in the full enable path.  The
current static driver neither saves nor writes CR32.  Part I and both vendor
paths require bit 0 clear for by-frame base changes.  Do this once under the
existing VGA lock; ordinary `pipe_update` must not perform indexed-VGA I/O.
The quiescent-only `chrome9_vga_snapshot` now includes CR32 so the pre-change
and programmed values can be captured without guessing.

After the incoming render fence wait, `pipe_update` device-syncs the new
mapping, takes its independent display reference, and under `submit_lock`
disarms T11, verifies that CR32 was configured for by-frame updates, rejects
`0204.bit1` already pending, and publishes the pending record before writing
packed `0214`.  It preserves
`0214[31:29]` and inserts `(new_base >> 1) & 1fffffff`, then readbacks `0214`
only as a posting/address check.

The hard IRQ completes only on a newly handled post-arm bit-3 edge with
`0204.bit1 == 0` and matching `0214` low 29 bits.  It sends the event under
the DRM event lock and queues sleepable work.  Deferred work transfers
`active_fb` to the new framebuffer, drops the old display pin/reference and
the flip vblank reference, then clears the pending record.  Delivery of the
retained event completes the helper's `flip_done` and admits the next commit.
Generic atomic cleanup may drop prepared-state references earlier; independent
display references keep both PTE images alive through the hardware latch.

A first-edge miss keeps both mappings and tries a bounded later real vblank.
Timeout, address mismatch, stopped IRQ cadence, or disable failure sends no
successful event, blocks further flips, and retains both mappings until a
confirmed complete IGA1 restore.  This fail-closed rule is identical to the
static active-display ownership fixed by commit `6193bb5`.

Commit `bf2d4a3` implements the same ownership rule synchronously while the
IRQ path remains unproven.  It saves/restores CR32, clears bit 0 while IGA1 is
blanked, rejects async flips, syncs and independently display-pins the new BO,
disarms T11 under `submit_lock`, and writes packed `0214`.  Completion waits
for a strictly post-arm `0200.bit1` blank-level transition, then requires
`0204.bit1=0` and the exact masked `0214` value.  The level is used only as a
bounded frame-boundary poll; it is not counted as a vblank interrupt.

Success transfers `active_fb`, sends the retained event, and then releases the
old display pin.  A failure before base publication retains the event and
blocks later flips.  A failure after publication additionally retains the new
display pin beside the unchanged old active pin because either mapping may be
fetched.  Confirmed full restore releases the retained ownership.  Strict
checkpatch and `W=1` passed; the reviewed module SHA-256 is
`b4ee5b1aff68a095adaaea3fe6d7a9618160bf5ba891211bf3df0c7b57d2127b`.

This primitive does not imply that a 300x300 window swap changes the KMS FB.
The X server ordinarily uses a glamor COPY into the unchanged full-screen
scanout pixmap.  The exact path split and live discriminators are in
[xorg-window-present-copy.md](xorg-window-present-copy.md).
