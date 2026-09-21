# VX900 hardware lab

## Current target

Inventory date: 2026-09-20.  The development target is reachable as
`root@192.168.1.230` and boots T2/Linux 26.9.15 with kernel
`7.2.5-t2`.  PCI function `0000:00:01.0` is VIA `1106:7122`, subsystem
`1106:7122`, revision `00`.

Measured PCI resources are:

| BAR | CPU address | Size | Flags |
|---:|---:|---:|---|
| 0 | `fd000000` | 16 MiB | memory, non-prefetchable |
| 1 | `fc000000` | 16 MiB | memory, non-prefetchable |
| 2 | `f0000000` | 128 MiB | memory, prefetchable |
| ROM | `000c0000` | 128 KiB | disabled |

PCI I/O and memory space were enabled in the firmware state, but bus mastering
was disabled (`PCI_COMMAND=0003`).  The experimental driver enables it only
while bound and clears it on teardown.  MSI is present but disabled.  No kernel graphics driver was bound,
`viafb` was merely available, and there was no `/dev/dri` or `/dev/fb` node.
The BAR sizes are **Measured**.  The manual's PCI configuration offset `14h`
identifies Linux BAR1 as MB1, the engine aperture.  BAR0/BAR1 reads mirror on
this machine, but only BAR1 is safe for engine writes; a BAR0 CR write caused
an immediate platform stall.

The machine is netbooted from an NFS root with overlay images.  SSH remained
available while the VGA function was bound to the discovery-only driver.
Serial console and remotely controlled power have not been established, so
engine writes, reset experiments, and modesetting remain gated.

## First live driver test

The matching T2 kernel tree produced a module with vermagic
`7.2.5-t2 SMP preempt mod_unload`.  The experimental module was inserted with
`enable=1`, created `card0` and `renderD128`, and did not map or access MMIO.
The smoke client measured:

```text
device                  0x7122
revision                0
capabilities            0x1 (CPU GEM only)
BAR0                    0x01000000
BAR1                    0x01000000
BAR2                    0x08000000
GEM mmap cookie         0x100000000
64 KiB CPU round trip   passed
```

The module was unloaded automatically and `lspci -k` confirmed that the PCI
function was unbound afterward.  Reboot-persistent, but deliberately
uninstalled, artifacts are stored on the target at:

```text
/root/tmp/chrome9/chrome9.ko
/root/tmp/chrome9/chrome9-drm-smoke
/root/tmp/chrome9/mesa/libgallium-26.2.2.so
/root/tmp/chrome9/mesa/libdril_dri.so
/root/tmp/chrome9/mesa/chrome9-screen-smoke
```

Nothing in that directory is loaded automatically.

A later clean-boot run enabled the separate `mmio_probe=1` read-only gate.
BAR0 and BAR1 returned the same status signature; the current clean baseline
on BAR1 is:

```text
0400 engine status   04000000
0404 FIFO empty      00000000
0408 FIFO full       00000000
040c engine error    00000000
0410 FIFO status     00000000
0494 current fence   0f3e9fdf
```

No write was issued in that run.  Identical reads did not establish identical
write semantics; subsequent experiments proved that assumption unsafe.

SHA-256 at deployment:

```text
cc4bd28f0b4b75428aaf8cab03e4993d6154da56ff1ded29e6ff01eee605ab7a  chrome9.ko
1d1467a9b729b90e6367ccfe121b75aa2e39827c32e43c1140f2efe0af83d059  chrome9-drm-smoke
a8fa0040c838fd6c790babe844f57abfb6df39e3b9f815ea98e5471ba1da0cf8  libgallium-26.2.2.so
271eb4352197b822a2db7b4fef78e62c624896a969b9fafd74aed75de6be1097  libdril_dri.so
2f01da3d8d8cb71e5f80b530294bc0770b45815e5e78010c93fbc92a1f4f4348  chrome9-screen-smoke
```

The Mesa smoke executable subsequently opened `renderD128` through
`chrome9_screen_create`, verified PCI `1106:7122` and `accelerated=0`, created
and destroyed a 4 KiB Gallium `PIPE_BUFFER`, and exited successfully.  This is
a live test of the Mesa UAPI consumer; it is not a rendering test.

## Memory-controller state

The target's relevant sequencer and PCI values are:

```text
SR14=00  SR68=c0  PCI_B0=01  PCI_B2=60
SR6A=00  SR6B=00  SR6C=00
SR6D=00  SR6E=07  SR6F=00
SR3F=ff  SR4F=5f  SR59=df
```

`SR68=c0` and PCI `B2=60` agree on a 128 MiB SL framebuffer.  `SR14=00`
retains an older 512 MiB encoding and is not the usable aperture size on this
system.  `SR6D:SR6F` decodes to SL physical base `e0000000`; the RTSF/GART
table base in `SR6A:SR6C` is zero, so firmware left no system-page table
active.  BAR2 maps 128 MiB at CPU address `f0000000`, not at the GPU's SL
physical base.

A later read-only PCI-config dump of the VX900 DRAM controller `D0F3`
(`1106:3410`) returned `84..85=e801`, `a0..a1=df02`, and
`f8..fb=00218201`.  Per the VX900 system programming manual these mean:

- nominal low top `e8000000`, with framebuffer-subtraction decode enabled;
- 128-MiB UMA framebuffer, making the effective below-4G DRAM top and UMA
  base `e0000000`;
- hardware memory remapping enabled for `200000000..218000000`, the 384 MiB
  displaced by the below-4G hardware hole `e8000000..ffffffff`.

The values reconstruct the Windows KMD's private PFN rule as enabled threshold
PFN `200000` and replacement base PFN `e8000`.  Consequently live render PFNs
`ddf92`, `de4fe`, and `1088` are all identity mapped.  The firmware remap is
active but is not the cause of the partial high-shmem SF writes.

## Engine-write experiments

The following sequence is retained because negative results constrain the
next probe:

1. Direct 2D writes through BAR0, initially with pitch bit 31 copied from an
   older generation, hard-stalled the machine.
2. BAR1 accepted the documented 2D registers.  Pitch bit 31 put the VX900
   destination in reserved location `10b`, yielding engine status `04000012`
   and no pixels.  Pulsing documented `SR4E.bit2` reset the 2D engine and
   restored `04000000`.
