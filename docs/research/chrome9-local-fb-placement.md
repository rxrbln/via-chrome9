<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 (VX900) local frame buffer (SL) placement

Status: research note, 2026-09-24.  No kernel or Mesa change.  Evidence
labels:

* **M** manual: VX900 OGPM Part II 3D/Video (`~/tmp/ogpm3d.txt`) or Part I
  Core/2D (`docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`, extracted with
  `pdftotext -layout`);
* **S** static: disassembly or vendor source (`via_chrome9_dri.so` SHA-256
  `fddb4dea…`, `via_drv.so` `55324613…`, `VTGUModeDX32A.dll` `8ac6959f…`,
  vendor KMD source `binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/`);
* **D** dynamic: measured on the VX900 target (cited run, not repeated here);
* **I** inference.

## Trigger measurement

The kernel knob `vram_target_test=<offset>` in `chrome9_engine.c`
(`chrome9_build_fragment_program`, commit `fb9265f`) replaces only
`A50/A51` with `50000000|offset>>8` / `51000000`.  With
`tools/re/chrome9_fill_bench.c` (flat 800x600 fill) the commit records
383 Mpixel/s into SL against 141 Mpixel/s into SF (**D**, throughput only; no
pixel readback was recorded for the SL run).  The figure is per ioctl
wall-clock time.  The SF destination BO stays bound and flushed in both runs,
so the fixed CPU cost is the same.  The GPU-only ratio is therefore at least
2.7x (**I**).  In bytes this is 564 MB/s of SF color writes against
1.53 GB/s into SL.

## 1. Memory-location selectors

The manual uses one 2-bit code everywhere (Part II pp. 92--175; Part I
2D `08h`, DMA `DARn`, SR/PCI pages):

| code | name | VX900 status |
|---:|---|---|
| `00` | S.L. System Local Frame Buffer: the UMA carve-out, BAR2 | usable |
| `01` | S.F. System Dynamic Frame Buffer: GART/RTSF-translated system pages | usable (current driver) |
| `10` | S.M. system memory | "Reserved"; the old verifier rejects it |
| `11` | L.L. local-memory local frame buffer (discrete VRAM) | "Reserved": VX900 has none |

