# Chrome9 HD driver research

This directory is the engineering notebook for a new Linux DRM and Mesa 3D
driver for the VIA Chrome9 HD in the VX900 system processor.

The first target is PCI `1106:7122`, the device ID stated by the VX900
programming manual.  Older VIA generations are references, not initial support
targets.

## Documents

- [mesa-ps-encoder-v1.md](mesa-ps-encoder-v1.md) defines the first bounded
  Mesa NIR-to-native fragment encoder, operand packing, closed scheduler
  tuples, dependency constraints, and byte-exact golden tests.
- [chrome9-ps-encoder-v1-corpus.tsv](chrome9-ps-encoder-v1-corpus.tsv) is the
  finalized oC0 and texture-consumer golden corpus for the bounded encoder.
- [chrome9-cmd19-fp-contract.md](chrome9-cmd19-fp-contract.md) separates
  value-independent Linux/Windows MOV/MUL compiler evidence from numerical
  silicon behavior, defines the finite/exceptional-value gate, a guarded
  alpha-test denormal oracle, and the NIR FP-control admission policy.
- [chrome9-cmd19-fp-compiler-corpus.tsv](chrome9-cmd19-fp-compiler-corpus.tsv)
  records the exact signed-zero, denormal, finite-endpoint, NaN, and infinity
  `DEF c0` differential across both proprietary compilers.
- [chrome9-next-alu-one-record.md](chrome9-next-alu-one-record.md) records the
  bounded next-operation compiler evidence, static operand boundary, and
  fail-closed promotion order beyond MOV/MUL.
- [chrome9-next-alu-one-record-corpus.tsv](chrome9-next-alu-one-record-corpus.tsv)
  is the 32-row Windows/Linux byte-matched v0/c0 ALU corpus.
- [chrome9-next-alu-static-policy.tsv](chrome9-next-alu-static-policy.tsv)
  separates byte-exact rows from static-only, numerical, and reject-only
  swizzle/modifier candidates.
- [chrome9-min-add-rung.md](chrome9-min-add-rung.md) defines the first guarded
  MIN silicon gate and the exact source-B and finite-domain boundary for ADD.
- [chrome9-min-add-rung-corpus.tsv](chrome9-min-add-rung-corpus.tsv) records
  the eight additional Windows/Linux ADD/MIN swizzle and NEG differentials.
- [windows-umd-wine-entrypoint-boundary.md](windows-umd-wine-entrypoint-boundary.md)
  inventories public D3D/OGL entry points, proves the UMD's 208-byte KMD
  adapter-record dependency under Wine, and records the private compiler path
  which remains usable hardware-free.
- [source-audit.md](source-audit.md) inventories the supplied primary sources,
  their applicability, and important provenance issues.
- [chrome9-hd-hardware.md](chrome9-hd-hardware.md) records the hardware model
  established so far and lists facts that still require experiments.
- [legacy-driver-analysis.md](legacy-driver-analysis.md) separates reusable
  knowledge from obsolete DRI1/kernel mechanisms.
- [via-5.75-kmd.md](via-5.75-kmd.md) is the bit-anchored audit of the supplied
  Chrome9 KMD source: translated submission, packets, fences, verifier hazards,
  interrupts, initialization, and recovery gaps.
- [vx900-reset-recovery-oracle.md](vx900-reset-recovery-oracle.md) recovers the
  Windows destructive reset/replay/restart chain including its exact
  1,355-write VX900 replay, separates late retirement and GTI retry from a real
  wedge, and defines exact display, fence, epoch, translation, and
  quarantine-release ordering for reboot-free recovery.
- [chrome9-fe3x-completion.md](chrome9-fe3x-completion.md) specifies the exact
  FE33 3D completion packet, coherent record lifetime, staged polling/IRQ
  oracle, mixed-register-safe `0200/0204` handling, and the proof which
  distinguishes a lost notification from an engine timeout.
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
- [mip-guarded-import-contract.md](mip-guarded-import-contract.md) reconciles
  the vendor-packed and current compatibility-guarded mip layouts, gives the
  exact H3 offset for each, and proves that the recent unguarded-import smoke
  failure was a shader-classification issue rather than a missing shadow.
- [two-level-mip-ioctl-design.md](two-level-mip-ioctl-design.md) specifies the
  exact 192-byte versioned two-level ioctl, fail-closed guarded-object
  validation, H3 insertion indices, A30/A31 lowering, ownership rules, and a
  raw live oracle sequence ready for the next implementation tranche.
