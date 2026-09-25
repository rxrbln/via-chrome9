<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 HD (VX900) CBU windows as a CPU detiling aperture

Status (2026-09-25): static recovery plus read-only target state (section
8); kernel ladder ready (section 9).  No CBU register has been written on
the target.  This note extends section 4 of
[chrome9-tiling-re.md](chrome9-tiling-re.md) and answers its open CBU
questions where the sources allow it.

Evidence labels:

* **M**: manual text.  Line numbers refer to the `pdftotext -layout`
  extractions in `~/tmp/ogpm/`:
  * VX900 Part I, `OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.txt` (`M900`);
  * VX855/VX875 HCM Part I, `OGPM_Chrome9 HCM_R100_Part1_Core_2D.txt` (`MHCM`);
  * VX800/VX820 HC3 Part I, `OGPM_UniChrome_ProII_R108a_Part1_Core_2D.txt`
    (`MHC3`; the file name is misleading, its page header says HC3).
* **S**: static code:
  * `kmd:` `VTGKModeDX32A.sys` (SHA-256 `61812960…`, image base `0x10000`);
  * `drv:` `via_drv.so` 5.75 (`55324613…`);
  * `dri:` `via_chrome9_dri.so` 5.75 (`fddb4dea…`);
  * `ksrc:` vendor KMD source `binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/`.
* **D**: measured on the target, read only (section 8).
* **I**: inference.

## Summary

