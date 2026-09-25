<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 (VX900) RTSF PTE bit 25 and CPU-cache snooping

Status: research note, 2026-09-25.  Static analysis plus existing live
results.  The target was not accessed for this note.  It closes the
static part of the open question in
[chrome9-throughput-re.md](chrome9-throughput-re.md) section 7 ("Whether
RTSF PTE bit 25 actually snoops CPU caches on VX900").  It ends with a
guarded silicon ladder that the main session can run.

Evidence labels:

* **M** manual: Chrome9 HD OGPM Part I
  (`docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`, Rev 1.0) and the VX900
  Series System Programming Manual Rev 1.41 (bitsavers, SHA-256
  `1b33ccbe…8a009f49`), both extracted with `pdftotext -layout`.  Page
  numbers are the printed page numbers;
* **S** static: disassembly or vendor source.  Windows addresses are
  analysis VMAs (ImageBase `0x10000`) in `VTGKModeDX32A.sys` (SHA-256
  `618129602e2f…0a56`), `VTGUModeDX32A.dll` (`8ac6959f…9b45`) and
  `VTGOGL32A.dll` (`a54e38ca…4c2c`).  The VIA 5.75 KMD source is
  `binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/`;
* **D** dynamic: measured on the VX900 target in earlier sessions (cited,
  not repeated);
* **I** inference from the above; **U** unknown.

Current-driver citations use `linux-7.2/drivers/gpu/drm/chrome9/` at HEAD
`c318378`.  The working tree has uncommitted edits, so the function names
are the stable references.

## 1. Answer in brief

1. **Bit 25 is the "SF snoop" attribute (S).**  The Windows KMD mapper ORs
   `02000000` into a PTE for three reasons, and all three are cache-policy
   switches:
   * a mapping requested as cache-coherent;
   * the registry override `ForceSFSNOOPEnable`;
   * a chip or strap force.

   No vendor path treats the bit as a valid bit.
2. **The vendor uses the bit only where the CPU mapping is cached (S).**
   * WDDM *Map Aperture*: the bit is set from the paging operation's
     `CacheCoherent` flag.
   * The PCIe nonlocal heap: set when the heap is mapped `MmCached`.
   * The paging-unmap dummy page: always set.
   * The VIA Linux 5.75 KMD never sets it.  It backs its whole PCIe heap
     noncached and uses CLFLUSH explicitly.
3. **On this target's revision the Windows KMD does no CPU cache maintenance
   on the cached-aperture path (S + M).**  The target reads `SR3B=03`
   (**D**).  For `SR3B<=2` the KMD adds WBINVD after every aperture map or
   unmap.  Revision 3 has no WBINVD.  The KMD contains no CLFLUSH, and
   `KeFlushIoBuffers` is not imported.  The Windows UMDs still execute
   CLFLUSH+MFENCE over one class of CPU-written buffer before use.  That
   buffer's memory type is not resolved.
4. **Live data already point to real snooping (D, I).**
   * SWIOTLB bounce-buffer failures: 18/30 runs failed with plain PTEs and
     0/30 with bit 25.  The bounce copies do no cache maintenance
     ([chrome9-per-draw-state.md](chrome9-per-draw-state.md) 4.3).
   * A 3D target in SF runs at 141 Mpixel/s with bit 25 and 343 Mpixel/s
     without it.  The bit changes the memory path, not just a label.
   * Plain PTEs are proven non-coherent: a command page failed with
     `dma_wmb()` alone and passed with CLFLUSH.
   * No test so far has isolated "bit 25, no CPU cache maintenance, per
     cache line".  The per-job CLFLUSH cannot yet be dropped on evidence.
     Section 6 gives the ladder.
5. **Chipset (M):** the SPM documents no graphics-side cache-snoop enable.
   * GFX engine requests go through GMINT straight to the DRAM controller.
   * The documented chipset controls cover the CPU posted-write buffer and
     the C5 redirection.
   * Section 4 lists the registers to capture before testing.
6. **Trade-off (D):** snooped GPU access costs about 2.4x fill rate.
   * Even if the ladder passes, render targets, depth and textures should
     stay non-snooped with explicit ownership flushes.
   * Snooping pays off for CPU-written streams with little GPU traffic:
     the ring, the index arena, vertex and constant uploads, and readback
     staging.

## 2. RTSF page-table entry format

| bits | meaning | evidence |
|---|---|---|
| `31` | invalid entry is exactly `80000000` | S: `via_chrome9_dma.c:283-284`; Windows unmap `0x2dce7..0x2dd22` |
| `29:0` | 4 KiB page frame (`PFN & 3fffffff`) | S: `via_chrome9_dma.c:179,445`; Windows `0x2de8e..0x2de93` |
| `25` | snoop (cache-coherent) attribute; overlaps nominal PFN bit 25 (PA bit 37), so all RTSF backing must stay below 4 GiB | S: Windows `0x2def5..0x2df20`, registry name below |
| others | no other attribute recovered; no writable or valid bit | S (absence in both vendor mappers) |

Neither Chrome9 HD manual nor the HC3, HCM or UniChrome Pro II Part I
manuals define the PTE format.  They define only the table registers (M,
Part I pp. 46-47):

* `SR6A`, `SR6B`, `SR6C[0]`: RTSF base in SL;
* `SR6C[7]`: "GART Table Write Protect Enable";
* `SR6D`..`SR6F[6:0]`: SL base in system memory;
* `SR6F[7]`: "GTI Cache Flush, set by SW and reset by GTI".

A search for "snoop" in the Chrome9 HD Part I/II texts finds only VGA
palette snooping and the PCIe capability bit `D1F0 Rx78[11]` "Enable No
Snoop" (default 0, p. 14).  The integrated GFX is not on a PCIe link.
Whether that bit affects the internal SF client is **U**.

## 3. Vendor usage

### 3.1 Windows KMD mapper (`0x2ddf0`)

For each page the mapper computes `pte = mdl_pfn & 3fffffff`.  It then
applies:

```text
0x2de9c..0x2ded1  if remap enabled && !cached && pfn >= threshold: PFN remap
0x2ded4..0x2defe  if (adapter+9c0 & 00000080) && (adapter+9c0 & 10000000):
                      pte |= 02000000                      # chip/strap force
0x2df01..0x2df20  if cached_arg || (adapter+9c8 & 00000004):
                      pte |= 02000000                      # request / registry
0x2df2c           table[index] = pte;  residency[index] = 1
0x2dfe6..0x2e00c  if (adapter+9c8 & 00004000): WBINVD; two OUT 81h delays
0x2e013..         SR6F flush write(s)
```

The sources of each flag are:

| flag | set at | condition | on target |
|---|---|---|---|
| `+9c8 & 4` | `0x121916..0x121943` | registry DWORD `ForceSFSNOOPEnable` (UTF-16 string at `0x12d350`) nonzero | default not set (I) |
| `+9c0 & 10000000` | `0x12240b..0x12241c` | `0x7122` branch (VX900) of the chip switch at `0x122056..0x1220b6`, unconditional | set |
| `+9c0 & 80` | `0x122a00..0x122a27` | common tail: `SR3B == ff` | not set, target `SR3B=03` (D: `artifacts/2026-09-24/tcl/ucp-u6-timeout-dmesg.txt:704`) |
| `+9c8 & 4000` (WBINVD) | `0x1223b7..0x1223c9` | `0x7122` and `SR3B <= 2` (`+0x79a` is `SR3B`, read at `0x112833..0x11285d`) | not set |
| `+9c8 & 8000` (MFENCE on unmap) | `0x12239f..0x1223b1` | `0x7122` and `SR3B <= 2` | not set |

The registry name `ForceSFSNOOPEnable` controls the same OR as the
cached-request argument.  This is the direct static evidence that the
vendor names bit 25 "SF snoop".

Callers of the mapper and how they choose `cached_arg`:

* **WDDM `BuildPagingBuffer` *Map Aperture*** (`0xd613f..0xd617d`, log
  string at `0xecd4c`): `cached_arg = args[+0x30] & 1`.  The mapper
  receives `args[+0x2c]` as the MDL, `args[+0x28]` as the page count and
  `&args[+0x24]` as the first page.  With the x86 Win7 WDK layout of
  `DXGKARG_BUILDPAGINGBUFFER`, where the operation union is 8-byte aligned
  at `+0x18`, these are `MapApertureSegment.pMdl`, `NumberOfPages`,
  `OffsetInPages` and `Flags` (**I** for the field names).  Bit 0 of
  `DXGK_MAPAPERTUREFLAGS` is `CacheCoherent`.  Under the WDDM contract the
  OS uses a coherent aperture mapping for cached CPU allocations and does
  not flush CPU caches for it (**I**, WDDM documentation, not verified in
  the binary).
* **PCIe nonlocal heap** (`0x2e3b0`): `cached_arg = (request+0x20 == 1)`,
  which is `MmCached` (`0x2e568..0x2e5a4`).  The same field is the cache
  type passed to `MmMapLockedPagesSpecifyCache` (S; details in
  [sf-render-target-coherency.md](sf-render-target-coherency.md)).
* **Paging *Unmap Aperture*** (`0xd61ca` → `0x2db10`): evicted ranges point
  to a dummy page as `dummyPFN | 02000000`, unconditionally
  (`0x2db6e..0x2dbc6`).

On revisions `SR3B <= 2` the WBINVD runs after map and unmap without regard
to the snoop bit (`0x2dfe6`, `0x2dbe6..0x2dbf7`, `0x2dd96..0x2ddae`).  The
binary does not show why.  Possible readings are:

* snooping on early silicon was not trusted;
* stale cache aliases of pages entering the aperture had to be evicted.

This is **U**.  For revision 3, the target's revision, the KMD relies on
the attribute alone.

The KMD has no `clflush` instruction.  Its seven `wbinvd` hits are:

* the mapper and unmapper sites above;
* `0xc6291`, gated by bit 0 of a per-command record field `+0x3c`, in a
  routine at `0xc5520` that is reached through a function-pointer table at
  `.data 0x10849c/0x108504` (purpose **U**);
* two disassembly artefacts in data (`0x2cae7`, `0x90ca8`).

`KeFlushIoBuffers` is not imported (S, `objdump -p`).

### 3.2 Windows UMDs

`VTGUModeDX32A.dll` has 7 `clflush` sites and `VTGOGL32A.dll` has 4.  They
come from shared code (for example D3D `0x1015d770`, GL `0x101d2da0`).

* The code takes `buf = [ctx+0x364]`.
* If `[[buf+0x26c]+0x40] == 1` and `[buf+0x274] != 0`, it CLFLUSHes
  `[buf+0xc] .. +[buf+0x274]` in 64-byte steps, executes MFENCE, and
  clears `+0x274`.
* D3D `0x100f9479..0x100f94a6` flushes the same range with no `+0x40`
  test.

`+0x274` behaves as a dirty-byte cursor over a CPU-written buffer.  Two
things are **U**:

* which buffer class this is (command stream or vertex stream);
* whether descriptor field `+0x40 == 1` means a cached or nonlocal memory
  type.

What is certain: the vendor UMD does not rely on snooping for at least this
buffer class.  CLFLUSH is harmless on snooped memory, so this does not
prove that bit 25 fails to snoop.

### 3.3 VIA Linux 5.75 KMD/UMD

* The PCIe heap is `__vmalloc(..., PAGE_KERNEL_NOCACHE)`
  (`via_chrome9_dma.c:363`).  PTEs are plain `page_to_pfn & 3fffffff`
  (`:445`).  Nothing sets bit 25 (S: the only `0x02000000`
  constants in the source are register and command values, never PTEs).
* Command buffers are CLFLUSHed page by page before H6S1/H6S2/H5S2VP1
  execution (`via_chrome9_dma.c:1812-1824`).  `via_chrome9_clflush_pages()`
  does `mb(); clflush...; mb()` and falls back to WBINVD (`:44-75`).
* `DRM_VIA_CHROME9_FLUSH_CACHE` (`via_chrome9_dma.c:1859-1887`) is called by
  the GL UMD before PCIe vertex-buffer use (`__glDpMemPcieFlushCache
  0x1e8dd0`, see [sf-render-target-coherency.md](sf-render-target-coherency.md)).
  A flush over an NC kernel mapping implies that the user mapping of the
  heap is cacheable.  The legacy DRM `_DRM_SCATTER_GATHER` mmap keeps the
  VMA's default page protection (**I**; the 2.6.31 DRM core source is not
  in the tree).