- [ioquake3-q3dm1-mip-envelope.md](ioquake3-q3dm1-mip-envelope.md) proves that
  default-picmip q3dm1 needs only the explicit L0--L8 address table, records
  the exact current guarded 256x256 chain, and defines the smallest combined
  two-texture+D16 nearest-mip successor and promotion sequence.
- [legacy-texture-format-oracles.md](legacy-texture-format-oracles.md)
  recovers A8/L8/L8A8 and RGB565/ARGB1555/ARGB4444 sampler and render-target
  encodings, channel and alpha semantics, guarded readback tests, and safe
  32-bit upload-expansion fallbacks.
- [ioquake-texture-format-sampler-plan.md](ioquake-texture-format-sampler-plan.md)
  cross-checks both Windows UMDs, proprietary Linux, the VX900 manual, and old
  Mesa for ioquake's next sampler formats; it gives the complete min/mag/mip
  and wrap words plus standardized level-zero and guarded two-level vectors.
- [sampler-next-implementation-audit.md](sampler-next-implementation-audit.md)
  reconciles Windows/Linux sampler builders with the current guarded layout,
  closes mirror-once and signed-LOD conversion details, separates ordinary
  filtering from coefficient-backed anisotropy, and ranks executable mip and
  packed-format promotion gates.
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
- [chrome9-uapi-allocation.md](chrome9-uapi-allocation.md) is the canonical
  private command-number and capability-bit ledger for merged and reserved
  structured ABIs.
- [d16-rhw-run-ioctl-design.md](d16-rhw-run-ioctl-design.md) specifies the
  bounded eight-segment D16/RHW run, including checked UAPI bounds, one-fence
  resource lifetime, per-segment cookies, quarantine rules, Mesa integration,
  and no-wedge gates.
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
- [ioquake3-gl2-fragment-constants.md](ioquake3-gl2-fragment-constants.md)
  closes the split 32-vector fragment constant store, its H3 upload packets,
  ioquake startup demand, structured job-local ABI, and guarded bank-boundary
  tests.
- [ioquake3-gl2-ps-next-rungs.md](ioquake3-gl2-ps-next-rungs.md) inventories
  ioquake's post-constant GL2 shader families, records exact common ALU and
  multi-phase sampler objects, proves fragment flow/derivatives are silently
  unsupported, and defines fail-closed NIR keys and live promotion oracles.
- [ioquake3-gl2-generic-texturecolor.md](ioquake3-gl2-generic-texturecolor.md)
  maps the first executable GL2 generic and texturecolor programs, fixes the
  job-local semantic-program UAPI, constant/sampler linkage, post-TNL varying
  contract, unchanged packet indices, and guarded promotion sequence.
- [ioquake3-gl2-first-native-sequence.md](ioquake3-gl2-first-native-sequence.md)
  resolves the configuration-dependent first GL2 link failure, maps the
  reduced renderer's generic/fast-light/texturecolor execution order, proves
  that fast lightall needs a packed-coordinate post-TNL split rather than a
  native VS ABI, and records fresh Windows compiler hashes and staged gates.
- [ioquake3-gl2-texturecolor-command16-audit.md](ioquake3-gl2-texturecolor-command16-audit.md)
  fixes the state-complete command-16 TEX0*c0 packet map, exact constant and
  format semantics, depth/blend/alpha collision rules, validator/lifetime
  contract, and guarded native-c0 silicon sequence.
- [ioquake3-gl2-linked-texturecolor-lightall.md](ioquake3-gl2-linked-texturecolor-lightall.md)
  records the exact six reduced-lightall linked NIR hashes and common DAG,
  closes texturecolor constant-offset edge cases, specifies the packed
  XY/ZW post-TNL split and no-projector rule, and gives bounded native packet
  gates and live oracle vectors.
- [ioquake3-gl2-fogpass-native-design.md](ioquake3-gl2-fogpass-native-design.md)
  closes ioquake3 GL2 fogpass as a five-ALU semantic program with exact
  float-varying linkage, packet indices, D16/source-over state and guarded
  zero/negative/square-root silicon oracles.
- [ioquake3-gl2-eight-unit-linkage.md](ioquake3-gl2-eight-unit-linkage.md)
  identifies ioquake3's exact default six-active/seven-logical sampler limit,
  derives Chrome9 stages 2..7 FE/FVF/VS state, records stage-7 and five-sample
  compiler objects, and orders guarded promotion without falsely exposing 8.
- [wine-closed-key-shader-differentials.md](wine-closed-key-shader-differentials.md)
  records reproducible Wine compiler differentials for the next closed
  fragment keys, exact Xorg two-texture alpha objects in both orientations,
  and their fail-closed resource/control validation contract.
