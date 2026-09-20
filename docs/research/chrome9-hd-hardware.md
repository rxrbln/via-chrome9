# Chrome9 HD hardware model: first pass

Status: incomplete.  This file summarizes facts needed to design the driver;
the register database will carry exhaustive bitfield-level detail.

## Identity and address spaces

The VX900 manual identifies the GPU as VIA vendor `1106`, device `7122`.

VX900 Part I describes three manual apertures.  Its BAR prose/table headings
are internally inconsistent, so PCI configuration offsets and live behavior
are authoritative:

| BAR/manual name | Advertised size | Purpose |
|---|---:|---|
| MB0 | 512 MiB | large graphics aperture described by the manual |
| MB1 | 16 MiB | MMIO, 2D host BitBLT, burst-command area |
| MB2 | 512 MiB | graphics/video framebuffer aperture; 32- or 64-bit form |

The useful low portion of the 16 MiB engine aperture is partitioned as follows:

| Offset range | Block |
|---:|---|
| `0000`-`01ff` | 2D engine |
| `0200`-`03ff` | video-related engines 1 |
| `0400`-`07ff` | 3D engine and Command Regulator |
| `0800`-`0bff` | burst command area |
| `0e00`-`0fff` | DMA/AGP registers 1 |
| `1200`-`13ff` | video-related engines 2 |
| `1c00`-`1dff` | WMV motion compensation |
| `1e00`-`1fff` | CBU rotation |
| `2200`-`23ff` | extended video engines 1 |
| `2e00`-`2fff` | DMA/AGP registers 2 |
| `3200`-`33ff` | extended video engines 2 |
| `83cx`-`83dx` | memory-mapped VGA I/O |
| 2-4 MiB | 2D host BitBLT aperture |
| 4-8 MiB | burst command aperture |

Do not merge equal low-byte offsets across those blocks.  In particular,
MMIO `0014` is the M1 2D destination-base register, while MMIO `0214` is the
packed IGA1 display-start alias recovered from the Windows KMD and live VGA
console.  Header-0 command `0014` addresses the former; it does not acquire a
hidden `+0200` bias.  Live `0214=00000370` decoded to display byte offset
`0x6e0` and was changed through legacy CR0C/CR0D by vgacon, independently of
3D submission.  Header 1 instead targets the host-BitBLT aperture at
`200000+`, so it is not a route to `0214` either.

On the target, PCI BAR0 and BAR1 are each 16 MiB and BAR2 is 128 MiB.  The
manual maps MB1 at PCI configuration offset `14h`, which is Linux BAR1.  BAR1
also accepted documented 2D register writes.  A command-reader write through
BAR0 hard-stalled the platform, while BAR0/BAR1 read signatures happened to
mirror.  Therefore all engine writes are restricted to BAR1; matching reads
are not evidence that BAR0 is a second safe engine aperture.

## Command Regulator and 3D programming port

The 3D/CR base is MMIO `0400h`.  A parameter transfer selects:

- an 8-bit starting parameter address (`HParaAdr`);
- an 8-bit special offset (`HParaOS`);
- an 8-bit parameter type (`HParaType`);
- an 8-bit subtype (`HParaSubType`);
- a stream of 32-bit parameter words.

CR programming uses offsets `1c`-`3b` relative to the 3D base and does not
enable the 3D clock.  3D programming uses offsets `3c` onward and enables the
3D clock.  Thus the familiar legacy aliases are MMIO `043c` (transfer setup)
and `0440` (first transfer word).

Important type classes are:

| Type | Purpose |
|---:|---|
| `00` | inline primitive vertex data or vertex indices |
| `01` | non-texture 3D state |
| `02` | texture stage/sample state; subtypes select up to 16 stages; subtype `fe` carries general texture state |
| `03` | palettes, tables, program/constants, vertex-stream descriptors |
| `04` | vertex format, primitive, viewport, and clipping state |
| `08` | CDC/PPC diagnostic or performance controls |
| `10` | command decoded before the CR |
| `11` | framebuffer swapping and CR miscellaneous commands |

The documented draw sequence is: load global/state parameters, stream vertex
data or indices using type `00`, and let the CR issue a fire when a complete
primitive is assembled.  A primitive-list end is generated from vertex-count
state or an explicit stop command.