1. **16 windows, 32 bytes apart, at MMIO `0x1E00..0x1FFF` (M, S).**  They are
   programmed through the ordinary MMIO register BAR (MB1), the same mapping
   as the engine registers.  The window is not a separate aperture.  It
   rewrites CPU accesses that fall into `[base, end)` of the frame buffer
   **in place**.  The CPU-visible address stays the surface's own BAR2
   address (S: Linux DRI returns `fb_virtual + surface_offset`, Windows
   hands VidMm's own `CPUTranslatedAddress` range back).
2. **The address domain is SL only (S, M; I for the hardware mechanism).**
   * The base is a byte offset into the frame buffer (BAR2/MB2), not a CPU
     physical address.  Windows computes it as `CPUTranslatedAddress -
     FB BAR base`, the X driver uses the screen's frame-buffer offset, and
     the DRI uses the surface's frame-buffer offset.
   * The register has no location selector.  `[28:0]` spans exactly the
     512 MiB MB2/SL aperture.
   * Windows rejects a swizzling range for any segment other than the local
     one, and marks only local-segment allocations `Swizzled`.
   * SF (GART) pages are system RAM that the CPU reaches without passing
     through the GPU, so a CBU window cannot see those accesses (I).
     Tiled SF surfaces keep the software (or 2D blit) detile.
3. **VX900 register semantics differ from HC3/HCM (M, S).**
   * The reciprocal is 32-bit (`2^32 / pitch_px`) instead of 22-bit.
   * Pitch, width and height are 16-bit instead of 11-bit.
   * The end address is `[29:0]`.
   * Windows and the X driver switch to the 32-bit form only for PCI
     `7122`/`a122`.  The Linux 5.75 DRI aperture code always uses the
     22-bit form, so it is legacy HC3/HCM code.  It is also disabled.
4. **Tile-mode pitch unit is not settled (S conflict).**
   * Windows on `7122` programs the level width in pixels in both pitch
     halves.
   * Windows on other chips, and the Linux DRI on every chip, program
     `pitch_bytes >> 5` (tile columns).
   * The X driver's rotation (not tile) path uses pixels on every chip.
   * Candidate A (pixels) is primary for VX900; candidate B (`pitch >> 5`)
     is the fallback.  Silicon must decide (ladder C3).
5. **Bit 7 (16-row mapping) is never set by any vendor code (S).**
   * The VX900 and HCM manuals define it: "16 x 256 tile mapping" when
     bit 6 = 0.  HC3 has no bit 7.
   * By analogy with the HQV names "256 bits x 8/x 16", this is most
     likely 32 bytes x 16 rows (I).  Ladder C8 measures the mapping
     directly.
6. **No vendor default exercises the tile function on VX900 (S).**
   * Windows acquires ranges only for `Swizzled` allocations, which exist
     only when tiling is enabled (registry/VBIOS, off by default).
   * The Linux DRI aperture path is disabled (`reg+0x3ac = 1`).
   * The X driver uses the CBU for screen rotation only (windows 0/1,
     rotation mode, no tile bit).
   * The rotate function on `7122` is therefore the only vendor-exercised
     use of these registers.

## 1. Register map (M)

Window `n` (0..15) sits at MMIO `0x1E00 + n*0x20` (M900 l.312 MMIO map,
l.6115-6131 table).  Offsets within a window:

| off | name | VX900 definition | HCM / HC3 difference |
|---|---|---|---|
| `+00` | Rotate Control | bit 0 enable; `[2:1]` bpp `00` 8, `01` 16, `1x` 32; `[4:3]` rotate type `00` none, `01` 90, `10` 180, `11` 270; bit 5 tile function (1) vs rotate function (0); bit 6 rotate with tile; bit 7 16x256 tile mapping when bit 6 = 0; `[31:8]` RO (l.6156-6177) | HCM same; HC3 `[31:7]` RO, no bit 7 (MHC3 l.4456) |
| `+04` | Base Address | `[28:0]` byte, `[3:0]` read only (l.6179-6186) | HC3 `[28:2]` |
| `+08` | End Address | `[29:0]` byte, `[3:0]` read only (l.6189-6196) | HCM/HC3 `[28:0]` (MHCM l.5011) |
| `+0C` | Source Pitch Reciprocal | 32 bits, `1/source_pitch` as 0.32 fixed point, source pitch in pixels; example pitch 256 gives `0x01000000` (l.6205-6211) | 22 bits, pitch 256 gives `0x4000` (MHCM l.5025-5027, MHC3 l.4507) |
| `+10` | Rotate Pitch | `[31:16]` destination pitch, `[15:0]` source pitch, pixels, 1..65536 (l.6213-6222) | `[26:16]`/`[10:0]`, 1..2048 (MHCM l.5036-5040) |
| `+14` | Height and Width | `[31:16]` height, `[15:0]` width, pixels, 1..65536 (l.6225-6234) | 11-bit fields |
| `+18..+1C` | reserved | | |

**Readback limits (M).**  M900 l.6133-6135 says "Rotation Base Address[28:16],
Rotation End Address[28:16] and Rotation Source Pitch[21:0] are write-only.
Reading these bits always gets 0."

* The `[21:0]` reciprocal width matches the HCM layout, not the VX900 32-bit
  field.  The note may be a leftover from the older manual (I).
* Register readback therefore cannot prove that a window was programmed.
  Ladder C1 records what actually reads back.

**Related registers (M).**

* SR7C "CBU Request Kill Number", default `C8h` (l.2660-2663).  It sits next
  to the SL/SF kill numbers SR79/SR7A (l.2637-2652).  So the CBU issues its
  own memory requests into the GTI arbiter.
* The DMA registers say certain bits "can ONLY be written from CBU path"
  (l.5890, 5893).  HC3 SR4F bits 7/6 select DMA register access "From CBU"
  vs "From CR".  **I:** CBU is the host (CPU) bus interface unit.  That fits
  a translator that sits only on the CPU path.

## 2. Vendor programming

### Windows KMD: `S3gAcquireSwizzlingRange` (`kmd:0x936e0..0x93d8e`, S)

The function is identified by its debug string at `kmd:0xebb4c`.  The
argument is `DXGKARG_ACQUIRESWIZZLINGRANGE`: `+0` hAllocation, `+4`
PrivateDriverData, `+8` RangeId, `+0xc` SegmentId, `+0x18/+0x1c`
CPUTranslatedAddress.

* Caps: `NumberOfSwizzlingRanges = 16` (`DXGK_DRIVERCAPS+0x20`, `kmd:0x88bf6`).
  The window index is the VidMm RangeId (`kmd:0x93771..0x93783`, `<< 5`).
* It rejects a SegmentId other than the local segment `[adp+0x10ac]`
  (`kmd:0x9378c..0x937a7`), and a range already marked busy in
  `[dev+0x22f0c + id*4]` (`kmd:0x937ac..0x937c9`).
* PrivateDriverData selects the subresource: mip level `[19:12]`, face
  `[3:0]` (face size = `alloc+0x40 / 6`), slice `[27:20]`, cube flag bit 28,
  volume flag bit 29 (`kmd:0x937eb..0x9384a`).
* The level walk adds `height*pitch` per level.  While `pitch > 32` it halves
  pitch and width, and halves height while height > 1 (`kmd:0x938ff..0x93a53`).
* Register values (`kmd:0x93a8b..0x93d24`), all written with
  `WRITE_REGISTER_ULONG`, control last:

```text
base    = CPUTranslatedAddress.low - [adp+0x640] + level_offset   # 1e04
end     = base + pitch_bytes * height                              # 1e08
pitch   = 7122: width_px << 16 | width_px                          # 1e10
          else: (pitch_bytes >> 5) << 16 | (pitch_bytes >> 5)
          rotated: height<<16 | width  (180: width<<16 | width)
recip   = 7122: _alldiv(2^32, pitch & 0xffff)                      # 1e0c
          else: 0x400000 / (pitch & 0xffff)
hw      = height << 16 | width_px                                  # 1e14
control = 1 | (bpp==16 ? 2 : bpp==32 ? 4 : 0) | rot | (tiled ? (rot ? 0x40 : 0x20) : 0)   # 1e00
```

Additional details:

* The `7122` switch is adapter flag `0x9c0` bit 23.  It is set only for PCI
  device `7122`/`a122` (`kmd:0x114d5b..0x114d8c`).
* `[adp+0x640]` is the frame-buffer BAR start taken from the translated PCI
  resources.  It is the third memory resource in the three-BAR layout and
  the first in the two-BAR layout (`kmd:0x115452`, `kmd:0x1154ee`).
  **I:** it is MB2 (BAR2) on VX900.
* The bpp is `alloc+0x54 [15:8]`.  The tiled flag is `alloc+0x20`.
* Nothing waits for engine idle.  The only calls are debug print,
  `KeSave/RestoreFloatingPointState` (import `0xe31bc/0xe31c0`), `_alldiv`
  and register writes.  VidMm's lock synchronisation guarantees that the GPU
  is not using the allocation (I).
* Release (`kmd:0x93da0..0x93e67`): read control, clear bit 0, write it
  back, clear the busy flag.  The other fields are left unchanged.
* Init zeroes all 16 windows for `7122`/`a122` (`kmd:0x3b4a7..0x3b586`).
  The recovery helper zeroes one dword per window (`1e00..1fe0` step
  `0x20`, [vx900-reset-recovery-oracle.md](vx900-reset-recovery-oracle.md)).

Which allocations become swizzled (S):

* CreateAllocation sets `alloc+0x20 = 1` and `DXGK_ALLOCATIONINFO.Flags |=
  0x80` (`Swizzled`) only when two conditions hold:
  * the preferred segment is the local segment (`[alloc+0x18] ==
    [adp+0x10ac]`, `kmd:0x86a41..0x86a73`);
  * the device tiling enable `[dev+0x22ea8]` is set.
* Format-keyed paths `kmd:0x86148` and `0x861ac` use the same
  `[dev+0x22ea8]` gate.  Format `0x29` clears both flags (`kmd:0x860f2`).
  `kmd:0x87430` sets them from bit 1 of another descriptor word (not
  traced further).
* The UMD's linear lock of SL surfaces (`0x10104710`,
  [chrome9-tiling-re.md](chrome9-tiling-re.md) section 3) is therefore
  served by VidMm through this range (I: the UMD lock-flag path was not
  traced to `AcquireAperture`).

