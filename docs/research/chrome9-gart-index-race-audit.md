# Chrome9 GART bind-cleanup EIO audit

Status: static source audit after the 2026-09-24 eight-second `glxgears`
failure.  The indexed-register race below is conclusive.  The existing log is
not sufficient to prove that the race, rather than a genuine GTI completion
failure, caused that particular run.

## Observed boundary

The run first rendered 479 frames in five seconds and later returned `EIO`
from legacy `DRAW_TRIANGLE_D16_RHW_BATCH` work after 178,276 triangles.  The
only kernel diagnostic was:

```text
SF draw-cache bind cleanup failed; mapping retained for reboot
```

That line is emitted only by
`chrome9_draw_mapping_acquire_locked()` at `chrome9_engine.c:3047-3067`.
The new mapping's first `chrome9_gart_set()` failed, and the immediate
invalidation `chrome9_gart_set(..., NULL, page_count)` failed as well.  The
driver then keeps the node, PTE image, GEM reference, and module/device holds
and wedges the engine.  That quarantine is the safe response; this was not a
post-publication command-ring cleanup or cmd22 failure.

The message does not preserve either errno or the failing GART phase.  After
range validation, `chrome9_gart_set()` can fail at VGA arbitration, the first
`SR6F.bit7` pre-wait, or the post-pulse wait inside `chrome9_gart_flush()`.
The same collapsed message is therefore not proof that a command packet,
PRIMARY source, PTE value, or page-table size was wrong.

## Confirmed same-device sequencer-index race

VX900 sequencer registers use one shared index/data latch.  The GART helpers
access its MMIO mirror at `83c4/83c5`:

```text
chrome9_engine.c:404-412  write SEQ_INDEX=6f once, then poll only SEQ_DATA
chrome9_engine.c:414-426  read/OR/write SEQ_DATA, then poll only SEQ_DATA
chrome9_engine.c:2706-2743 save index, access SR6C/SR6F, restore index
```

No driver-wide indexed-register lock covers that sequence.  `engine->vm_lock`
serializes only GART callers.  `vga_get_uninterruptible()` is arbitration
between conflicting VGA devices, not a mutex between threads using the same
device: `drivers/pci/vgaarb.c:235-240` jumps directly to the nested lock-count
path when the device already owns the resource, and the API explicitly says
that same-owner nested calls succeed (`vgaarb.c:430-432`).

The DVP1 DDC implementation is an independent same-device caller:

```text
chrome9_kms.c:113-147  each I2C edge/read selects SR31
chrome9_kms.c:149-169  holds kms->lock and VGA arbitration for the transfer
chrome9_kms.c:630-645  drm_edid_read() enters that bit-banged transfer
```

It does not take `engine->submit_lock` or `engine->vm_lock`.  Consequently an
EDID probe can replace index `6f` with `31` while the GART thread is polling
data.  There are several concrete corruptions:

1. the GART pre- or post-wait reads `SR31.bit7` instead of `SR6F.bit7`;
2. the flush read/modify/write can set bit 7 in SR31 rather than request a GTI
   flush;
3. an interleave in `chrome9_seq_read(cdev, 0x6c)` can save SR31 as `saved_6c`
   and later write the wrong value into the GART base/protection register;
4. restoring the old index does not repair any wrongly addressed data write.

A bit-banged EDID transaction can overlap both the initial 100-ms operation
and its immediate 100-ms cleanup retry.  This is a real race whether or not it
was the cause of the captured run.  Modeset/flip sequencer access is less
exposed because `chrome9_engine_display_begin()` holds `submit_lock`, but DDC
does not enter that ownership domain.

The race was introduced when DVP1 DDC support was added in kernel commit
`2fb60e4`; the GART single-index poll predates it.  Kernel PRIMARY commit
`b3e3ce6` changes only the fragment compiler, its self-test, and capability
advertisement.  It does not change GART, mapping-cache, KMS, locking, or
cleanup code.  Mesa PRIMARY changes can alter workload selection and timing,
but there is no direct state or lifetime linkage to this failure.

## Vendor sequence comparison

The proprietary Linux 5.75 mapper clears `SR6C.bit7`, selects SR6F, spins
until bit 7 is clear, writes every PTE, writes `old_SR6F | 80`, and restores
`SR6C.bit7` immediately (`via_chrome9_dma.c:432-454`).  Initialization uses
the same pre-wait/single-pulse/no-post-wait pattern at lines 283-299.  It has
neither a deadline nor same-device indexed-register serialization, so it is
not a model for modern error handling.

Windows `VTGKModeDX32A.sys` reads SR6F through the indexed helper at analysis
`0x1ad10`, ORs bit 7 into the saved byte, updates the PTE/residency arrays, and
writes SR6F through `0x1adc0`.  The map path is
`0x2ddf0..0x2e0ca`; unmap is `0x2db10..0x2ddd7`.  Depending on a device flag,
it writes the same flush byte once or repeats it nine times.  No SR6F-clear
poll appears in those map/unmap paths.  The read helper retries indexed reads
up to three times until the returned index byte matches the request
(`0x1ad10..0x1adaa`), which also shows that indexed access was treated as a
fallible transaction.  Neither vendor family proves that Linux should remove
its bounded post-wait.

The current driver is deliberately stricter:

```text
wait SR6F clear
clear SR6C write protect
write and read back PTEs; wmb
wait SR6F clear again
set SR6F.bit7
wait SR6F clear
restore SR6C and the prior sequencer index
```

If the final wait genuinely times out, the immediate cleanup begins by
waiting on the same still-set bit and will also time out without rewriting
PTEs.  Retaining the mapping is therefore correct.

## Required discriminator and repair boundary

Before attributing the captured EIO, record for both the bind and cleanup:

- `prewait` versus `postwait` phase and original errno;
- current SEQ index readback, SR6F, SR6C, PTE range, and page count;
- whether a DDC transfer is active;
- CR status and engine-work status, to separate translation-only failure from
  a command-engine failure.

The correctness repair is a device-wide sequencer-index mutex shared by every
SEQ index/data user, including the complete DDC transfer and the complete
GART save/unprotect/PTE/flush/reprotect/restore transaction.  Re-selecting
SR6F on every poll iteration is useful defensive validation, but cannot by
itself make the index-plus-data write atomic.  Preserve the bounded pre- and
post-waits and the current quarantine policy.