## 3D state blocks

The VX900 register descriptions expose at least these functional groups:

- engine enable and cache control;
- Z/W buffer address, pitch, format, location, compression/cache, bias, and
  compare operation;
- stencil compare, masks, and pass/fail/Z-fail operations;
- alpha test and color/alpha blending;
- destination/render-target address, pitch, format, masks, dithering, and ROP;
- fog, line pattern, point/line/triangle primitive state, culling, and shading;
- up to four documented render-target state groups;
- texture formats, addresses, pitch, mip levels, filtering, wrapping, border,
  LOD, bump mapping, and texture combiners;
- 16 texture stage and 16 texture sample selectors in the type/subtype space;
- flexible vertex formats, up to 16 vertex streams, indexing, primitive setup,
  viewport transformation, and clipping;
- pixel/vertex shader upload selectors named by the CR table, but not usable
  from the supplied VX900 descriptions because their encodings are absent or
  marked reserved.

This is a register-space inventory, not a claim that all combinations work or
that all resources operate concurrently.

## Memory domains and address fields

The 3D registers repeatedly distinguish:

- **S.L.**: System Local Frame Buffer;
- **S.F.**: System Dynamic Frame Buffer;
- **L.L.**: Local Memory Local Frame Buffer;
- values marked reserved as System Memory in some fields.

Addresses have block-specific units (bytes, 16 bytes, 32 bytes, 256 bytes,
1 KiB, tiles) and many registers split an address across multiple parameters.
For example, the VX900 Z-buffer base is documented in 256-byte units and its
linear pitch in 32-byte units; texture mip offsets must be 256-byte aligned;
vertex-stream bases are in 1 KiB units and pitches in four-byte units.
The texture subtype-10 table contains exactly L1--L8, while A30 can name levels
through L11; vendor builders emit at most nine total levels and leave the
L8-rooted tail implicit.  See
[resource-layout-clear-cache.md](resource-layout-clear-cache.md) for the exact
allocation and validation limits.

The live target reports `SR68=c0`, a 128 MiB SL size, and
`SR6D:SR6F=00:07:00`, which decodes to SL physical base `e0000000`.  BAR2 is
also 128 MiB at CPU address `f0000000`; CPU BAR addresses and GPU SL addresses
are therefore not interchangeable.  `SR6A:SR6C=00:00:00` shows that firmware
did not leave an RTSF/GART table active.  Coherent pages allocated with a
32-bit DMA mask are reached by mapping their page-frame numbers into the SF
translation table and using location `01b`; BAR2 targets use SL location `00b`
plus an SL-relative address.

VIA's 2010 Linux Chrome9 kernel source resolves the VX900 table programming:
the table occupies 64 KiB in framebuffer memory, has 16,384 32-bit entries,
and therefore covers 64 MiB of SF virtual space with 4 KiB pages.  Invalid
entries are `80000000`; valid entries contain the physical page-frame number
masked with `3fffffff`.  `SR6A`, `SR6B`, and `SR6C.bit0` encode table-address
bits 12 through 28.  The KMD uses `SR7B[3:0]=4` for the 64 KiB size even though
the VX900 manual marks that field reserved, an explicit source/manual conflict.
The manual defines `SR6C.bit7` as table write protection; the KMD clears it
while changing the table and restores it afterward.  `SR6F.bit7` is the GTI
cache-flush/invalidate request and must be observed clear before setting it.

This path is now measured, not merely static: the live VX900 fetched a coherent
page mapped at SF address zero, advanced its current pointer to `00000080`,
paused at offset `70`, and returned idle.  The temporary translation table and
sequencer state were restored afterward.  The Windows `02000000` observation
belongs to a different KMD bookkeeping/encoding context and must not be mixed
into the confirmed Linux VX900 PTE format.

The same path now executes header-0 2D work.  VX900 selects the Xorg H6 packet
layout: mode `0004`, pitch `0008`, dimension `000c`, destination position
`0010`, destination base `0014`, foreground color `0058`, and command `0000`.
The older color offset `004c` is not valid for this path.  A successful 32-bpp
solid fill used mode `00000300`, base `SL_offset >> 3`, pitch
`(bytes >> 3) << 16`, inclusive width/height minus one, and command
`f0002001`.  Xorg's address-free VX900 2D initialization is required first;
without it all state latched but the engine produced no pixels.