- [ioquake-texture-environment-combine.md](ioquake-texture-environment-combine.md)
  derives exact per-base-format REPLACE/MODULATE/ADD alpha semantics, records
  compiler-exact partial-mask ADD and mixed two-unit objects, and bounds the
  closed crossbar keys and guarded promotion sequence needed by ioquake GL1.
- [gl-feature-matrix.md](gl-feature-matrix.md) reconstructs the exact Linux and
  Windows OpenGL extension sets, raw limits, texture/render/depth formats,
  framebuffer/query support, and conservative Gallium/NIR capability envelope.
- [game-raster-depth-stencil-oracles.md](game-raster-depth-stencil-oracles.md)
  gives packet-exact depth, stencil, culling, polygon-offset, color-mask,
  clear, native-line, and point oracles plus state-transition closure rules.
- [ioquake-next-raster-state-ladder.md](ioquake-next-raster-state-ladder.md)
  turns the post-ALWAYS ioquake state sequence into an exact 208-byte
  D24S8/stencil successor, 204-dword packet map, validators, capability
  allocation, alpha-kill ordering checks, and guarded promotion suite while
  retaining software cull and polygon offset.
- [native-cull-bias-lowering.md](native-cull-bias-lowering.md) closes the
  post-viewport Gallium-to-physical winding formula, proves the proprietary
  CW/CCW orientation swap, fixes every current D16 cull index, and gives
  exact D16/D24S8 bias words, packet growth, ownership rules, and live
  discriminators.
- [two-sided-stencil-orientation.md](two-sided-stencil-orientation.md)
  reconstructs the Windows mirror-then-exchange face builder, maps Gallium
  front/back state and dynamic refs to physical CW/CCW A23--A26 payloads,
  reserves the unequal-face capability, and defines the live winding matrix.
- [line-point-raster-contract.md](line-point-raster-contract.md) closes the
  proprietary point/list/strip topology words, point-size insertion without
  packet growth, variable immediate-body bounds, current draw-expansion gap,
  and guarded coverage/diamond-rule promotion suite.
- [msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md)
  proves both Windows multisample render states are packet no-ops, separates
  edge AA from sample storage, recovers the H6 smooth-line special cycle,
  closes post-viewport validators and native C-to-screen coupling, and keeps
  user clip planes fail-closed behind shader/software lowering.
- [stipple-polygon-mode-contract.md](stipple-polygon-mode-contract.md)
  closes A01/A84/A85 line-stipple enable, pattern, repeat and per-primitive
  reset state, gives exact packet growth and phase oracles, and keeps broken
  native polygon stipple and two-sided unfilled modes behind safe lowering.
- [flatshade-edge-ownership-contract.md](flatshade-edge-ownership-contract.md)
  closes H6 FlatA/B/C/Gouraud state across Linux and both Windows builders,
  identifies the current draw-pipeline flat-color gap, and defines bounded
  provoking-color and shared-edge ownership oracles without inventing a
  top-left control bit.
- [fixed-depth-cull-stencil-contract.md](fixed-depth-cull-stencil-contract.md)
  cross-checks Linux and both Windows fixed-state builders, defines the
  structured depth/cull/stencil lowering boundary, records Gallium's stencil
  operation remap, and keeps compatibility alpha test on fixed A33.
- [ioquake-depth-always-mask-zero.md](ioquake-depth-always-mask-zero.md)
  records the live ioquake depth-only state, fixes the D16 ALWAYS/mask-zero
  192-dword packet and capability-35 contract, and separates its bounded
  promotion from later software-cull and D24S8 stencil work.
- [ioquake-fixed-function-state-plan.md](ioquake-fixed-function-state-plan.md)
  integrates every recovered blend/depth/alpha/stencil/color-mask encoding,
  closes polygon-offset units by depth allocation, gives exact packet-growth
  formulas and ranks the safest structured ABI successors for ioquake-era GL.
- [ioquake3-next-gl1-keys.md](ioquake3-next-gl1-keys.md) ranks the expected
  post-blit GL1 keys, specifies projected one- and three-factor classifiers,
  and fixes the three-factor D16 packet indices and rejection oracles.
- [ioquake-projected-texture-constant.md](ioquake-projected-texture-constant.md)
  closes the two live projected current-color NIR forms, distinguishes alpha
  bypass from alpha replication, and gives exact D1/native-c0 no-depth and
  D16 mappings with precision and Q=1 gates.
- [ioquake3-blend-alpha-matrix.md](ioquake3-blend-alpha-matrix.md) intersects
  shipped shader-stage blend/alpha/depth tuples with current structured draw
  support, fixes the A52 boundary, and proves A7B is not per-draw state.
