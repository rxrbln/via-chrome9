# Linux 7122 initialization-to-first-draw prerequisites

This note traces the hardware-visible path which precedes the recovered Xorg
H5/H6 quad body on PCI `1106:7122`.  It exists to distinguish persistent engine
configuration from per-draw packets.  That distinction mattered because the
historical alleged setter-complete quad stopped at `MMIO[0400]=00080031` while
the lab driver neither left the engine in VIA's persistent state nor emitted
the actual `7122` H6 body.  Both defects are now corrected, and the exact
188-dword vendor stream has retired successfully on hardware.

The primary binary is the symbol-rich 32-bit
`binary/5.75.32.87a-u910-55689/bin/via_drv.so`, build ID
`2652605384b383d1377e040b8f8173b856e9a067`.  Addresses are ELF virtual
addresses in that file.  The companion DRI binary is
`via_chrome9_dri.so`, build ID
`f140f76cfb18e3f368245054629d1ef2e67be129`.

## Confirmed defect: the engine initializers were reversed in the lab path

When Xorg has to initialize both engines during a mode set, it always executes
the address-free 2D initialization **before** the full 3D initialization:

```text
VIAModeInit
  VIAInitialize2DEngine
  VIAInitialize3DEngine
  ... normal EXA operation later ...
```

There are two concrete control-flow paths:

- `VIAModeInit:0x558a2` calls `VIAInitialize2DEngine`, then `0x558b6` calls
  `VIAInitialize3DEngine`;
- the alternate path at `0x558da` calls `VIAInitialize2DEngine`, jumps to
  `0x5551e`, and calls `VIAInitialize3DEngine` at `0x55521`.

If the driver records that 2D was initialized already, mode setting may call
only the 3D initializer.  No observed path which performs both calls leaves
the 2D initializer last.  This reinforces that the intended persistent state
is the state left by `VIAInitialize3DEngine`.

The historical lab probe did the opposite in
`chrome9_selftest_pcie_ring()`:

```c
if (chrome9_selftest_init_3d)
        chrome9_init_3d_h5s2vp1(cdev);
if (do_2d_fill)
        chrome9_init_2d_h6(cdev);
```

Both routines write the shared HParaType-FE configure/miscellaneous register
bank.  Consequently this is not an interchangeable ordering difference.
After the exact 7122 3D initializer has cleared the entire FE bank and installed
its final values, the lab's trailing 2D initializer overwrites these entries:

| FE subaddress | state left by 7122 3D init | overwritten by 2D init | exact delta |
|---:|---:|---:|---:|
| `00` | `001004` | `000004` | clears `001000` |
| `40` | `00840f` | `008c0f` | sets `000800` |
| `44` | `000000` | `000000` | none |
| `45` | reset value `000000` | `080c04` | installs 2D value |
| `46` | `000005` | `800408` | replaces the 3D value |
| `47` | `000404` | untouched | remains from 3D init |

The result is a hybrid which neither vendor path uses.  The names of the FE
fields are not public in the VX900 manual, so these values must remain opaque;
the source calls HParaType FE “Configure and Misc Setting.”  Their ordering and
identity are nevertheless exact.

This is one proved defect in the stalled path.  The shader-store probes do not
activate a vertex program or consume a primitive, so they can succeed under
the hybrid configuration.  The complete quad is the first test which makes the
command regulator configure vertices and activate the VP; it is also the first
test to stop with the VP busy.  Initialization order is therefore a confounder
in both the original synthetic quad and the later alleged setter-complete
quad.

That later quad has now also been proved not setter-complete for `7122`: its
offline capture omitted the real H6 FVF/linkage and constant state because the
harness missed `state+0x178=1`.  The observed `00080031` is exactly consistent
with feeding vertices to that mixed H5/H6 setup.  It does not establish that a
prerequisite outside the recovered Xorg path is missing.

The current driver performs `chrome9_init_2d_h6()` first and
`chrome9_init_3d_h5s2vp1()` second, followed by CR setup.  Combined with the
corrected 188-dword H6 body, this sequence has now retired idle and rendered
the expected target without guard corruption; see the measured result below.

## Complete Xorg path by lifetime

### 1. Mode-set engine initialization

`VIAInitialize2DEngine` (`0x223a0`) first clears the address-free 2D MMIO
register range.  On the VX900-class path it then writes:

```text
HParaType 10/pre-CR:
00000000 00333004 60000000 61000000
62000000 63000000 64000000 7d000000

HParaType fe02:
00000000

HParaType FE configure/misc:
00000004 40008c0f 44000000 45080c04 46800408
```

`VIAInitialize3DEngine` (`0x542c0`) waits through the driver's synchronization
callback first.  PCI `7122` branches at `0x54356..0x54360` directly to the
H5S2VP1 body at `0x549c4`; unlike older branches, it does not skip the sweep
when the DRM initialization flag is already set.  Its operation is:

1. initialize HParaType 01 subaddresses `00..9a`;
2. initialize all 16 texture-attribute banks, 49 entries each;
3. initialize all 16 texture-sample banks, 55 entries each;
4. initialize FE02 entries `00..13`;
5. install the five-entry de-gamma and 31-entry gamma tables;
6. write complete palette/coefficient/stipple write-through groups;
7. initialize HParaType 04 subaddresses `00..62`;
8. initialize HParaType FE subaddresses `00..47`;
9. initialize HParaType 11 subaddresses `00..20`;
10. install the 7122 final FE configure values and misconfiguration table.

This is byte-for-byte the sequence in `chrome9_init_3d_h5s2vp1`; no omitted
Xorg 7122 state was found inside the 3D initializer itself.  The defect is its
relative order with the 2D initializer.

Both initializers must remain before CR ring establishment.  The 3D sweep
explicitly initializes the HParaType-11 bank and therefore destroys previously
programmed CR state.  The corrected order is thus:

```text
wait for idle
2D initialization
3D H5S2VP1 initialization       # final shared FE state
program HParaType 11/10 CR state
install and prime the SF ring
publish work after cache maintenance
```

### 2. EXA software-state initialization

`VIAInitAccel` and `viaInitExa` call `viaExa3DInit` and `viaExa2DInit`, but
these calls construct software dispatch/state objects; they do not replace the
mode-set hardware sweeps.  For `7122`, `viaExa3DInit:0x279f8` sets the H6 flag
at the private-state offset corresponding to `+0x7ac`, then calls
`viaInit3DState_H6`.

