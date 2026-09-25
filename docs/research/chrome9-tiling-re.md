<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 HD (VX900) tiled surface layouts

Status (2026-09-25): static recovery only.  No tiled word has been run on
silicon.  The kernel and Mesa keep every surface linear.  Evidence labels:

* **M**: VX900 manual.  Part I is Core/2D, Part II is 3D/Video (`OGPM_Chrome9 HD DX9 _R100_Part*`).
  Line numbers refer to the `pdftotext -layout` extractions in `~/tmp/ogpm/`.
* **S**: static code, from these binaries:
  * `VTGUModeDX32A.dll`, SHA-256 `8ac6959f…`: bare `0x10…` addresses.
  * `VTGKModeDX32A.sys`, SHA-256 `61812960…`: `kmd:` addresses (image base `0x10000`).
  * `via_chrome9_dri.so`, SHA-256 `fddb4dea…`: `dri:` addresses.
  * `via_drv.so`, SHA-256 `55324613…`: `drv:` addresses.
  * `via_chrome9.ko`, SHA-256 `017beb61…`: `ko:` addresses.

  Full hashes are in [windows-driver-re.md](windows-driver-re.md) and
  [via-5.75-dri-state.md](via-5.75-dri-state.md).
* **I**: inference.

Why this matters: every surface is linear in SF today.  The following
measurements show that memory locality is the limit:

* 1600x1200 BGRA GPU mip generation with 4 nearest taps runs at about 135 Mtexel/s.
* Bilinear sampling is slower than that.
* Depth-tested fill runs at about 120 Mpixel/s.

## Summary

1. **One tile format, no macrotiles, no bank swizzle (M, S).**
   * A tile is 32 bytes wide and 8 rows high: 256 bytes.
   * Inside a tile the layout is row-major.
   * Tiles are laid out row-major across the surface pitch.
   * The width in pixels is 32/16/8 at 8/16/32 bpp.
   * A second mode is 32 bytes by 16 rows (512 bytes).  The hardware defines it, but no vendor driver ever programs it.
2. **The 3D engine can tile every surface class (M, S).**

   | surface | tile bit | 16-row bit |
   |---|---|---|
   | colour RT0..3 | A51/A59/A61/A69 bit 23 | bit 22 |
   | Z/stencil | A12 bit 0 | none |
   | texture | A22 bit 17 | A22 bit 16 |
   | 2D engine destination | reg `0x10` bit 31 | none |
   | 2D engine source | reg `0x18` bit 31 | none |
   | IGA1 scanout | CR43 bit 2 | none |
   | IGA2 scanout | CR62 bit 0 | none |
   | HQV / video overlay | own tile modes | none |

   Pitch fields keep the same value in both modes: byte pitch / 32, which is also the number of tile columns.
3. **Tiling is off by default in every vendor driver (S).**
   * Windows tiles only when the VBIOS or registry enables it, and never tiles GART surfaces.
   * Windows tiles textures only in local frame buffer (SL).
   * Linux 5.75 contains complete tiled paths, but forces all enables to 0.
4. **A CPU detiling window exists: CBU "rotation" windows at MMIO `0x1E00..0x1FFF` (M, S).**
   * There are 16 windows.
   * Windows uses them as WDDM swizzling ranges.
   * They translate CPU accesses in an SL byte range (I: BAR2 frame-buffer path).
   * They cannot help CPU mappings of SF shmem pages.  Those need software or blit detiling.
5. **No fence or surface-range registers exist besides CBU (S).**
   * Relocation selector 16 is a start/end byte-nibble patch, not tiling.

## 1. Tile geometry and address equation

### 3D surfaces (S, M)

Four independent sources give the same layout:

* Linux `sfmCalculateTileOffset_invi` (`dri:0xdee30`).  It was executed under qemu: `(17,9,256)` gives `0x831/0x1122/0x2224` at 8/16/32 bpp.
* Windows `0x1020c1d0`.
* The Z pitch definition (M Part II l.5079).
* The HQV equations (M Part II l.3739).

```c
/* x, y in pixels; pitch in bytes (multiple of 32); cpp 1, 2 or 4 */
static inline uint32_t tile8_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t cpp)
{
	uint32_t tw = 32 / cpp;                      /* pixels per tile row */

	return (y >> 3) * pitch * 8 + (x / tw) * 256 + (y & 7) * 32 + (x % tw) * cpp;
}
```