* No D0Fx configuration access appears in the KMD (S: no
  `pci_read_config`/`pci_write_config`/`pci_get_device` calls).

## 4. Chipset (VX900 SPM Rev 1.41)

| register | definition | relevance |
|---|---|---|
| overview, p. 101-102; GMINT p. 163 | GFX and GFX-display requests go through GMINT to DRAMC.  PCIe/SB upstream cycles "go to host controller to perform cache snooping" | the default GFX path does not pass the CPU bus.  A snooped SF request must be routed differently.  The route is undocumented (**U**).  The fill-rate cost with bit 25 fits a host-bus detour (**I**) |
| `D0F2 Rx56[1]`, p. 47 | "Flushing Write Requests Upon Snoop Requests from GFXCTL": GFXCTL snoops the CPU bus controller's post write buffers.  A hit flushes them first if the bit is 1; if it is 0 "the snoop result is ignored" | covers CPU writes already in the chipset write FIFO (UC/WC/evicted lines), not CPU caches.  Record the value |
| `D0F2 Rx98[6:5]`, p. 78 | CPU cycles to GFXCTL (MMIO, doorbell) can flush the CPU-to-memory write FIFO (`10` read or write, `11` write only, `0x` off) | ordering of CPU memory writes before the doorbell.  Keep the MMIO read-back until the value is known |
| `D0F2 Rx98[0]`, `Rx9F[6]`, p. 78-80; `D0F4 Rx8F[2]`, p. 227 | in C5, upstream cycles may go to DRAM "without snooping the CPU" (CPU caches are flushed on C5 entry) | snoop behaviour may depend on C-state.  Capture and test with and without deep idle |
| `D0F3 RxA3..A7`, p. 164-166 | GMINT merging, priority and SID arbitration | no snoop control |