3. Corrected direct and quick-start BAR1 writes latched but did not execute,
   showing that register acceptance is not equivalent to a valid submission.
4. An incomplete PCI-packaging attempt through the burst aperture stuck CR at
   `04000010`; it is not a usable submission method.
5. CR rings placed in the BAR2 SL aperture, tried with both SL offsets and
   decoded physical addresses, did not start.  Their pause/status remained
   unchanged and no target pixels changed.
6. Repeating a CR write through BAR0 hard-stalled the complete platform.  No
   future test may select `engine_bar=0`.

The first corrected stop-stream test used a 32-bit coherent DMA page and SM
location `10b`.  The trigger was accepted: engine status changed from
`06000000` to `06000010`.  It did not fetch the page: current address `043c`
remained `00dd1200`, `0444` stayed zero, and the idle/status polls timed out.
The host and SSH remained responsive, and clearing PCI bus master made the
state quiescent enough to reboot.  This localizes the failure to the ring's
memory path rather than the BAR1 CR programming sequence.

The missing memory-path detail was recovered from VIA's 5.75.32.87a Linux
Chrome9 source.  On H5S2VP1/VX900 the CR does not fetch a raw coherent DMA
address.  It fetches a System Framebuffer (`SF`, location code `01b`) virtual
address translated by a framebuffer-resident page table.

## First successful command fetch

An explicit `selftest_cr_pcie=1` gate installed a temporary 64 KiB page table
at BAR2 offset `07ff0000`, mapped one coherent 4 KiB command page at SF virtual
address zero, performed the documented GTI invalidate/enable sequence, and
submitted the source-driver's dummy header and pause layout.  The result was:

```text
VX900 PCIe CR ring dma=0x0000000001013000 pte=00001013
gart=0x7ff0000 pause=0x70 current=e7fe0040->00000080
cr=c0000000 engine=00000000 pause_ret=0 idle_ret=0
```

This proves all of the following on the target hardware:

- the BAR1 CR programming path is functional;
- a 64 KiB framebuffer-resident table maps a 64 MiB SF GPU address space;
- the valid PTE payload is the 4 KiB physical page-frame number;
- the CR fetches location `01b` through that translation and parses the dummy
  header/padding sufficiently to reach the pause at offset `70`;
- `043c` is the CR fetch/request pointer (it may run ahead into the 34-entry
  request FIFO), while `0444.bit31` reports pause;
- the engine can pause and become idle without a wedge.

The test saved all 64 KiB at `07ff0000` before use.  The live region and saved
copy had identical SHA-256 afterward:

```text
e30feb5852c89eef4273269f302f0967fc176668cc2b8d6eb7d39a2882f090f3
```

`SR6A/SR6B/SR6C/SR7B`, the VGA index register, the prior PCI bus-master state,
and the table contents were restored.  PCI command returned to `0003`.

A later lifecycle check corrected one wrong teardown assumption.  Firmware
boots with an idle CR already paused (`0444=c0000000` or `80000000` depending
on the readback group).  VIA's source replaces all ring fields in that state.
Changing the pause ID to STOP without immediately installing a replacement
ring instead left `0400.bit4` asserted.  Tests now leave the idle CR paused;
they never use STOP as teardown.

## Header-0 and 2D execution

The first header-0 probe changed the VX900 mode latch at `0004` from zero to
`00000300`, then restored it.  Static Xorg disassembly corrected the foreground
color register from the older `004c` to the VX900/H6 offset `0058`; its readback
remains zero and is probably write-only.  This proves header-0 parsing without
relying on that color readback.

An initial exact H6 solid packet latched but produced no pixels.  Disassembly
of `VIAInitialize2DEngine` showed the missing prerequisite: VX900 requires its
address-free pre-CR/config initialization before 2D work.  With those vendor
words installed, a translated-ring fill succeeded:

```text
VX900 PCIe CR ring ... pause=0xf0 current=00000180->00000300
cr=c0000000 engine=00000000 pause_ret=0 idle_ret=0
VX900 H6 2D fill offset=0x7fe0040 mode=00000300 pitch=00100000
dim=001f001f pos=00000000 base=00ffc008 color=41a7c9e3
cmd=f0002001 bad_pixels=0 bad_guard=0 restored=yes
```

This proves header-0 CR-to-2D dispatch, the VX900/H6 register layout, a linear
SL-relative destination base in eight-byte units, pitch in eight-byte units,
the `f0002001` GXcopy solid command, and the required `006c=00400000` 2D/3D
synchronization state.  All 1,024 pixels matched, both 64-byte guards matched,
and the saved 8 KiB span was restored exactly.  Its persistent backup is
`/root/tmp/chrome9/2d-07fe0000-8k.backup`, SHA-256:

```text
20bfdce9c0abe3bd5100dadac24b90374039f2040960582f1eb792718bbc2164
```

The successful 2D module is
`/root/tmp/chrome9/chrome9-pcie-2d-init-fill-test.ko`, built from local source.

## First successful 3D parameter transaction

The next test inserted the exact address-free packet emitted by the proprietary
Linux DRI function `hwmUpdateStatus_h5` before the trailing CR dummy:

```text
FE020440 0000043C 00010000 CC000000
1C000000 1D000000 CCCCCCCC CCCCCCCC
```

All headers were first aligned to 16 bytes; this also fixes the earlier
experimental stream's misaligned trailing Header-2.  On the live VX900 the
mixed 3D-status/2D-fill ring completed:

```text
VX900 H5/H6 address-free 3D status packet completed
engine_error=00000000->00000000
VX900 PCIe CR ring ... pause=0x170 current=00000200->00000380
cr=c0000000 engine=00000000 pause_ret=0 idle_ret=0
VX900 H6 2D fill ... bad_pixels=0 bad_guard=0 restored=yes
```

This is measured proof that an H2/type-1 packet reaches the 3D parameter
engine and coexists with H0 2D work on VX900.  It does not yet prove shader or
raster execution.  The persistent module is
`/root/tmp/chrome9/chrome9-h2-3d-status-errorcheck.ko`, SHA-256:

```text
ae4fa3679b1ea66f40a6199018732e0320a3852e654a6f1e6796f48aab0bf515
```

