# Legacy driver analysis

## Linux VIA DRI1 driver

The old driver combines unrelated services in one 3,630-line file:

- legacy AGP and framebuffer heaps managed with `drm_mm`;
- SAREA/futex compatibility;
- command-buffer verification and relocation of selected address fields;
- AGP command-ring control and a PCI submission fallback;
- IRQ/vblank/HQV handling;
- two-channel PCI DMA blits;
- legacy authentication, hardware lock, and DRI1 ioctls.

Its UAPI exposes allocation, mapping initialization, decoder futexes, DMA
initialization, raw command buffers, flush, ring-space queries, IRQ waits, and
DMA blits.  None of that UAPI should be copied into a new driver.

### Knowledge worth retaining

- MMIO status bits at `0400`, transfer setup at `043c`, and transfer data at
  `0440` agree with the manuals for the older command path.
- The verifier is an executable description of older packet headers, legal
  address-bearing fields, sequencing hazards, and primitive fire behavior.
- Ring start/pause/jump/rewind code documents cache-line padding and historical
  ordering assumptions that can guide experiments.
- IRQ and PCI DMA definitions provide candidate bit meanings to cross-check
  against each generation's manual.
- `via_3d_reg.h` provides symbolic names that can seed the register database.

### Ring-control cross-check against VX900

The old ring is useful for ordering and safety, but its register layout is not
the VX900 layout.  The old driver writes type-10 parameters through `043c/0440`
and packs start/end high bytes together in subtype `62`.  VX900 instead selects
the CR port through `041c`, streams words through `0420`, and gives start, end,
pause, and jump separate low/high subtype pairs `60/61`, `62/63`, `64/65`, and
`66/67`.  The exact Windows VX900 KMD uses the latter sequence.

Three safety rules do carry across generations and agree with the VX900 manual
and KMD:

- start, end, and pause addresses are at least 128-bit aligned;
- the ring contains its pause/stop command and padding; PAUSE is not an address
  immediately beyond the last valid command;
- command bytes are published before the trigger, and the engine is checked
  for idle before a stopped/paused reader is changed.

The old driver detects a paused reader through `041c.bit31`; the VX900 KMD
tests `0444.bit31` before extending its ring and reads `043c` as the current
command-request address.  Live work shows that `043c` may lead retirement by
the request FIFO depth, so it is not a completion pointer.  Both values should
be captured during bring-up, but only the VX900 observations may drive the new
implementation.

### Mechanisms to discard

- DRI1 SAREA, cliprects, drawable stamps, DRM authentication, and hardware
  locks;
- userspace MMIO mappings and userspace-managed ring pointers;
- global framebuffer/AGP heaps and integer allocation keys;
- raw userspace physical/GPU addresses;
- busy-loop ring-space ioctls;
- monolithic driver layout and legacy DRM callbacks.

The old verifier must be treated as a historical reference, not security proof.
It was written for a different command grammar and different supported chips.

## VIA 5.75.32.87a Linux Chrome9 driver

The package `binary/5.75.32.87a-u910-55689/` is a substantially better VX900
reference than the older upstream DRI1 driver.  Its release notes identify it
as VIA's 2010 Ubuntu 9.10 driver for VX800, VX855, and VX900, with both 2D and
3D DRI components.  It includes:

- the complete `VIA_Chrome9_2.6.31` kernel source, including the DMA/ring
  implementation, command verifier, memory manager, and exact register header;
- an unstripped `via_drv.so` Xorg driver;
- an unstripped `via_chrome9_dri.so` containing DWARF debug information;
- the matching kernel module and libGL binary.

For VX900 (`H5S2VP1`) the source establishes the missing PCIe-memory path:

- the translation table is a 64 KiB region in framebuffer memory containing
  16,384 32-bit PTEs, hence a 64 MiB system-framebuffer virtual address space;
- an invalid PTE is `80000000`; a valid PTE is the physical page frame number
  masked to 30 bits;
- `SR6A`, `SR6B`, and `SR6C.bit0` select table-address bits 12 through 28;
  the KMD uses `SR7B[3:0]=4` for a 64 KiB table despite the manual marking the
  field reserved; `SR6C.bit7` is table write protection, not a proven
  translation-enable bit;
- PTE changes require waiting for `SR6F.bit7` to clear and setting that bit to
  invalidate the translation cache;
- PCIe CR addresses use System Framebuffer location code `01b`; the ring starts
  at virtual address zero, rather than at a raw DMA address;
- before the ring is started, H5S2VP1 receives CR parameters `06000000`,
  `07100000`, and the CR readback selector `02000000` through `041c/0420`.

These details were confirmed on the live VX900: a one-page coherent ring mapped
at SF virtual address zero advanced the CR current pointer from `e7fe0040` to
`00000080`, asserted `0444.bit31` for pause, and returned the engine to idle.
The temporary table and sequencer state were restored byte-for-byte afterward.

The source also gives the exact Chrome9 packet grammar rather than only the
older UniChrome form: header 0 contains 2D register/value pairs; header 1
streams host-BLT data; header 2 streams typed CR or 3D parameters; header 3
carries 3D data; and headers 4/5/7/9/A cover branching, fences, and other
engines.  The verifier's address-hazard tables are being treated as the
starting point for a new allowlist, subject to VX900-specific tightening.

The DRI binary is unusually valuable for static recovery.  Its symbols name
the render-target, texture, draw, DMA, vertex-shader, and pixel-shader paths,
including `stmPVSCompile_H5`, `stmPPSCompile_invi`, `stmSetRenderTarget`, and
`stmHWPrepareTextureRegs_invi`.  DWARF types and source-unit names can recover
state structures and enums while disassembly of those named functions can
recover the exact emitted packets and shader instruction encodings.

## Mesa classic UniChrome driver

The Mesa driver supports only CLE266, KM400, K8M800, and PM800/CN400.  It
advertises two texture units, one draw buffer, no MSAA, 16/24-bit depth in the
enabled framebuffer configurations, and uses software transform-and-lighting.

Its major paths are:

- `via_state.c`: translates fixed-function GL state into cached hardware words;
- `via_tex.c` and `via_texcombine.c`: texture layout, upload, filtering, and
  combine state;
- `via_tris.c`: Mesa T&L vertex emission, primitive assembly, and fallbacks;
- `via_ioctl.c`: clear, swap/page flip, cliprect emission, buffer flushing, and
  breadcrumb synchronization;
- `via_context.c`: DRI1 context setup and direct register mappings;
- `via_3d_reg.h`: older fixed-function register definitions.

### Knowledge worth retaining

- mappings from OpenGL compare/blend/stencil/texture-combine enums to older VIA
  fields;
- state emission ordering and cache-clear points;
- immediate vertex layouts and primitive termination behavior;
- format/alignment restrictions and known fallback conditions;
- small command streams suitable as initial golden-vector tests.

### Code not suitable for reuse

- classic DRI1 loader integration and SAREA synchronization;
- direct MMIO access from userspace;
- Mesa 7.11 driver hooks, software T&L pipeline, and template rasterization;
- drawable cliprect handling, old page flipping, and texture heap management;
- assumptions that the hardware has exactly two fixed-function texture units.

The new Mesa driver should be written as Gallium code around current Mesa APIs.
Port concepts and verified constants; do not mechanically port this driver.

## Cross-generation rule

A legacy definition can enter target code only after it is:

1. matched to a VX900 manual field, or
2. confirmed by a focused VX900 experiment.

If neither is possible, keep it in the research database with generation and
confidence metadata.  This prevents an older UniChrome behavior from silently
becoming a Chrome9 HD ABI guarantee.