Importantly, the `7122` branch does **not** call
`EXAInitialize3DEngine:0x27190`.  That direct-MMIO helper belongs to the
fallback device branch at `viaExa3DInit:0x279aa..0x279bb`.  Its shorter older
sequence must not be added to VX900 bring-up as a guessed missing step.

`viaInit3DState_H6` (`0x2ff50`) installs the H5 destination, texture, state,
pixel-shader, and quad callbacks together with the H6 vertex-shader callback.
Its large table work is CPU-side state metadata, not MMIO initialization.

### 3. Normal untextured composite setters

For a no-source/no-mask `PictOpSrc` operation, normal preparation executes:

```text
viaSet3DDestination_H5
viaSet3DCompositeOperator
viaSet3DDrawing
viaSet3DFlags
```

The old 160-dword capture in
[`via-5.75-dri-state.md`](via-5.75-dri-state.md) is **not** the `7122` result.
Its offline harness called `viaInit3DState_H6`, but did not reproduce the
preceding `viaExa3DInit` write of one to screen-private offset `+0x7ac`.  Once
the state base is advanced by `0x634`, that is the H6 selector at
`state+0x178`.  Leaving it zero made `via3DEmitState_H5` serialize the older H5
FVF branch while the harness still called the H6 vertex-shader emitter.

The corrected no-texture `7122` draw is the verified 188-dword oracle in
[`vp-stall-analysis.md`](vp-stall-analysis.md).  Compared with the rejected
capture it adds the H6 FVF/linkage controls `61/62`, uploads the H6 vertex
floating and integer constants, and uses a different FVF prefix.  This is a
second, independent prerequisite defect in the stalled lab draw; reversing
the engine initializers cannot make the mixed H5/H6 160-dword body valid.

No hidden MMIO write, cache command, context restore, or shader enable occurs
between the setters and the emitters.  The setters only construct the state
object, but the caller-established `state+0x178=1` software selector must be
present before emission.

### 4. Exact first-draw order

`viaExaComposite_H5:0x298e8..0x29a07` emits this order:

1. H0 `006c=00800000` 2D-to-3D synchronization;
2. `via3DEmitState_H5`;
3. `via3DEmitClipRect_H5`;
4. on `7122`, `via3DEmitVetexShader_H6` because the H6 flag is one;
5. `via3DEmitPixelShader_H5`;
6. `via3DEmitQuad_H5`;
7. H0 `006c=00800000` synchronization again;
8. command-manager flush/kick.

The current lab wrapper and body match this order.  The old body was the mixed
160-dword H5/H6 capture described above.  The successful test corrected both
defects together: it established vendor engine order and replaced the body
wholesale with the verified 188-dword H6 oracle.  Patching only the new
constants or only the hidden `61/62` controls into the old FVF remains invalid.

## Why the historical `00080031` occurred

The observed status decomposes as:

```text
00080031 = unclassified bit 19 | VP busy | CR busy | overall 3D busy
```

Clip, setup, raster, read-color, pixel shader, pixel, and burst-engine bits are
clear.  Thus the primitive did not advance beyond the vertex/front-end stage.
The trailing 2D initializer changes global FE configure state immediately
before CR setup, while instruction-memory stores alone remain idle.  That is
exactly the boundary at which the live observations diverge.

The recovered HParaType-04 primitive state itself is internally consistent
with the immediate payload:

- subaddress `20` is `1c5c00`, command-mode vertices with the vendor's
  automatic primitive-cycle setting;
- subaddress `23` is `000f02`, triangle primitive mode;
- subaddress `24` is `000004`, exactly four vertices in the list;
- subaddress `00` is `000006`, matching six dwords per untextured vertex;
- the HParaType-00 payload is exactly 24 dwords, four complete vertices.

The manual says CR configures and fires a primitive as complete vertex data
arrives, and ends the list according to the vertex-count field.  A probe must
therefore never send one, two, or three vertices with this state: that is a
known way to leave the front end waiting and would not isolate a useful bit.

## DRI initialization cross-check

The proprietary GL DRI path does perform additional process/context setup, but
none is a prerequisite for Xorg's EXA composite path:

- `hwmInitialize_inv:0xae9a0` maps MMIO/framebuffer resources, allocates
  shadow/event areas, sets capabilities, selects legacy CR readback fields,
  disables a traditional branch through HParaType-10 subaddress `69`, and
  writes sequencer `SR50=1f`;
- `hwmReInitializeRegister_inv:0xad180` restores legacy CR fields and may call
  `SetAGPRingCmdRegs_inv` for its own ring manager;
- `hwmContextCreate_inv` allocates and initializes software context storage;
- `stmContextInit_inv` calls `stmInitChipImage_inv` to build the per-context
  hardware-state image.

These facts are relevant to a future full GL context implementation and should
be captured separately.  They are not candidates to splice into the XRender
quad: Xorg's native composite path is designed to work without a DRI client,
and the newer 7122 KMD uses HParaType-11 subaddresses `06/07`, not the generic
DRI helper's older `02/03` pair.  The exact KMD ring state already present in
the lab is the stronger 7122 source.

### Exact DRI context-image layout and two independent H6 defaults

`stmInitChipImage_inv:0x1786a0` makes the context image sufficiently concrete
to use as a cross-check.  It first writes the subaddress in every word's high
byte and lays out these banks:

| image offset | bank | entries |
|---:|---|---:|
| `0000` | attribute/type `01` | `00..9a` |
| `026c` | 16 texture-attribute banks | 49 each |
| `0eac` | 16 texture-sample banks | 55 each |
| `1c6c` | FE02 | `00..13` |
| `20bc` | type `04` | `00..62` |
| `2248` | type `11` | `00..20` |
| `22cc` | type FE | `00..47` |

The common path at `0x178e4d..0x178e9f` clears FE00 bits `15/16`, sets bit
`12`, and sets low bit `2`, with a few device-feature bits preserved.  This is
independent evidence that the `001000` delta in Xorg's `FE00=001004` is
deliberate per-context 3D state.  A trailing 2D initializer clearing that bit
does not reproduce the DRI default either.

For the H6-generation path, type-04 entry `62` is the word at image offset
`2244`.  The code ORs its low `01ff`, clears the generation field, and, after
the explicit PCI-device comparison against `7122` at `0x178f87`, sets the
VX900 value at `0x178fcc`.  Its base is therefore `620081ff`; the Xorg draw
emitter adds `(texture_count + 2) << 20`, yielding the verified no-texture
`622081ff`.  This independently confirms that subaddress `62` is required H6
FVF/linkage state, rather than optional state guessed from the successful
capture.