The next hardware gate is a guarded, kernel-owned 3D triangle using the exact
recovered H5/H6 render-target, fixed state, shader, FVF, and immediate-vertex
stream, followed by an independently observable fence.

## First guarded raster attempt: safely rejected vector

A QEMU capture of the individual Xorg emitters was assembled into a 7122 H6
quad stream at BAR2 offset `07fd0000`.  Before testing, the complete target plus
one 4 KiB page on each side was saved to
`/root/tmp/chrome9/3d-07fcf000-12k.backup` (SHA-256
`f25d83645ac704acd88c187c17e529be5b4508257783af89b3cc842bfc7832c4`).
The command page retained `0x220` bytes of valid CR read-ahead beyond pause.

The stream contained the proven H6 2D prefill, 2D-to-3D sync, four immediate
vertices, clip state, Xorg's no-texture PS, a synthetic forced aggregate state,
H6 FVF/internal constants/VS, and the address-free status packet.  CR consumed
the stream and paused, `040c` remained zero, and neither 4 KiB guard page was
touched.  However, all 1,024 target pixels remained the 2D prefill
`41a7c9e3`; no raster write occurred.  A repeat made the failure observable as:

```text
pause=0x470 current=00000680->000005c0 cr=c0000000
engine=00080031 pause_ret=0 idle_ret=-110
first=41a7c9e3 last=41a7c9e3 red_rgb=0 unchanged_2d=1024
bad_guard=0 restored=yes
```

The target and both guards were restored exactly; the post-test 12 KiB hash
matched the persistent backup.  The host stayed reachable and a controlled
reboot cleared the busy 3D pipeline.  After reboot, the VRAM window still
matched the backup byte-for-byte.

The failure is now attributed to using a synthetic zero-default aggregate
state object: executing an emitter proves packet shape, but bypassing Xorg's
destination/drawing/composite setters does not produce a draw-safe state.
Consequently `selftest_3d_quad` is disabled in the kernel probe until the
complete 7122 setup path is replayed.  No individual bit will be changed on
hardware by trial and error; the next vector must be captured after every
normal Xorg setter and compared with the manual and proprietary DRI path.

## Isolated shader-store transactions

Four address-free shader-state subsets were subsequently split out of the
rejected draw vector.  None includes render-target addresses, FVF state,
vertex data, a primitive/fire command, or rasterization.  Each was followed
by the already proven guarded H6 2D fill so that failure to resume useful
engine work would be visible independently of register readback:

| `selftest_3d_store` | Isolated transaction | Live result |
|---:|---|---|
| 1 | Xorg no-texture pixel-shader instruction store | paused, idle, `040c=0`, 2D pass |
| 2 | zero/NOP vertex-shader store | paused, idle, `040c=0`, 2D pass |
| 3 | complete four-instruction Xorg 7122 no-texture VS store | paused, idle, `040c=0`, 2D pass |
| 4 | Xorg PS control registers plus no-texture PS store | paused, idle, `040c=0`, 2D pass |
| 5 | complete H6 VS store plus `60800003` valid/length control | paused, idle, `040c=0`, 2D pass |
| 6 | exact H6 general/FVF/linkage, float/integer constants, VS and valid control; no vertices | paused, idle, `040c=0`, 2D pass |

Modes 1 and 2 paused at ring offset `0170`; modes 3 and 4 paused at `01f0`.
All four retained the same raw observations at `045c/0460/0464`:
`5778e8b1 bbfee773 1f2a0f31`.  Those three values are recorded without names
because their semantics have not been established.  The full VS transaction
uses the corrected Xorg words:

```text
00380150 00000000 00000000 0008010f
0008014c 00100150 00384154 0004021f
0008414c 00000000 00000000 0008422f
0008814c 00000000 00000000 0008423f
```

The earlier store test used `00004154` in place of the second instruction's
third word.  A later capture through the complete normal Xorg setup path and
static inspection of `via3DEmitVetexShader_H6` both establish `00384154`:
the emitter explicitly installs byte `38` at scratch offset `0172`.  The
address-free store transaction accepted the incomplete word, but that is not
evidence that it describes a valid executable instruction; the source and
future probes now use the normal-path value shown above.

Static tracing shows that `60800003` is the HParaType4 subaddress-`60` VS
valid/length control, with low nine bits equal to instruction count minus one;
it is not part of the instruction-store payload.  Mode 5 subsequently proved
that the complete VS plus this activation word retires safely.  Mode 6 then
proved the entire 92-dword, address-free H6 delta recovered from the corrected
vendor capture, including reserved-in-the-manual linkage words `61/62` and H3
float constants.  Its work ring paused at `0370`, `0400=0`, `040c=0`, and the
guarded 2D regression passed.

The tested module is stored as
`/root/tmp/chrome9/chrome9-3d-store-probes.ko`, SHA-256:

```text
2207092af9fe035f2a49164ba39a136e29ada31a51e141b7b8cc8a1fadb666f9
```

One cold-boot sequence exposed an additional lifecycle hazard.  Immediately
after the complete vendor 3D initialization sweep and CR setup, the first
mode-1 ring advanced the CR pointer and returned idle but its trailing 2D fill
did not execute (2D registers stayed zero and all 1,024 pixels differed).
A following no-upload baseline ring passed, and repetitions of modes 1--4 all
passed.  The current evidence is consistent with the first replacement ring
acting as a command-reader priming transaction after initialization, but one
observation is not sufficient to define that behavior.  Bring-up must keep
initialization/priming separate from the first observable workload until this
is reproduced and the exact ordering dependency is isolated.

## Command-cache coherency and first-work dispatch

Three bounded cold-boot tests isolated the apparent first-ring anomaly.  A
dummy-only ring reached PAUSE at `0070`; publishing a second command suffix
and changing only A64/A65 made `043c` reach the later PAUSE, but none of the
H0/H2 commands had side effects.  Re-arming the owned paused ring at the suffix
start behaved the same way.  In both cases the engine returned idle, guards
were intact, and the mapping could be restored safely.

