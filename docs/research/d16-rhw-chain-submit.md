# Bounded D16 RHW chain submit

## Purpose

The fixed-frame glmark2 Gouraud cat validates correctly with command `0x0f`,
but a five-frame trace contains exactly 75 command-`0x0f` ioctls: 15 per
frame. Each call repeats userspace/kernel entry, GEM lookup, two reservation
waits, two vmaps, SF mapping acquisition, DMA ownership transfer, fence
attachment, guard preparation and verification, and teardown. The packet
itself is already live-proven at its immutable 544-triangle limit.

Command `0x1c`, capability bit 47, removes only that repeated ownership work.
It does not widen the hardware packet, omit state from later packets, expose
native words, make submission asynchronous, or weaken the command-`0x0f`
fallback.

## UAPI contract

`drm_chrome9_draw_triangle_d16_rhw_chain` is a distinct 128-byte request with
the same version-one state fields as the run request. Its count is `1..16384`
and `vertices_ptr` addresses `triangle_count * 3` immutable
`drm_chrome9_vertex_d16_rhw` records. The maximum is enough for the complete
14,348-triangle cat asset before Draw's clipping/culling and bounds the copied
array to 983,040 bytes.

All fixed state, checked surface sizes, flags, blend, scissor, coordinates,
Z, finite positive RHW values, and nondegenerate triangles are validated by
the common batch validator. `vmemdup_array_user()` completes before GEM object
lookup or command construction, avoiding a nearly 1-MiB physically contiguous
allocation. Reserved fields remain zero-only. The handler
is fail-closed with `-EOPNOTSUPP` and capability bit 47 is hidden unless the
read-only module parameter `chrome9.rhw_chain=1` is supplied.

## Kernel lifetime

The chain deliberately reuses `chrome9_submit_depth_triangle()` rather than
introducing a second ownership implementation:

1. Look up and validate the exact guarded color and D16 objects.
2. Lock both reservations once, wait dependencies once, vmap once, and acquire
   both SF mappings once.
3. Prepare guards and transfer both objects to the device once.
4. Attach one fence to both WRITE reservations. It covers the whole chain.
5. For each chunk of at most 544 triangles, make a kernel-local request view,
   select the corresponding already-copied vertices, and invoke the unchanged
   command-`0x0f` packet builder.
6. Alternate `00010001`/`00020002` completion cookies for every packet. Wait
   for cookie, PAUSE, and non-CR engine idle before advancing. Reusing a cookie
   could falsely retire a newly published packet and is forbidden.
7. After the final packet, transfer both objects to the CPU once, verify every
   guard, release SF mappings and reservations once, and signal the one fence.

If PID10 publication or retirement fails, the existing path sets the fence
error, wedges the engine, and quarantines the job with all mappings. A packet
builder failure after an earlier packet is safe because each earlier packet
has already completed the full retirement wait; validation makes such a
failure an internal error rather than a userspace-controlled partial reject.

The complete state packet is intentionally rebuilt for each segment. “State
reuse” here means one immutable semantic state object and one resource
ownership transaction, not reliance on undocumented state persistence across
packet boundaries.

## Mesa policy and expected result

Mesa accumulates fixed-primary perspective D16 triangles to the chain maximum
only when capability bit 47 is present. Compiler-backed command `0x19` keeps
precedence, and kernels without bit 47 retain the 544-triangle command-`0x0f`
path unchanged. A state change still flushes the retained batch, preserving
Gallium ordering.

For the observed cat frame, the expected syscall count changes from 15 RHW
draw ioctls to one chain ioctl. The kernel still creates and synchronously
retires approximately 15 proven packets. This optimization therefore targets
ioctl, GEM/reservation, mapping, cache-ownership, and guard-scan overhead; it
does not claim native hardware TCL or eliminate per-packet GPU retirement.

## Tests before capability promotion

- Kernel module build with `CONFIG_DRM_CHROME9=m`.
- Mesa Chrome9 driver and screen-smoke compilation plus the offline shader
  corpus.
- Compile `tools/chrome9_rhw_run.c` with ABI size assertions.
- With the opt-in parameter, run counts 545, 8160, 14348, and 16384; verify
  alternating pass/reject depth cells and all guards.
- Reject zero, 16385, bad padding, invalid RHW, a degenerate triangle, object
  aliases, wrong exact sizes, and absent module opt-in.
- Run the complete Chrome9 smoke suite before and after the chain cases, check
  SWIOTLB remains at its baseline and MMIO engine status `0x040c` is zero.
- Re-run glmark2 built-in Gouraud-cat validation and a five-frame trace; require
  one command-`0x1c` ioctl per frame and no command-`0x0f` draw ioctl.

Do not advertise capability bit 47 by default until all live cases pass.

## Independent pre-live audit

The implementation at kernel `19ccbc4`, UAPI `c83ea14`, Mesa Chrome9
`62eff6c`, Mesa parent `91bd8c6`, and tools `3d16ae3` received a separate
source audit before deployment.  The audit found no blocker and made no code
change.  In particular it rechecked typed vertex-offset arithmetic at 16,384
triangles, the 64-KiB staging and 1-MiB ring bounds (including exact fit and
wrap), fresh-cookie retirement, one fence and DMA-ownership interval across
the whole chain, timeout quarantine, and Mesa's exact-capacity flush.  The
128-byte UAPI layout is asserted independently in kernel, Mesa and the direct
test tool.

The audit also examined a partial-success/internal-error case: an earlier
segment has completed cookie, PAUSE and idle retirement before the next
segment is constructed.  Releasing the shared mappings after a later
pre-publication failure is therefore safe; a failure after publication takes
the quarantine path instead.  This is a safety conclusion, not a live-silicon
performance result.  The live ladder listed above remains mandatory.
