# Chrome9 implementation status

Status date: 2026-09-21.  This is a buildable, live-tested foundation with a
bounded kernel-generated 3D submission path, not yet a general GL renderer.

## Exact integration trees

- kernel: `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2`;
- Mesa: `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2`;
- T2 sysroot:
  `/srv/t2/build/x86-64-desktop-26.9.15-desktop-x86-64-nocona-linux/`;
- hardware: VX900 Chrome9 HD `1106:7122`, revision `00`.

## Kernel driver

The new driver is `drivers/gpu/drm/chrome9/`.  It is separate from the removed
DRI1 `via` driver and supports only `1106:7122`.  `CONFIG_DRM_CHROME9=m`
builds `chrome9.ko`; binding requires the explicit `enable=1` module parameter.

The provisional UAPI in `include/uapi/drm/chrome9_drm.h` contains:

- `GET_PARAM`: device, revision, capability bits, and the three PCI BAR sizes;
- `GEM_CREATE`: page-backed, mmap-able buffer creation with zero flags;
- `FIXED_QUAD`: a kernel-generated diagnostic draw into the guarded middle
  page of an exactly three-page native GEM object, plus guarded D16 depth and
  source-alpha blend diagnostic variants;
- `DRAW_TRIANGLE`: one kernel-generated three-vertex draw into a bounded,
  guarded linear XRGB8888 shmem resource.
- blend-aware color and textured triangle successors: validated RT0 ADD with
  `ZERO`, `ONE`, `SRC_ALPHA`, `INV_SRC_ALPHA`, and an API-order channel mask;
  userspace supplies no packet words or GPU addresses;
- a separately gated BGRA8888 destination selector on those successors,
  preserving their ioctl sizes and storing destination alpha through A52
  format 99.

The always-advertised capability bits are `DRM_CHROME9_CAP_GEM` and
`DRM_CHROME9_CAP_GEM_SHMEM_CACHED`; the latter identifies the live-tested
cached-shmem/PTE-bit-25 contract.  `DRM_CHROME9_CAP_FIXED_QUAD` is advertised
only after the persistent engine initialized successfully.  There is still no
raw/general submit ioctl, userspace GPU address, relocation ABI, context, MMIO
mapping, or display ioctl.  The fixed path creates and attaches a DMA fence,
but completes synchronously and quarantines all reachable memory on an
unproved timeout.  By default the module does not request or map a BAR.  A
separate `mmio_probe=1` gate maps BAR0/BAR1 and compares documented status,
CR, 2D, and fence candidates; it exposes no MMIO to userspace.  Explicit lab
parameters also exist for a sequencer reset, a kernel-owned command-reader
test, and an off-screen 2D test.  Engine writes default to proven BAR1, and
BAR0 must not be selected: a BAR0 command-reader experiment hard-stalled the
machine.

The original `selftest_cr=1` raw-DMA experiment correctly failed to fetch:
VX900 PCIe submission uses a translated System Framebuffer address rather than
a raw coherent DMA address.  The new, separately gated `selftest_cr_pcie=1`
path implements VIA's exact H5S2VP1 setup with a temporary 64 KiB page table in
a caller-selected, saved BAR2 region.  On the live VX900 it fetched the ring,
advanced `043c` from `e7fe0040` to `00000080`, asserted the pause state at
`0444`, and returned the engine idle.  It restored the whole table, sequencer
state, and prior PCI bus-master state while leaving the idle reader paused,
matching firmware and VIA's ring replacement path.  STOP is not a teardown
operation on this hardware.  This remains a kernel-owned diagnostic and is not
a userspace submission path.

The same gated path now has three proven packet-level tests.  A header-0 write
changes and restores the VX900 2D mode latch.  A guarded H6 solid fill, after
the exact address-free `VIAInitialize2DEngine` state, wrote and verified all
1,024 pixels of a 32x32 BAR2 surface with intact guards, then restored the
entire saved span.  Finally, the exact address-free H2/type-1 status packet
from `hwmUpdateStatus_h5` completed with CR paused and the engine idle, followed
by the same passing 2D regression.  This proves 3D parameter-engine dispatch,
not yet shader/raster execution; all paths remain probe-only code.