The standard form is `256 * (tile_columns * (y/8) + x/tw)`, with `tile_columns = pitch / 32`.

Consequences:

* **Contiguous runs.** Each 32-byte tile row is contiguous.  A CPU tile or detile is a gather of `memcpy(32)` per tile row.  The Windows retile fast path `0x1020c650` copies whole tiles.
* **Tile row stride.** One row of tiles covers `8 * pitch` bytes, the same as eight linear rows.  A tiled surface therefore occupies exactly `pitch * align(height, 8)` bytes.  Every tile row starts at `base + 8*pitch*k`.

### HQV confirmation (M)

The HQV source-mode field (Part II l.3739, Rx3DC[31:30]) expresses the same layout with X in 128-bit units and Y in lines:

```text
01  256 bits x 8 :  SA + Y[10:3]*PTH*8  + {X[7:1], Y[2:0], X[0]}
10  256 bits x 16:  SA + Y[10:4]*PTH*16 + {X[6:1], Y[3:0], X[0]}
11  512 bits     :  SA + Y[10:4]*PTH*16 + {X[6:2], Y[3:0], X[1:0]}
```

* **Mode `01`** is the 3D 8-row tile: 32 bytes x 8 rows.
* **Mode `10`** is the "16-pixel-high" RT/texture tile: 32 bytes x 16 rows, 512 bytes.
* **Mode `11`** (64 bytes x 16 rows, 1 KiB) has no 3D counterpart in the 3D register pages.

The HQV destination (Rx3EC bit 30, l.3862) and the V1 overlay ("Adder Tile Mode", Rx230 bit 27, l.784) use the same equations.

### 16-row mode (M; not produced by any vendor code)

This is the inferred form:

```text
offset = (y >> 4) * pitch * 16 + (x / tw) * 512 + (y & 15) * 32 + (x % tw) * cpp
```

* The manual gives the RT pitch as a count of 256- or 512-byte tiles, "depend[ing] on HMRT0TileH16" (Part II l.6144).  The field still counts 32-byte-wide tile columns.
* No vendor setter exists for A51 bit 22 or A22 bit 16 (S: RT builder `0x101fa18a..0x101fa3f1`, texture builder `0x101d7c10`, Linux RT/texture builders).
* Treat this mode as an experiment only.

### No swizzle (M, S)

* No XOR or bank bit appears in the HD, HC3 or HCM manuals, in the Linux tile helpers, or in the HQV equations.
* `S3GDW_DisableTileSwizzle` is a UMD-only registry option.  It is read at `0x100eac42` into option `+0x138`, defaults to 0, and has no located consumer.  The KMD does not know it.

### Separate stencil (M, for completeness)

Part II l.5521 (A28) gives the separate stencil layout:

```text
addr = HSTBas*256 + Y[10:3]*HZWBPit*64 + X[10:3]*64 + Y[2:1]*16
HZWBPit = (W+7) >> 3        # linear or tiled Z alike
```

The separate stencil buffer is therefore always 8x8-blocked (64 bytes per block).  The current driver does not use it.

## 2. Register fields

| word | field | linear | tiled | notes |
|---|---|---|---|---|
| RT0 A51 | bit 23 | 0 | 1 | Windows sets it from resource `+2c` bit 1 (`0x101fa162`). Linux sets it from surface `+20` bit 1 (`dri:0x195e90`). RT1..3 repeat this in A59/A61/A69. |
| RT0 A51 | bit 22 | 0 | 0 = 8-row, 1 = 16-row | Never set by vendor code. |
| RT0 A52 | `[9:0]` | pitch/32 | pitch/32 (tile columns) | [render-target-format-oracles.md](render-target-format-oracles.md) |
| Z A12 | bit 0 | 0 | 1 | Windows computes `(flags >> 1) & 1` in the RT/Z builder. |
| Z A11 | `[22:13]` | pitch/32 | pitch/32 (256-byte tile columns) | M Part II l.5079 |
| texture A22 | bit 17 | 0 | 1 | Windows `(A22 & fffd7fff) \| tiled<<17 \| pot<<15` (`0x101d7c10`). |
| texture A22 | bit 16 | 0 | 0 = 8-row, 1 = 16-row | Never set by vendor code. |
| texture A22 | bit 15 | POT | Windows keeps POT for tiled, compressed and cube (`0x101d7cc6..0x101d7ce5`) | With POT set, the vendor skips the level table, so the hardware derives level offsets. |
| FE A00 | bit 1 | 0 | 0 | 0 = "fetch 4 QWs (512 bits) for tiled-mode texture", 1 = 2 QWs (M Part II l.7699). VX900 always programs 0 (S `0x10154d25`, caps gate `0x101bf8a1`). The current driver emits 0. |
| 2D `0x10` | bit 31 | 0 | 1 | Destination tiled. Pitch in reg `0x08` is unchanged. Base (`0x14`/`0x1c`) relocations do not touch the bit. |
| 2D `0x18` | bit 31 | 0 | 1 | Source tiled. Set by Windows `kmd:0xc88a0`, `0xbf190`, `0xc1010`, `0xc1e10`, `0xbaac0` from the allocation's tiled flag, and by Linux M1 `dri:0xe4a54`, `0xe6cd0..0xe6e80`. |
| CR43 | bit 2 | 0 | 1 | IGA1 tiled scanout (`kmd:0x7bbc0`, called from mode set `0x445bf`). |
| CR62 | bit 0 | 0 | 1 | IGA2 tiled scanout. Same Windows code. Linux `via_drv.so` only preserves the bit. |
| SR42 | bit 7 (RO) | | | "0: force all engines to use linear addressing mode; 1: decided by engine" (M Part I l.1984). Read it before any experiment. |

