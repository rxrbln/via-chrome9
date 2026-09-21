# Direct SF scanout implementation review and live gate

This is the post-implementation review for Chrome9 kernel commits `2166397`
and `6193bb5`.  It covers static same-device scanout only: one native Chrome9
GEM object is rendered through SF and IGA1 fetches it through a persistent SF
alias.  Page flips remain rejected until the separate IRQ/latch milestone is
implemented and proven.

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
or reboot, never PTE invalidation.  There is still no direct page flip or DRM
vblank event support.

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
4. Run `chrome9_vga_snapshot` and reconstruct the programmed word address:

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

## Page-flip implementation boundary

The simple-pipe helper has no atomic-flush callback and does not consume a
driver's page-flip event automatically.  A Chrome9 direct flip therefore needs
an explicit pending-flip state and must detach `crtc_state->event` in
`pipe_update`; `drm_atomic_helper_fake_vblank()` must never see that event.

Use a commit-time atomic reservation, not only a runtime check in
`pipe_update`.  Two nonblocking atomic requests can both pass `atomic_check`
before the first worker programs hardware.  A small wrapper around
`drm_atomic_helper_commit()` should atomically reserve the one flip slot before
the generic prepare/swap path and release it on any synchronous failure.  The
slot remains reserved until IRQ/deferred completion or confirmed CRTC disable.

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

After the incoming render fence wait, `pipe_update` device-syncs the new
mapping, takes its independent display reference, and under `submit_lock`
disarms T11, clears CR32 bit 0, rejects `0204.bit1` already pending, and
publishes the pending record before writing packed `0214`.  It preserves
`0214[31:29]` and inserts `(new_base >> 1) & 1fffffff`, then readbacks `0214`
only as a posting/address check.

The hard IRQ completes only on a newly handled post-arm bit-3 edge with
`0204.bit1 == 0` and matching `0214` low 29 bits.  It sends the event under
the DRM event lock and queues sleepable work.  Deferred work transfers
`active_fb` to the new framebuffer, drops the old display pin/reference and
the flip vblank reference, then releases the atomic flip slot.  Generic
atomic cleanup may drop prepared-state references earlier; independent
display references keep both PTE images alive through the hardware latch.

A first-edge miss keeps both mappings and tries a bounded later real vblank.
Timeout, address mismatch, stopped IRQ cadence, or disable failure sends no
successful event, blocks further flips, and retains both mappings until a
confirmed complete IGA1 restore.  This fail-closed rule is identical to the
static active-display ownership fixed by commit `6193bb5`.
