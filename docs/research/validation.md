# Validation strategy

## Hardware lab requirements

Use a VX900 machine that can be recovered without a working GPU:

- serial console or independent SSH path;
- remotely controlled power;
- known-good kernel and root filesystem rollback;
- firmware register dumps and photographs/board identification;
- CPU, northbridge, GPU PCI revision, RAM topology, and output wiring recorded.

Do not develop reset or modesetting paths on the only interactive console.

## Evidence-producing tools

Build small tools before a full driver:

- PCI/BAR and VGA register snapshot/diff tool;
- command stream encoder and decoder sharing generated register metadata;
- guarded framebuffer/aperture memory tester;
- 2D fill/copy and DMA micro-tests;
- 3D state packet builder for one clear and one triangle;
- shader binary upload harness once legal ISA samples exist;
- hang injector and reset verifier.

Each run should save machine identity, kernel revision, driver revision,
registers before/after, submitted bytes, BO layout, expected result, observed
result, timeout, and recovery outcome.

## Linux-hosted Windows compiler oracle

`tools/wine_shader_probe.c` demonstrates that the exact PE32 Direct3D UMD can
be loaded under Wine and that its private compiler-object constructor,
shader-model-2 tokenizer, and programmable emitter dispatcher run without a
Windows KMD or GPU.  Use this first for bulk one-variable differential tests:
opcode, register file/index, writemask, swizzle, source modifier, result scale,
and saturation.  Preserve the D3D token stream, normalized 36-byte IR records,
raw ALU/TAU vectors, DLL hash, Wine version, and probe revision for every case.

Emitter output is not automatically final hardware upload.  The harness now
runs the scheduler/finalizer and retains both pre-pass and post-pass streams;
golden vectors must preserve both so rewriting and metadata changes remain
visible.

## Optional Windows golden-vector capture

This is a confirmation path, not a prerequisite for ISA recovery.  If a
suitable setup becomes available, use the exact binaries hashed in
`windows-driver-re.md` on a disposable VX900 Windows 7 installation.  The
ICD/UMD reads dump DWORDs from the active display adapter's
`HKLM\SYSTEM\CurrentControlSet\Control\Video\{GUID}\0000` key.

Capture minimal shader pairs and vary one feature at a time.  Archive the
source/API tokens, shader binary dump, full command capture, registry export,
rendered output, and Process Monitor log together.  First cover MOV and END,
then register indices, writemasks/swizzles/modifiers, scalar/vector ALU,
constants, texture sampling, control flow, and resource limits.  Repeat golden
samples after every inferred assembler/disassembler change.

## Kernel tests

- KUnit tests for packet parsing, length arithmetic, relocations, reserved bits,
  and state transitions;
- property/fuzz tests for arbitrary command buffers and handle tables;
- IGT tests for GEM lifetime, mmap, synchronization, implicit/explicit hazards,
  context isolation, close-while-busy, eviction, and reset;
- lockdep, KASAN, UBSAN, and KCSAN runs where practical;
- suspend/resume, runtime PM, repeated bind/unbind, and forced timeout loops;
- multi-process tests from an unprivileged render-node user.

Add a SWIOTLB-sensitive ownership regression for every structured draw ABI:
create/map a BO, fill its useful pages with a nonzero pattern after GEM_CREATE,
submit a degenerate primitive which cannot cover a sample, and require the
pattern to survive.  This catches the specific invalid lifetime where an
eager bidirectional DMA mapping is followed by a redundant pre-submit
sync-for-CPU that copies stale create-time bounce contents over newer mmap
writes.  Pair it with a real draw and require the lifecycle CPU-at-create,
sync-for-device at submit, sync-for-CPU only after retirement.

Security invariants:

- a submitter can reference only BOs named in its submission;
- read/write intent is enforced and overflow cannot widen a range;
- no packet can write arbitrary MMIO, page tables, ring control, display state,
  or another process's memory;
- failed validation submits nothing;
- reset does not leak stale context or memory contents.

## Shader-specific hard limits

The proprietary compilers are format oracles, not validators.  The new Mesa
compiler and any kernel-owned packet builder must enforce these recovered
limits before constructing an H3 upload:

| item | accepted range | reject/handling rule |
|---|---:|---|
| PS normalized IR | at most 128 records | vendor record 129 crashes |
| PS native ALU | at most 96 records / 384 dwords | vendor silently truncates record 97 |
| PS native TAU | at most 32 records / 32 dwords | vendor silently truncates record 33 |
| VS float constants | `c0..c255` | `c256` wraps unified index 512 to zero |
| VS integer/Boolean | `i0..i15`, `b0..b15` | invalid ids alias each other in fixed native class 3 |
| PS float constants | `c0..c31` | `c32..47` collide with internal/gap ids; `c48..63` wrap |
| VS texture-coordinate outputs | `oT0..oT7` | `oT8` wraps out of the varying class; tested `oT16` crashes |
| PS color outputs | native `oC0..oC3`; expose `oC0` initially | `oC4` aliases depth without the depth flag |
| sampler id | native 0..15; expose 0..7 initially | validate before masking; larger ids corrupt TAU fields |
| VS source/destination modifiers | NONE/NEG; NONE/SAT | all other vendor codes silently parity-alias |
| PS destination modifier | NONE/SAT | all other codes silently parity-alias |
| PS result shift | `0,1,2,3,13,14,15` | reserved 4..12 silently become identity |
| flow | straight-line initially | forbid VS branch records and PS predication/coissue until silicon proof |

H3 subtype 20 and 31 counts must be multiples of four dwords.  Subtype 20 is
bounded to 384 dwords and subtype 21 to 32.  Keep the VS payload count separate
from type-04 A60's active last index; the vendor compiler intentionally uploads
inactive tail records.  Never expose raw H3/MMIO packets through a render-node
UAPI; accept structured shader/state objects and let trusted code build the
packet after these checks.

## Mesa tests

- assembler/disassembler encode/decode round trips;
- NIR lowering unit tests and shader golden vectors;
- resource layout tests for every format/modifier/level/layer combination;
- Piglit targeted first, then broad runs with reproducible skip/fail lists;
- GLES dEQP before claiming GLES versions; GL CTS only for claimed desktop GL;
- render comparison against llvmpipe with format-aware tolerances;
- apitrace replay for real applications after basic conformance stabilizes;
- parallel contexts, process exit while busy, memory pressure, and reset recovery.

## Bring-up safety ladder

Advance only after the previous level is repeatable:

1. read-only probe;
2. write a scratch register with documented harmless semantics;
3. CPU map and verify an owned memory region;
4. bounded 2D write into a guarded off-screen BO;
5. bounded DMA copy;
6. CR packet with no draw;
7. clear into an off-screen BO;
8. pre-transformed triangle off-screen;
9. scan out a previously verified BO;
10. expose unprivileged render-node submission.

Every mutating level needs a timeout and recovery plan before it is attempted.