The SL address is a byte offset from the SL start, not a CPU or physical
address.  It is identical to the BAR2 byte offset.  **D**: the first H6
draw used BAR2 offset `0x07fd0000` both for the CPU fill and as the 2D base
`00ffa000 = 0x7fd0000>>3` and 3D `A50`
([hardware-lab.md](hardware-lab.md#first-verified-h6-3d-draw)).  The
hardware adds the SL physical base itself: SR6D/SR6E/SR6F[6:0] hold "Base
Address [28:21]/[36:29]/[43:37] of SL in System Memory", and SR68 / PCI
D1F0 `B2` give the SL size (**M**, Part I pp. 15, 46--47).

### Per-surface word and bit table

"Now" is what the current kernel emits.  "SL" is the change for a
local-frame-buffer surface.  Only the named bits change.  Base and pitch
fields stay the same except that they now carry an SL offset.

| surface | word(s) | location bits | now (SF) | SL form | address field | pitch / limits | evidence |
|---|---|---|---|---|---|---|---|
| color RT0 | type-01 `A50`, `A51`, `A52` | `A51[21:20]` | `A51=51100000` | `A51=51000000` (tiled: bit 23, H16: bit 22 unchanged) | `A50[23:0]` = byte>>8 (256 B aligned) | `A52[9:0]` = pitch>>5, 32 B aligned | M p.106; S Linux `STM_RenderTarget_INV` `0x196802..0x196814` (mem type 4→bit 20, else 00), Windows `0x101fa123..0x101fa1d6` + helper `0x101c9bb0`; D SL H6 draw and SL textured draw at `0x07fd0000`, SF draws; D 383 Mpix/s knob |
| color RT1..3 | `58/59/5a`, `60/61/62`, `68/69/6a` | `[21:20]` of 59/61/69 | not emitted | same as RT0 | same | same | M (RT1 page mislabels the field reserved), S Windows RT1..3 repeat at image `160/180/1a0` |
| depth/stencil (Z) | type-01 `A10`, `A11`, `A12` | `A11[1:0]` | `A11=11000001\|(pitch>>5)<<13` | `A11=11000000\|(pitch>>5)<<13` | `A10[23:0]` = byte>>8 | `A11[22:13]` = pitch>>5 (10 bit) | M p.92 "Location Setting of Z Buffer"; S Windows `0x101fa51a..0x101fa54a` (type 4→1, type 2+flag→3, else 0); S Linux family-8 has no writer, see note; D SF only |
| separate stencil | `A28` base, `A12[6:5]` | `A12[6:5]` | unused (`A12` bit 2 = 0) | `00` | `A28[23:0]` = byte>>8, pitch = Z pitch | M p.93; S Linux `STM_RenderTarget_INV` `0x1962d4`/`0x1967e0` sets `01` on its PCIe path |
| coarse Z | `A29`, `A2A` | `A29[21:20]` | unused | `00` | `A2A[23:0]` = byte>>8 | `A29[8:0]` pitch>>5 | M p.~99; S Windows `0x101fa786..0x101fa7f6` (helper<<20) |
| texture stage n, image | type-02 subtype n `A00..A05` (face L0 base), `A18` | `A18` `[1:0]` F0, `[3:2]` F1, `[5:4]` F2, `[7:6]` F3, `[15:14]` F4, `[17:16]` F5 | 2D `18001101`; cube `18015355` | 2D `18001100`; cube `18001300` | `A0f[23:0]` = byte>>8 (address bits 31:8) | pitch implicit from width/format; `A22` tile bits | M p.122--123 (two face labels misprinted, resolved by S `stmHWPrepareTextureRegs_invi` `0x1b27c5`, `0x1b286a`, `0x1b2c5c`, `0x1b2caa`; see [chrome9-format-cube-bridge-closure.md](chrome9-format-cube-bridge-closure.md)); D SL texture `0x07fc0000` Xorg oracle |
| texture levels L1..L8 | type-03 subtype `10`, entry `n*8+m-1` | entry `[31:30]` | `40000000\|off>>8` | `00000000\|off>>8` | `[23:0]` offset>>8 relative to L0 when `A18` bit 12 = 1 | 256 B aligned | M p.144--145; S Linux `(type==4)<<6` into byte 3 at `0x1b265e`/`0x1b2adc` |
| vertex buffers (all streams) | type-04 `A00`; type-03 subtype `30` palette | `A00[16:15]` (one field for all 16 streams) | `01` (TCL stage B, bit 15) | `00` | palette `[31:10]` = base>>10, element offset `A01+e[19:12]` in dwords | stride `[8:0]` dwords | M p.153--154; S Linux `0x17d291` (01 only for type 4), Windows KMD patch `0x3b` `0xb5e1b` (01 aperture, 00 local) |
| index fetch (FE43, not used) | FE43 header | header bit 0 | n/a | clear | 32-bit | n/a | S Windows `0x101b1430` (`\|1` for aperture); vendor Linux sends indices inline |
| 2D engine (COPY_RECT, fill) | H0 register `08` | `[31:30]` dst, `[13:12]` src, `[15:14]` pattern | copy `40001000\|pitches`; fill `40000000\|pitch` | SL→SF `40000000\|…`; SF→SL `00001000\|…`; SL→SL `00000000\|…` | `14`/`1c` `[25:0]` = byte>>3 (512 MiB, 16 B aligned) | `[26:16]`/`[10:0]` = pitch>>3, 16 B aligned, ≤16368 B | M Part I p.98--99; S Windows KMD SF→SL copy `0x2f8a2..0x2f95a`; D SL fill `08=00100000` regression |
| command ring | type-10 `A60` (and CMDQ `A70`) | `A60[1:0]`, `A70[1:0]` | `A60=60000001` | not recommended (see 2) | `A60[23:4]`+`A61` | 16 B aligned | M p.~70; S Windows ring `A60.bit0` = SF |
| traditional branch | type-11 `A6A` | `A6A[23:22]` | n/a | `00` | `A69[23:1]`+`A6A[7:0]` | n/a | M p.87 |
| FE33 / traditional fence | type-11 `A05..A07` | none | coherent DMA slot | cannot be SL | PCI-master write to a bus address (`HFCMode[2]=1`) | 16 B aligned | M p.82--83; [fe33-persistent-ring-production.md](fe33-persistent-ring-production.md) |
| PPC write-back | type-08 `A03` | `A03[23:22]` | unused | `00` | `A02`/`A03[7:0]` dword address | n/a | M p.174--175 |

Note on depth in the vendor Linux UMD (**S**): `STM_RenderTarget_INV`
`0x1961d8..0x196280` writes the Z base (`surface+140 >> 8`) into image `+40`
and the pitch (`surface+184 << 8 & 7fe000`) into `+44`.  It leaves
`A11[1:0]` untouched.  The only writers of that field are
`STM_ZWBEnable_INV` `0x195df0..0x195e07`, `(type==4)` → `01`, and
`stmInitChipImage_inv` `0x178f0e`, which clears it.  Both run only for chip
families `7` and `0xe`.  The family-8 path (`0x1792f0`, the VX900 family
according to [chrome9-throughput-re.md](chrome9-throughput-re.md#31-which-path-is-vx900s))
never writes it.  A search of every byte store to image offset `0x44` found
no other writer.  The vendor VX900 Z buffer is therefore never tagged SF.  It
is SL if the image starts zeroed, which was not proven (**I**).  Windows sets
the field per surface.

## 2. Address width, alignment, and mixing

* **Width.**  The 3D base fields are 24 bits in 256-byte units, so 32-bit
  offsets.  The 2D bases are 26 bits in qwords, so 512 MiB.  The vertex
  palette is 22 bits in 1-KiB units, so 32 bits.  The limit that applies is
  the SL size (SR68/`B2`, max 512 MiB), currently 256 MiB, which BAR2 exposes
  completely (**M/D**).  Any SL offset fits every field.
* **Safety (I).**  SL has no translation: the GPU address is SL physical base
  plus offset.  An offset at or beyond the SL size may reach system RAM
  outside the carve-out.  The manual does not say it faults.  The kernel must
  check every SL surface extent (base + pitch*rows, including tile rounding)
  against its SL allocator range.  Keep the last 64 KiB (the GART table,
  `engine_gart_offset`), any scanout carve-out, and firmware/VGA memory
  (`B0.bit0` "VGA in S.L.") out of the allocator.  The debug knob currently
  accepts an unchecked offset.
* **Alignment.**  RT/Z/stencil/coarse-Z/texture bases: 256 B.  Mip offsets:
  256 B.  RT/Z pitch: 32 B, 10-bit field, so 32736 B; keep the existing
  16352 B cap.  2D base/pitch: 16 B, pitch ≤16368 B.  Vertex stream base:
  1 KiB plus a dword element offset.  Ring/branch: 16 B.  These rules are the
  same for SF and SL (**M**).
* **Mixing inside one draw.**  Each surface class has its own location field,
  so a local color target with an SF texture and SF vertex buffer is legal by
  the register definition (**M**).  The vendor Linux texture residency code
  tries pool `0x22b8`, then `0x22bc` (`__glDpTextureMakeResident`
  `0x2012c2`/`0x2012ee`).  Its RT, Z and texture builders pick the location
  per surface.  The vendor therefore produces mixed draws in normal use
  (**S**).  One restriction applies: `A00[16:15]` covers all vertex streams,
  so every stream of one draw must share a location.  The vendor sends mixed
  or system-located streams to the `_VM` inline fallback (`0x1e7526`,
  `0x1e7586`) (**S**).  Silicon status: SL color with an SF ring and inline
  vertices is **D** (the knob and the first H6 draws).  All-SL color+texture
  and all-SF color+texture are **D**.  SL+SF within one draw and any SL depth
  are **not measured**.
* **Ring in SL.**  The field allows it.  Both vendors place the ring in the
  aperture (**S**).  CPU ring writes through a WC BAR2, with CR read-ahead,
  add hang risk without a known gain.  Keep the ring and FE33 slot in system
  memory.

## 3. The SF path: snoop, translation, and vendor defaults

What each SF access pays today:

1. **Snoop.**  Every production SF PTE sets bit 25 (`CHROME9_GART_CACHED`,
   `chrome9_collect_sgt_ptes`).  That bit is the Windows `MmCached`
   "cached/snooped" attribute: `VTGKModeDX32A.sys` ORs `02000000` exactly
   for `MmCached` and writes a plain PFN for `MmNonCached`/`MmWriteCombined`
   (`0x2e568..0x2e5a4`, `0x2df01..0x2df20`, **S**).  Linux WB-shmem
   correctness depends on the bit (**D**, see
   [sf-render-target-coherency.md](sf-render-target-coherency.md)).  All
   current SF traffic is therefore the snooped kind.
2. **Non-snooped mode exists.**  A plain PFN is the vendor form for NC/WC
   CPU mappings.  The vendor Linux KMD backs its whole PCIe heap with
   `__vmalloc(PAGE_KERNEL_NOCACHE)` (`via_chrome9_dma.c:363`), and Windows
   uses NC/WC (**S**).  A `dma_alloc_wc` DMA-GEM target with plain PTEs has
   passed correctness (**D**, [linux-dri-prereqs.md](linux-dri-prereqs.md)).
   It has never been timed.
3. **Translation.**  SF addresses go through the GTI.  The table itself
   lives in SL (SR6A..SR6C hold "Base Address of RTSF in SL"), so a GTI cache
   miss adds an SL read.  SR6F bit 7 flushes the GTI cache (**M**).  The GTI
   cache size is unknown.
4. **Arbitration knobs** (**M**, Part I pp. 45--49, effect unknown): SR66,
   SR67, SR69, SR79, SR7A, SR7C and SR7E are request kill numbers
   ("SL/SF", 3D, GTI, CBU, CR; default `C8h`).  SR71..SR75 are "typical
   request" queue, track FIFO, max-ACK and "Max. Burst Length to GTI" (SR73,
   default 1).  D1F0 PCIe Device Control bit 11 "Enable No Snoop" has manual
   default 0.  Whether the internal SF client obeys it is unknown.  No
   `lspci -vv` of the target GPU is recorded.  The D0F3 north-bridge snoop
   controls are in the VX900 System Programming Manual, which is not in the
   tree.

Nothing here attributes the 2.7x to snoop, translation or arbitration.
Ladder step 7 separates snoop from translation.  Local placement avoids all
three costs and also relieves the 64-MiB SF aperture.

Vendor defaults (all **S**):

* **Window color/depth:** DRI1 static buffers inside the frame buffer.  The
  KMD init struct carries `front_offset/back_offset/depth_offset`
  (`via_chrome9_drm.h:198-200`).  Xorg EXA destinations use
  `A51=51000000` (SL).
* **FBO RT:** location follows the surface memory type (type 4 = SF).
  **Z:** see the family-8 note above.
* **Textures:** made resident in VRAM first (prior audit, `0x2012c2`).
  Upload is a 2D host-data BLT (`cc002041`) through the command stream, not a
  CPU copy.  CPU-direct writes into the SF heap first wait for chip idle.
  Level location bits follow the memory type.
* **Vertex/client arrays:** VRAM, via the preference fields set by
  `__glS3InvInitializeLocalMemoryFlag` `0x1d74ca/0x1d74ff`.  SF is the
  fallback, and only that fallback uses the `FLUSH_CACHE` clflush ioctl.
* **Ring and heaps:** the ring sits at SF VA 0 in the PCIe SG heap.  The KMD
  comment at `via_chrome9_mm.c:702` says generic allocations try AGP/PCIe
  first and VRAM second.  That applies to KMD `ALLOCMEM` users; the UMD pool
  order above governs textures and VBs.

## 4. CPU access to SL through BAR2

* **Mapping type.**  The vendor Xorg `VIAScreenInit` maps the frame buffer
  with `pci_device_map_range(..., flags=3)`, which is
  `WRITABLE|WRITE_COMBINE` (`via_drv.so` `0x578b2..0x578f3`, **S**).  The
  vendor KMD maps the GART table inside the frame buffer with `ioremap_wc`
  (`via_chrome9_dma.c:248`, **S**).  For PCI `0x7122` the vendor KMD clears
  `DRIVER_USE_MTRR` (`via_chrome9_drm.c:1032-1036`), so the DRM core adds no
  WC MTRR for the Xorg-created `_DRM_FRAME_BUFFER` map.  That map is
  `drmAddMap` type 0, flags 0, at `0x488b8`.  The DRI client's `drmMap` of it
  is then UC unless something else provides WC (**I**).  The current kernel
  maps BAR2 with `pcim_iomap(pdev, 2, 0)`, which is UC.  Bulk kernel copies
  should use a WC mapping (`ioremap_wc` plus `arch_io_reserve_memtype_wc`).
  Userspace mmap of SL BOs should use `pgprot_writecombine`.
* **CPU writes, then GPU reads.**  A BAR2 mapping has no CPU cache lines, so
  no clflush is needed (**I**).  WC buffers must drain before the command
  that consumes the data is published: `wmb()`, then the existing ring
  barrier.  BAR2 writes reach DRAM through the north bridge's frame-buffer
  path, while the doorbell is a BAR1 MMIO write, and nothing documents their
  ordering.  Until silicon proves it unnecessary, read back one dword from
  BAR2 after the last write and before the kick (**I**).
* **Write after GPU read.**  Before the CPU overwrites SL data the GPU may
  still read (VB, texture), wait for that job's completion.  The vendor UMD
  does a full `FlushDMABufferAndWait` on every `BufferData`/`SubData`/`Map`
  (**S**).  Per-BO fences replace this.
* **GPU writes, then CPU reads.**  After FE33, cookie or idle retirement (the
  same completion as SF), CPU reads through UC/WC BAR2 fetch DRAM directly.
  Nothing is stale on the CPU side (**I**).  These reads are slow.  For bulk
  readback, a 2D SL→SF blit into a cached, snooped BO followed by cached CPU
  reads is likely faster (**I**; test before relying on it).
* **GPU caches.**  Texture, read-Z and read-color caches do not depend on
  location.  Keep the existing texture synchronisation (`A18` bit 23 force
  miss, `A11` bit 11 clear read-Z cache as in
  [fixed-function-texture-sync.md](fixed-function-texture-sync.md)).

## 5. Minimal silicon verification ladder

One variable per step.  Every write step uses 4-KiB poison guards on both
sides of the SL surface.  Every step compares SHA-256 hashes of the SL GART
table (last 64 KiB) and of the scanout carve-out before and after.  Runs stay
under about 7 s, and the NanoKVM power cycle is the recovery path.

| # | change (only this) | checks | hang risk |
|---:|---|---|---|
| 0 | none; read-only capture: `lspci -vv -s <gfx>` (DevCtl NoSnoop/RlxdOrd), `lspci -xxx -s 00:00.3`, SR66..SR7E | record values for §3 | none |
| 1 | repeat the `vram_target_test` fill with full BAR2 readback of the target and guards | all pixels equal the fill color; guards and GART hash intact | very low (already run) |
| 2 | SF color, texture in SL: `A18` location bits `00`, level entries `[31:30]=00`; texels written through BAR2 with `wmb` and readback | quadrant samples; source unchanged | low (GPU only reads SL) |
| 3 | SL color + SF texture | same as 2 | low |
| 4 | D16 depth in SL, color SF: `A10` = SL>>8, `A11[1:0]=00` | depth-test result pixels; Z words read through BAR2; guards | low-medium (GPU writes SL) |
| 5 | as 4 with the knob also moving color to SL; rerun `chrome9_fill_bench … depth=1` | correctness, then Mpix/s | low-medium |
| 6 | D24S8 in SL (`A12` format `110`) with stencil on | stencil and depth values | low-medium |
| 7 | SF discriminator: a DMA-GEM `dma_alloc_wc` color target with plain PTEs (no bit 25) against a bit-25 shmem target, same fill bench | ≈383 means snoop cost; ≈141 means translation/GTI cost | low (plain-PTE WC correctness already passed) |
| 8 | 2D COPY_RECT SL→SF (`08=40000000\|…`), then SF→SL (`08=00001000\|…`) | exact bytes and guards | low (SL 2D fill proven) |
| 9 | TCL vertex buffer in SL: `A00[16:15]=00`, palette = SL>>10 | geometry correct; guards | medium (bad fetches can make huge primitives; keep the kernel bounds and `end+64` margin) |
| 10 | optional, only if 7 is inconclusive: GART PTE pointing at an SL physical page (SR6D.. base + offset), color location SF | translation-only cost | medium (GTI behaviour for this PFN range unknown) |

Do not test ring-in-SL or `10`/`11` codes.

## Implementation consequences

* Kernel: add an SL allocator (`drm_mm`) over a validated free interval of
  BAR2, below the GART table and outside scanout and firmware memory.  Add a
  per-BO domain.  Each builder above takes its location bits from the BO
  domain rather than constants.  Validate SL extents against the allocator
  range.  Provide WC mmap and transfer paths.
* Place depth first, since the vendor VX900 never tags Z as SF.  Next
  color/FBO targets, then textures (uploaded by CPU WC write or 2D SF→SL
  blit).  Vertex buffers come last and must stay single-location per draw.
