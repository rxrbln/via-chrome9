# Proposed modern driver architecture

This is a staged design.  Choices that depend on unresolved memory or command
behavior are deliberately gates, not assumptions.

## Kernel DRM driver

Current experimental internal name: `chrome9`.  It is not an ABI commitment;
the final upstream name must be agreed with DRM and Mesa maintainers before a
rendering UAPI is merged.

### Core objects

- one `drm_device` for PCI `1106:7122`;
- immutable per-generation capability data rather than scattered device-ID
  conditionals;
- GEM buffer objects with explicit placement/domain metadata;
- per-file GPU contexts and an address space or relocation namespace;
- kernel-owned command queues, fences, and reset state;
- `drm_gpu_scheduler` jobs if its dependency and timeout model fit this
  non-preemptive engine;
- syncobj timeline integration for explicit input/output synchronization;
- a render node with no privileged MMIO or display access.

### Buffer manager gate

Do not choose TTM, GEM shmem, or a dedicated aperture allocator by taste.  Run
the memory experiments first:

- If the GPU has a programmable GART and consumes scatter/gather system pages,
  GEM shmem plus a driver GPU-VA manager may be sufficient.
- If objects must migrate between a CPU-visible aperture, dedicated/stolen
  framebuffer, and dynamic system domain, TTM is likely the better fit.
- If early silicon can access only a fixed contiguous framebuffer aperture, a
  small GEM VRAM-style manager may be the bring-up path, but it must not become
  a premature permanent UAPI limitation.

### Submission UAPI principles

Keep the UAPI minimal and extensible:

- `GET_PARAM` for immutable capabilities;
- context create/destroy only if default per-file context is insufficient;
- GEM create/mmap/info ioctls using standard helpers where possible;
- one submit ioctl carrying a command BO/range, BO handle table, relocation or
  GPU-VA metadata, flags, and syncobj dependencies;
- no raw CPU or bus addresses, no MMIO mapping, no userspace ring control;
- zero-initialized padding and size/version extension rules from the first ABI.

The kernel parser must validate packet length, type/subtype/address range,
reserved bits, state-machine transitions, draw termination, memory references,
integer overflow, and command-buffer bounds.  It must patch or verify all GPU
addresses against pinned BOs and access permissions.

### Execution, timeout, and reset

Start with one serialized hardware queue.  Each job receives a monotonically
ordered fence.  A timeout must collect status/register diagnostics, stop further
submission, reset the smallest safe engine scope, restore kernel-owned state,
and signal affected fences with an error.  Repeated hangs from one context must
not permanently wedge display or other clients.

Polling is acceptable only for bounded early bring-up.  It is not the final
completion mechanism unless experiments prove there is no usable interrupt.

### KMS

The eventual driver should use atomic modesetting and standard DRM helpers:

- discover the number and routing of IGA/CRTC engines;
- model primary planes first, then cursor and overlay planes;
- implement vblank and page-flip events before acceleration-dependent flips;
- add VGA/LVDS/DVI/HDMI/DisplayPort paths one tested connector at a time;
- keep output and clock tables in generation-specific data structures;
- use firmware state readout for a non-destructive first modeset path.

The VX900 fbdev code is a behavioral reference, not code to transplant.  A
render-only bring-up branch is useful, but the same PCI function means it will
normally displace the existing display driver; remote access is mandatory.

## Mesa Gallium driver

Suggested layout once a current Mesa tree is added:

```text
src/gallium/drivers/via9/       # pipe_screen/context/resource/state/draw
src/gallium/winsys/via9/drm/    # libdrm-free DRM winsys where practical
src/compiler/via9/              # NIR lowering, scheduling, assembly/disassembly
```

### Bring-up order

1. loader/winsys connection, capability query, BO allocation/mapping, and fence
   waits;
2. resource creation and linear transfer paths;
3. command builder plus disassembler, with no rendering;
4. hardware clear/copy and a fixed-state scissored clear;
5. one pre-transformed triangle using the documented immediate-vertex path;
6. render-target, depth/stencil, blend, rasterizer, viewport, and texture state;
7. NIR shader compiler only after an ISA assembler/disassembler is validated;
8. format/modifier expansion, performance work, and higher GL/GLES versions.

Gallium capability bits must describe implemented, tested behavior.  The
marketing term "DX9" is not evidence for any Mesa `pipe_cap`.

### Shader compiler gate

Build ISA work as its own testable component:

- a bit-exact assembler and disassembler;
- instruction metadata describing sources, destinations, types, modifiers,
  control flow, and pipeline hazards;
- NIR lowering to the actually supported arithmetic and texture operations;
- register allocation and scheduling constrained by measured latency/hazards;
- encode/decode round-trip tests and known binary vectors;
- hardware micro-tests for every opcode and modifier before advertising it.

Until that exists, a limited fixed-function Gallium path can validate kernel,
memory, command, and raster state but should not claim programmable API support.

## Milestones and acceptance gates

### M0: reproducible inventory

- all manual registers entered with source pages and generation tags;
- legacy symbols mapped to manual entries;
- conflicts and undocumented constants listed;
- register dump and command decode tools operate offline.

Gate: no untracked "magic" constants in bring-up code.

### M1: safe hardware probe

- PCI bind/unbind, BAR map, chip/stepping identification;
- read-only register dump and firmware-state capture;
- debugfs diagnostics with no raw write interface.

Gate: repeated load/unload and suspend/resume do not disturb firmware scanout.

### M2: memory and 2D diagnostics

- BO allocation/mapping and CPU coherency established;
- bounded 2D fill/copy or DMA self-test with guard regions;
- first fence/completion mechanism and timeout path.

Gate: randomized guarded copies pass across every supported placement.

### M3: validated 3D submission

- parser, relocation/VA checks, queueing, fences, and reset;
- clear plus pre-transformed triangle from a standalone test;
- malformed command fuzzing cannot escape owned BOs or wedge the machine.

Gate: render-node access is safe for unprivileged users.

### M4: initial Mesa renderer

- Gallium resource and state paths;
- clears, triangles, depth/stencil/blend, and basic textures;
- conservative API/version exposure and software fallbacks where valid.

Gate: selected Piglit and dEQP smoke sets are stable, including parallel
contexts and forced reset.

### M5: programmable pipeline

- characterized ISA, assembler/disassembler, NIR compiler, and shader cache;
- documented shader limits and failure behavior.

Gate: opcode micro-tests plus targeted compiler and conformance suites pass.

### M6: KMS and upstream hardening

- atomic KMS for tested outputs, vblank/page flips, suspend/resume;
- IGT coverage, lockdep/KASAN/KCSAN testing, UAPI documentation;
- reviewable kernel and Mesa patch series with no dependency on the old DRI1
  ABI.

## Patch-series discipline

Keep hardware definitions, generated register data, decode tools, kernel core,
submission, KMS, Mesa winsys, and Gallium/compiler changes in separable commits.
Every change that introduces a register value must cite the database entry or a
saved measured trace.

The kernel UAPI series and the real Mesa consumer must be developed and
reviewed together.  Current DRM policy requires a canonical open-source
userspace implementation for new rendering UAPI; a standalone demo is useful
for bring-up but is not the UAPI's upstream justification.