The manual gives no 2D tile geometry.  **I:** the 2D engine uses the same
256-byte tile.  Windows presents from tiled sources, and Linux fills tiled
surfaces, with ordinary pitch values.

## 3. What the vendor tiles, and when

### Windows D3D UMD (S)

**Option loading.**

* The registry reader is at `0x100e9650`, called from `0x100f0741`, and fills the option block at `adapter+0x230`.
* The KMD asks the VBIOS through `int10 AX=4F14 BX=0200` (`kmd:0x117fa7`).  Capability bit 30 becomes `UseVbiosControlTile` and bit 29 becomes `BiosEnableTile`.  The KMD writes both to the registry (`kmd:0x12078c..0x1207cf`).
* `BiosEnableTile` presets the four keys below; they default to 0.  If `UseVbiosControlTile` is set, the registry values are not read.
  * `HalcyonTiledTexEnable` (`+0x270`)
  * `HalcyonTiledRTEnable` (`+0x274`)
  * `HalcyonTiledZWEnable` (`+0x278`)
  * `HalcyonTiledDisplayEnable` (`+0x27c`)
* The enables are copied into a static tile-info block at `0x103379b8`.  That block is byte-identical to Linux `hwmTileInfo_Innovation` (`dri:0x411640`): tile size 256, tile width 32 bytes, tile height 8.
* Application profiles `0x5f` and `0x1cd` force texture and ZW tiling on (`0x101c02ab`).
* When all four enables are 0, the KMD sets adapter `+0x9c8` bit 7 (`kmd:0x121ac9`).

**Per-resource decision** (`0x101d5340`, called through the chip table `0x10331258+0x38`):

* RT, texture and depth are tiled per format class when their enable is set:
  * colour: RGB565/555, ARGB8888 and similar formats;
  * P8: texture only;
  * depth: `0x101d`, `0x2032/33`, `0x203f/40`.
* Primaries are tiled only with the Display enable.
* The following are always linear:
  * GART-located surfaces (`desc[0] == 4`);
  * system-pool resources (`0x101030c9`);
  * DXT (`0x101d5600`, `0x101d7d85`).
* Textures are re-decided in `0x101345b0`: tiled only with the Tex enable **and** location 2 (local frame buffer).  The texture pitch re-tile path (`0x10133c5e`) also excludes cube, volume, DXT, and surfaces smaller than one tile.

**Allocation** (`0x101d5600`, `0x10102f60..0x101030b5`):

* Pitch is `max(32, align(w*cpp, 32))`.
* A tiled surface rounds width to the tile width and height to 8.
* Size is `max(align(h,8)*pitch, ceil(pitch/32)*ceil(h/8)*256)`; these are equal for tiled surfaces.
* Base alignment is 256 bytes (`0x10337a58`).
* Mip levels (`0x101d6a40`):
  * pitch `align(w*cpp, 32)`, `align(h, 8)` rows;
  * tiled levels are rounded to 256 bytes;
  * tiled and linear offsets are identical for non-DXT formats;
  * a 1x1 level is one tile, and the whole chain stays tiled.
* Cube: 256-aligned chain times 6.

**CPU access.**