The missing operation is explicit in VIA's H5S2VP1 KMD:
`via_chrome9_clflush_pages()` flushes every CPU cache line of a command buffer
before execution (or uses `wbinvd` when CLFLUSH is unavailable).  The target's
GPU is not reliably snooping CPU cache.  After adding the same x86 cache-line
flush around command publication, the clean-boot two-phase test passed on its
first attempt:

```text
prime pause=0x70 current=00000080 status=c0000000 engine=00000000
pause_ret=0 idle_ret=0 dispatch_ret=0
work pause=0x270 current=00000000->00000340
cr=c0000000 engine=00000000 pause_ret=0 idle_ret=0
2D mode=00000300 ... bad_pixels=0 bad_guard=0 restored=yes
```

This proves that `dma_wmb()` alone is insufficient for this SF command path,
even with a `dma_alloc_coherent()` page.  A production command-submission path
must preserve the architecture-appropriate cache maintenance; PTE publication,
GTI invalidation, and CPU command-cache flushing are separate operations.  The
tested persistent module is
`/root/tmp/chrome9/chrome9-two-phase-clflush.ko`, SHA-256:

```text
9cc4f49d0e399dad12756816a589eaf4a1a73ec6acdf11ddcf42ba73d5984d3a
```

## Rejected 160-dword H5-branch capture

The next draw used a 160-dword body captured after the apparent normal setter
sequence.  It included the corrected blend, clip, VS, PS and vertex words, but
the offline harness had failed to set the Xorg screen generation flag.  The
real `viaExa3DInit` writes `screen+0x7ac=1`; because the state object begins at
`screen+0x634`, this is `state+0x178=1`.  Leaving it zero selected the old H5
FVF branch while still uploading the H6 VS.

The result still stopped at the same front/vertex state:

```text
prime pause=0x70 current=00000080 status=c0000000 engine=00000000
work pause=0x470 current=00000000->00000600 cr=c0000000
engine=00080031 pause_ret=0 idle_ret=-110 error=00000000
2D prefill mode=00000300 base=00ffa000
first=41a7c9e3 last=41a7c9e3 red_rgb=0 unchanged_2d=1024
bad_guard=0 restored=yes
```

The complete 12 KiB guarded span was restored; no poison page changed.  Since
the CR was paused but the VP/front end remained busy, the driver deliberately
retained the coherent ring, PTE, GART programming, and PCI bus mastering until
a controlled reboot.  The target returned healthy afterward.  This is not a
negative result for the real 7122 path: the missing flag also omitted H6 FVF
subaddresses `61/62`, H3 VS constants c256/c257, integer constants, and the
general-state word `05000000`.  The tested rejected module is
`/root/tmp/chrome9/chrome9-exact-xorg-quad.ko`, SHA-256:

```text
70f1c44358b24317457b7a58e0a441c7488a0ec3b8e708f68d0c71bf4e7f74dd
```

## First verified H6 3D draw

Re-running the vendor emitter with `state+0x178=1` produced an independently
audited **188-dword** body.  It differs structurally from the rejected H5
branch and was therefore substituted wholesale.  Before vertices were
enabled, mode 6 above proved every new address-free H6 packet.  The full draw
then ran after a clean reboot with the correct Xorg engine-init order (2D then
3D), a cache-flushed two-phase CR prime, 4 KiB poison guards on both sides of
the destination, bounded waits, and exact backup/restore.

The first submission retired cleanly and wrote `000000ff` at both ends of the
target; its only reported error was the diagnostic expecting the wrong memory
channel order.  After correcting that assertion, the unchanged draw passed:

```text
prime pause=0x70 current=00000280 status=c0000000 engine=00000000
work pause=0x4f0 current=00000670->00000680 cr=c0000000
engine=00000000 pause_ret=0 idle_ret=0 dispatch_ret=0 error=00000000
2D prefill offset=0x7fd0000 base=00ffa000
3D first=000000ff last=000000ff solid_pixels=1024
unchanged_2d=0 zero=0 bad_guard=0 restored=yes
```

This is the first measured proof that the new kernel path executes a complete
Chrome9 H6 vertex/pixel/raster command stream on PCI `1106:7122`.  All 1,024
pixels were the vendor path's solid blue result, neither full-page guard changed, the
12 KiB window was restored, the engine returned idle, and the DRM render node
was created normally.  The verified module is
`/root/tmp/chrome9/chrome9-h6-draw-verified.ko`, SHA-256:

```text
71290715146fa8a0565870d422e12d1e14d5e6366821f3553835b37714eb86b9
```

The exact vector and flag proof are in
[vp-stall-analysis.md](vp-stall-analysis.md).  Static cross-checking resolves
the channel value: destination format `980000` is X8R8G8B8, while type-04 word
`23000f02` selects solid rather than Gouraud shading.  State words
`860000ff/870000ff` supply opaque blue, so the red per-vertex colors are
intentionally ignored and the correct memory result is `000000ff`.

## Translated system-framebuffer render target

The next probe changed only the destination address/domain and page-table
population.  A fresh 12 KiB coherent allocation was mapped by GART PTEs 1--3;
the middle page appeared at SF GPU address `0x2000`, with one full guard page
on either side.  The destination state used `A50=50000020` and
`A51=51100000`, matching the old verifier's `A51[21:20]=01` SF location.  The
command ring remained at PTE 0, and the independent 2D regression remained in
local VRAM.

The first run retired with `0400=0`, `040c=0`, and intact guards, but CPU
verification observed only 272 updated pixels.  This was not a partial draw:
the CPU had initialized the write-back mapping and no cache maintenance had
been performed.  The result is consistent with the already established fact
that VX900's internal SF path does not reliably snoop CPU cache.

With the exact same PTEs, addresses, and 188-dword draw, adding x86 cache-line
writeback/invalidation before GPU access and after retirement produced:

```text
prime pause=0x70 status=c0000000 engine=00000000
work pause=0x4f0 cr=c0000000 engine=00000000 error=00000000
SF gpu=0x2000 dma=0x01088000 first=000000ff last=000000ff
solid_pixels=1024 bad_guard=0
SL 2D bad_pixels=0 bad_guard=0 restored=yes
```