No register in the SPM enables or disables snooping for GFX engine
requests.  The only snoop control is the per-PTE bit (S) (**M** for the
absence in this manual).

## 5. Live evidence already in the tree

| # | observation | label | reading |
|---|---|---|---|
| L1 | Command page (`dma_alloc_coherent`, plain PTE) failed with `dma_wmb()` only and passed with CLFLUSH ([hardware-lab.md](hardware-lab.md), command-cache section) | D | plain PTEs are not coherent with CPU WB caches |
| L2 | H6 SF target at DMA `01088000` (coherent allocation, plain PTE) showed 272/1024 px without cache maintenance and 1024/1024 with two-sided CLFLUSH | D | same as L1 for GPU writes |
| L3 | SWIOTLB repro: plain draw PTEs 18/30 failures; `gart_nosnoop=0` (bit 25) 0/30 with 14080 bounce slots in use ([chrome9-per-draw-state.md](chrome9-per-draw-state.md)).  SWIOTLB copies between shmem and slot with cached `memcpy` and no cache maintenance on x86 | D | strongest existing sign that bit 25 is coherent in both directions: CPU-dirty slot lines read by the GPU, and GPU writes seen by later cached reads.  It is statistical and not per-line |
| L4 | Earlier "plain PTE fails, PTE25 passes" high-PFN shmem tests.  PTEs came from `sg_dma_address()` (`chrome9_drv.c:1448-1464`); CLFLUSH covered the shmem vmap, not a bounce slot (`:556`, `:1516`).  DMA `ddcae000..de4fe000` lies in the top 64 MiB below the then low-DRAM top `e0000000`.  Current boots place the 64-MiB SWIOTLB pool directly below the low-RAM top (`cbfb0000-cffb0000`, `ucp-u6-timeout-dmesg.txt`) | D + I | probably the same SWIOTLB mechanism as L3.  Bounce status was not logged, so this is **I** |
| L5 | Fill 800x600: SF bit 25 55 ms (141 Mpix/s); SF plain 22.4 ms; SL 22.4 ms ([chrome9-performance-20260924.md](chrome9-performance-20260924.md)).  Glamor composite estimate 3.4 ms snooped versus 1.4 ms plain ([chrome9-present-path.md](chrome9-present-path.md)) | D | bit 25 changes the transaction type.  Snooped writes cost about 2.4x |
| L6 | Production `gart_nosnoop=1` (plain draw PTEs on DMA32 shmem with explicit ownership flushes) passes glmark2/piglit validation | D | plain PTE plus CLFLUSH is a correct protocol.  Bit 25 is not required for reachability |
| L7 | FE33 fence: direct PCI-master system write (not RTSF) into a WB `dma_alloc_coherent` slot that the CPU poisoned with cached stores (`WRITE_ONCE(slot[0], ~id)`, `chrome9_engine.c:1454`) and polls with `READ_ONCE` without cache maintenance (`:6667`) | D | the FE3x system-write path is coherent.  It is a different client and does not decide the RTSF question |