* Lock of an SL surface returns linear addressing (`0x10104710`).  **I:** this works because the KMD maps it through a CBU swizzling range (section 4).
* GART surfaces and small textures use a linear shadow copy:
  * lock: CPU detile (`0x10135f10`, `0x1020da60`);
  * unlock: CPU retile (`0x101362e0`, `0x1020c650`, `0x1020ccc0`) or a 2D blit.

### Linux 5.75 (S)

* `sfmGetSurfaceFlags_inv` (`dri:0xeb850`) and `sfmiPatchTexture_inv` (`dri:0xdd2c8`) set the tiled flag, gated by `hwmTileInfo_Innovation` RT, texture and Z entries.
* `osGetRegistryOptions` sets those entries to constant 0 (`dri:0xa44be..0xa44d2`).  No environment or driconf switch exists.
* Lock goes through `ClaimTiledApertureAccess`, then a staging surface with software detile (`sfmSWWriteLinear_invi`, `sfmSWWriteTile*`, 11 callers of the tile-offset helper at `dri:0xdf8d4..0xe0c2a`).
* `hwmAllocateAperture_inv` (`dri:0xae000`) programs the CBU windows, but is disabled (`reg[0x3ac]=1`).
* `via_chrome9.ko`: the aperture ioctls `0x1880`/`0x1890` are stubs.  The verifier rejects `0x1E00..0x21FF` ("CBU ROTATE SPACE").
* `via_drv.so` never sets tiled scanout or the 2D tile bits.

**Conclusion (S/I):** no shipped vendor configuration exercises tiling by
default on VX900.  Windows enables it only through the VBIOS capability or the
registry, and then only for SL surfaces.  Tiled SF (GART) surfaces were never
produced by the vendor, so they are the least-proven combination.  That is
exactly the combination our driver would use first.

## 4. CPU detiling: CBU windows (M Part I l.6117-6234, S)

Sixteen windows are at `0x1E00 + n*0x20`:

| offset | field |
|---|---|
| `+00` | control: bit 0 enable; `[2:1]` bpp (`00` 8, `01` 16, `1x` 32); `[4:3]` rotation (0/90/180/270); bit 5 tile function (vs rotate); bit 6 rotate with tile; bit 7 16x256 instead of 8x256 tile mapping |
| `+04` | base, byte address `[28:0]`, 16-byte granular |
| `+08` | end, byte address `[29:0]` |
| `+0C` | 1/source pitch in pixels, 0.32 fixed point (manual: pitch 256 gives `0x01000000`) |
| `+10` | destination pitch `[31:16]` and source pitch `[15:0]`, in pixels |
| `+14` | height `[31:16]` and width `[15:0]`, in pixels |

**Windows `S3gAcquireSwizzlingRange`** (`kmd:0x936e0`, verified instruction by instruction here):

* It reports `NumberOfSwizzlingRanges = 16` (`kmd:0x88bf6`).  All 16 windows are zeroed at init for `7122`/`a122` (`kmd:0x3b4a7..0x3b586`).
* It rejects a segment other than `[adp+0x10ac]` and ranges already in use.  Then it writes:

```text
1e04 = CPUTranslatedAddress - [adp+0x640] + subresource_offset   # SL byte offset
1e08 = 1e04 + pitch_bytes * rows
1e10 = pitch_px << 16 | pitch_px          # non-rotated
1e0c = 2^32 / pitch_px                    # 7122 (adapter 0x9c0 bit 23; _alldiv 0x5da40)
1e14 = rows << 16 | width_px
1e00 = 1 | (bpp16 ? 2 : bpp32 ? 4 : 0) | (tiled ? 0x20 : 0)      # control last
```

* Release (`kmd:0x93da0`) clears bit 0.
* The subresource offset walks mip levels, halving rows, width and pitch while the pitch is greater than 32 bytes.

**I:** the window sits on the CPU path into the SL frame buffer, i.e. BAR2
offsets relative to `[adp+0x640]`.  On VX900 SF memory is ordinary system RAM
that the CPU reaches directly.  CBU therefore does not apply to the shmem-backed
SF BOs the driver uses today.  It becomes useful only for SL placement
([chrome9-local-fb-placement.md](chrome9-local-fb-placement.md)).  There is no
separate fence register, GART tiling bit, or PCI-config tiling range.  The D0F3
memory-controller manual is not in the tree, so this is not verified there.

## 5. Scanout and present

