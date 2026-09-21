# Chrome9 HD driver research

This directory is the engineering notebook for a new Linux DRM and Mesa 3D
driver for the VIA Chrome9 HD in the VX900 system processor.

The first target is PCI `1106:7122`, the device ID stated by the VX900
programming manual.  Older VIA generations are references, not initial support
targets.

## Documents

- [source-audit.md](source-audit.md) inventories the supplied primary sources,
  their applicability, and important provenance issues.
- [chrome9-hd-hardware.md](chrome9-hd-hardware.md) records the hardware model
  established so far and lists facts that still require experiments.
- [legacy-driver-analysis.md](legacy-driver-analysis.md) separates reusable
  knowledge from obsolete DRI1/kernel mechanisms.
- [via-5.75-kmd.md](via-5.75-kmd.md) is the bit-anchored audit of the supplied
  Chrome9 KMD source: translated submission, packets, fences, verifier hazards,
  interrupts, initialization, and recovery gaps.
- [via-5.75-dri-state.md](via-5.75-dri-state.md) records exact H5/H6 state,
  target, 2D/3D synchronization, and draw packets recovered from the symbol-rich
  proprietary Linux DRI and Xorg binaries.
- [linux-dri-prereqs.md](linux-dri-prereqs.md) traces the complete 7122
  initialization-to-first-draw call chain and records the engine-initializer
  ordering defect which can explain the measured VP/front-end stall.
- [vp-stall-analysis.md](vp-stall-analysis.md) proves why the old H5-branch
  capture stalled, records the exact 188-dword 7122/H6 oracle, and ties it to
  the first verified live 3D draw.
- [xorg-one-texture-oracle.md](xorg-one-texture-oracle.md) records the exact
  232-dword normal one-texture successor, including sampler state, texture
  instruction, five-instruction VS, PS, and normalized quad coordinates.
- [glamor-copy-raster-contract.md](glamor-copy-raster-contract.md) maps Xorg
  `copy_area_` through Gallium draw to the proven native texture packet,
  identifies every sampler-without-triangle exit, and defines the minimal
  viewport/scissor/cull boundary trace for the current black-window failure.
- [mesa-texture-bringup.md](mesa-texture-bringup.md) turns that live oracle
  into the smallest truthful one-unit Gallium/UAPI path, records the exact
  REPLACE and MODULATE programs, and lists the remaining GL 1.3 state gates.
- [mesa-capability-audit.md](mesa-capability-audit.md) traces Mesa 26.2's
  context/version/extension gates, identifies unsafe generic cap defaults,
  and explains why the bounded one-unit path is not yet a conformant GL API.
- [raster-front-end-oracles.md](raster-front-end-oracles.md) records exact
  cull, edge-AA, clip/scissor, viewport packing, polygon-offset, vertex-stream,
  and inline-index state plus guarded silicon discriminators.
- [resource-layout-clear-cache.md](resource-layout-clear-cache.md) records
  practical RT stride/extent limits, linear mip allocation and the exact
  eight-entry mip table, guarded clear derivatives, and the render-to-texture
  cache-invalidation protocol.
- [mip-tail-l9-l11-oracle.md](mip-tail-l9-l11-oracle.md) closes the vendor
  resource layout through L11 and defines bounded POT/NPOT silicon tests for
  the implicit L8-rooted hardware tail.
- [npot-2d-capability-gate.md](npot-2d-capability-gate.md) cross-checks both
  Windows UMDs, Linux, Xorg, and the manual; it gives the exact A20/A22,
  pitch, wrap/filter/mip contract and separates the immediately testable
  level-zero glamor path from the requirements for a truthful Gallium cap.
- [fixed-function-texture-sync.md](fixed-function-texture-sync.md) records
  exact texture channel order, cube/volume addressing, color-mask, logic-op,
  alpha-test, stencil and fog words, plus 2D/3D/present synchronization.
- [blend-alpha-packet-recipes.md](blend-alpha-packet-recipes.md) gives exact
  A00/A33--A37/A53 lowering recipes for common blend equations, channel masks,
  and alpha test, while separating live clamped state from the proprietary
  no-clamp policy.
- [fixed-function-state-transitions.md](fixed-function-state-transitions.md)
  combines blend, alpha-test, fog, scissor, and texture-format words into a
  transition-closed two-texture state image with exact body-index formulas.
- [fixed-function-fog-alpha-shader-keys.md](fixed-function-fog-alpha-shader-keys.md)
  separates fixed A33 alpha test from fog shader variants and records
  Wine/Linux compiler differentials for bounded LRP and TEXKILL keys.