### X driver: `EnableCBUrotate` / `DisableCBUrotate` (`drv:0x7fbd0`, `drv:0x7fde0`, S)

* Window 0 serves IGA1 and window 1 serves IGA2 (`[pVia+0x8a0] != 0`,
  `drv:0x7fc9d..0x7fca7`).
* Values:
  * base = the screen's frame-buffer byte offset, stored as KiB `<< 10`
    (`drv:0x577d4..0x577dd`);
  * end = `base + align(virtualX,8) * virtualY * bpp/8`;
  * pitch and reciprocal in pixels;
  * control = rotation table `{8, 0x10, 0, 0x18}` (`drv:0x8b694`) | bpp | 1.
* `GetReciprocal` (`drv:0x7fb90`): `7122` gives `(0x80000000 / p) * 2`
  (floor of `2^32/p`, possibly 1 LSB low); other chips give `0x400000 / p`.
* Disable zeroes all six dwords, control last (`drv:0x7fdfb..0x7fe60`).
* Called from `VIAModeInit` (`drv:0x558d0`) and around VT switches
  (`VIALeaveVT` `drv:0x4eff1..0x4f064`).  The enabling conditions
  (`ProbeRotateCaps`) were not traced.

**I:** in rotation mode the X server draws through its ordinary frame-buffer
mapping while scanout reads the rotated image.  So the CBU rewrites CPU
accesses in place, and neither the display engine nor the 2D engine is
translated.  The X driver has a separate `Enable2DEngineRotate` for the
engine path.

### Linux 5.75 DRI: `hwmAllocateAperture_inv` (`dri:0xae000`, S)

The argument is the DRI form of `struct drm_via_chrome9_aperture`
(`ksrc:via_chrome9_drm.h:303-333` without `isPCIEVIDEO`): `+0` offset,
`+4` pitch, `+8` width, `+0xc` height, `+0x10` format, `+0x14` rotation,
`+0x18` tiled, `+0x1c` allocate-only, `+0x20` OUT linear address, `+0x24`
OUT pitch, `+0x28` OUT handle, `+0x2c` `0xAAAAAAAA`, `+0x30/+0x34` aligned
width/height.

* **Gate:** registry option `reg+0x3ac` is forced to 1
  (`osGetRegistryOptions` `dri:0xa48ec`).  It leads to the no-hardware path
  (`dri:0xae0d0..0xae0d8`), which uses `sfmCreate` staging plus
  `sfmSurface_RawTransfer_inv`.
* **Enabled path** (`dri:0xae0e8..0xae4ad`):
  * `hwmWaitChipIdle` first;
  * slot search over `[hw+0x18]` windows (the KMD constant is
    `NUMBER_OF_APERTURES_CLB 16`, `ksrc:via_chrome9_3d_reg.h:139`);
  * a tiled request with `pitch <= 32` gets no window (`dri:0xae1c2`);
  * values:
    * `1e04 = offset`;
    * `1e08 = offset + pitch*height`;
    * `1e10 = (pitch>>5)<<16 | pitch>>5` (rotation 0);
    * `1e0c` = the 22-bit reciprocal of `pitch>>5`, from a float `1.0 /
      p` converted by hand (`dri:0xae34a`, `0xae436..0xae462`);
    * `1e14 = height<<16 | width`;
    * control = `1 | bpp` (format 2 gives 3, format 3 gives 5,
      `dri:0x3de344`) `| 0x20` (tiled, no rotation) or `| 0x40`
      (tiled + rotation).
* **Returned linear address:** `offset + [hw+0xf4]`, the user mapping of the
  frame buffer from `osGetFBInfo` (`dri:0xae4c6..0xae4e4`,
  `hwmInitialize_inv` `dri:0xaea72`).  `[hw+0xf0]` is the 16-MiB MMIO
  mapping (`dri:0xaea39`).
* **Free:** `control &= ~1` (`dri:0xadfbf..0xadfd9`).
* **Caller:** `sfmSurface_ClaimTiledApertureAccess` (`dri:0xc2620`).  It
  falls back to a 2D `sfmBlt` staging copy (`dri:0xc24f4`).
* **Kernel:** the aperture ioctls are stubs (`ksrc:via_chrome9_mm.c:610-620`).
  The command verifier rejects MMIO `0x1E00..0x21FF` as "CBU ROTATE SPACE"
  (`ksrc:via_chrome9_verifier.c:561-563`).

