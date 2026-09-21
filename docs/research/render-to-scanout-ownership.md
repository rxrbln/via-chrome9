# Chrome9 same-device render-to-scanout ownership

This note narrows the presentation contract to the intended architecture:
Chrome9 3D renders a native GEM BO and Chrome9 IGA1 scans out the **same BO on
the same PCI device** through one stable SF mapping.  PRIME/offload and a CPU
or 2D staging copy are not requirements for this path.

The important result of the binary audit is negative but precise: the shipping
VX900 page-flip path orders the 3D/2D producers before the display command, but
does not insert a CPU-cache ioctl, texture-cache clear, GTI page-table flush,
or an extra render-target-cache packet between producer and display.  A new
driver should preserve that device-to-device ordering and avoid inventing a
CPU ownership round trip.

## Shipping 7122 producer-to-flip sequence

Linux `via_chrome9_dri.so` selects `VT3353_Flip:0xeccc0` specifically for PCI
IDs 1122, 5122, and 7122 in `sfmFlip_inv:0xede76..0xedf09`.  The 7122 callback
calls `stmAddCRWaitChipIdle_invi` with both engine arguments true and emits:

```text
FE000000 00000001 00000000 CC000000
0000006C 00C00000 CCCCCCCC CCCCCCCC
```

It then appends the IGA1 automatic-flip packet:

```text
FE020420 0000041C 00110000 CC000000
10BBBBB8 110000HH 12000008 CC000099
```

`006c=00400000` is the 2D-only wait/order form, `00800000` is 3D-only, and
`00c00000` requests both.  The T11 address shown by the proprietary template
is SL (`A10[1:0]=00`); the separately documented SF oracle substitutes
`A10[1:0]=01` only after direct SF scanout is proven.

The higher OpenGL path corroborates the ownership boundary:

- `__glS3InvSwapBuffers:0x1f4480` queries the prior tracked event and, when
  necessary, kicks queued DMA and polls with bounded hang detection;
- its copy-present branch emits `sfmBlt`/`via3DBlit`, then `hwmSendEvent`, then
  `cmKickoffDMA` at `0x1f4684..0x1f4887`;
- its flip branch calls `sfmFlip` at `0x1f4955`, then exchanges front/back
  resource pointers at `0x1f495a..0x1f4978`.

There is no call to `__glDpFlushCache:0x1e9bf0` in the direct flip branch and
no FE texture-cache-clear group.  `sfmFlip_inv` dispatches directly to the
chip/display callback.  The callback itself contains the both-engine H0 wait
followed by T11 display state, not a CPU transition.

This is static evidence for this ordering:

```text
3D writes -> command-stream 3D/both-engine drain -> display-base arm
          -> vblank/base latch -> old-buffer release
```

It is not proof that the untested SF-display substitution works, nor that H0
alone is a Linux fence implementation.  The modern driver still needs its
bounded render retirement and vblank completion proofs.

## Four mechanisms which must remain separate

| mechanism | purpose | when required |
|---|---|---|
| render fence/cookie + PAUSE + idle | producer completion and RT write drain | before IGA may consume a freshly rendered BO |
| H0 `006c=00c00000` | vendor in-stream 2D/3D ordering before T11 | kernel-only automatic-flip oracle |
| SR6F bit 7 pulse | invalidate GTI translations after PTE changes | bind, rebind, or unbind; not every frame |
| DMA/CPU cache transition | ownership across a CPU access boundary | CPU upload/read/map, not device-to-device handoff |

The type-FE A00 bit-0 pulse is a fifth, unrelated mechanism: it invalidates the
3D texture cache before a render target is sampled as a texture.  IGA scanout
is not a texture consumer, and neither proprietary direct-flip path emits that
pulse.  Do not add it to KMS commits.

Likewise, an SR6F pulse says only that clients will observe new PTE contents.
It does not flush render-target pixels, wait for 3D, or prove a display latch.
With an unchanged persistent object-level SF mapping, no PTE publication occurs
at flip time and no per-frame SR6F pulse is justified.

## Preferred native-BO ownership state machine

Use one persistent object-level SF mapping for both render and display:

```text
UNBOUND
  | bind PTEs + bounded SR6F flush
  v
DEVICE_IDLE
  | 3D submission; destination reservation WRITE fence
  v
RENDERING
  | fence/cookie + PAUSE + work-idle retirement
  v
DEVICE_READY
  | atomic commit waits fence; arm IGA1 base
  v
DISPLAY_PENDING
  | vblank proves new base latched
  v
DISPLAY_ACTIVE
```

`DEVICE_READY -> DISPLAY_PENDING` is a same-device transition.  Keep the
mapping and the BO device-owned; do not call `dma_sync_sgtable_for_cpu()`, do
not CLFLUSH device-written lines, and do not remap/rewrite its PTEs.  The new
framebuffer mapping must be prepared before the atomic commit and the old
mapping remains held until the latch-completion event.

