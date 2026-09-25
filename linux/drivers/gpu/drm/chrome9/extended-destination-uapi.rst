Extended fragment destination UAPI
==================================

The Chrome9 driver lives in a Git subrepository, while its installed UAPI
header is at ``include/uapi/drm/chrome9_drm.h`` in the surrounding kernel
tree.  The following definitions must be kept in that header and mirrored
in Mesa's ``include/drm-uapi/chrome9_drm.h``::

    #define DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED (1ULL << 53)
    #define DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED (1U << 2)
    #define DRM_CHROME9_CAP2_DESTINATION_OFFSET (1ULL << 22)

The flag applies only to the destination of
``DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM`` and of the fragment part of the
vertex program draws.  Without it, every BO retains the exact-size contract.
With it, the destination is a guarded object holding further images (mip
levels, cube faces): it must be page-aligned and at least as large as the
window plus the trailing guard page.  Only the object's first and last page
are guards; the kernel maps and synchronizes the whole object and flushes
only guard pages and ranges announced by CPU writes.  Depth and sampler BOs
still require their exact validated sizes.

With ``CAP2_DESTINATION_OFFSET`` (module parameter ``destination_offset``)
the former ``reserved0`` word is ``destination_offset``: the byte offset of
pixel (0, 0).  It is a multiple of 256 (A50 holds the base in 256-byte
units), at least ``PAGE_SIZE``, and ``destination_offset + pitch * height``
ends before the last page.  Zero selects ``PAGE_SIZE`` (level zero).  The
pitch stays ``ALIGN(width * cpp, 32)``.  A level whose offset is not
256-aligned but whole rows after a 256-byte boundary is drawn from that
boundary with a scissor on its rows.

A 2D texture of the same draw may name the destination handle (another
handle of the same object is still refused).  Its base level and every level
of its whole-level LOD range, ``[min_lod, max_lod]``, must lie outside the
rows ``[scissor.min_y, scissor.max_y)`` the draw can write; A30 clamps both
mip filters to that range.  This lets a draw build level L from level L - 1
of one object.  Jobs execute in order and every segment starts with a 3D
wait, so a level written by one ioctl is complete before the next samples
it.