- [textured-d16-blend-uapi-design.md](textured-d16-blend-uapi-design.md)
  defines capability-gated one-/two-texture D16 blend successors, resolves
  the A33/A10 collision at body 19, and fixes packet, lifetime and live gates.
- [command13-two-texture-d16-blend-audit.md](command13-two-texture-d16-blend-audit.md)
  audits the candidate 270-dword command-0x13 packet, exact masks and four-BO
  role wiring against both proprietary driver families, and proves that its
  object contract needs same-device aliases but no cross-device PRIME path.
- [two-texture-primary-uapi-design.md](two-texture-primary-uapi-design.md)
  defines the flag-compatible no-depth/D16 three-factor ABI, exact full-RGBA
  and primary-alpha-bypass programs, TEXLD/TEXLDP Q boundaries, packet maps,
  Mesa keys, format semantics, and staged four-object silicon tests.
- [three-factor-two-texture-runtime.md](three-factor-two-texture-runtime.md)
  closes the general nonwhite `tex0*tex1*primary` runtime path across all
  three two-texture ioctls, records the reproducible Wine compiler evidence,
  and fixes the alpha-mask, packet-index, validation, and silicon-oracle
  contract beyond the existing white-primary specialization.
- [ioquake-live-lightmap-alpha-successors.md](ioquake-live-lightmap-alpha-successors.md)
  records the two live projected lightmap/primary NIR variants, proves that
  exact XRGB/BGRA view translation lets one three-factor RGBA object cover
  all four texture-alpha cases, and maps later masked surfaces into the
  current command-12/13 A33 state.
- [ioquake3-gl2-lightall-bgra-object.md](ioquake3-gl2-lightall-bgra-object.md)
  closes the packed GL2 lightall case with BGRA unit 1 using an exact
  four-ALU texture-0-alpha object, maps all three packet forms and capability
  bit 39, and proves from the live bind trace that no third unsupported
  fragment key follows it in the captured interval.
- [ioquake-fog-dlight-successors.md](ioquake-fog-dlight-successors.md)
  closes ioquake GL1 fog and dynamic-light image formats, fragment keys,
  blend/depth words and identity-Q proof, and specifies the minimal
  capability-gated EQUAL/read-only successor with guarded silicon vectors.
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
- [native-ps-upload-transaction.md](native-ps-upload-transaction.md) closes the
  Linux/Windows A90--A9b, `2003`/`2103`/`2203` upload transaction, maps the
  Mesa canonical PS object onto a semantic kernel ABI and fail-closed
  validator, enumerates the unsafe raw-program fields, and gives the smallest
  staged ALU-only silicon oracle.
- [ioquake3-signed-repeat-command15.md](ioquake3-signed-repeat-command15.md)
  measures q3dm1's real base/lightmap coordinate ranges, proves that Chrome9
  transports raw float S/T and applies wrap in sampler A32, specifies a
  conservative two-unit signed-repeat capability and no-wedge oracle, and
  separates command 15's depth/blend flags from its semantic program selector.
- [program24-signed-repeat-hang-audit.md](program24-signed-repeat-hang-audit.md)
  combines the exact four-ALU TEX0-alpha-primary object with live signed
  stage-0 repeat, gives exact RGB/alpha and body-word oracles, and proves that
  its current one-triangle length and inactive command-15 path cannot explain
  increased ring pressure under sustained ioquake3 submission.
- [sustained-program24-lifetime-hang-audit.md](sustained-program24-lifetime-hang-audit.md)
  audits the four-BO reservation, DMA/SWIOTLB, SF-PTE, cache, ring-wrap,
  timeout, and quarantine lifetimes under sustained program-24 submission,
  and specifies the smallest counters and staged hardware bisect needed to
  distinguish resource pressure, CPU-side waiting, GART failure, and a true
  post-publication GPU stall.
- [persistent-batched-submit-performance-plan.md](persistent-batched-submit-performance-plan.md)
  replaces the earlier eight-page run execution with one exact 32-KiB
  immediate list, then defines a separately gated persistent SF/GART object
  cache, batched PTE invalidation, eviction/quarantine rules, counters, and
  deployment order aimed at removing measured per-triangle submission cost
  without weakening hang containment.
- [single-32k-list-kernel-patch-audit.md](single-32k-list-kernel-patch-audit.md)
  turns the first untextured 32-KiB list into an exact kernel-only patch
  checklist, quantifies the live 34-triangle PTE/VGA/cache cost, specifies
  sized-finisher golden offsets, and defines the 2/34/35/64/128/272 plus
  ring-wrap silicon oracle.
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