This proves that 3D render-target writes through the PCIe GART work at the
tested low DMA address and that explicit bidirectional cache maintenance is an
architectural requirement for CPU-mapped SF BOs on this machine.  It is a
prerequisite for, but not yet proof of, generic pinned GEM shmem render
targets.  Later high-PFN shmem tests are recorded below.  The passing module is
`/root/tmp/chrome9/chrome9-h6-sf-target-clflush.ko`, SHA-256:

```text
0093be37b4b33185d922d1d4055577af2e7f0f8604dd55fb1f7c5635e43b90bd
```

Two follow-up GEM shmem runs did not generalize this result.  PFNs
`000ddf92/93/94` exposed 144 pixels; write-combined shmem PFNs
`000de4fe/ff/e500` exposed 96 despite complete GTI-flush waiting and explicit
cache transitions.  Both jobs retired with engine/error zero and intact
guards.  The passing coherent PFN was `00001088`.  Subsequent Windows KMD RE
identifies internal RTSF PTE bit 25 (`02000000`) as the cached/snooped
attribute: the nonlocal allocator sets it exactly when its MDL is CPU-mapped
`MmCached`, while NC/WC mappings retain a plain PFN.  The old Linux KMD's
plain-PFN heap is allocated `PAGE_KERNEL_NOCACHE`, not as ordinary WB shmem.
This made cache-policy/PTE mismatch the first discriminator at that stage;
the later PTE25 passes below resolve it and reject upper-address reachability
or shmem DMA/physical-address identity for the tested objects;
see [sf-render-target-coherency.md](sf-render-target-coherency.md).

The driver's provisional DMA-GEM object class then passed the same test.  Its
write-combined contiguous allocation landed at DMA `01088000`, PTEs
`00001088/00001089/0000108a`; all 1,024 pixels and both guards passed with
engine/error zero, and the independent local-VRAM 2D fill also passed.  The
module is `/root/tmp/chrome9/chrome9-h6-dma-gem-target.ko`, SHA-256:

```text
6f3ae9f038e780aa80053bf9721422ddd50a6dc9541e51feaf1c2f5c460ba342
```

A separate no-engine test then allocated and mmaped a 64 KiB DMA-GEM object
through the render node and passed a full CPU write/read round trip;
`GET_PARAM(CAPABILITIES)` returned `0x3`.  This was the provisional public BO
allocator while the high-address/cache distinction remained unresolved.

The recovered cache-policy field resolved that distinction.  Windows writes
RTSF PTE bit 25 (`02000000`) precisely for `MmCached` mappings and leaves it
clear for NC/WC mappings.  A repeat using ordinary cached GEM shmem at DMA
`ddcae000` and PTEs `020ddcae/020ddcaf/020ddcb0` rendered all 1,024 pixels,
with both guards and the local 2D regression intact.  A six-page cached-shmem
texture/target job at DMA `ddcca000`, PTEs `020ddcca..020ddccf`, also passed
all 1,024 pixels, red/green/blue/white samples, source verification, four
guards, and 2D regression.

The public allocator was consequently switched back to DRM GEM shmem.  The
live no-engine ABI and Mesa smoke clients passed a 64 KiB mmap round trip and
reported `CAPABILITIES=0x5` (`GEM | GEM_SHMEM_CACHED`).  The tested artifacts
are `/root/tmp/chrome9/chrome9-h6-shmem-pte25.ko` and
`/root/tmp/chrome9/chrome9-shmem-pte25-abi.ko`; the latter's SHA-256 is:

```text
4890f6e97fc39a6dc6a01d24001e22c39160297e5e276bfc5da19e5931ff631f
```

## First verified textured H6 draw

The exact 232-dword normal Xorg one-texture oracle was then added without
changing its captured local-VRAM layout: a 32x32 X8R8G8B8 source at
`0x07fc0000` and target at `0x07fd0000`.  The test backed up three pages around
the source and the already guarded target, filled the source with asymmetric
red/green/blue/white quadrants, checked the generated body length before any
engine setup, and retained the proven initialization, cache flush, two-phase
CR prime, bounded waits, and restore path.

The live result was:

```text
prime pause=0x70 status=c0000000 engine=00000000
work pause=0x570 current=00000680->00000700 cr=c0000000
engine=00000000 error=00000000 pause_ret=0 idle_ret=0
src=0x7fc0000 dst=0x7fd0000 changed=1024
samples=00ff0000/0000ff00/000000ff/00ffffff
bad_pixels=0 bad_source=0 bad_guard=0 restored=yes
```

This proves texture fetch, address and format state, nearest sampling, the
eight-dword H6 vertex declaration and linkage, texture coordinates, the fifth
VS instruction, the texture-address PS instruction, and the textured PS ALU
path on the real VX900.  The driver registered normally afterward.  The module
is `/root/tmp/chrome9/chrome9-h6-texture-oracle.ko`, SHA-256:

```text
97db1f9b3e425b35ba86f1c7084712b3af295e67b85fdef23461d8ce0be4f543
```

The same oracle then passed with both resources in DMA-GEM-backed SF space.
One 24 KiB allocation at DMA `010c0000` was mapped by consecutive PTEs
`000010c0..000010c5`; target `0x2000` and texture `0x5000` each had a guard
page on both sides.  Texture A18 and render-target A51 both selected `01=SF`.
After a successful CPU cache-publication preflight, the result was:

```text
changed=1024 samples=00ff0000/0000ff00/000000ff/00ffffff
bad_pixels=0 bad_source=0 bad_guard=0
SL 2D bad_pixels=0 bad_guard=0 restored=yes
```

This closes the basic resource-domain gap for Mesa: translated DMA-GEM is
measured for texture reads and color-target writes in the same job.  The
module is `/root/tmp/chrome9/chrome9-h6-sf-texture-fixed.ko`, SHA-256:

```text
cc77856b4887870ad2a775accc061640a455e7a540902855301fbb7ea3f41158
```

## Persistent public fixed-draw path

The reviewed persistent engine was loaded with:

```text
enable=1 engine=1 engine_gart_offset=0x7ff0000
```

Initialization accepted the vendor-defined final 64 KiB GART location and
reported a coherent two-page ring at DMA `01042000`.  The render node exposed
capabilities `0xd` (`GEM | GEM_SHMEM_CACHED | FIXED_QUAD`).  A userspace
verifier created one exactly three-page native shmem BO and invoked the ioctl
four times, deliberately exercising both ping-pong command pages twice.  Each
call verified all 1,024 middle-page pixels and both full guard pages:

```text
pass 0: guarded 32x32 blue quad verified
pass 1: guarded 32x32 blue quad verified
pass 2: guarded 32x32 blue quad verified
pass 3: guarded 32x32 blue quad verified
caps=0xd, four persistent-ring submissions passed
```

The kernel waits for the destination cookie, CR PAUSE, and full engine idle
before invalidating the temporary RTSF mapping and signaling its reservation
fence.  On an unproved timeout it retains the BO, mapping, ring, GART, fence,
and module/device references for reboot rather than freeing GPU-reachable
memory.  Until full reset and state restoration are proved, engine mode pins
the module and disables manual PCI bind/unbind, so this live instance is
reboot-scoped.

Artifacts in `/root/tmp/chrome9/`:

```text
chrome9.ko          8f9b1fd77165bfe38f04056b9e021361620e0ffee850981b886788d648cb8a69
chrome9-fixed-quad  c6249dc3d1949a118717da2a546e84d46656e426b4b0ebe41abaa63c90b67347
```

Two packet-exact fixed-draw derivatives then passed after reboot.  The
constant-source pixel shader filled the complete target with `00ff0000`.  The
three-vertex Gouraud fan changed exactly 528 pixels; the known-inside sample
at `(24,8)` was `003c8044`, while `(8,24)` retained `deadbeef`.  This proves
the six-dword constant-source PS mutation, A24 count three, Gouraud A23 mode,
and color interpolation on silicon.

## Variable guarded immediate triangles

The next structured ioctl keeps the same kernel-owned shader/state envelope
but accepts a validated target width/height/pitch and three signed 16.16
screen-space x/y positions plus colors.  It maps an exact guarded shmem BO of
`PAGE + PAGE_ALIGN(pitch*height) + PAGE`, generates all IEEE-754 vertex words
in the kernel, and exposes no command bytes or GPU addresses.  Both live
cases passed:

```text
64x64 pitch=256: changed=2080 inside=003e8042 outside=00000000, guards/padding pass
257x129 pitch=1056: changed=16577 inside=00407f40 outside=00000000, guards/padding pass
caps=0x1d, variable immediate triangles passed
```

The NPOT case exercises 32-byte row-pitch padding, 36 shmem pages, dynamic
PTE collection with cached bit 25, exclusive clip endpoints, coordinates well
beyond the original 32x32 oracle, and a non-square target.  Every padding
dword and both full guard pages remained unchanged.  Artifacts:

```text
/root/tmp/chrome9/chrome9-triangle-v3.ko  8bbd87e844c533d32987edc9fdde75c006092ee5bb68fa1d8d4f202bd8dd7d63
/root/tmp/chrome9/chrome9-triangle-v3     99f54f383b95f00044dd751175c80327180b0330d8233e93d44e5f535d7e037a
```

## Live D16 depth and source-alpha blend oracles

The v4 module added two kernel-generated fixed-state derivatives without
exposing command bytes or GPU addresses.  The D16 test uses a guarded
color+D16 six-page BO, clears color to zero and depth to `ffff` on the CPU,
then draws the same Z=0 triangle twice: red followed by green with LESS and
depth writes enabled.  The live result was:

```text
D16 LESS/write oracle passed: covered=528 inside=00ff0000/0000 outside=00000000/ffff, four guards intact
```

The alpha test clears a guarded XRGB8888 target, draws 50%-alpha red followed
by 50%-alpha green using `SRC_ALPHA, INV_SRC_ALPHA, ADD`, and verifies every
pixel against the exact expected classes:

```text
SRC_ALPHA/INV_SRC_ALPHA blend oracle passed: covered=528 inside=00408000 outside=00000000, two guards intact
```

Both completed on the same persistent engine.  Module reference count stayed
one, `dmesg` recorded no new Chrome9 fault, and the 64x64 plus 257x129
variable-triangle suite passed immediately afterward.  Persistent artifacts:

```text
/root/tmp/chrome9/chrome9-depth-blend-v4.ko   676655007d22337154bde81aacff59ff59824b200282a18c247255024448ba7c
/root/tmp/chrome9/chrome9-depth-oracle-v4     77c97f68e1ef68dc1e0f4e7accab0f9970fbdbe715558a09bb24ca0ff08ce4ad
/root/tmp/chrome9/chrome9-blend-oracle-v4     7690ce9738d176a2fa36070394ca032934a957a0101f36147bc319dbadf62552
```

The matching Mesa resource layer was then rebuilt and its screen smoke test
expanded to allocate the same guarded NPOT layout through Gallium.  On the
live render node it reported PCI `1106:7122`, created a 4 KiB `PIPE_BUFFER`,
then created a 257x129 `B8G8R8X8_UNORM` render target with pitch 1056,
page-sized leading/trailing guards, and successful first/last-pixel mmap
round trips:

```text
Mesa Chrome9 PIPE_BUFFER and guarded 257x129 XRGB8888 smoke passed
```

The target artifact is
`/root/tmp/chrome9/chrome9-screen-smoke-xrgb`, SHA-256
`c96e366ee550c3b50acaab1aab7b154de5f09d9bb0b611560b833b561ad902bc`.

## First live Gallium draw/vbuf triangle

The next fixture created the real Chrome9 `pipe_context`, exported and
re-imported its guarded 257x129 target through PRIME dma-buf, installed
passthrough vertex/fragment shaders, framebuffer, viewport, rasterizer,
vertex elements, and a user vertex buffer, then called Gallium `draw_vbo`.
The software-T&L/vbuf stage decomposed and packed the post-transform triangle
and reached the structured kernel ioctl:

```text
Gallium draw/vbuf triangle passed: changed=16641 sample=00408040
Mesa Chrome9 context, Gallium triangle, guarded XRGB8888, and PRIME sharing smoke passed
```

An initial run returned `ENOENT` without touching hardware because destroying
a same-file PRIME alias closed the original GEM handle.  Imported aliases are
now non-owning for the bring-up path and remain valid until the screen fd
closes; production code still needs per-screen handle deduplication.  The
passing artifact is `/root/tmp/chrome9/chrome9-gallium-triangle-v2`, SHA-256
`3bf8dfa4a702c6558f3eab3e170b91678375ea137db92d3c756e33132f9c3cfd`.
The module remained at reference count one with no Chrome9 error in `dmesg`.