Rendering into `DISPLAY_ACTIVE` is deliberately unsupported initially.  IGA
can fetch while 3D writes, so neither reservation ordering nor cache state can
prevent visible tearing.  Double buffering is the first production contract:
render only the inactive BO, retire it, then flip.

On a flip from A to B:

1. B already has a complete persistent SF mapping and identical bit-25 PTE
   cache attributes for render and display.
2. Atomic prepare captures B's exclusive render fence without holding
   `submit_lock`, `vm_lock`, or VGA/display locks.
3. The helper waits for that fence.  The render retirement path has proved
   the command cookie, CR PAUSE, and relevant busy bits clear.
4. Program/arm the B display base while retaining display references to both A
   and B.
5. Complete the DRM page-flip event only at the vblank/base-latch proof.
6. Drop A's display reference after completion.  Unbind A only if it has no
   render, framebuffer-prepare, or display references.

The render fence is a producer-completion dependency.  The vblank event is a
consumer-latch dependency.  Neither substitutes for the other.

## Compatibility with the current synchronous render ioctl

The current bring-up ioctl conservatively returns every destination SG table
to CPU ownership after retirement with:

```text
dma_sync_sgtable_for_cpu(..., DMA_BIDIRECTIONAL)
```

That behavior was necessary for userspace pixel/guard verification, but it
means the BO is no longer in the preferred continuously device-owned state.
Until render retirement becomes fence-driven without an unconditional CPU
handoff, KMS must reacquire device ownership immediately before scanout:

```text
wait exclusive reservation fence
ensure no CPU writer/map transaction is active
clean/invalidate CPU aliases according to the proven WB policy
dma_sync_sgtable_for_device(..., DMA_BIDIRECTIONAL)
publish display base
```

This is a compatibility bridge caused by the current ioctl's CPU transition,
not an inherent 3D-to-display flush requirement.  Track ownership explicitly
per object so KMS does not issue a redundant sync-for-device on an object
which never left device ownership.

Never call sync-for-CPU merely as the prelude to sync-for-device.  On a
SWIOTLB system it can copy stale bounce contents over newer shmem contents;
the live D16 ownership experiment already demonstrated this failure mode.
Never CLFLUSH after GPU rendering while stale dirty CPU aliases may exist;
writeback can overwrite the completed frame.

## Stable address and alias rule

The first static oracle may use separate persistent and transient SF aliases
of the same PFNs because both use the same bit-25 attribute, but production
should converge on one object-level mapping:

```text
render A50 = mapping.node.start + image_offset
IGA1 base = mapping.node.start + fb->offsets[0]
```

For the guarded resource these offsets are both `PAGE_SIZE`.  Reusing one
mapping eliminates alias-tag uncertainty and per-job page-table churn.  It
also makes the absence of a per-flip SR6F pulse obvious.

Reject a framebuffer if any visible byte falls outside the mapped object, if
its offset is added twice, or if the render target and KMS framebuffer describe
different pitch/format/layout.  XRGB8888, linear, 4096-byte pitch, and the
fixed 1024x768 mode remain the only first-oracle combination.

## Failure-safe teardown

Display fetch is asynchronous.  A retired render fence permits presenting the
BO; it does not permit unbinding the currently scanned BO.  The safe release
sequence is:

1. disarm T11 automatic swapping;
2. blank/disable or restore IGA1 and prove it no longer fetches the mapping;
3. drop active/pending display references;
4. invalidate every PTE in the mapping and complete the bounded SR6F flush;
5. only then remove the SF node and release GEM/DMA backing.

If vblank/latch completion is lost, IGA restore fails, or PTE invalidation
times out, retain the mapping, PTEs, GEM reference, DMA state, and device/module
references in reboot-scoped quarantine.  Never make the render side free a
mapping still reachable by IGA merely because the rendering job retired.

## Minimal direct-scanout coherency oracle

Use two guarded native BOs A and B, each with one stable SF mapping.  Do not
map either from userspace after device ownership begins.

1. Render a red/green checker plus a monotonically increasing frame number
   into inactive B and retire the render fence.
2. Without CPU readback, PTE rewrite, SR6F pulse, texture pulse, or 2D copy,
   flip IGA1 from A to B.
3. At the first confirmed vblank, capture the visible output and packed base;
   verify the complete new pattern, not just one sampled pixel.
4. Repeat B->A for at least 1,000 flips while alternately rendering only the
   inactive buffer.
5. After full IGA disable/restore, return both to CPU ownership once, validate
   guards and frame numbers, then unbind.

Run one diagnostic comparison with the alias-based static oracle before
object-level mapping reuse.  If aliasing alone causes stale display content,
move both render and IGA to one address; do not add undocumented cache packets.
If one-address device-owned flips are stale despite complete producer/latch
proof, capture that as a silicon coherency result before testing any additional
flush hypothesis.