## 3. Address domain and CPU aperture (answer to "SL only?")

| question | answer | evidence |
|---|---|---|
| which addresses does the window compare against | frame-buffer (SL / BAR2) byte offsets | S: all three vendor programmers write an FB offset.  M: `[28:0]` = 512 MiB = MB2 size (M900 l.291-294) |
| where does the CPU read the linear view | at the same BAR2 address as the surface itself (`BAR2 + base`) | S: DRI returns `fb_map + offset`; Windows returns VidMm's own range; the X driver draws through its normal FB map |
| SF / GART surfaces | not reachable; no location selector exists, and CPU access to SF pages never enters the GPU | M (no field), I (bus path) |
| GPU engine accesses to the window range | not translated (I) | the X rotation design depends on it; ladder C7 checks it |
| alignment | base and end 16-byte granular (`[3:0]` RO).  Vendor bases are 256-byte (tiled surface) or page aligned | M, S |
| size | 1..65536 pixels per dimension; end below 1 GiB | M |
| number | 16, shared by all users; the X driver claims 0/1 when it uses CBU rotation | M, S |

**I:** the window is a pure address permutation over `[base, end)`.  A linear
offset `o = y*pitch + x_bytes` becomes `base + tile8(x, y)`.  For rows `y < h`
the result stays inside `base + pitch*align(h,8)`.  `end` bounds the
*linear* view (`pitch*h`), as every vendor programs it.  The tiled memory
behind the last partial tile row is reached only through linear addresses
below `end`.

## 4. Proposed VX900 programming for one tiled SL level (I)

For a tiled level at SL offset `B` (256-byte aligned) with byte pitch `P`
(multiple of 32), height `H` rows, and cpp 1/2/4:

```text
px   = P / cpp                                  # exact, P % 32 == 0
1e00 = 0                                        # disable before changing a live window
1e04 = B
1e08 = B + P * H                                # linear extent
1e10 = px << 16 | px                            # candidate A (Windows 7122)
                                                # candidate B: (P>>5)<<16 | P>>5
1e0c = (u32)(((u64)1 << 32) / (1e10 & 0xffff)) # 32-bit reciprocal of the programmed pitch
1e14 = H << 16 | px                             # width = full pitch, not the visible width
1e00 = 1 | bpp_bits(cpp) | 0x20                 # 8x8-row tile function, bit 7 = 0
```

Notes on these choices:

* Width is set to the full pitch in pixels.  This avoids an unknown
  behaviour for columns between the visible width and the pitch.  Windows
  uses the level width, which equals the pitch for its tile-aligned
  surfaces (I).
* The reciprocal is exact `floor(2^32/p)` as in Windows.  The X driver's
  `(2^31/p)*2` can be 1 LSB lower.  **Hazard:** a truncated reciprocal can
  misplace the last pixel of a row at large `y` for non-power-of-two
  pitches if the hardware divides by multiplication without correction.
  Ladder C3 covers this with 800/1600-pixel pitches and 1200 rows.
* Program each mip level as its own window, as Windows does.  The walk
  differs from the Mesa layout (`ALIGN(h,8)` rows at 256-byte aligned table
  offsets), so the driver passes its own level offsets.

## 5. Hazards

1. **All CPU accesses to the range are rewritten.**  This includes the
   kernel's `cdev->vram`/`sl_wc` mappings, any fbdev/scanout copy, and
   software detile code.  Never cover scanout, the GART table, the
   occlusion page, or a non-tiled BO.  While a window is active, the
   software detile must not be applied to the same mapping, or the data is
   detiled twice.
2. **Changing a window under a live CPU mapping.**  Userspace PTEs of the
   BO must be zapped (`unmap_mapping_range`) before a window is released or
   stolen.  Otherwise later accesses see the raw tiled layout.  A GPU idle
   wait is not needed for the translation itself (I), but CPU/GPU content
   coherence follows the existing BO fences.
3. **Write combining.**  A 64-byte WC burst spans two 32-byte tile rows that
   are 256 bytes apart in memory.  Whether the CBU splits such bursts, and
   how it handles byte masks, is untested.  Ladder C4 tests UC and WC stores
   separately.
4. **Reads through UC BAR2 are slow regardless.**  CBU removes the gather
   arithmetic, not the uncached read cost.  The expected gain is mainly for
   WC writes and zero-copy client maps.  Ladder C10 measures it.
5. **Unverifiable readback.**  Base, end and reciprocal bits read as 0 (M).
   Program from a kernel-side shadow copy, never with read-modify-write of
   those fields.  Control read-modify-write is acceptable (vendor release
   does it).
6. **Bus stall risk.**  A misprogrammed window in the CPU path could stall a
   UC read.  A stalled read locks up the CPU, not only the GPU.  The first
   access of every new step is one dword read, and recovery is the NanoKVM
   power cycle.
7. **Ownership.**  Only the kernel may write `0x1E00..0x1FFF`.  Any
   userspace-reachable register-write path (2D/H1 headers, debug knobs)
   must reject this range, as the vendor verifier does.  Disable all windows
   on unload, suspend, KMS handoff and in the reset path (the vendor
   recovery zeroes them anyway).