## Fixed-function and programmable paths

The old UniChrome state header and Mesa driver align well with the fixed
function subset: depth/stencil, alpha test, blend, fog, two texture units,
texture combiners, immediate vertices, and CR commands.  VX900 expands the
state substantially and names shader program stores and multiple streams.

The manuals do not provide the bit-level pixel or vertex instruction ISA.  A
static audit of the supplied Windows driver confirms that its OpenGL ICD
contains the ARB program compiler and that the OpenGL and Direct3D binaries
contain byte-identical native backend routines.  A modern NIR compiler
therefore requires one of:

1. a compatible public ISA description found elsewhere;
2. legal black-box characterization of an existing compiler/driver;
3. systematic hardware experiments that recover encodings;
4. an initial restricted fixed-function implementation while ISA work proceeds.

Option 2 now has native offline Linux VS and PS compiler harnesses under
`qemu-i386`, plus the independent Wine/Windows PS oracle.  The first corpus has
26/27 pixel programs byte-identical across the Linux and Windows compilers;
LRP has a documented generation-dependent lowering.  Controlled GPU captures
remain numerical confirmation.  Option 4 remains the safe initial Linux
bring-up path while the remaining ISA fields are decoded.

## Windows-driver additions

The supplied VX900 KMD recognizes AGP/DMA command headers with signatures
`fe000000` through `fe050000` and `fe070000`, plus padding/control classes
`cc000000`, `cd000000`, `ce000000`, and `fef60000`.  It requires 256-byte DMA
alignment in the decoded path and uses `cc000000`/`cc0000cc` padding.  Header 6
is recognized as a boundary inside one decoder but has no top-level handler.

Type-03 uploads contain a count, an `HParaOS:HParaAdr` selector, a dummy word,
and a streamed payload.  Static code establishes 128-bit pixel ALU and vertex
instructions, 32-bit pixel texture-address instructions, four pixel-constant
banks, 256 vertex `vec4` float constants, and 16 packed integer constants.  A
partial pixel ALU field map and exact analysis anchors are in
`shader-isa.md`.

The UMA patcher exposes several physical-address encodings: direct, aligned,
right-shifted, low-24/high-byte split, and a nibble-merge format likely tied to
tiling.  Its resource IDs include map base, 2D source/destination, four render
targets, texture buffer, six cube faces, and texture layout.  See
`windows-driver-re.md` for the versioned selectors and analysis addresses.

For VX900 device `7122`, the Windows `QueryCurrentFence` path reads MMIO
`0494`; older chips use `0474`.  This is a high-confidence register candidate,
not yet a complete fence protocol.

## Display, 2D, DMA, and video blocks

Part I documents the VGA-compatible display engines, extended CRTC registers,
IGA1 MMIO display registers, 2D engine, DMA channels, CBU rotation, LVDS/DVI,
and DisplayPort.  Part II documents video overlays, capture, and HQV.

These blocks matter to a 3D driver even when their user-facing features are
deferred:

- display owns scanout buffers and vblank;
- 2D can provide early copies/fills and diagnostic traffic;
- DMA may move data between memory domains;
- shared interrupt and reset registers affect isolation and recovery;
- firmware may leave video/HQV state active.

## Release-blocking unknowns

- why BAR0 mirrors BAR1 reads and which non-engine function BAR0 serves;
- allocation and lifetime policy for the now-confirmed 64 MiB SF translation
  space, plus CPU/GPU cache rules;
- full meaning and normal-submission use of the recovered DMA packet grammar;
- fence producer, interrupt/ack protocol, and wraparound behavior around the
  recovered VX900 `0494` current-fence candidate;
- reset sequence and the scope of each reset bit;
- header-6 meaning, packet bounds, privileged-register filtering, and
  VX900-specific command hazards;
- tiling equations, macro/micro-tile dimensions, and compression metadata;
- shader instruction encodings and shader resource limits;
- context state that is not captured by documented parameters;
- clock/power dependencies and resume ordering;
- stepping-specific workarounds.