The exact vendor H5S2VP1 address-free initialization sweep has also completed
on the live VX900 when ordered before CR setup.  It clears type-11 CR state,
so the ring must always be re-established afterward.  A first guarded raster
attempt established a useful negative result: an aggregate state emitted from
a synthetic zero-default Xorg object is not draw-safe.  CR consumed it without
an address fault and guards stayed intact, but no pixels were rasterized and a
repeat left `0400=00080031` until reboot.  The test gate is disabled while the
full normal Xorg setter path is captured; see [hardware-lab.md](hardware-lab.md).

Six narrower address-free tests now prove that pixel-shader instruction
storage, vertex-shader instruction storage (including Xorg's complete
four-instruction 7122 passthrough VS), and the no-texture pixel-shader control
transaction are independently accepted.  Mode 5 additionally proves the VS
valid/length control `60800003`; mode 6 proves the exact 92-dword address-free
H6 general/FVF/linkage, H3 float constants, integer constants, VS and valid
sequence.  Every mode returned idle with no engine error and left the guarded
2D regression functional.

The first-work anomaly is now resolved as command-cache coherency, not a
discarded first ring.  VIA's old H5S2VP1 KMD explicitly CLFLUSHes command
pages; adding the equivalent flush made a clean-boot dummy-prime plus staged
shader/2D transaction pass immediately.  The diagnostic also now retains the
ring, GART/PTE state, and bus mastering on an unsafe timeout rather than freeing
memory that the request FIFO may still reference.

The apparent 160-dword normal-path capture was then proved to be the wrong
branch: the harness omitted `state+0x178=1`, which real 7122 initialization
sets through `screen+0x7ac`.  It mixed an H5 FVF with an H6 VS and omitted H6
linkage and constants, explaining the measured VP wait.  A fresh vendor
emission with the flag set produced an exact 188-dword oracle.

That corrected stream has now executed successfully on the live VX900.  The
CR paused, all engine busy bits cleared, `040c` remained zero, all 1,024 target
pixels became `000000ff`, full-page guards remained intact, and the complete
window was restored.  This establishes working command fetch, H6 FE/FVF,
vertex shader, pixel shader, rasterization, render-target writeback, and 2D/3D
ordering for one untextured format.  The next implementation step is to turn
this fixed oracle into validated submit and relocatable BO-backed state rather
than exposing an unsafe raw command ABI.

The same draw now also succeeds with its destination in translated
system-framebuffer space.  GART PTE 0 maps the command page and PTEs 1--3 map a
fresh 12 KiB allocation whose middle page is the target.  `A51[21:20]=01`, all
1,024 pixels are correct, both full-page guards remain intact, and the local
2D regression passes.  A deliberately recorded first run without CLFLUSH saw
only 272 updated pixels from the CPU despite clean engine retirement; explicit
cache maintenance before and after GPU access made the whole target visible.
This validated the SF memory-domain encoding at low DMA while also proving
that ordinary coherent-allocation barriers are insufficient for this internal
non-snooping GPU path.  It does not yet validate arbitrary GEM shmem pages:
later PFNs near 3.7 GiB exposed only 144 and 96 pixels despite WC mapping,
explicit cache transitions, GTI completion, and clean engine retirement.  The
cause was subsequently resolved as a missing cached-PTE attribute, not a
physical ceiling.

An intermediate provisional BO allocator used the DRM DMA helper.  The exact
guarded H6 draw passed into that DMA-GEM object
at DMA `01088000` with all 1,024 pixels and both guards correct.  A live 64 KiB
handle/mmap/CPU round trip also passed.

Windows KMD analysis then proved that RTSF PTE bit 25 is the cached/snooped
attribute: it is set for `MmCached` pages and clear for NC/WC pages.  Repeating
the high-PFN shmem test with a cached mapping and PTEs
`020ddcae/020ddcaf/020ddcb0` produced all 1,024 pixels with intact guards.
A six-page cached-shmem texture/target test at PFNs `ddcca..ddccf` also passed
every pixel, quadrant sample, source check, and guard.  The public allocator is
therefore back on DRM GEM shmem and advertises
`DRM_CHROME9_CAP_GEM_SHMEM_CACHED` (`CAPABILITIES=0x5`).