The follow-up ran the same pipe draw first with the re-imported dma-buf bound
as the framebuffer.  The context allocated its guarded shadow, rendered
16,641 pixels there, synchronously copied them back on flush, and the imported
mapping read the same `00408040` interior sample.  A second direct guarded draw
then produced the identical count/sample.  Artifact
`/root/tmp/chrome9/chrome9-import-shadow-v1`, SHA-256
`bb76cde39d24e76569a24d4e8570e113c7fafdf5acc9ab547b9fd5d86856a6b6`.

## Structured D16 triangles and DMA ownership

The first structured D16 run retired safely but appeared to turn the complete
depth BO into zero.  A degenerate triangle produced the same result, proving
that this was not raster coverage.  GEM creation had eagerly established a
32-bit bidirectional DMA/SWIOTLB mapping while the shmem pages were still
zero.  The old pre-submit sync-for-CPU copied that stale bounce image over the
later userspace `ffff` clear.  GEM creation now returns ownership to the CPU,
structured submission goes directly from CPU cache flush to sync-for-device,
and retirement returns ownership to the CPU.  Fixed kernel-initialized oracle
jobs retain their intentional pre-initialization sync-for-CPU.

The variable-Z packet also emits Type-01 A20 `20007fff`, reconstructing the
normal wide-open far/near clip high bytes used by both proprietary D3D and GL
drivers.  With both corrections, a degenerate 32x32 draw changed zero samples,
whereas the real first draw changed exactly 528 color and depth samples and
stored requested Z `c000`.  The full live suite then passed:

```text
64x64 color-pitch=256 depth-pitch=128: covered=2080 inside=00ff0000/4000, LESS reject and guards pass
257x129 color-pitch=1056 depth-pitch=544: covered=16577 inside=00ff0000/4000, LESS reject and guards pass
caps=0x3d, variable D16 triangles passed
```

The fixed D16 and source-alpha oracles, both variable color-only triangles,
and both direct and imported-shadow Gallium draws passed immediately after it.
The v8 module is
`/root/tmp/chrome9/chrome9-d16-ownership-v8.ko`, SHA-256
`a66a5ee422bd2ac0cd942569c48fbeea91c82856c76db3d146f346c9f12a5f4a`.

## Structured RT0 blend and normalized packed color

The first run of the blend-aware triangle successor executed safely but
treated every nonzero packed alpha as opaque.  A four-point raw sweep made the
failure precise: A=`00` preserved green, while A=`40`, `7f`, and `80` all
produced opaque red.  The equation and A00 enable were therefore working; the
historical FVF word `020041d0` was passing BGRA bytes as 0..255 floats.  The
manual and proprietary declaration mapper require Normalize bit zero,
`020041d1`.

With that one-bit correction, the complete live raw suite passed:

```text
untextured source-over A=80 over green: 00807f00
blend disabled, R-only channel mask:   00ffff00
blend successor followed by legacy COPY: blue, exact
BGRA texture REPLACE source-over:      00807f00
```

All target and texture guards remained intact, and no engine error or wedge
occurred.  The successor zeroes A38--A3F, so the passing successor-to-legacy
transition also validates state closure for the current no-constant ABI.
Kernel commits are `2b2425e` and `8efc38c`.

Mesa then passed the full Gallium acceptance suite on the same hardware.  The
untextured source-over sample was `007f8000`, which differs from the raw
sample only by the permitted one-LSB blend rounding; textured BGRA blending,
D16, REPLACE, MODULATE, varying-RHW, and PRIME all passed.  The accepted Mesa
identifiers are nested commit `6c6d99d` and parent commit `2181d02`.

The persistent module is
`/root/tmp/chrome9/chrome9-normalized-diffuse.ko`, SHA-256
`69e111991ad78bdf3b77d209416ad73d86a197939ff1ca47962914ff32d05247`.

## Live format-99 BGRA destination and stored alpha

The blend successors now consume their first formerly-zero tail dword as a
validated destination-format enum.  BGRX remains zero for backward
compatibility; BGRA selects A52 format 99.  Three guarded raw discriminators
passed on the same VX900:

```text
ONE/ONE additive, source 00400000 over 00002000:     00402000
BGRA COPY, source 80ff0000:                         80ff0000
A-only COPY, prefill 44332211/source 80a0b0c0:     80332211
independent-alpha source-over over transparent green: 80807f00
```

The additive result came from the same current `chrome9_blend_triangle`
binary immediately before the format-99 cases.  It promotes RGB destination
factor ONE and the ONE+ONE ADD equation from static recovery to silicon
evidence; the test remained below saturation so clamp behavior did not mask
an incorrect destination term.

The last result proves that A36/A37 use the canonical independent alpha
factors `ONE/INV_SRC_ALPHA`; accidentally mirroring the RGB source-alpha
factor would have stored alpha near `40`.  All guards remained intact.  The
matching Gallium BGRA render-target test also passed; its float 0.5 color
quantized to byte `7f`, as expected.  Accepted commits are kernel `edd8da0`,
Mesa nested `79e0882`, and Mesa parent `3c9197c`.

## Live fixed-function alpha test and D16 successor

The normalized blend state now reuses its two formerly reserved dwords for an
API-order compare function and an 8-bit reference.  The kernel alone expands
the reference to A33's 10-bit fixed-point field and controls A00 bit 20.  A
new bounded successor combines the same state with the proven linear D16
LESS/write path; no userspace command words or GPU addresses are accepted.

Three independent guarded routes passed on VX900 on 2026-09-21:

```text
solid color, alpha LESS/ref80:       alpha40 red passes, alphaC0 green kills
texture REPLACE, alpha LESS/ref80:   sampled alpha40 red passes, alphaC0 green kills
D16 alpha ordering:                 near alphaC0 red killed; far alpha40 green stores 0000ff00/8000
```