8. **SR42 bit 7.**  The manual lists CBU among the engines that bit 7 forces
   linear.  The 3D engine tiles with bit 7 = 0
   ([chrome9-tiling-re.md](chrome9-tiling-re.md) silicon results), but the
   CBU may still honour the bit.  If C3 shows an identity mapping for both
   pitch candidates, this is the first suspect.  Do not write SR42 in this
   ladder.

## 6. Guarded silicon test ladder

Rules for every step:

* follow the target rules: GPU lock via `flock -o`, hash-gated module
  handoff, runs of 7 s or less, `xset s off -dpms` before any window
  readback, NanoKVM power-off/on as recovery;
* one variable per step;
* the kernel alone programs windows, through a debug-only knob or debugfs
  file that is absent from production builds;
* test surfaces come from the `sl_mm` pool, away from scanout and the GART
  table, with 4-KiB poison guards on both sides;
* SHA-256 of the GART table and of the scanout region before and after each
  step;
* window 15 only, so windows 0/1 stay free (X rotation convention); all 16
  controls must read 0 before a step starts;
* every step ends with `control = 0`, and the raw view is re-read to prove
  that the window is off.

Pattern `T(x,y)` = dword `(y << 16) | x` written raw (window off) at
`tile8_offset(x, y, P, 4)`, the equation from
[chrome9-tiling-re.md](chrome9-tiling-re.md) section 1.  The steps up to
C6 are CPU-only and do not start any engine.

| step | action | pass condition / decides | risk |
|---|---|---|---|
| C0 | read all 16 windows (`+00..+14`) | all zero after POST; records firmware state | none |
| C1 | window 15 with **enable = 0**: write distinct values to each field, read back, then zero | which bits read back; whether M900 l.6133 applies to VX900 | none expected |
| C2 | 64x64 BGRA linear surface, window enabled with control `0x05` (32 bpp, rotate type 0, tile 0) | reads equal the raw data (identity), guards intact | low |
| C3a | 256x64 BGRA tiled (`P=1024`), raw pattern `T`, candidate A (`px`), control `0x25`; read the linear view one dword, then all | `linear[y*P + 4x] == T(x,y)` for all pixels.  Decides candidate A | low (first live translation) |
| C3b | only if C3a fails: same with candidate B (`P>>5`) | same | low |
| C3c | winning candidate at 800x600 and 1600x1200 (`P=3200/6400`) | exact at every pixel, especially `x = px-1` at large `y` | low |
| C4 | window enabled, CPU **writes** a linear pattern: (a) dword UC, (b) byte/word, (c) 16-byte SSE, (d) WC `memcpy` through the `pgprot_writecombine` map; window off, compare raw memory with `tile8` | exact; guards intact.  Decides the burst and byte-mask behaviour | low |
| C5 | bounds: read/write `base-16`, `end`, `end+16`; `hw.width < px` variant | outside `[base,end)` is untranslated; records the width-clip semantics | low |
| C6 | 16-bpp tiled surface: control bpp `01` with `px = P/2`, then bpp `1x` with `px = P/4` | both give the same byte mapping (byte-based model) | low |
| C7 | GPU: render a tiled RT (A51 bit 23) into SL with the window off.  Then (a) enable the window and compare the linear view with `chrome9_tiled_copy` of the raw memory; (b) repeat the draw with the window **enabled** and compare the raw memory with (a) | (a) equal; (b) GPU writes not translated | low-medium (existing R2 draw) |
| C8 | bit 7: raw pattern with unique dwords, control `0xa5`; map every linear dword back to its raw offset | records the 16-row mapping: 32x16 (inferred) or other.  Needed before any 16-row surface use | low |
| C9 | two windows (14, 15) on two surfaces at once; then reprogram 15 while 14 stays active | independence; no residue after release | low |
| C10 | 1600x1200 BGRA: time the window read and WC write against `chrome9_tiled_copy` on the same mapping | decides whether Mesa should use CBU for reads, writes, or both | low |

Abort criteria: any guard, GART-hash or scanout-hash change, any CPU stall
over 1 s, or a C2 mismatch.  In each case stop the ladder, power-cycle, and
record the step.

## 7. Integration outline (after the ladder passes)

* Kernel:
  * a 16-slot window allocator (skip slots 0/1);
  * a per-BO/level window assigned on CPU access to a tiled SL BO (mmap
    fault or the CPU-access ioctl);
  * LRU steal with PTE zap, disable on release/unload/suspend/reset;
  * a new CAP bit.
* Mesa:
  * `chrome9_resource` with `local && tiled` maps the level linearly while
    the kernel reports an active window, and skips `chrome9_tiled_copy`
    for it;
  * SF tiled resources keep the software path.

## Remaining unknowns

* Pitch unit in tile mode on VX900 (A: pixels, B: `pitch>>5`), C3.
* Burst splitting and byte masks for WC/SSE stores, C4.
* Width clipping between visible width and pitch, C5.
* The bit 7 mapping (16-row), C8.
* Whether SR42 bit 7 = 0 forces the CBU to an identity mapping.
* Reciprocal precision (exact versus truncated), C3c.
* Whether GPU engine accesses bypass the window (inferred yes), C7.
* Whether the UMD lock uses `AcquireAperture`; not traced.

## 8. CPU path to SL on the target (D, read only, 2026-09-25)

`lspci -xxx` and `/sys/kernel/debug/x86/pat_memtype_list` on the target,
no register written:

| register | value | meaning (source) |
|---|---|---|
| D0F0 RxB0 | `db` | bit 0 = 1: CPU cycles to the SL range (BAR2) are forwarded to GFXCTL (SPM "Access Capability for GFXCTL") |
| D1F0 RxB9 | `00` | bit 1 = 0: direct frame buffer access (DFA) off (SPM D0F3 RxA1 note; the OGPM lists B3-F2 as reserved) |
| D0F2 Rx9E | `cc` | bit 1 = 0: "Tile Mode Address Windows" off; the SPM defines it only for DFA mode, where it routes the 16 tile windows back to GFXCTL |
| D0F3 RxA1:A0 | `ed00` | graphics on, 256 MiB, DFA base `0xd0000000` (unused while DFA is off) |
| MTRR | `0xd0000000` 256 MiB uncachable | |
| PAT | `d0000000-e0000000` UC-, `d1000000-dffef000` UC- | the whole-BAR2 `pcim_iomap` (UC-) is reserved first; the later `ioremap_wc` of the local pool and the WC user mappings of local objects inherit UC- |

**I:** with DFA off, every CPU access to BAR2 passes GFXCTL, so the CBU sits
on that path and Rx9E bit 1 is not needed.  Ladder C0 re-reads these
registers and stops if SL cycles bypass GFXCTL.

**D:** local objects are mapped uncached, not write-combined, on this
kernel: PAT keeps the memory type of the earlier whole-BAR2 UC- mapping.
C4 "memcpy WC" and C10 "WC map" therefore measure UC- stores until BAR2 is
mapped UC only for the GART and occlusion pages.

## 9. Kernel ladder (`cbu_selftest`)

`drivers/gpu/drm/chrome9/chrome9_cbu.c`.  `cbu_selftest=N` runs steps 1..N
at module load, after the engine and the local pool are up and before
any client exists.  Window `cbu_window` (default 15, step 10 also uses
the one below), `cbu_skip` bit n skips step n (not C0), `cbu_pitch_mode`
forces candidate A (0) or B (1).

| N | step | surface(s) | stops when |
|---|---|---|---|
| 1 | C0 | none; dumps 16 windows, D0F0 B0, D0F2 9E, D0F3 A0, D1F0 B0/B2/B9, SR42 | a window is enabled, or SL bypasses GFXCTL |
| 2 | C1 | none; enable 0, two patterns per field, control `0xfe` | control does not read 0 afterwards |
| 3 | C2 | 64x64 BGRA linear, control `0x05` | any dword differs from the raw bytes |
| 4 | C3a | 256x64 BGRA tiled, pitch in pixels | mismatch with a forced mode; with mode -1 it hands over to C3b |
| 5 | C3b | same, pitch in 32-byte columns; skipped if C3a passed | mismatch |
| 6 | C3c | 800x600 and 1600x1200 BGRA, 256x128 and 800x600 RGB565, 100x61 BGRA (pitch 416), 50x33 RGB565 (pitch 128) | mismatch |
| 7 | C4 | 256x64 BGRA: dword, byte/word, qword, WC memcpy; 100x61 dword; 256x128 RGB565 byte/word | raw bytes differ from the linear writes (tail rows must stay raw) |
| 8 | C5 | 100x61 BGRA: `base-16` and `end` read and write, window width 100 of 104 | `base-16` translated, `end` neither raw nor tiled, or a pixel below the width wrong |
| 9 | C6 | 256x64 raw bytes programmed as 16 and 8 bpp | mismatch with the byte model |
| 10 | C9 | windows 14+15, then 15 moved to a third surface | any mismatch, or the released range not raw |
| 11 | C10 | 1600x1200 BGRA: throughput | window writes wrong |
| 12 | C8 | 256x64 BGRA, control `0xa5` | mapping is not the inferred 32x16 tile |

Every raw dword holds `0xa5 << 24 | offset / 4`, every linear write
`0x5a << 24 | linear offset / 4`, so a mismatch line decodes the source
tile position.  Each case checks both 4-KiB guard pages (`13579bdf`), and
each step the xxh64 of the last 68 KiB of SL (occlusion page and GART
table); the window is disabled before the check, and all touched windows
are restored from the C0 snapshot at the end.

## 10. Silicon results, 2026-09-25

Kernel `c38d0a7` (the ladder only), module k131-cbu, one load per step,
no lock-up, no guard or GART hash change.

| step | result | dmesg summary |
|---|---|---|
| C0 | pass | all 16 windows, every field `0`; `D0F0.B0=db D0F2.9E=cc D0F3.A0=ed00 D1F0.B0=01 B2=40 B9=00 SR42=30: SL via GFXCTL 1, DFA 0, NB tile windows 0` |
| C1 | pass | masks ctl `000000fe` (bit 0 not tested), base `1ffffff0`, end `3ffffff0`, recip/pitch/hw `ffffffff`; `1234567f/2345678f` read `12345670/23456780` |
| C2 | pass | control `05` on a 64x64 linear surface: 4096/4096 dwords unchanged |
| C3a | mismatch | 256x64 BGRA, pixel pitch: 128/16384 match, **untranslated 16256**, other 0 |
| C3b | mismatch | same with column pitch: identical counts, ladder stopped |

Findings:

* **D:** every field latches with the documented widths: base `[28:4]`,
  end `[29:4]`, reciprocal, pitch and size all 32 bits.  The manual note
  (l.6133) that base/end `[28:16]` and the reciprocal read 0 does not
  apply to VX900.  C1 did not test control bit 0; step R logs it.