The exact 232-dword one-texture successor has also passed on the live VX900.
Using its captured local-VRAM source `0x07fc0000` and target `0x07fd0000`, all
1,024 destination pixels changed and four interior samples reproduced the
red/green/blue/white source quadrants exactly.  The source and full-page guards
were unchanged, CR and engine retired cleanly, and all touched VRAM was
restored.  This adds measured H6 texture fetch, sampler, eight-dword vertex
format/linkage, five-instruction VS, texture-address PS instruction, and
textured PS ALU execution to the working command path.

The textured path also passes with both resources in translated SF space.
Both a six-page DMA-GEM object and, decisively, a six-page cached-shmem object
mapped by GART PTEs 1--6 placed the guarded target at
GPU `0x2000` and guarded texture at `0x5000`.  With A51 and texture A18 both
selecting `01=SF`, all 1,024 pixels and four asymmetric quadrant samples were
correct, the source and four guards were intact, and the local 2D regression
passed.  Shmem SG mappings with PTE bit 25 are therefore the measured resource
basis for the initial Mesa renderer.

The first persistent public path is now live-tested as well.  With
`engine=1 engine_gart_offset=0x7ff0000`, the driver uses the vendor-defined
final 64 KiB VRAM table, keeps a two-page coherent ping-pong ring in RTSF PTEs
0--1, and allocates each guarded three-page target from the remaining 64 MiB
SF VA aperture.  Four consecutive `FIXED_QUAD` ioctls alternated command
slots, observed cookie then PAUSE then full-idle retirement, invalidated each
temporary mapping, and produced exactly 1,024 blue pixels with both guard
pages intact.  The live capability value was `0xd`.  Persistent-engine mode
is deliberately reboot-scoped: the module pins itself and suppresses manual
PCI bind/unbind until a proved reset/teardown path exists.

Two minimal derivatives also passed: a constant-source pixel shader filled
the target red, and a three-vertex Gouraud fan rasterized exactly 528 pixels
with interpolated RGB.  The structured `DRAW_TRIANGLE` successor then passed
at 64x64/pitch 256 and NPOT 257x129/pitch 1056.  It accepts only three bounded
signed-16.16 screen coordinates and colors, generates the complete command
stream in the kernel, dynamically maps every guarded shmem page, and verifies
both guard pages after retirement.  The NPOT result preserved all row padding
and produced the expected inside/outside samples.  The current live
capability value is `0x1d`, adding `DRM_CHROME9_CAP_DRAW_TRIANGLE`.

Two further fixed-state derivatives have now passed on the same persistent
engine.  A six-page color+D16 oracle drew red at Z=0, then rejected an
equal-depth green triangle under LESS: 528 pixels were red with depth zero,
all uncovered samples remained color zero/depth `ffff`, and four guards were
intact.  A source-alpha oracle drew 50%-alpha red then green and produced
exactly 528 `00408000` pixels with two intact guards.  Re-running both
variable-triangle sizes afterward passed, with no engine errors or wedge.

The bounded blend successors are now live as well.  Packed post-TNL COLOR0
uses FVF declaration `020041d1`: bit zero performs the required D3DCOLOR
normalization.  The prior `d0` declaration left byte values in 0..255, which
a live A=`00/40/7f/80` sweep exposed as zero followed by three saturated
opaque results.  With `d1`, raw untextured and BGRA-textured source-over both
produced `00807f00`.  A disabled R-mask case produced `00ffff00`, and a
successor-to-legacy COPY transition produced blue.  The current raw binary
also passed sub-saturating ONE/ONE additive RGB exactly: source `00400000`
over `00002000` stored `00402000`.  Guards remained intact.  The matching
kernel commits are `2b2425e` and `8efc38c`.

The separately gated format-99 destination is live too.  Raw COPY stored
`80ff0000`; an A-only mask preserved `44332211` as `80332211`; and canonical
independent-alpha source-over stored `80807f00` over transparent green.  The
Gallium BGRA render-target path passed with the expected float-0.5-to-`7f`
packing.  Kernel commit `edd8da0`, Mesa nested commit `79e0882`, and Mesa
parent commit `3c9197c` identify this accepted extension.

Build:

```sh
make -C src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2 \
  M=drivers/gpu/drm/chrome9 CONFIG_DRM_CHROME9=m modules
```

## Mesa driver