* **Windows:** the KMD sets CR43 bit 2 (IGA1) or CR62 bit 0 (IGA2) only when the primary allocation is tiled.  That requires `HalcyonTiledDisplayEnable`.  `S3gStopDevice` clears both registers (`kmd:0x128243`).
* **Pitch:** primary pitch stays `align(w*cpp, 32)` (`kmd:0x3f850`).  Tiled scanout needs no other pitch rule beyond 32-byte tile columns.  The bandwidth estimate uses factor 100 for tiled and 110 for linear (`kmd:0xd66a0`).
* **Present to a tiled primary** uses the 2D copy with tile bits (I: Present `0x100b7730` not fully traced).
* **For this driver:** keep scanout, dumb buffers, DRI3/dma-buf exports and anything CPU-shared linear.  A tiled back buffer is presented by the existing 2D copy with source bit `0x18[31]`.  **I:** that copy detiles while it copies, and needs rung R7 below.  Tiled KMS framebuffers (CR43/CR62) come last and are optional.

## 6. Guarded silicon experiment

Rules:

* Follow the existing target-safety rules: GPU lock, hash-gated module handoff, runs of 7 s or less, and NanoKVM power cycle for recovery.
* One variable per rung.
* The kernel needs a debug knob, `tile_re` (0600, default 0, only honoured with `DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED` guarded objects).  It XORs bits into the words the generic fragment draw already emits:

| `tile_re` bit | word | effect |
|---:|---|---|
| 0 | A51 | bit 23 (RT tiled) |
| 1 | A51 | bit 22 (RT 16-row) |
| 2 | A12 | bit 0 (Z tiled) |
| 3 | A22 of unit 0 | bit 17 (texture tiled) |
| 4 | A22 of unit 0 | bit 16 (texture 16-row) |
| 5 | 2D reg `0x18` | bit 31 (source tiled) |
| 6 | 2D reg `0x10` | bit 31 (destination tiled) |

**Surfaces.**

* 64x64 BGRA8888: pitch 256, 8 tile columns, exactly 16 KiB.  Tiled and linear extents are identical, so no tiled rounding can escape the object.
* Each object is `4 KiB guard + 16 KiB payload + 4 KiB guard`, using the mipdst layout (`destination_offset = PAGE_SIZE`).
* Guards are filled with `13579bdf`.  The payload is poisoned with `a5a5a5a5` before each draw.
* Check the SL GART table hash (last 64 KiB) before and after every rung.

**Pattern.** `T(x,y) = 0xff000000 | ((x ^ y) << 18) | (y << 10) | (x << 2)`:
unique, and exact through unorm8.  It is drawn by a single NEAREST TEX of
varying 0 into a full 64x64 quad (the proven mipgen path, 1:1 texel centres).

**Words** (SF location; `va` = GPU address of payload byte 0):

```text
RT      A50=50000000|va>>8   A51=51100000 (lin) / 51900000 (tile8) / 51d00000 (tile16)
        A52=52990008                       # ARGB8888, pitch 256/32
Z X8Z24 A10=10000000|va>>8   A11=11010001  # pitch 256/32 << 13, SF
        A12=123e0000 (lin) / 123e0001 (tile)  A13=13010000 (LESS)
Z D16   A11=11008001 (pitch 128)   A12=12380000 / 12380001
Tex     A18=18001101  A20=20040040  A21=21000001
        A22=22008066 (lin) / 22028066 (tile8) / 22038066 (tile16)
FE      A00 unchanged (bit 1 = 0: 512-bit fetch)
```

**Rungs.**

| rung | change | expected | on mismatch |
|---|---|---|---|
| R0 | read only: SR42, CR43, CR62, `lspci -xxx` | SR42 bit 7 = 1 | if 0, the engine is forced linear: stop |
| R1 | none: linear texture to linear RT | RT == T, guards intact | baseline broken: stop |
| R2 | `tile_re=1` | `mem[tile8_offset(x,y)] == T(x,y)` for all 4096 pixels, no byte outside 16 KiB changed | dump tiles 0, 1, 8 and the permutation; linear image means the bit is ignored |
| R3 | CPU writes T tiled; `tile_re=8` | linear RT == T | dump RT |
| R4 | R2 output as tiled texture into linear RT (`tile_re=8`) | RT == T | |
| R5 | X8Z24, `tile_re=4`: plane `z = 0.25 + 0.5*x/64 + 0.125*y/64`, then full quad z=0.5 green LESS | tiled Z words == linear-run Z words permuted by `tile8_offset`; colour equals the linear run | |
| R5b | D16 variant (tile 16x8 px) | as R5 | |
| R6 | `tile_re=3` and `tile_re=0x18` (16-row) | inferred tile16 equation | record the observed permutation |
| R7 | 2D COPY_RECT of R2 image, `tile_re=0x20` / `0x40` | linear dst == T / tiled dst == R2 image | 2D stays linear-only |
| R8 | after R1-R7 pass: 1600x1200 GPU mipgen and bilinear, 1024x1024 D24 depth fill, linear vs tiled; FE A00 bit 1 = 1 with linear textures | timing only | |
| R9 | optional, SL (the `vram_target_test` offset): R2/R3 with A51=51800000, A18 SL | as R2/R3 | |

