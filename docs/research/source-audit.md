# Source audit and provenance

## Supplied programming manuals

The PDFs are primary register references.  Page numbers below are PDF page
counts, not the manuals' printed page labels.

| Family | Product | PCI device stated by manual | Revision/date | Part I | Part II |
|---|---|---:|---|---:|---:|
| UniChrome Pro II | CX700/VX700 | `3157` | 1.08, 2008-11-19 | 100 pages | 91 pages |
| Chrome9 HC3 | VX800/VX820 | `1122` | 1.0, 2008-11-19 | 100 pages | 157 pages |
| Chrome9 HCM | VX855/VX875 | `5122` | 1.0, 2009-07-29 | 110 pages | 172 pages |
| Chrome9 HD | VX900 | `7122` | 1.0, 2011-01-19 | 151 pages | 182 pages |

Source files:

- `docs/OGPM_UniChrome_ProII_R108a_Part1_Core_2D.pdf`
- `docs/OGPM_UniChrome_ProII_R108a_Part2_3D_Video.pdf`
- `docs/OGPM_Chrome9 HC3_R100a_Part1_Core_2D.pdf`
- `docs/OGPM_Chrome9 HC3_R100a_Part2_3D_Video.pdf`
- `docs/OGPM_Chrome9 HCM_R100_Part1_Core_2D.pdf`
- `docs/OGPM_Chrome9 HCM_R100_Part2_3D_Video.pdf`
- `docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`
- `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`

The manuals permit copying and distribution while requiring attribution and
disallowing modification.  Generated summaries and register data must retain
source identity and must never replace or silently rewrite the PDFs.

### Manual coverage

Part I covers PCI/configuration, VGA and extended I/O, display, 2D, DMA, CBU
rotation, and—depending on generation—LVDS/DVI/DisplayPort.  Part II covers
video display/capture, HQV, the Command Regulator (CR), and 3D state.

The manuals are detailed register references, not complete driver design
specifications.  They do not establish all ordering, coherency, timeout,
reset, or security behavior needed by a production driver.

### Known internal contradiction

The VX900 Part II CR type table names these `HParaType 03h` subtypes:

- `20h`: pixel-shader ALU instructions;
- `21h`: pixel-shader TAU instructions;
- `22h`: pixel-shader constants;
- `30h`: vertex-buffer base and pitch;
- `31h`: vertex-shader instructions;
- `32h`: vertex-shader constants;
- `33h`: vertex-shader integer registers.

The detailed VX900 3D chapter later labels `20h`-`22h` and `31h`-`33h` as
reserved.  The HC3 and HCM manuals do describe program upload sizes—96
four-dword pixel ALU instructions and 32 one-dword TAU instructions—but still
do not define the instruction bit encoding.  Shader compilation therefore
cannot be implemented from the supplied PDFs alone.

## Legacy Linux DRM source

The supplied tree is at commit `cfc8860eacec5da2ee2880c502b10daf196c6cbb`
(2023-01-13).  The relevant snapshot contains:

- `linux/drivers/gpu/drm/via/via_dri1.c` (3,630 lines);
- `linux/drivers/gpu/drm/via/via_3d_reg.h` (1,771 lines);
- `linux/include/uapi/drm/via_drm.h` (legacy UAPI);
- `linux/include/drm/drm_pciids.h` (legacy device table).

The driver was removed by commit
`8391e000065d4fac88548e071fc43c3e07cb7047` on 2023-01-13 because it was a
long-obsolete UMS/DRI1 driver whose Mesa user-mode driver was deprecated.

The legacy PCI table contains `3022`, `3118`, `3122`, `7205`, `3108`, `3344`,
`3343`, `3230`, and `3157`.  It does **not** contain VX900 `7122`.  Device
`3230` is tagged `VIA_DX9_0`; the verifier rejects 3D packets and AGP DMA for
that family.  This is evidence about the old driver's limitations, not about
VX900 itself.

The kernel `via_3d_reg.h` contains additions not present in Mesa 7.11.2,
including texture-sample subtypes and YUV controls, and carries copyright years
through 2011.  Its provenance and generation applicability must be checked
definition by definition.

## Mesa 7.11.2 UniChrome source

The supplied classic driver is under
`Mesa-7.11.2/src/mesa/drivers/dri/unichrome/` (about 10,000 lines excluding
server headers).  Its chipset enum ends at CLE266, KM400, K8M800, and
PM800/CN400.  It does not model HC3, HCM, or VX900.

The driver is useful evidence for older fixed-function state and command
ordering.  It is not a viable base for a modern Mesa driver because it uses
the classic DRI1 loader, SAREA and hardware locks, Mesa software T&L, direct
MMIO mappings, and the removed VIA DRI1 UAPI.

## Additional source required for display

The legacy VIA DRM driver has no modern KMS implementation.  The supplied
Linux tree's `drivers/video/fbdev/via/` code recognizes VX900 (`7122`) and
contains display clocks, FIFO tuning, connector paths, and modesetting magic.
It must be audited as a secondary code source.  Values recovered from it still
need to be reconciled with the manuals and measured on hardware.

## Missing evidence to acquire

Priority order:

1. a physical VX900 board with serial console, remotely controllable power,
   and a known-good firmware/display configuration;
2. PCI configuration, VGA register, and MMIO dumps before and after firmware,
   framebuffer, 2D, and 3D operations;
3. continued static extraction of the shared compiler backend and kernel
   packet paths in the supplied 2015 VX900 Windows driver package, augmented
   by the now-working headless Wine compiler oracle; optional native GPU dumps
   can later confirm recovered golden vectors;
4. the latest openchrome DDX/KMS development history and any unreleased VIA
   register headers;
5. upstream kernel and Mesa guidance before a UAPI is frozen.

## Supplied Windows VX900 package

The package under `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/` is direct
VX900 evidence rather than a neighboring-generation reference.  Its INF binds
PCI `1106:7122` and identifies driver version `8.14.14.0235`; the KMD also
contains explicit `7122` paths.  The 32-bit Direct3D UMD, OpenGL ICD, KMD,
display support module, and VGA ROM have been hashed and statically audited in
`windows-driver-re.md`.  The UMD's private shader tokenizer and raw emitter
also run headlessly under Wine using the version-locked probe in `tools/`.

The Windows binaries are evidence, not redistributable source and not a design
to reproduce wholesale.  Record only interoperability facts and independently
authored descriptions in the hardware specification.

## Current upstream design references

The implementation target is current upstream, not the versions bundled in
this research tree.  Re-check these living documents when implementation
starts and before each submission series:

- [DRM userland interfaces](https://docs.kernel.org/gpu/drm-uapi.html),
  especially open-source userspace requirements, render nodes, reset, UAPI
  structure rules, and IGT expectations;
- [DRM memory management](https://docs.kernel.org/gpu/drm-mm.html), including
  GEM/TTM, syncobjs, execution locking, GPUVM, and the GPU scheduler;
- [Mesa Gallium](https://docs.mesa3d.org/gallium/index.html), including current
  `pipe_screen`, `pipe_context`, resources, formats, and synchronization;
- [Mesa NIR](https://docs.mesa3d.org/nir/index.html) for the shader compiler IR
  and its lowering/testing infrastructure.