* **D:** with the window enabled, every CPU read of BAR2 returns the raw
  dword at its own offset.  The 128 "matching" dwords are exactly those
  whose tiled and linear offsets coincide: `992 r = 224 c`, i.e. row
  `8k` at tile column 0 and row `8k+7` at column 31, 8 dwords each, in
  8 bands.  The window has no effect for either pitch unit, so neither
  the pitch encoding nor the reciprocal is the cause.
* C2's identity pass therefore proves nothing about the window: an
  inactive window gives the same result.

## 12. Why the window does not translate, and the next probe

Candidates, in order of evidence:

1. **The CBU sees only the direct-access (DFA) path (S, M, I).**
   * SPM D0F2 Rx9E[1] "Tile Mode Address Windows Support": under DFA the
     DFA range "can be divided into several (16) windows", and cycles in a
     window with the "tile mode accessible" attribute go to GFXCTL, all
     others straight to DRAM.  16 is the CBU window count.
   * `via_drv.so` `ProbeRotateCaps` (`drv:0x7ffe0`) keeps the rotation
     option only if `IsSupportDirectAccessWindow(chip)` (table
     `drv:0x8b6a4`, 1 for chip indices 2 and 4..8).  It selects the CBU
     path (`[pVia+0x510] = 1`) only if the chip has no 2D rotation engine
     (table `drv:0x8b6c0`, 1 for indices 2 and 6..8).  Which index
     VX900 has was not traced.  If it is 6..8, the X driver rotates
     through the 2D engine on VX900, and the "vendor-exercised CBU use" of
     section 2 does not exist on this chip.
   * The BIOS programmed the DFA base D0F3 RxA1:A0 to exactly BAR2
     (`0xd0000000`, 256 MiB) but left DFA off (D1F0 RxB9 = 00) and
     Rx9E[1] = 0.
   * **I:** without DFA, CPU cycles to BAR2 reach GFXCTL as ordinary
     downstream PCI memory cycles.  GFXCTL may serve them through its
     memory interface without passing the CBU, which would be on the NB
     direct-access interface.
   * Against it: neither the Windows KMD, `via_drv.so` nor
     `via_chrome9.ko` writes D0F2 9E, D1F0 B9 or D0F3 A0/A1 (constant
     search, S).  Windows tiling needs the VBIOS `BiosEnableTile`
     capability (chrome9-tiling-re.md), so a tiling-capable BIOS may be
     what sets them.
2. **Control bit 0 does not latch** (not measured).  C1 wrote `0xfe`, and
   the arm check masked bit 0 out.  Step R logs the control readback.
3. **SR42 bit 7 = 0 forces the CBU linear** (M l.1984 lists CBU among
   the forced engines).  The 3D engine tiles regardless (tiling note),
   so this is weaker, but the CBU is a different unit.
4. **Only stores are translated.** Rotation is a write-path feature
   (the CPU draws, scanout reads).  C4 never ran.  Step R does one
   fenced write.
5. **The tile function is unsupported but rotation works.** Step R reads
   a rotate-180 window: rotated dwords there, with raw tile reads, would
   isolate the tile function.

Excluded: the pitch unit and the reciprocal (both candidates behave the
same), the register block (fields latch at MMIO `0x1e00` with the
documented widths), and the address domain as an FB offset.  Windows
also writes `CPUTranslatedAddress - BAR` and only stores the unchanged
address in the allocation (`kmd:0x93d5d`); no other register is written
around the six window registers (`kmd:0x93c3f..0x93d24`).

### Step R (`cbu_route`, patch "CBU routing probe")

`cbu_selftest=13 cbu_route=<bits>` runs C0, C1 and R (steps 3..12 are
skipped).  R fills a 256x64 BGRA surface, applies the routing change,
and re-reads the surface and the GART hash with the window off; any
difference restores and stops before anything is written.  It then
runs:
* tile reads with both pitch units;
* a rotate-180 read;
* a fenced dword tile write.

Each result is logged as counts of raw / tiled / rotated-180 / other
dwords, with the control readback.  Every change is restored and logged
at the end of R.

| `cbu_route` | change | risk |
|---|---|---|
| `1` | none (probe only: bit 0, stores, rotation) | as C3 |
| `3` | D0F2 Rx9E[1] = 1 (bit 5 must be 0, it is) | low: SPM defines it only under DFA |
| `7` | Rx9E[1] and DFA (D1F0 RxB9[1] = 1); refused unless the D0F3 DFA base and size equal BAR2 | medium: every CPU BAR2 access changes path; a wrong translation would read or write other DRAM.  The read-back check runs before any write |
| `9` | SR42 = `0xb0` | low-medium: DRAM arbiter register with scanout live (tiling note E3) |

Suggested order: 1, 3, 7, then 9 (and `0xf` only if 7 and 9 each change
something).

### Step R results, 2026-09-25

Kernel `0b6ea00` (route patches on `ac343fb`, write-combined pool), one
load per value, no lock-up.  Every run ended with `restored D0F2.9E=cc
D1F0.B9=00 SR42=30` and step R "pass".