The D16 result distinguishes four failure classes at once: a disabled or
reversed test leaves red/depth `4000`; a color-only kill which incorrectly
writes depth leaves zero/depth `4000`; the correct alpha-before-depth path
leaves green/depth `8000`.  ALWAYS and NEVER controls also passed before the
LESS discriminator.  All color, texture, and depth guards remained intact,
the earlier blend/scissor/sampler regressions passed immediately afterward,
and the kernel recorded no engine fault or timeout.

Gallium then passed the same fixed test through
`pipe_depth_stencil_alpha_state`, including alpha plus D16, BGRX/BGRA targets,
textures, perspective interpolation, scissor, guarded resources, and PRIME
sharing.  This makes `pipe_caps.alpha_test` truthful for every draw
combination the bounded driver otherwise accepts and prevents the state
tracker from lowering compatibility alpha test to an unsupported discard
shader.

Persistent artifacts:

```text
/root/tmp/chrome9/chrome9-alpha-d16-v1.ko       5e1f8f4394f0b4566a8a62def7e7862b70e3c7e2b1d55ee82dc27a70896e0883
/root/tmp/chrome9/chrome9-alpha-depth-v1        aebc6fa4cb2811db0c416da72dd6fc7b22087605fba657dfe76637109c665b62
/root/tmp/chrome9/chrome9-blend-alpha-v1        aa1c2249619c4faa6d0a3c16ed4a93f842af4a0e390c7efe9cb6c74a2fb1cc13
/root/tmp/chrome9/chrome9-texture-alpha-v1      d95895e65b2ba2cf1ed4f86218d8f4405def8cd3cb30d37207401425a86c8346
```

## Native KMS starting point

The live device exposes `card0` and `renderD128` but no connector or CRTC; the
only framebuffer observed during the earlier Xorg experiment belonged to
VKMS.  A read-only BAR1 snapshot found IGA1 base `00000000`, IGA2
`00000002`, interrupt/status `00080002`, and interrupt enable `00000c04`.
The opt-in `kms=1` scaffold now creates a deliberately disconnected atomic
VGA pipe without hardware writes.  Dumb-buffer and framebuffer validation
rejects current shmem BOs rather than presenting them as SL/BAR2 scanout.
Real enablement still requires an explicit, proven BAR2 scanout reservation
plus the narrowly translated VX900 IGA1 timing/FIFO/PLL sequence.

## First live Xorg and GLX gears execution

On 2026-09-21, nested Mesa commit `2af31bf` exposed the already silicon-proven
mixed 32-bit-color/Z16 combination.  Its complete Chrome9 screen smoke passed,
including untextured and textured draws, D16, blend, alpha test, scissor,
BGRA, perspective interpolation, guarded resources, KMS handles, and the
optional same-device dma-buf sharing regression.

With `MESA_GL_VERSION_OVERRIDE=2.1` still explicitly set, Xorg modesetting and
glamor created a direct Chrome9 GLX context and `glxgears` ran for the complete
15-second bounded interval rather than failing visual selection:

```text
GL_RENDERER = VIA Chrome9 HD (validated triangle bring-up)
VisualID 112, 0x70
92 frames in 5.0 seconds = 18.327 FPS
96 frames in 5.0 seconds = 19.054 FPS
```

This is execution evidence, not a truthful GL 2.1 capability claim.  With the
override removed, the current truthful GL 1.3 advertisement still produced no
usable RGB GLX visual and `glxgears` stopped before context creation.  Xorg
also logged `GL_INVALID_OPERATION in st_bind_egl_image` while trying to bind
some dma-buf-backed pixmaps as glamor FBOs; present/copy correctness is not yet
proved.

The live DRM debugfs capture during the gears run showed Xorg FB 40 as native
Chrome9 GEM (`imported=no`), XR24, linear, 1024x768, pitch 4096, offset zero,
and exact size 3,145,728 bytes.  The primary plane and CRTC were active.  This
rules out PRIME offload: rendering, GEM allocation, and KMS all belong to the
same Chrome9 DRM device.  The kernel nevertheless still copied that GEM into
the temporary BAR2 `0x6000000` staging surface.  The required next milestone
is to retain one SF/GART mapping for the same framebuffer and make IGA1 scan
it out directly; dma-buf remains only an optional DRI3 process-sharing
transport, never the presentation architecture.

The first bounded fragment-uniform path then passed on the same hardware at
Mesa nested commit `80c2c9d`.  It accepts only one finite vec4 at fragment CB0
offset zero and lowers it to the proven D1 primary-color input; both user and
resource constant buffers produced exact samples and intact guards:

```text
Gallium c0 solid shader passed: sample=004080bf
Gallium resource c0 solid shader passed: sample=00ff0080
```

This did not remove the drawable-import failure.  A run with
`CHROME9_DEBUG_RESOURCES=1` proved the precise frontend mismatch: because
`pipe_caps.npot_textures` remains false, `dri_init_screen()` chooses
`PIPE_TEXTURE_RECT` for dma-buf imports.  Glamor passes the same EGL image to
`glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ...)`; `st_bind_egl_image()` then
returns `GL_INVALID_OPERATION` because the Gallium resource target and GL
texture target differ.  Immediately preceding format probes accepted BGRX as
both RECT render/sampler and 2D sampler, ruling out format support as the
failure.  The aligned fix is real NPOT 2D texture support—including mip and
repeat semantics—rather than falsely setting the cap merely to change the DRI
import target.

The first resulting NPOT discriminator passed immediately on VX900 at nested
Mesa commit `fe4f05b`.  A guarded 30x18 BGRX image with normalized S/T sampled
the expected quadrant colors for clamp, repeat, and mirrored repeat under both
nearest and linear filtering; the repeat/mirror linear center produced the
expected `0x7f/0x80` four-color average.  Twelve submitted triangles, source
padding, and every guard remained exact.  This promotes single-unit,
level-zero NPOT dimensions plus those wrap/filter combinations to measured
silicon behavior.  Two-unit NPOT and actual mip-level selection remain gates
before advertising full `npot_textures`.

An independent bounded `glxgears` trace with `CHROME9_DRAW_LIMIT=50` proved
that the application is already reaching hardware rather than merely swapping
empty buffers: all first 50 transformed primitives were accepted as
`color+d16` Chrome9 triangle submissions.  Correct window presentation still
depends on resolving the RECT-versus-2D EGL-image mismatch above.