**Pass criteria for R1-R7:**

* Exact equality.
* FE and 3D idle within the existing timeout.
* All guard pages and the GART hash unchanged.

Any hang or guard change disables the knob.  R2 and R5 cover the tiled SF
combination the vendor never produced (section 3); R9 checks SL separately.

## 7. Implementation plan (after R1-R7 pass)

**Kernel** (it owns validation):

1. Add capability bit `DRM_CHROME9_CAP_TILING`, gated by module parameter `tiling`, default 0.
2. Add per-surface layout flags in the draw UAPI: destination, depth, and texture[n] tiled.  No 16-row flag until R6 passes.
3. Validation:
   * base 256-aligned;
   * `pitch % 32 == 0` and `pitch <= 16352`;
   * extent `pitch * align(h, 8)` inside the object (the extended-destination guard rules apply unchanged);
   * a tiled texture must use the vendor level layout, i.e. `align(w*cpp,32) * align(h,8)` per level, 256-aligned.
4. Emit the bits from section 2.  The 2D COPY_RECT UAPI gets source and destination tiled flags, used by present and by resolves.
5. KMS keeps rejecting tiled framebuffers.
6. Later, with SL placement: CBU windows for BAR2 mmaps of tiled SL BOs.  Allocate one window per mapped BO from the pool of 16 and fall back to a linear staging copy when the pool is exhausted.  Userspace command streams must never reach `0x1E00..0x1FFF`.

**Mesa:**

1. Add a tiled flag to `chrome9_resource_layout()` for private resources only.  Excluded:
   * `PIPE_BIND_SCANOUT`, `SHARED`, `LINEAR`, DISPLAY_TARGET;
   * imported and exported resources;
   * DXT;
   * buffers;
   * surfaces narrower than one tile row (`w*cpp < 32`) or lower than 8 rows at level 0.
2. Level layout for tiled textures follows the vendor layout (section 3) and keeps A22 POT for POT sizes.  Mesa's current packed POT tail does not.
3. Transfers:
   * `texture_subdata` tiles on the CPU in 32-byte runs;
   * `transfer_map` of a tiled resource returns a linear staging buffer: detile on map for read, retile on unmap for write;
   * readback of GPU-written tiled RTs waits for the job fence first, as today.
4. Roll out one surface class at a time, each behind `CHROME9_TILING=<mask>`:
   1. sampled textures, for mip generation and bilinear;
   2. depth, private and normally never mapped;
   3. colour FBOs;
   4. window back buffer through the tiled-source 2D present.

   Measure each step against R8.  Keep the linear path as the conformance fallback.

**Expected effect (I only):**

* A 512-bit texture fetch covers two tile rows, 8x2 texels at 32 bpp, instead of 16x1.
* A 2x2 bilinear footprint and an 8x8 pixel block each stay within one 256-byte tile, instead of touching 8 rows that are `pitch` bytes apart.
* The gain on the UMA DDR path is unknown until R8.  The SF path's snoop and translation costs ([chrome9-local-fb-placement.md](chrome9-local-fb-placement.md)) are independent of tiling.

## Remaining unknowns

* SR42 bit 7 on the target (who sets it; RO in the manual).
* Whether tiled SF works at all, since the vendor excludes GART surfaces.
* The 16-row layout, and CBU bit 7.
* 2D tile geometry.
* The CBU address domain (SL only?).
* The consumer of `S3GDW_DisableTileSwizzle`.
* The VBIOS `4F14/0200` bit 29/30 values in `VTROM.bin`.
* The application IDs `0x5f`/`0x1cd`.
* The meaning of `ctx+0x1c` and `desc+0x34` bits `0x200000`/`0x8000` in the Windows decision.