## 6. Guarded silicon ladder

Run on kernel-owned scratch memory from a debug-only selftest, in the style
of the existing `chrome9_drv.c` selftests, not from userspace.  The test
controls cache state per line.  Follow the usual target rules:

* GPU lock with `flock -o`;
* hash-gated module hand-off;
* runs of about 7 s;
* `xset s off -dpms` before any window read-back;
* NanoKVM power-off/on if the target does not return.

### 6.0 Read-only capture (no GPU work)

```sh
lspci -s 00:01.0 -vv | grep -i -A2 DevCtl      # D1F0 Rx78[11] NoSnoop
lspci -s 00:00.2 -xxx                          # D0F2 Rx56, Rx98, Rx9F
lspci -s 00:00.3 -xxx                          # D0F3 RxA0..A7, F8..FB
lspci -s 00:00.4 -xxx                          # D0F4 Rx8F
dmesg | grep -i "software IO TLB"; cat /sys/devices/system/cpu/cpuidle/current_driver
```

The driver already logs `SR3B` (`03`).  Also log `SR6C` and `SR6F` before
load.

### 6.1 Common setup

* **Pages.** Test pages are `alloc_pages(GFP_DMA32)`.  Abort unless
  `sg_dma_address == page_to_phys` for every page.  This rules out SWIOTLB
  and IOMMU.  Each variant (snooped, plain) gets its own pages with a
  guard page on each side.  Never alias one physical page with both PTE
  flavours in one run.