## Independent Windows KMD ordering cross-check

The Windows 7 KMD contains two implementations of the full 3D replay and two
concrete `7122` call chains which establish the same lifetime rule: command
reader and low-level engine preparation precede the 3D replay; no 2D FE
initializer is called after it.

The normal setup path executes:

```text
127b7c: call 3b200       # command-reader/engine preparation
127b85: call 11d150      # complete 3D default-state replay
```

The timeout/recovery path repeats the same pair:

```text
8b832: call 3b200
8b83b: call 11d150
```

`0x3b200` first clears the address-free 2D MMIO latches through `0x11caa0`,
then performs sequencer, HParaType-10, HParaType-11, and CR preparation.  It
selects `7122` explicitly at `0x3b3e6..0x3b4a7`, clears the per-engine register
window, writes the traditional-branch control at type-10 subaddress `69`, and
finishes with `SR50=1f`.  `0x11d150` then performs the same broad attribute,
texture, table, type-04, FE, and type-11 sweep as the direct initializer.  Its
device dispatch selects `7122` at `0x11dc08` and enters the generation branch
at `0x11e0b7`.

The KMD reset helper has a second, direct-MMIO implementation.  After the
documented reset pulses and its status-poll logic, `0x8a76d` calls `0x896b0`.
That function again clears the address-free MMIO/register windows and executes
a complete 3D replay; its `7122` device branch is selected at
`0x89e9c..0x8a1c0`.  This independently explains why a reset is not complete
after merely toggling a sequencer bit.

These Windows values are **not** a replacement for the Linux/Xorg oracle.  In
particular, the Windows `7122` branch leaves FE `40=008408`, `46=000000`, and
`47=000404`, while Linux Xorg leaves `40=00840f`, `46=000005`, and
`47=000404`.  The pageable Windows implementation also writes
`FE[00]=000e1004`, whereas its direct implementation writes `00001004`.
Those differences may encode WDDM context-management or revision policy and
remain opaque.  They prove ordering and reset-replay scope, not that the
Windows FE constants should be mixed into the Xorg stream.

The Windows 2D-like initializer at `0x11cc20` does contain the familiar
`40=008c0f`, `45=080c04`, and `46=800408` values, but its only direct call at
`0x114387` is guarded by device ID `3157`, not `7122`.  It is therefore not
evidence for applying the 2D FE tail after the VX900 3D replay.

## Probe sequence and successful result

The live sequence retained all existing cache flushes, two-phase prime, poison
guards, bounded polls, backup/restore, and retain-until-reboot behavior on busy
status.  It advanced through these gates:

1. Run 2D initialization, then 3D initialization, then establish CR.  Submit
   only the address-free H2 status transaction and the proven guarded 2D fill.
   This proves the vendor final FE state does not regress the safe baseline.
2. Under the same order, repeat the full-VS store and PS-control/store probes.
   They should remain idle and preserve 2D execution.
3. Emit the corrected 188-dword oracle only through its H6 state, VS program,
   and `60800003`, but **no HParaType-00 payload**.  This must include the H6
   floating/integer constants and type-04 `61/62`; do not splice pieces into
   the rejected 160-dword FVF.  The manual says a fire is generated by
   vertex/index data, so this isolates VS-valid/FVF activation without an
   incomplete primitive.  Require idle and a passing 2D regression.
4. Submit the exact corrected 188-dword body, including its complete
   four-vertex, 24-dword HParaType-00 tail.  Do not split that tail across
   experiments and do not vary an individual unknown bit.