| `cbu_route` | change as read back | tile read pixels / columns | rotate 180 read | tile write |
|---|---|---|---|---|
| 1 | none | 16384 raw, 0 tiled / same | ctl `15` read `15`: 16384 raw | 128/16384 tiled, 16256 untranslated |
| 3 | D0F2 9E `cc -> ce` | all raw | all raw | same |
| 7 | 9E `ce`; DFA base `0xd0000000` size 256 MiB equal to BAR2; **D1F0 B9 `00 -> 00`** | all raw | all raw | same |
| 9 | SR42 `30 -> b0` | all raw | all raw | same |

Findings (D):

* Control bit 0 latches (`25` reads `25`, `15` reads `15`), so the
  window is enabled in the register.
* Neither tile nor rotate function changes any CPU read or write of
  BAR2 while CPU cycles reach GFXCTL through the SL BAR path.  This holds
  with Rx9E[1] set, and with SR42 bit 7 set.
* The CBU therefore does not act on the SL BAR path.  SR42 bit 7 and the
  NB tile-window bit alone are not the missing enable.
* D1F0 RxB9 does not accept a byte write of bit 1, so DFA was never on:
  the route 7 result says nothing about DFA.

### Why D1F0 RxB9 does not latch (open)

Evidence collected (S, M; none measured beyond the byte write):

* No vendor component writes it.  The board VBIOS (`VT3410 Embed NoTV
  Ver07`) reaches PCI configuration only through its CF8/CFC helpers
  (`0x6caf` read, `0x6ccb` write).  Their only callers touch D0F0 RxC4
  (bit 17) and read D1F0 Rx18 (BAR2).  `VTROM.bin` has the same two
  helpers.  The Windows KMD, `via_drv.so` and `via_chrome9.ko` contain
  no D1F0 B0/B9 or D0F2 9E access (constant search).  DFA is therefore
  a system-BIOS decision, and this BIOS left it off after setting the
  DFA base to BAR2.
* SPM: D1F0 RxB9 exists, with bit 1 = DFA enable (D0F3 RxA1 text) and
  bit 0 = debug-signal routing to VCP/DVP1 (D0F4 Rx41[5] text).  The
  OGPM lists D1F0 B3..F2 as reserved.
* OGPM D1F0 RxB0 (Memory Base Control): bit 3 "UNLOCK", RW, default 0,
  no description; it reads `01` on the target.  It is the only
  documented lock near B9.
* The D0F3 DFA fields are "RWS" (sticky, BIOS-owned); a matching
  write-once or strap attribute on D1F0 B9 would explain the result,
  but no document says so.

Candidates: (a) B9 is write-protected until D1F0 RxB0[3] UNLOCK;
(b) this configuration block ignores byte enables at a non-aligned
offset, so a dword write is needed; (c) DFA is fused or strapped off, or
BIOS-locked on this board.

### Next probe (patch "CBU routing probe, DFA enable variants")

Two new `cbu_route` bits:
* bit 4: write B9 as a read-modify-write of the B8 dword;
* bit 5: set D1F0 RxB0[3] before the change and restore it after.

D1F0 B0..BF are logged before, after and on restore.  If B9 bit 1 still
reads 0, step R restores and ends without the window probe.  If it
latches, the existing read-back check (surface and GART hash with the
window off) runs before any write through DFA.

Order: `0x17` (dword write), then `0x27` (UNLOCK, byte write), then
`0x37` (both).  If none latches, DFA is not reachable from software on
this board, and the CBU is unusable as a CPU detiling aperture here.
CPU access to tiled surfaces then stays on `chrome9_tiled_copy` or a
2D-engine detile blit.

## 11. Production path (`cbu_linear`, default off)

* **uAPI** (`GEM_LINEAR_VIEW` 0x25, CAP2 bit 32 `LINEAR_VIEW`): `ADD`
  registers a tiled level of a local object (page-aligned offset, pitch a
  multiple of 32 and at least 64, cpp 2 or 4, tiled extent
  `pitch * ALIGN(height, 8)` inside the object, no overlap); `REMOVE`
  names it by offset.  CPU accesses to `[offset, offset + pitch*height)`
  of the object's own mapping are then linear; the device keeps the tiled
  layout.  The CAP bit needs `cbu_linear=1` and a known pitch unit
  (`cbu_pitch_mode` or the ladder in the same load).
* **Ownership:** windows 2..15 (14) are shared by all views of all
  objects; 0/1 stay free (vendor X rotation).  Local objects map on
  demand; a fault in a view's pages takes a free window, else steals the
  least recently used one after zapping the owner's PTEs of that range.
  At most 16 pages of one segment are inserted per fault, so every PTE is
  covered by exactly one live window.  A page-aligned view offset keeps
  every page in at most one view.
* **Lifetime:** `ADD` and `REMOVE` zap the range; object free releases
  its windows before the SL range is returned; unload disables every
  window after zapping.  A window therefore never outlives or crosses its
  object, and a translated access stays inside the object's tiled extent.
* **Mesa** (not implemented): for `local && tiled` resources with a
  page-aligned level offset, `ADD` the level after creation; transfers
  then use `bo.map + level_offset` with the level pitch instead of the
  staging copy, and swfb, VS texture and copy paths call
  `chrome9_tiled_copy(..., tiled = false)` for such levels.  Before
  `chrome9_context_demote_local` copies the object byte for byte, the
  views must be removed (or the copy must detile explicitly), or the GART
  copy receives linear bytes where the device expects tiles.
* **Open:** GPU accesses to an active window range (C7) must bypass the
  translation; not covered by the ladder yet, since it needs a tiled draw
  into a local object while a view is mapped.
