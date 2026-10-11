<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# chrome9

Linux DRM kernel driver and Mesa Gallium driver for the VIA Chrome9 HD
graphics in the VX900 system processor (PCI `1106:7122`).

The kernel driver provides KMS for VGA and HDMI on IGA1 with fbdev emulation
and a hardware cursor, GEM objects in GART and local frame buffer memory,
and an asynchronous command ring with fences. It also has a validated 3D
submission UAPI for fragment programs and hardware TCL vertex programs. The
Mesa driver implements OpenGL on top of it: vertex and fragment programs,
2D, cube and DXTn textures, mip chains, depth and stencil, occlusion queries
and native points, lines, strips and fans on the hardware, with llvmpipe for
the features the silicon lacks. ioquake3 (GL1 and GL2 renderers), glxgears
and the glmark2 scenes render on hardware. Per-feature status:
[docs/research/chrome9-hw-feature-status.md](docs/research/chrome9-hw-feature-status.md).

## Layout

| path | contents |
|---|---|
| `linux/drivers/gpu/drm/chrome9/` | DRM driver (GPL-2.0-only) |
| `linux/include/uapi/drm/chrome9_drm.h` | kernel UAPI |
| `mesa/src/gallium/drivers/chrome9/` | Gallium driver and unit tests (MIT) |
| `mesa/include/drm-uapi/chrome9_drm.h` | Mesa copy of the UAPI |
| `patches/` | changes outside the driver directories, per topic |
| `docs/` | hardware research and design notes, start at [docs/research/README.md](docs/research/README.md) |

`linux/` and `mesa/` mirror the upstream source trees, so they copy
directly over a linux-7.2 or Mesa 26.2.2 source tree.

## Patches

All patches are `-p1` against the upstream release tarballs.

| patch | change |
|---|---|
| `linux-build.patch` | DRM Kconfig and Makefile entries |
| `mesa-build.patch` | meson option, pipe-loader descriptor, dri/dril targets |
| `mesa-nir-sincos.patch` | constant sin/cos fold exactly with `lower_sincos` |
| `mesa-draw-wide-points.patch` | GL point rasterization option for the wide point stage |
| `mesa-draw-fs-texcoord.patch` | point sprite coordinates on drivers without the texcoord cap |
| `mesa-draw-wide-lines.patch` | a zero wide-line threshold also converts 1-pixel lines |
| `mesa-gallivm-legacy-booleans.patch` | legacy float set comparisons, select for no-integer booleans |
| `mesa-ntt-vertex-lod.patch` | explicit LOD for implicit-LOD lookups before TGSI |
| `mesa-dri-backbuffer.patch` | `PIPE_BIND_BACKBUFFER` from `__DRI_IMAGE_USE_BACKBUFFER` |
| `mesa-st-depth-preflight.patch` | mutable depth proxy images at level zero |
| `mesa-st-clip-planes.patch` | lowered user clip planes with unpacked uniform storage |

## Building

Kernel (linux-7.2):

    cp -a linux/. ../linux-7.2/
    patch -d ../linux-7.2 -p1 < patches/linux-build.patch
    # CONFIG_DRM_CHROME9=m

Mesa (26.2.2):

    cp -a mesa/. ../mesa-26.2.2/
    for p in patches/mesa-*.patch; do patch -d ../mesa-26.2.2 -p1 < $p; done
    meson setup build ../mesa-26.2.2 -Dgallium-drivers=chrome9,llvmpipe \
        -Dllvm=enabled -Dvulkan-drivers= -Dglx=dri -Dplatforms=x11
    ninja -C build && meson test -C build --suite chrome9

`chrome9-shader-nir` and `chrome9-mipmap` currently fail; both are under
investigation.

## History

The history merges the separate kernel driver, kernel UAPI, Mesa driver,
Mesa UAPI header and documentation repositories in commit time order.
Commit ids quoted in `docs/` refer to those repositories;
[docs/commit-map.txt](docs/commit-map.txt) maps them to the commits here.
The captured shader and state corpora (`*.tsv`, `*.json`) and the VIA
programming manuals the research notes cite are not part of this
repository.
