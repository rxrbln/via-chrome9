Extended fragment destination UAPI
==================================

The Chrome9 driver lives in a Git subrepository, while its installed UAPI
header is at ``include/uapi/drm/chrome9_drm.h`` in the surrounding kernel
tree.  The following definitions must be kept in that header and mirrored
in Mesa's ``include/drm-uapi/chrome9_drm.h``::

    #define DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED (1ULL << 53)
    #define DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED (1U << 2)

The flag applies only to the destination of
``DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM``.  Without it, every BO retains
the exact-size contract.  With it, the destination must be page-aligned and
larger than the exact guarded L0 size.  The kernel maps and synchronizes its
entire size, but renders only L0 and checks the guard at both the end of the
L0 allocation and the end of the full BO.  Depth and sampler BOs still
require their exact validated sizes.  The capability bit gates Mesa's use
of this flag for mip-capable guarded render targets.
