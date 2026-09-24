Chrome9 VX900 IGA1 ARGB cursor
===============================

The IGA1 hardware-icon cursor is opt-in with ``cursor_sf=1``.  The default is
off until its SF image-address domain and channel order pass a bounded silicon
oracle.  It does not enable IGA2 cursor hardware.

The cursor plane accepts linear ARGB8888 images up to 64 by 64 pixels.  It
copies each *new framebuffer* into the inactive of two permanently SF-pinned
64-by-64 images, synchronizes CPU writes for the display fetcher, hides the
old cursor, waits for the next IGA1 blank, changes the image base, then shows
the new cursor.  The two private mappings stay pinned through KMS teardown;
they are released only after successful restoration of the saved display
state.  Movement-only commits change position and clipped origin without
copying image data or waiting for a blank.

The legacy cursor ioctl's hotspot is metadata: CRTC X/Y already describe the
image's top-left corner.  MMIO 02fc is used only for clipping when X or Y is
negative; it is not the hotspot.  Userspace changing pixels in place within
the *same* framebuffer must submit a new framebuffer for this shadow-copy
path to refresh the image.  An explicit DIRTYFB refresh is future work.

``tools/chrome9_cursor_sf_oracle.c`` is the bounded DRM-master test.  It never
programs a primary framebuffer or a mode.  It shows an ARGB pattern, moves it
through positive and clipped-negative positions, switches to a second image,
then hides it.  Only run it with an active IGA1 CRTC and no X server holding
DRM master.  Compare 02f0/02f4/02f8/02fc readback, the external display, and
the immediate kernel log; retain the known-good module for rollback.  Any
timeout or register mismatch is a failed qualification, not permission to
turn on ``cursor_sf`` by default.

The first 2026-09-24 VX900 run passed the register oracle under an existing
1024-by-768 IGA1 fbdev mode: image A at (100,100) produced control
``36000005``, SF base ``20014000``, position ``00640064``, origin zero;
moving A to (-8,-12) kept that base and produced position zero, origin
``0008000c``.  Image B changed the base to ``20010000`` at (180,120), and
hide cleared bits 2 and 0 to ``36000000``.  Six ``-ENOSPC`` SF-pin messages
at kernel times 9263.757--9263.795 came from the *old* module while X was
shutting down, before the opt-in module initialized at 9264.068.  None
recurred during the cursor test or after known-good X restoration at
9270--9271; there was no new fault, timeout, or quarantine record.  This
proves programming/readback and safe handoff, not
that the image's RGB/alpha pixels were visibly fetched: the available KVM
capture does not support this output mode.  Do not enable the opt-in by
default on register evidence alone.

The opt-in Xorg modesetting/glamor run on the same date showed hardware
cursor visibility enabled in 02f0 readback.  Movement from pointer
(160,150) to (300,250) changed only 02f8 by (+140,+100).  Changing the X
root cursor from
crosshair to hand2 left the *final* SF base at ``20014000``.  That alone was
ambiguous: two image commits can toggle the private slots twice.  A second,
temporary diagnostic build traced the first nine cursor atomic updates,
framebuffer IDs, upload counts, and CRC32 of the packed source and active
private image.  Crosshair CRC ``990e6ecc`` changed to hand2 CRC
``a6d49d9f`` after ``XDefineCursor``.  The later ``XUndefineCursor``
restored the default cursor with CRC ``0671ec7f``.  Each shape change
uploaded the new image twice, switching slots 1->0->1, with source
and active-image CRCs equal at every step.  Pure position commits used the
same framebuffer, uploaded nothing, and retained equal CRCs.  Therefore the
unchanged final base was valid double-buffer parity, not a stale cursor.
The diagnostic instrumentation was removed from the product source after
this test.  The exact trace is archived at
``artifacts/2026-09-24/cursor-sf/x-cursor-diagnostic-20260924-091438.log``
in the workspace and at
``/root/tmp/chrome9-live/cursor-sf-oracle-20260924/run-20260924-091438``
on the target.  These checks prove the X cursor path updates the private
pixels, but do not externally prove displayed RGB/alpha at 1024-by-768.

After this qualification, the committed product module (Build ID
``3d0ce5f8c16dbfb76ade68afa772cc7335c00d9c``) was loaded on the test
target with ``cursor_sf=1`` and Xorg modesetting/glamor left running.  Its
bounded X cursor test again passed movement/control readback; X :0 and
``renderD128`` remained available, and no new GPU timeout, fault, or
quarantine appeared in the handoff log.  The activation trace is archived at
``artifacts/2026-09-24/cursor-sf/x-cursor-product-activation-20260924-092314.log``.
The opt-in is a live-session choice; the module parameter still defaults to
off.  To restore the exact known-good module/X on that target, hold the shared
GPU lock while running the staged, hash-checked rollback script::

  flock -x /tmp/chrome9-vx900-gpu.lock ssh root@192.168.2.60 \
    'bash /root/tmp/chrome9-live/cursor-sf-oracle-20260924/chrome9_cursor_sf_rollback.sh'

Do not force-unbind the GPU if quiesce or unload refuses the transition.