The address-free H6 state gate remained idle.  The complete submission then
retired with `engine=00000000`, `error=00000000`, `idle_ret=0`, and
`dispatch_ret=0`.  It wrote `000000ff` to all 1,024 target pixels, changed no
guard, and restored the complete 12 KiB test window.  This confirms that the
reversed persistent initialization and rejected mixed H5/H6 vector jointly
explain the previous `00080031`; no extra undocumented MMIO prerequisite was
needed for the recovered first draw.  The full log and module hash are in
[`hardware-lab.md`](hardware-lab.md#first-verified-h6-3d-draw).

## Safest GEM-backed fixed-draw submission design

The successful vector is sufficient for a tightly constrained first submit
ioctl, but it does **not** yet justify raw command submission.  The recovered
EXA destination setter normally emits:

```text
A50 = 50000000 | (SL byte address >> 8)
A51 = 51000000
A52 = 52000000 | format | (pitch >> 5)
```

The old verifier identifies `A51[21:20]` as the render-target location.  The
global location convention is `0=SL`, `1=SF`, `2=SM`, `3=LL`, so that Xorg
form targets local framebuffer memory.

A subsequent guarded live test has now proved the translated form.  It mapped
three pages from a low-address coherent allocation at PTEs `1..3`, used the
middle page at SF GPU byte address `00002000`, changed A51 bit `20` so its
location was `01=SF`, and left
the rest of the verified draw unchanged.  The first run, without explicit CPU
cache flushing, exposed only 272 of 1,024 expected pixels.  Flushing before
GPU access and again after retirement made all 1,024 pixels read back as
`000000ff`; both full-page guards remained intact and engine/error status was
zero.  The passing module is
`/root/tmp/chrome9/chrome9-h6-sf-target-clflush.ko`, SHA-256
`0093be37b4b33185d922d1d4055577af2e7f0f8604dd55fb1f7c5635e43b90bd`.

This closes the address-domain encoding question: an SF target through the
GART works.  It does **not** yet prove that every DMA address returned for a
generic shmem GEM BO is reachable.  A follow-up using ordinary shmem pages at
high DMA addresses (`ddf92000` and `de4fe000` were observed in two runs) still
retired with zero engine/error status and intact guards, but exposed only 144
and 96 of 1,024 expected pixels despite the GTI post-wait, write-combined mmap
policy, and explicit cache maintenance.  In contrast, the low coherent mapping
near `01088000` produced all 1,024 pixels.  Thus explicit cache maintenance is
a hard correctness prerequisite, not an optimization, and the production DMA
address ceiling remains to be measured before generic shmem can be admitted.

The driver's provisional DMA-GEM class has separately passed the guarded draw.
Its `dma_alloc_wc()`-backed contiguous allocation landed at DMA `01088000`,
installed PTEs `00001088/00001089/0000108a`, and produced all 1,024 pixels with
both guards intact, engine/error zero, and the local-VRAM 2D regression passing.
The module is `/root/tmp/chrome9/chrome9-h6-dma-gem-target.ko`, SHA-256
`6f3ae9f038e780aa80053bf9721422ddd50a6dc9541e51feaf1c2f5c460ba342`.
This is the allocation class to admit in the first fixed-draw ioctl.  A public
64-KiB DMA-GEM mmap has also passed a complete CPU write/read round trip;
generic scattered shmem remains disabled.

The proprietary DRI path independently confirms the one-bit change.
`STM_RenderTarget_INV:0x195e90` stores RT0 in the context image at offsets
`140/144/148`, exactly attribute subaddresses `50/51/52`; RT1 through RT3 use
`160/164/168`, `180/184/188`, and `1a0/1a4/1a8`.  For RT0 it:

- copies surface field `+140` to A50 after shifting the byte address right by
  eight (`0x195fec..0x196006`);
- copies pitch in 32-byte units into A52 and installs the result of
  `stmLookupRenderTargetFormat_invi` (`0x196026..0x19604d`);
- derives a tiling/mode bit in A51 bit `23` from surface field `+20` bit `1`;
- reads the surface memory-type field at `+14` (also exposed by
  `sfmGetSurfaceMemType`) and programs A51's location.

The location code has a chip-capability split.  When capability byte
`[chip+254]->69` bit `6` is clear, memory type `4` sets A51 bit `21`, location
`10=SM` (`0x196066..0x196078`).  When that capability is set, the code clears
both location bits and memory type `4` instead sets A51 bit `20`, location
`01=SF` (`0x196802..0x196814`).  The same construction is repeated for the
other three render targets.  The successful live bit-20 test therefore matches
the proprietary PCIe/translated-resource path, rather than being an encoding
found only by experiment.

### Memory layout and ownership

Use disjoint allocations and preserve their domains:

```text
SL/BAR2: [display/firmware reservations]
         [64-KiB GART table, 4-KiB aligned in known-unused VRAM]

SF VA:   [two persistent coherent 4-KiB command slots at VA 0/0x1000]
         [page-aligned GEM BO GPU-VA allocations]
```

The driver must not allocate indiscriminately from BAR2 until it owns display
scanout.  The old KMD needed userspace-supplied `back_offset` and
`available_fb_size` for exactly this reason.  The first SF path needs only
one explicitly reserved, known-unused 64-KiB local interval for the GART
table; exclude active scanout/cursor, firmware memory, and all diagnostic
windows before registering the DRM device.  A later local-VRAM GEM heap may
use `drmm_vram_helper_init()` over a separately proved free interval, but it is
not required for the now-proven fixed SF target.

Keep the already proven 64-KiB/code-4 GART configuration for a 64-MiB SF
aperture.  Initialize every PTE to `80000000`.  Map two persistent command
pages at SF VA `00000000` and `00001000`, one PTE per page.  Each page is an
independent, fully initialized command slot with its own start, PAUSE, end,
and at least `0x220` bytes of legal read-ahead padding; neither page is a
continuation of the other.  Exclude both pages from the future GPU-VA
allocator and initialize `drm_mm` over `[2 * PAGE_SIZE, 64 MiB)`.  The fixed
three-page BO then normally occupies PTEs `2..4` and its guarded target starts
at SF `00003000`.  This is a production lifetime improvement over the
successful one-ring-page experiment, whose BO occupied PTEs `1..3` and whose
target was `00002000`; the target encoding itself is not tied to either
address.
The valid PTE is the DMA/page PFN masked with `3fffffff`; do not force
`02000000`.  That 30-bit source mask only describes the software encoding; it
does not prove the hardware accepts the full nominal 42-bit physical range.
A production DMA mapping should use DMA-API bus addresses and reject
misaligned or out-of-mask pages.  The only fully passing target proof so far is
at low DMA; high addresses below 4 GiB have produced incomplete visibility.
Set a conservative DMA mask or use a low-address/bounce allocation until a
bounded address-threshold sweep establishes the real VX900 ceiling.

Every PTE transaction must be serialized under the VM/GART lock and VGA
arbitration: wait boundedly for old `SR6F.bit7` to clear, clear only
`SR6C.bit7` write protection, update complete PTEs, issue the write/DMA
barrier, request `SR6F.bit7`, wait boundedly for it to self-clear, restore
write protection, and only then publish the SF address to CR.  Replacing
`SR6C.bit0`, omitting the post-wait, or interpreting bit 7 as a translation
enable would repeat old-KMD defects.

### Initial ioctl contract

For the first ioctl, keep the current DMA-GEM allocation rather than making
ordinary shmem a production dependency before its high-address failure is
understood.  Do **not** make BO bindings persistent yet.  A persistent binding
would need a binding-owned GEM reference, a last-handle/eviction path, and a
recoverable answer when GTI unbind times out inside a `void` GEM free callback.
The synchronous fixed job can instead own its `drm_mm_node`, bind before
publication, and unbind after proved retirement.  The job already survives a
timeout in quarantine, so no stack-local lifetime or custom GEM wrapper is
needed.  This deliberately pays one GTI flush per fixed draw for a much smaller
and safer initial lifetime model; the current GEM_CREATE ioctl and DMA-GEM
object functions remain unchanged.

The first production-shaped ioctl should accept exactly one driver DMA-GEM handle
and no command pointer, shader, GPU address, relocation list, format, or
arbitrary geometry.  A minimal contract is:

```text
FIXED_QUAD {
        u32 destination_handle;
        u32 flags;                 // must be zero
        u64 pad[2];                // must be zero
}
```

Assign it the next private command number (`DRM_CHROME9_FIXED_QUAD = 0x02`)
and define the ioctl with `DRM_IOW`; it has no userspace-controlled output and
is synchronous.  Register it with `DRM_RENDER_ALLOW`, but advertise a new
`DRM_CHROME9_CAP_FIXED_QUAD` capability bit only after persistent GART/ring/3D
initialization has succeeded.  The ABI guarantees only that, on return zero,
the middle page contains the fixed 32-by-32 blue X8R8G8B8 result and both outer
guard pages still contain the kernel-defined guard pattern.  All other values,
including format, pitch, dimensions, shader, blend mode, addresses, and command
bytes, remain kernel constants.  No syncobj or output fence is needed until an
asynchronous ABI exists.

Require a driver-owned DMA-GEM object of exactly three pages for the first
guarded ABI.  Its DMA helper allocation is contiguous in bus address space;
reject a non-page-aligned `dma_addr`, arithmetic overflow, or any last byte
above the independently proved DMA ceiling.  Install exactly three consecutive
PTE PFNs at one consecutive SF VA allocation.  Do not treat successful DMA
allocation/mapping as proof that the graphics client can reach the returned
address: the current high-DMA shmem experiment contradicts that assumption.
Render only into the middle page so the first and third pages remain guards;
require the middle base to be 256-byte aligned and validate the full 4-KiB
target range before submission.  For this diagnostic ABI the kernel
initializes and checks both guards.  Lock the BO reservation, wait its prior
write/read dependencies, and reserve an exclusive fence slot before touching
hardware.  With one BO the lock order can remain `destination reservation ->
engine/VM submit lock`; when multiple resources are admitted, replace the
ad-hoc order with `drm_exec` and WW reservation locking.

The fixed operation is the already verified 32-by-32, 128-byte-pitch
`x8r8g8b8` PictOpSrc quad.  The kernel copies the exact 188-dword oracle,
patches A50 with `(bo_sf_va + PAGE_SIZE) >> 8`, and sets A51 bit `20` for SF.
It also emits both recovered H0 `006c=00800000` wrappers, the exact
address-free `hwmUpdateStatus_h5()` H2 transaction, the final no-payload H2
tail, and the established ring padding.  Those packets were all present in the
passing suffix and should remain in the first ioctl even though only the H0
wrappers are known Xorg render dependencies.  The guarded local-VRAM 2D
regression was a separate diagnostic and need not run per userspace draw.
Userspace never supplies a hardware address.

Serialize the sole engine with one submission mutex.  The SF ring remains
permanently pinned/mapped and is re-armed only from an owned PAUSE.  Publish
all command bytes before the complete type-10 A60..A68 replacement tuple with
the appropriate DMA barrier.  An A64/A65-only extension reached the new PAUSE
but did not dispatch the appended commands on live VX900, so it is not the
first-submit primitive.  A barrier
alone is insufficient on this non-snooping GPU: even the current coherent ring
required explicit x86 cache-line flushing in the live two-phase test.  Clean
the complete command and target/guard ranges before execution, and invalidate
or flush the target range again after proven retirement before CPU inspection.
Use `dma_sync_sgtable_for_device()` / `dma_sync_sgtable_for_cpu()` only for a
future streaming-mapped shmem object.  Do not apply streaming sync calls to a
DMA-GEM allocation returned by `dma_alloc_wc()`; follow that allocator's
coherent/WC ownership contract, retain the required publish/read barriers, and
preserve the architecture-specific cache operation used by the passing
DMA-GEM draw.  Do not assume the word “coherent” alone makes the measured
SF-client requirement redundant.

The proprietary GL stack confirms that this CPU-cache operation is intentional
for PCIe resources.  `__glDpMemPcieFlushCache:0x1e8dd0` converts a CPU pointer
to an offset in the shared PCIe heap and sends the eight-byte `{offset,size}`
record through DRM command `0c`.  The KMD's
`via_chrome9_ioctl_flush_cache()` turns that range into SG page indexes and
calls `via_chrome9_clflush_pages()`, which executes CLFLUSH over every cache
line on x86 or falls back to WBINVD.  The direct UMD caller
`__glS3InvFlushVertexBufferPcieCache:0x2314f0` enables it for chip selectors
`7`, `8`, and `14` before PCIe vertex-buffer consumption.  The legacy range
calculation has an off-by-one-page defect and must not be copied, but the need
for an explicit clean is unambiguous.

Keep three similarly named operations separate:

1. CPU data-cache clean/invalidate makes SF command and resource bytes visible
   across CPU/GPU ownership changes.
2. `SR6F.bit7` flushes the GTI translation cache after PTE changes; it does not
   publish dirty payload cache lines.
3. H0 `006c=00800000` and later decoded GPU pipe/cache commands order 2D/3D
   engines and internal render visibility; they do not clean the CPU cache or
   install translations.

All three are required at their respective ownership boundaries; none is a
substitute for another.

For this synchronous first ioctl, a kernel-created `dma_fence` may be completed
after a bounded wait proves the owned PAUSE and the complete 3D engine-idle
mask, rather than depending immediately on the still-unproved FE30 interrupt
path.  Attach the fence to the destination reservation before publication and
signal it only after completion, post-GPU cache invalidation, and the required
read barrier.  Keep the H0 post-draw synchronization: it is the only
live-proven 2D/3D render-target
visibility operation in the vendor path; no separate render-cache flush has
yet been decoded.

Use the reservation usages literally.  Because this job writes the
destination, wait existing dependencies through
`dma_resv_usage_rw(true)` (`DMA_RESV_USAGE_READ`, which includes earlier reads
and writes), reserve one fence slot while the reservation is held, and add the
new fence as `DMA_RESV_USAGE_WRITE` before the complete A60..A68 re-arm makes
the job reachable.
Give the engine one fence context from `dma_fence_context_alloc(1)` and a
monotonic sequence number.  `dma_resv_add_fence()` takes its own fence
reference; the job must retain a separate reference until success or transfer
it to the timeout quarantine.  On normal completion, signal after cache
ownership returns to the CPU and drop the job reference.  The reservation's
reference remains valid until superseded or object destruction.

On timeout, mark the engine wedged, set an error on the fence, but retain the
BO GEM reference, its DMA backing/mapping, SF VA/PTEs, the ring, GART table,
and every mapping that the GPU can still reach.  Do not signal-and-unmap a
destination that a late GPU may still overwrite.  Transfer those resources to
a quarantine owned by the device until a proved reset/replay or reboot.  This
lifetime rule is more important than making the first ioctl asynchronous.

### Kernel 7.2 transaction and lifetime plan

The minimum persistent device state is:

```text
chrome9_device:
        mutex submit_lock                 # sole engine and CR owner
        mutex vm_lock                     # drm_mm and PTE publication
        drm_mm sf_mm                       # [ring_end, 64 MiB)
        ring CPU pointer, DMA address, two SF pages/slots
        u8 active_slot; u32 completion_cookie, saved_dst_pos
        GART VRAM pointer/base, saved SR6A/B/C/7B
        u64 fence_context; atomic64_t fence_seqno
        bool stopping, wedged
        list_head quarantine

chrome9_job:
        chrome9_fence fence
        drm_gem_dma_object *destination    # held GEM reference
        drm_mm_node sf_node
        u32 first_pte
        u32 completion_cookie
        u8 ring_slot
        bool sf_bound, fence_attached, published, retirement_proven
        list_head quarantine_link
```

Make `dma_fence` the first member of `chrome9_fence`, and `chrome9_fence` the
first member of the separately allocated job.  The fence ops need only the two
mandatory name callbacks; signaling is always enabled by the synchronous poll,
the default wait is sufficient, and the default fence release can RCU-free the
allocation because the base fence address is also the job allocation address.
Initialize the embedded spinlock and use `dma_fence_init64()` with the device
context and incremented sequence.  Before dropping the job's GEM reference on
success/recovery, clear its destination pointer; the job memory itself remains
owned by fence references until the reservation eventually releases the last
one.  An unsignaled quarantined fence retains both explicitly.

Set the DMA mask before allocating any ring or GEM memory.  Until a wider
threshold passes, 25 bits is the smallest mask which includes the proved
`01088000` allocation; failing probe rather than silently relaxing that mask is
safer.  Allocate one contiguous two-page coherent ring, validate both DMA
pages against that mask, map them at PTEs zero and one, initialize every other
PTE invalid, and complete the bounded GTI post-wait.  Establish vendor
2D-then-3D state and CR state, prime slot zero with the dummy-only transaction,
and leave the reader at its owned PAUSE.  The first real draw uses slot one.
Initialize `sf_mm` only over `[2 * PAGE_SIZE, SZ_64M)` and register the DRM
device only after all of those steps pass, so userspace cannot race partial
hardware initialization.

Use the two ring pages as ping-pong command slots, not as a wrapping ring.
Each page contains one complete fixed-draw stream, its final 128-byte-aligned
PAUSE, and at least `0x220` bytes of initialized read-ahead padding before that
page's end.  Build only the inactive page while the reader is paused in the
other page; this prevents the request FIFO from prefetching storage which the
CPU is modifying.  Flush the complete inactive page and re-arm it with the
same complete tuple used by the live test, with every address based on the
selected page:

```text
A60/A61 start = (slot_base + body_start) | 1 # SF location selector
A62/A63 end   = slot_base + PAGE_SIZE
A64/A65 pause = slot_base + pause, ID 00
A66/A67       = 66fffff0 / 670000ff
A68           = 680a0001
```

The previously attempted A64/A65-only extension reached the later PAUSE but
did not dispatch its packets.  Therefore every job uses this full A60..A68
replacement; prefix/suffix extension is not a production submission method.
After the recovered draw, post-draw H0 synchronization, status H2 transaction,
and no-payload H2 tail, append one aligned H0 pair which writes the otherwise
inert 2D `DST_POS` latch (`MMIO 0010`) to a small per-job cookie.  Alternate
cookies such as `00010001` and `00020002`, first choosing one different from
the current latch.  No 2D command follows it.  The same latch path is already
live-proven and is safer than depending on the unproved FE30 fence encoding.
Save its pre-driver value and restore it only after safe teardown.

Completion requires the new cookie, then the owned PAUSE, then the complete
engine-idle mask.  This ordering matters: `043c` can lead retirement by
`0x210`, and the failed A64/A65 test produced pointer motion plus PAUSE/idle
without executing the body.  `043c` remains diagnostic only.  The cookie proves
that the selected body reached the parser; the preceding post-draw H0 orders
the 3D work against that marker, and PAUSE plus full idle proves the engines
drained.  Alternating slots additionally makes the final fetch-address page a
distinct generation and ensures the inactive slot cannot lie in the active
slot's read-ahead window.  Re-prime slot zero only after a proved engine reset
or complete ring reconstruction, not for every ioctl.

Probe unwind has the same reachability boundary as submission.  Any failure
before A68 first enables the owned ring may restore the saved table/registers
and free allocations.  Once the reader has been enabled, a prime timeout or
unproved PAUSE/idle state must retain the ring, PTEs zero and one, GART mapping,
bus mastering, and device structure.  Do not return a normal probe error and
let managed-resource unwind free those objects.  Instead bind in a non-DRM,
wedged quarantine state, take the module/device lifetime hold, emit a critical
reboot-required diagnostic, and expose no render node.  A later proved reset
path may release it; until then only reboot crosses that safety boundary.

The fixed-draw ioctl can then use this exact order:

1. Copy and validate the fixed-size UAPI record, including every zero flag and
   padding field.  `drm_gem_object_lookup(file, handle)` returns the job's GEM
   reference or `NULL`; transfer that reference to the job and eventually drop
   it with `drm_gem_object_put()`.  Reject `drm_gem_is_imported(obj)`, a device
   mismatch, any size other than 12 KiB, or a missing native DMA-GEM CPU
   mapping.  This driver creates only `drm_gem_dma_object` instances, so the
   cast is valid after rejecting imports.  Validate page-aligned `dma_addr`,
   checked `dma_addr + 3 * PAGE_SIZE - 1`, and the complete range against the
   active DMA mask.  Allocate a zeroed job, initialize its embedded spinlock,
   then call `dma_fence_init64()` with the device's one fence context and next
   sequence number.  That initial fence reference and the GEM lookup reference
   are the job's two lifetime anchors.
2. Lock `obj->resv` with `dma_resv_lock_interruptible(resv, NULL)`.  While held,
   call `dma_resv_wait_timeout(resv, dma_resv_usage_rw(true), true, timeout)`;
   positive means success, zero becomes `-ETIMEDOUT`, and a negative return is
   propagated.  Then call `dma_resv_reserve_fences(resv, 1)`.  Keeping the
   reservation held prevents a competing submitter from adding a dependency
   between this wait and attachment.  The timeout is bounded; a prior
   quarantined job cannot trap another ioctl indefinitely.
3. Acquire `submit_lock`, recheck `stopping/wedged`, select the page other than
   `active_slot`, and then acquire `vm_lock`.  This fixes the only submission
   nesting as `one BO reservation -> submit_lock -> vm_lock`.  Multi-BO work
   must switch to `drm_exec`; teardown/recovery may take `submit_lock ->
   vm_lock` only when it never takes a BO reservation.  No path may acquire a
   reservation after either device mutex.
4. Insert a zeroed three-page node with
   `drm_mm_insert_node_generic(..., 3 * PAGE_SIZE, PAGE_SIZE, 0,
   DRM_MM_INSERT_LOW)`.  Write the three consecutive DMA PFNs under the full
   SR6C/SR6F protocol and set `sf_bound` only after the GTI post-wait succeeds.
   Initialize target and guards and perform the passing DMA-GEM cache clean.
   Release `vm_lock`; the GEM reference, reservation, and `submit_lock` keep the
   ephemeral binding stable.
5. Call `dma_resv_add_fence(resv, &job->fence.base,
   DMA_RESV_USAGE_WRITE)` and set `fence_attached`.  Kernel 7.2's helper takes
   its own fence reference; do not manufacture a replacement reference for the
   reservation.  Build and flush the complete inactive command page: the exact
   patched 188-dword body, both H0 wrappers, recovered H2 packets, completion
   cookie, aligned PAUSE, and initialized read-ahead padding.  Issue the DMA
   publish barrier.  Conservatively set `published` immediately before the
   first A60 write, then write the complete A60..A68 replacement tuple and
   flush posted MMIO with a readback.  From `published=true`, even a partial
   tuple failure is treated as GPU-reachable.
6. Poll first for the selected job's `DST_POS` cookie, then `0444.bit31`, then
   `0400 & 00101ff3 == 0`, all under one bounded deadline.  Record `043c`,
   `040c`, `045c`, `0460`, and `0464` for diagnostics but do not use `043c` as
   retirement evidence.  Once all three completion conditions pass, set
   `retirement_proven`, perform the post-GPU target cache transition/read
   barrier, and verify result and guards.  Reacquire `vm_lock`, invalidate all
   three PTEs with a successful GTI post-wait, remove the SF node, and clear
   `sf_bound`.  Make the selected page `active_slot`, unlock `submit_lock`, and
   unlock the reservation **before** calling `dma_fence_signal()`: fence
   callbacks must not run inside either reservation or driver-mutex critical
   sections.  Then clear and drop the GEM reference and finally drop the job's
   initial fence reference.  The reservation may concurrently discard its
   signaled fence, but the initial reference keeps the job alive through this
   cleanup.
7. Classify failures by reachability, not merely by which source line failed:
   before publication, set a fence error because this job cannot execute.  If
   its PTEs can be invalidated with a proved GTI post-wait, remove the node and
   release normally.  If bind/unbind leaves translation state unknown, wedge
   the VM and quarantine the job solely to retain
   its BO/node/PTE backing.  After publication but before all cookie/PAUSE/idle
   conditions pass, set `wedged`, set `-ETIME`/`-EIO`, do **not** signal, and
   quarantine the live job unchanged because it may execute late.  If
   retirement is proved but pixel/guard validation fails, unbind normally and
   signal an error fence; bad diagnostic output is not evidence of a live DMA
   engine and need not leak resources or permanently wedge the driver.  If the
   post-retirement GTI unbind itself fails, quarantine the mapping and BO but
   signal the error fence because engine access has already ended.  In every
   signalable error case, unlock `vm_lock`/`submit_lock` and the reservation
   first, then call `dma_fence_signal()` while the initial fence reference keeps
   the job alive.  An attached fence remains visible but briefly unsignaled
   across that unlock window, which is valid; another user merely waits until
   this release phase signals it.

Use byte units consistently in the allocator:
`drm_mm_init(&sf_mm, 2 * PAGE_SIZE, SZ_64M - 2 * PAGE_SIZE)` and the aligned
`drm_mm_insert_node_generic()` call above; `first_pte = node.start >>
PAGE_SHIFT`.
For a native DMA-GEM allocation, PTE `i` is
`(dma_addr >> PAGE_SHIFT) + i`, after validating the complete 12-KiB addition
under the active DMA mask.  Under `vm_lock`, each bind/unbind obtains VGA
legacy-I/O arbitration, saves the indexed-register selector and SR6C, waits for
old SR6F.bit7 to clear, clears only SR6C.bit7, writes every complete PTE, flushes
posted table writes with a readback/barrier, requests SR6F.bit7, waits for its
new self-clear, restores SR6C and the selector, then drops arbitration.  On any
failure restore all reachable register state; never publish `sf_bound` or the
GPU address before the post-wait.  If the new GTI flush itself times out, mark
the VM/engine wedged and retain the table/ring rather than continuing with an
unknown translation cache.

No normal GEM free callback can now encounter a live PTE: successful jobs
unbind before signaling/dropping their BO reference, and timed-out jobs retain
that reference in quarantine.  The stock DMA-GEM free path remains sufficient.
The quarantine list is protected by `submit_lock`.  On the empty-to-nonempty
transition, take one device-wide `try_module_get(THIS_MODULE)` and
`get_device(&pdev->dev)` hold *before* dropping any ioctl locks; record a
`quarantine_hold` bit so later jobs do not take duplicate device-wide holds.
Link the fully initialized job only after it owns the GEM reference, initial
fence reference, and any live SF node.  Then unlock `submit_lock`, unlock the
BO reservation, and return without putting either job-owned reference.  A
signaled VM-only quarantine and an unsignaled live-engine quarantine use the
same ownership container, but recovery must distinguish them by
`published/retirement_proven`.

On later reset recovery, take `submit_lock` and prove the engine can no longer
access *any* quarantined job before taking `vm_lock`.  Invalidate every retained
PTE range with one successful GTI transaction, remove the corresponding
`drm_mm_node`s, and clear `sf_bound`.  Preserve every stored fence error and
splice the recovered jobs to a private release list.  Drop `vm_lock` and
`submit_lock`; only then signal each still-unsignaled fence, clear/drop its GEM
reference, and drop its initial fence reference.  Already signaled VM-only
quarantine fences need no second signal.  Only after the private list is empty
may recovery clear `quarantine_hold`, call `put_device()`, and `module_put()`.
Never signal a fence or drop a GEM/fence reference while traversing the
protected list, while holding a driver mutex, or while holding a BO reservation
that object destruction could tear down.

Normal remove first takes `submit_lock`, sets `stopping`, and drops it; then
`drm_dev_unplug()`/unregister prevents new userspace entry.  It next retakes
`submit_lock`, proves the selected cookie/PAUSE and full idle, and requires an
empty quarantine before taking `vm_lock` and invalidating the two ring PTEs.
After the GTI post-wait it may free the ring, restore the saved GART/sequencer
and `DST_POS` state, disable bus mastering, and release normal resources.  It
must never take a BO reservation while holding `submit_lock` or `vm_lock`.

An unsafe timeout cannot be made unload-safe by leaking only the BO: managed
teardown would still free the ring/GART/MMIO state which the engine can reach.
While this experimental no-reset design exists, set
`chrome9_pci_driver.driver.suppress_bind_attrs = true` so sysfs cannot invoke a
manual unbind behind the quarantine, and let the quarantine's module reference
reject module unload.  Probe allocations which become GPU-reachable must also
be explicitly owned rather than left to unconditional devm/pcim unwind.
Physical hot-unplug is a separate terminal case because the device can no
longer DMA, but ordinary remove/unload must not masquerade as hot-unplug.  If
no proved reset exists, reboot is the only safe release point.

### Evolution to general BO submission

The SF color-target mode is now proved.  Generalizing beyond the guarded fixed
quad still requires range validation for user-selected offsets, pitches,
dimensions, formats, tiling, and compression.  Textures require their base
divided by 256 plus the corresponding two-bit SF selectors in type-02 A18.
Buffer-fed vertices use a different, now-recovered encoding: 1-KiB-aligned
stream bases and dword pitches in type-03 subtype `30`, per-element offsets in
type-04 A01..A10, and the SF selector in type-04 A00.
Allocate SF VAs in whole pages after the persistent ring, keep each referenced
BO's DMA backing stable through its last fence, and invalidate its PTEs with
the full post-wait GTI protocol before releasing the VA.

The exact texture construction is visible in
`stmHWPrepareTextureRegs_invi:0x1b22f0`.  For stage `s`, the DRI context image
uses a `0xc4`-byte texture-attribute stride.  At `0x1b2491..0x1b24a4` it reads
the surface byte address from `surface+140`, shifts it right by eight, and
stores the low 24 bits in the stage's first type-02 attribute word (A00).  The
stage's word at image offset `2cc + s*0xc4` is A18.  On the PCIe capability
path at `0x1b260d..0x1b261f`, the code clears A18 bits `1:0` and writes one
when `surface+14 == 4`: the same global `01=SF` domain encoding as the render
target.  Mip/cube addresses are also expressed in 256-byte units; their
per-level fields and complete byte-range rules still need decoding before a
texture relocation may be exposed.

The H5/H6 vertex-stream construction is in
`stmUpdateVertexInput_H5:0x17cbb0`; the apparently relevant
`STM_SetStreamSource_H5:0x1bb920` is intentionally a no-op, so the older
`STM_SetStreamSource_INV` A21/A22 path must not be projected onto VX900.  The
H5 updater forms each element byte address from `surface+140 + stream_offset +
declaration_offset + context_base_adjustment`.  It writes the address rounded
down to 1 KiB into the selected type-03 subtype-30 stream word bits `31:10`,
and the stream pitch divided by four into bits `8:0`
(`0x17cd9c..0x17cdde`, repeated at `0x17cfda..0x17d027`).  Address bits `9:2`
become the corresponding data element's type-04 A01..A10 offset field
`19:12` (`0x17cd82..0x17cd98`).  The VX900 manual independently defines these
same base, pitch, and element-offset units.  A surface memory type of four
writes `01` to type-04 A00 bits `16:15` on the newer capability branch
(`0x17d288..0x17d29c`), selecting SF.

Indexed draws do not name a separately addressed index BO in this interface.
The manual defines type-00 payload as immediate vertex indices when type-04
A20 bit 0 selects vertex-index mode; A20 bit 1 chooses 16- versus 32-bit
indices.  `STM_SetIndices_INV:0x1bc250` implements that mode encoding and
writes the 32-bit maximum and minimum allowed vertex indices to A2C/A2D, with
their high bytes in A2E (`0x1bc298..0x1bc34c`).  The values previously mistaken
for split index-buffer addresses are therefore bounds, not relocations.  This
resolves address packing and domain selection, but not the maximum fetched
vertex range: kernel validation must derive it from every stream base, element
offset/width, pitch, the validated index extrema, instance count, and primitive
count rather than trusting an address-only relocation.

Do not expose those pieces as unchecked relocations.  A later submit ABI must
name BO handles and typed relocation slots; the kernel must derive the address,
domain selector, alignment, pitch, dimensions, mip extents, and complete byte
range for every target, texture, Z/stencil surface, and vertex/index stream.
The old verifier collected many of these fields but did not reconstruct or
bound their addresses, so it is not a security model.

### Quiesce and teardown

Teardown order is the reverse of reachability: reject new jobs, wait all
fences, require the owned final PAUSE and the complete engine-idle mask, then
invalidate all non-ring PTEs, flush GTI with a bounded post-wait, invalidate
the ring PTEs, flush again, and only then free coherent pages or the page
table.  Restoring the saved GART and sequencer state from this exact paused-idle
condition is the live-proven cleanup.  Do not change the PAUSE ID to STOP as a
standalone teardown: on VX900 that leaves CR busy unless it is immediately
paired with a complete replacement.  Restore the saved `SR6A/SR6B/SR6C/SR7B`
state after CR can no longer fetch the driver ring.  `SR6C.bit7` merely protects
table writes and cannot stop the reader.  If PAUSE/idle proof times out,
preserve reachable storage and mappings rather than repeating the legacy
cleanup use-after-free.

## Remaining adjacent reverse-engineering targets

- name and decode FE subaddresses `00`, `40`, `45`, `46`, and `47` from the
  Windows KMD/UMD, because the public VX900 manual omits HParaType FE;
- finish mapping the remaining DRI context-image dirty groups to emitted H2
  state now that the complete bank layout and H6 entry `62` are known;
- replace the successful explicit x86 SF-target flush with the correct
  cross-architecture DMA/cache-maintenance abstraction and verify identical
  visibility;
- determine the role of `SR50=1f` and whether it is display/clock state which
  Xorg already restores during mode set;
- trace HParaType-10 subaddress `69` branch-disable lifetime independently of
  normal ring start/pause/end;
- recover the complete vertex fetch-range and instance/primitive-count rules
  from the H6 GL draw functions, including limits on immediate type-00 indices,
  without projecting them onto the immediate XRender path.