- [mip-format-projective-oracles.md](mip-format-projective-oracles.md)
  defines bounded mip-level, format, software/native projection, and texture
  cache-transition gates for the post-two-texture implementation.
- [texture-coordinate-lod-contract.md](texture-coordinate-lod-contract.md)
  recovers the exact A04/A08 coordinate controls, cross-checks Windows and
  Linux projective state, and defines bounded rectangle/derivative/LOD gates.
- [two-level-mip-abi-oracle.md](two-level-mip-abi-oracle.md) resolves the
  A18 versus reserved A19--A1F namespace, cross-checks the fixed eight-entry
  table and LOD/filter words in both Windows UMDs, and specifies a bounded
  two-level kernel/Mesa successor ABI and silicon sequence.
- [sf-2d-copy-present.md](sf-2d-copy-present.md) records the exact translated
  SF screen-copy packet, base/pitch/domain encoding, sticky-state reset,
  overlap directions, fence/cache ownership, and guarded copy-present oracle.
- [vx900-2d-operations.md](vx900-2d-operations.md) defines bounded H6 solid,
  copy, overlap, byte-mask, clipping and CFC recipes, characterizes the
  AlphaBLT boundary, and records why 2D stretch remains gated.
- [display-flip-vblank.md](display-flip-vblank.md) records the exact 7122 T11
  automatic-flip packets, direct IGA1/IGA2 scanout-base fields, vsync/blank
  status bits, and the render-fence-to-DRM-vblank ownership protocol.
- [iga1-vblank-flip-oracle.md](iga1-vblank-flip-oracle.md) gives the exact
  VX900 IGA1 IRQ enable/RW1C sequence, the `0204[1]` direct-base latch proof,
  and failure-safe atomic-flip ownership and teardown rules.
- [direct-sf-scanout-review.md](direct-sf-scanout-review.md) audits the first
  persistent SF scanout implementation, gives its exact no-reboot live gate,
  and defines the modern DRM IRQ/event/page-flip integration boundary.
- [chrome9-submit-bottleneck.md](chrome9-submit-bottleneck.md) accounts for
  the synchronous batch path's exact PTE, cache-ownership, guard-readback and
  retirement costs, and orders the fail-closed performance work.
- [vx900-kms-next-oracles.md](vx900-kms-next-oracles.md) defines the exact
  primary ARGB cursor, scanout format/pitch limits, conservative IGA2 bring-up,
  DDC/hotplug, DPMS, and VX900 pixel-PLL sequences and restore rules.
- [sf-render-target-coherency.md](sf-render-target-coherency.md) proves the
  translated SF render-target domain, records the live two-sided CPU-cache
  requirement, and cross-checks Linux CLFLUSH and Windows GART cache policy.
- [via-5.75-shader-re.md](via-5.75-shader-re.md) records the executable offline
  Linux shader compiler, native VS fields, PS object limits, and program/control
  upload ABI.
- [general-shader-runtime-contract.md](general-shader-runtime-contract.md)
  cross-checks both Windows shader uploaders and the Linux compiler, defines
  constants, ALU/TEX, varying linkage, activation and termination as one
  bounded runtime contract, and prioritizes the glamor forms beyond the
  current fixed fragment classes.
- [wine-closed-key-shader-differentials.md](wine-closed-key-shader-differentials.md)
  records reproducible Wine compiler differentials for the next closed
  fragment keys, exact Xorg two-texture alpha objects in both orientations,
  and their fail-closed resource/control validation contract.
- [gl-feature-matrix.md](gl-feature-matrix.md) reconstructs the exact Linux and
  Windows OpenGL extension sets, raw limits, texture/render/depth formats,
  framebuffer/query support, and conservative Gallium/NIR capability envelope.
- [game-raster-depth-stencil-oracles.md](game-raster-depth-stencil-oracles.md)
  gives packet-exact depth, stencil, culling, polygon-offset, color-mask,
  clear, native-line, and point oracles plus state-transition closure rules.
- [fixed-depth-cull-stencil-contract.md](fixed-depth-cull-stencil-contract.md)
  cross-checks Linux and both Windows fixed-state builders, defines the
  structured depth/cull/stencil lowering boundary, records Gallium's stencil
  operation remap, and keeps compatibility alpha test on fixed A33.
- [textured-d16-format-contract.md](textured-d16-format-contract.md) gives the
  exact one-texture-plus-D16 packet lengths and indices, three-BO lifetime,
  X-versus-alpha sampler semantics, and strict render-target reinterpretation
  boundary.