The Gallium driver is `src/gallium/drivers/chrome9/`.  Mesa's option,
pipe-loader descriptor, DRI target, and dril compatibility name are wired to
the kernel driver name `chrome9`.  It:

- verifies device `7122` and both GEM/cached-shmem capabilities through
  `GET_PARAM`;
- duplicates and owns the DRM file descriptor;
- allocates/maps/destroys linear buffer resources and guarded linear
  `B8G8R8X8_UNORM` 2D render-target resources through the Chrome9 UAPI;
- imports/exports linear render resources through PRIME dma-buf handles;
- creates a Gallium context backed by the software `draw` module and a custom
  `vbuf_render` stage that decomposes primitives and sends post-TNL triangles
  through the structured triangle ioctls;
- recognizes the proven primary-color, texture REPLACE and texture MODULATE
  fragment forms, including varying reciprocal-W texture coordinates;
- lowers the live RT0 ADD blend subset and channel masks explicitly, and
  rejects unsupported factors, functions, independent RTs, logic operations,
  multisample blend state, depth+blend combinations, and partial scissor.

Imported unguarded drawable images use a guarded render shadow and a
synchronous CPU copy-back on flush; this is a correctness bridge, not the
final presentation path.  The complete GL/EGL/GBM/DRI configuration builds
and links with the T2 cross compiler and exact sysroot.  The cross file is
`cross-t2-chrome9.ini`; the verified build directory is `objs-chrome9-full`.

## Live acceptance result

`tools/chrome9_drm_smoke.c` was cross-built against the same sysroot.  On the
VX900 it queried the expected device, revision, capabilities, and BAR sizes,
then allocated, mapped, wrote, and verified a 64 KiB GEM object.  The driver
was unloaded after the test and the PCI function returned to its prior
unbound state.  The cross-built `chrome9-screen-smoke` then created the real
Gallium screen and a 4 KiB `PIPE_BUFFER` against the live render node.  Its
expanded successor also created a guarded 257x129 XRGB8888 render resource,
verified pitch 1056, page-sized leading/trailing guards, edge-pixel mmap
round trips, and left the live v4 engine healthy.  Exact measurements and
persistent artifact hashes belong in
[hardware-lab.md](hardware-lab.md).

The expanded fixture then created a real `pipe_context`, exported and
re-imported the guarded render BO through PRIME, installed TGSI passthrough
VS/FS state, viewport, framebuffer, vertex elements and a user VBO, and called
`draw_vbo`.  The Gallium draw/vbuf path changed 16,641 pixels through the
kernel ioctl; an interior sample was `00408040`, all resource checks passed,
and the engine remained healthy.  The same draw then passed twice in one
fixture: once into an imported unguarded PRIME image through the guarded
shadow plus flush copy-back, and once directly into the guarded native BO.
Context creation, color/D16 rendering, texture REPLACE/MODULATE, varying-RHW,
RT0 source-over blending and PRIME are therefore live.  The blend run produced
`007f8000`, a valid one-LSB rounding complement to the raw `00807f00`, and its
textured BGRA successor also passed.  Mesa nested commit `6c6d99d` and parent
commit `2181d02` identify that accepted userspace.  Actual loader/display
presentation remains a separate correctness gate.

## Next implementation gate

The next kernel change remains a kernel-generated, validated draw ABI rather
than raw command bytes.  It requires, together:

1. generalize the confirmed PTE enable/flush and CPU/GPU cache rules
   sequence into a bounded kernel VA manager;
2. prove the recovered FE30 system-memory fence packet and IRQ acknowledgement,
   retaining `0494` as the fallback/readback candidate;
3. implement timeout diagnostics and a recoverable reset sequence;
4. implement an offline packet parser/relocator, seeded from VIA's Chrome9
   verifier hazard tables, with fuzz tests;
5. replace synchronous per-triangle submission with a validated batched job
   format only after reset and parser coverage are adequate.

The immediate-triangle and initial 2D resource gates are complete; Mesa
context/draw/state wiring can now grow in lockstep with this validated UAPI.
The proprietary Linux VS compiler and Windows PS compiler run as offline
differential oracles; remaining ISA work is tracked in
[via-5.75-shader-re.md](via-5.75-shader-re.md) and
[shader-isa.md](shader-isa.md).
