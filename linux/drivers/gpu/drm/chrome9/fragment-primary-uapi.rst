Chrome9 fragment primary-color UAPI
====================================

The kernel UAPI header lives outside this nested driver repository, at
``include/uapi/drm/chrome9_drm.h``.  Keep the Mesa mirror byte-identical.
This driver revision requires these additions::

    #define DRM_CHROME9_CAP_FRAGMENT_PRIMARY (1ULL << 54)

    enum drm_chrome9_fragment_file {
        /* existing values 0..3 unchanged */
        DRM_CHROME9_FRAGMENT_FILE_PRIMARY = 4,
    };

PRIMARY accepts index zero only.  It names the normalized packed vertex D1
color, not a T coordinate.  Native PS ALU operands use class 1, ID 0;
VARYING remains class 3 and is allocated from the compact T-coordinate set.
PRIMARY never contributes to ``varying_count``, A90 direct-coordinate bits,
TAU address state, or external resource high-water.

The kernel compiler self-test matches the Windows ``MOV oC0,v0`` record
``01c00530 301c0053 00f04005 000d0820`` and the Linux production
``TEX r0,t0,s0; MUL oC0,r0,v0`` ALU/TAU/control tuple.  The first target
oracle was a single unblended 64x64 BGRA triangle, 1,176 exact-color pixels
(``ff4080c0``), zero exterior, and untouched guard pages.  This does not yet
qualify blended textured primary-color programs.