- [iga1-flip-pending-contradiction.md](iga1-flip-pending-contradiction.md)
  reconciles the live stuck `0204[1]` flip status with the manual and Windows
  KMD, proves it is not RW1C, and defines the next latch discriminators.
- [render-to-scanout-ownership.md](render-to-scanout-ownership.md) defines the
  same-device, same-BO 3D-to-IGA1 ownership boundary and separates producer
  drain, display latch, GTI invalidation, and CPU cache transitions.
- [windows-driver-re.md](windows-driver-re.md) records the static reverse
  engineering of the supplied VX900 Windows UMD, OpenGL ICD, and KMD, including
  packet headers, relocation formats, fence candidates, and capture controls.
- [shader-isa.md](shader-isa.md) records the statically recovered program-upload
  ABI, common OpenGL/Direct3D compiler backend, and partial instruction fields.
- [`../../tools/chrome9_alu_decode.py`](../../tools/chrome9_alu_decode.py)
  decodes the established fields from a four-dword pixel-ALU instruction while
  keeping unresolved fields visible.
- [`../../tools/chrome9_vbios_rtsf.py`](../../tools/chrome9_vbios_rtsf.py)
  validates the checksummed VX900 private shadow table and decodes its
  Windows-KMD PFN-remap threshold/base fields.
- [`../../tools/wine_shader_probe.c`](../../tools/wine_shader_probe.c) is a
  minimal PE32 harness for running the hashed UMD's private tokenizer,
  instruction dispatcher, scheduler, and finalizer headlessly under Wine.
- [`../../tools/d3d_shader_blob.py`](../../tools/d3d_shader_blob.py) constructs
  reproducible one-variable D3D token streams for that probe.
- [via-5.75-shader-corpus.tsv](via-5.75-shader-corpus.tsv) is the replay-checked
  Linux/Windows native shader corpus used to assign ISA fields and expose
  generation-dependent lowerings.
- [windows-closed-key-shader-corpus.tsv](windows-closed-key-shader-corpus.tsv)
  pairs each bounded D3D compiler differential with its executable
  Linux/Xorg words, control tuple, and exact resource masks.
- [`../../tools/chrome9_drm_smoke.c`](../../tools/chrome9_drm_smoke.c) validates
  the provisional discovery/GEM UAPI without sending commands to the GPU.
- [driver-architecture.md](driver-architecture.md) proposes the modern kernel
  and Mesa design, milestones, and acceptance gates.
- [implementation-status.md](implementation-status.md) records the exact T2
  kernel/Mesa integration, build commands, provisional UAPI, and live tests.
- [hardware-lab.md](hardware-lab.md) records the read-only VX900 inventory and
  recovery constraints for the current test machine.
- [validation.md](validation.md) defines the hardware, security, and conformance
  test strategy.
- [registers/README.md](registers/README.md) defines the register database and
  coverage rules used to turn the manuals into an auditable specification.

## Evidence convention

Every technical statement should eventually have one of these labels:

- **Manual**: stated in a named programming manual with PDF page number.
- **Code**: observed in a named source file and line or symbol.
- **Measured**: reproduced on named hardware with a saved test and trace.
- **Inference**: plausible but not yet confirmed.

Conflicting sources are retained as conflicts.  A newer manual is not assumed
to be correct merely because it is newer, and an old driver constant is not
treated as a hardware specification merely because it worked on another chip.

## Scope

The intended end state is:

1. a maintainable, reviewable hardware specification;
2. a modern DRM driver with GEM buffer objects, render nodes, validated command
   submission, explicit synchronization, recovery, and atomic KMS;
3. a Mesa Gallium driver using NIR and the normal Mesa winsys/loader model;
4. reproducible tests, traces, and conformance results.

Video decode/capture/HQV support is documented because it shares registers,
memory paths, and interrupts, but codec acceleration is not on the critical
path for the first 3D driver.

## Immediate next gate

Do not freeze a kernel UAPI or promise an OpenGL version until the following
questions have measured answers:

- which VX900 memory locations the 3D engine can read and write coherently;
- how command submission is started, fenced, interrupted, and reset;
- whether the VX900 shader ISA is compatible with HC3/HCM, and the remaining
  binary encoding fields (static comparison of the shared OpenGL/Direct3D
  backend is the primary acquisition path; native dumps are optional checks);
- which tiling layouts and alignment constraints apply to render targets,
  depth, textures, and vertex buffers;
- whether display and render can safely be brought up in separate development
  stages on the same PCI function.