* **Line pattern.** Per 64-byte line `i`, word `w` holds
  `f(gen, i, w) = 0x5a000000 ^ gen << 16 ^ i << 4 ^ w`.  Classify each line
  as `fresh` (expected gen), `stale` (previous gen), or `corrupt` (neither).
  Any `corrupt` line aborts the ladder and keeps the job quarantined.
* **Independent read-back.** The CPU must not read the result through its
  own cache.  Copy the SF test page with the GPU into an SL (local frame
  buffer) scratch object and read it through BAR2 (UC).  Use only a copy
  path already validated in the running build: `COPY_RECT` SF→SL, or a
  one-texture draw into a local target.  If none is validated, copy into a
  plain-PTE SF page and CLFLUSH that page before reading (proven L2
  transition).
* **Cache locality.** Keep the working set small (4 KiB, well inside L1).
  Publish the job right after the CPU step, with preemption disabled
  between the last store and the doorbell.  Pin the CPU step to a known
  core.
* **Safety.** Hash the GART table and guards before and after.  Use the
  existing bounded fence wait.  One job per step.

### 6.2 Steps

| # | name | procedure | pass | notes / risk |
|---:|---|---|---|---|
| T1-P | detector control, read | gen0 → CLFLUSH (DRAM=gen0); gen1 with plain stores, no flush; GPU reads through a **plain** PTE | some or all lines `stale` | proves the test can see stale data.  If all are fresh, the lines left the cache before the GPU read.  Shrink the buffer and retry before trusting T1-S |
| T1-S | CPU write → GPU read | as T1-P through a **bit 25** PTE | 100% `fresh` | low risk (GPU reads only) |
| T1-X | cross-core | T1-S with the gen1 stores done on the other core (`smp_call_function_single`) | 100% `fresh` | FSB snoop must cover both Nano cores |
| T2-P | detector control, write | gen0 → CLFLUSH → read every line (clean cached gen0); GPU writes gen1 through a plain PTE (2D solid fill or copy from an SL gen1 source); after the fence, cached CPU reads | some `stale`; after CLFLUSH, 100% `fresh` | proves the GPU wrote and the detector sees stale clean lines |
| T2-S | GPU write → CPU read | as T2-P through bit 25 | 100% `fresh` **before** any CLFLUSH | low risk.  Scratch page only |
| T3-S | GPU write over CPU-dirty lines | gen0 → CLFLUSH; gen2 plain stores (dirty, no flush); GPU writes gen1 through bit 25; fence; cached read, then CLFLUSH and read again | both reads 100% gen1 | gen2 after CLFLUSH means a modified line survived and was written back over GPU data.  That is unsafe even if T1/T2 pass.  This deliberately uses the "post-only flush" hazard from [sf-render-target-coherency.md](sf-render-target-coherency.md), so scratch only |
| T4-S | partial-line merge | CPU dirties bytes 0..31 of each line (gen2); the GPU writes a 4-byte column at bytes 32..35 through bit 25 | CPU bytes stay gen2, GPU bytes are gen1, rest unchanged | covers narrow GPU writes (D16, column spans) |
| T5 | soak | T1-S/T2-S/T3-S with random line subsets, 10^4 iterations; both cores stream 8 MiB memcpy traffic to force evictions; repeat with deep C-states enabled and with `cpuidle.off=1` | zero `stale`/`corrupt` | C-state dependence (section 4, Rx98[0]/Rx9F[6]).  Hang risk low-medium: a snoop to a sleeping core is the untested case |
| T6 | production candidate | only after T1-T5 pass: map the ring and index arena with bit 25 behind a debug parameter.  Skip their per-append CLFLUSH (`chrome9_append_segment`, `chrome9_install_prime_ring`, `chrome9_engine_init`; index arena in `chrome9_emit_fragment_draw` in the working tree).  Run glmark2 and ioquake3 short scenes | no fence timeout, unchanged validation, lower `clflush_cache_range` share in `perf` | medium risk.  Before the run, fill the ring with the existing dummy words (`chrome9_fill_ring_dummy`) and CLFLUSH once.  A stale fetch then executes dummies up to the PAUSE register bound and ends in a fence timeout, not in stale commands.  Keep `dma_wmb()` and the MMIO read-back before the doorbell |

### 6.3 Interpreting results

* **T1-S, T2-S, T3-S, T4-S and T5 pass.**  Bit 25 gives full coherence on
  this silicon.  Objects mapped with it need ordering only (`wmb` or
  `dma_wmb`, doorbell read-back), and no CLFLUSH in either direction.
  Candidate users are CPU-written, lightly GPU-read buffers:
  * the ring and index arena;
  * vertex and constant uploads;
  * readback staging;
  * `chrome9_flush_map_guards` and `chrome9_bo_cpu_sync_locked` for such
    objects.

  Render targets, depth and textures stay non-snooped with the explicit
  ownership protocol, because of L5.
* **T1-S passes and T2-S or T3-S fails.**  Snooping covers GPU reads only.
  Keep the post-retirement invalidate for GPU-written snooped objects.
  Pre-submit flushes can go.
* **T1-P shows no stale lines.**  The detector is insufficient.  Do not
  conclude anything.
* **Any `corrupt` line or GART hash change.**  Stop.  The PTE or copy path
  is wrong, not the cache policy.

### 6.4 Lockup and corruption risks

| risk | mitigation |
|---|---|
| a snooped request to a core in a deep C-state stalls (untested) | T5 with and without `cpuidle.off=1`; record D0F2 Rx98/Rx9F and D0F4 Rx8F first |
| stale command bytes in the ring (T6) | dummy-filled ring; PAUSE bound; bounded fence wait; T6 only after T1-T5 |
| GTI flush `SR6F[7]` stays set after PTE writes ([chrome9-gart-flush-timeout.md](chrome9-gart-flush-timeout.md)) | existing bounded wait and quarantine; the test changes PTEs once per run |
| bit 25 is taken as PA bit 37 by a future wider mapping | keep the 32-bit DMA mask and the `upper_32_bits` check in `chrome9_collect_sgt_ptes` |
| SWIOTLB hides the real page | abort on `sg_dma_address != page_to_phys` |
| CLFLUSH after dirty CPU lines overwrites GPU output (T3) | intentional only on scratch pages |
| fill-rate regression if snooped mappings reach render targets | apply bit 25 per object class, never as a global default |

## 7. Open unknowns

* Per-line coherence of bit 25 on revision 3 silicon: ladder T1-T5 (**U**).
* The route snooped SF requests take through the north module (**U**).
* The Windows UMD buffer class behind `ctx+0x364` and descriptor field
  `+0x40 == 1` (**U**).
* The reason for the `SR3B <= 2` WBINVD in the Windows KMD (**U**).
* Whether `D1F0 Rx78[11]` has any effect on the internal SF client (**U**).

## 8. Silicon results (2026-09-26)

Kernel `9035916` (ladder `c721acc`, T6 `9035916`), loaded with
`selftest_snoop=1`, then `selftest_snoop_soak=20000
selftest_snoop_soak_ms=6000`, then `gart_snoop_ring=1` (plus
`fe33_fence=1 hw_tcl=1`).  Evidence **D**.  cpuidle driver: none (no deep
C-states in use); 2 CPUs; test pages dma32 direct (no SWIOTLB).  The
section 6.0 capture is in `~/tmp/snoop/capture-6.0.txt`.

Ladder (2D COPY_RECT job per step, 64 lines of 64 bytes, test CPU 0):

```
T5 soak bit25: iterations=1000/1000 in 236 ms, load-cpus=1, lines=64000 stale T1=0 T2=0 T3=0 pass
T1-P cpu-write/gpu-read plain cpu=0 attempts=1: fresh=0 stale=64 corrupt=0 detector ok
T1-S cpu-write/gpu-read bit25 cpu=0: fresh=64 stale=0 corrupt=0 pass
T1-X cross-core bit25 cpu=0 store-cpu=1: fresh=64 stale=0 corrupt=0 pass
T2-P gpu-write/cpu-read plain: cached fresh=0 stale=64 corrupt=0, flushed fresh=64 stale=0 corrupt=0 detector ok
T2-S gpu-write/cpu-read bit25: cached fresh=64 stale=0 corrupt=0, flushed fresh=64 stale=0 corrupt=0 pass
T3-S gpu-write over cpu-dirty bit25: cached fresh=64 stale=0 corrupt=0, flushed fresh=64 stale=0 corrupt=0 pass
T4-S partial-line merge bit25: cached fresh=64 stale=0 corrupt=0, flushed fresh=64 stale=0 corrupt=0 pass
T5 soak bit25: iterations=20000/20000 in 4697 ms, load-cpus=1, lines=1280000 stale T1=0 T2=0 T3=0 pass
```

* The detector controls work: plain PTEs show all 64 lines stale for a CPU
  write read by the GPU (T1-P) and for a GPU write read through the cached
  CPU mapping (T2-P).
* Bit 25 is coherent for the 2D engine client in both directions on
  revision 3, across both cores, over dirty CPU lines (no write-back over
  GPU data) and for partial-line merges; the 20000-iteration soak (1.28 M
  lines, other core streaming memcpy) saw no stale line.
  Verdict: `COHERENT (T1-S T1-X T2-S T3-S T4-S T5 pass)`.
* T6 (`gart_snoop_ring=1`: ring and index arena with bit 25, no per-append
  CLFLUSH) ran glmark2 (score 117) and ioquake3 (GL1 60.7, GL2 43.2 fps)
  without fence timeouts, equal to the default within noise.  CPU cache
  maintenance is 0.67 % of the ioquake3 opengl2 profile, so bit 25 for more
  object classes would not pay; T6 stays default off.  Not covered: the 3D
  texture and render clients (only the 2D engine and, in T6, the command
  reader were exercised).
