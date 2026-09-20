# SF render-target domain and CPU coherency

This note isolates three mechanisms which use the word "flush" but operate on
different state.  The destination conclusion is measured: a linear H6 render
target in translated **SF** memory works with `A51[21:20]=01`.  A low-address
coherent allocation also proves the two-sided CPU cache transition.  Cached
GEM shmem works at high PFNs when its internal RTSF PTE carries the Windows
cached/snooped attribute, bit 25.  This closes the apparent high-address
failure as a cache-policy mismatch rather than a physical-address ceiling.

## Destination-domain encoding

The VX900 Part II manual, internal page 106, defines the RT0 triplet:

```text
A50[23:0]  base address in 256-byte units
A51[23]    0 linear, 1 tiled
A51[22]    tiled height, 0 eight pixels, 1 sixteen pixels
A51[21:20] location: 00 SL, 01 SF, 10 reserved/system memory,
                     11 reserved/LL
A52        format and pitch state
```

RT1, RT2, and RT3 repeat the same layout at `58/59/5a`, `60/61/62`, and
`68/69/6a`.  The old Linux verifier independently extracts
`(A51 & 00300000) >> 20` and rejects value 2 when direct system memory is not
enabled (`via_chrome9_verifier.c`, `check_render_target_addr_mode`).  It accepts
value 1 as the translated aperture form.

The proprietary Linux DRI state builder is more specific.  RT0 is constructed
by `STM_RenderTarget_INV:0x195e90`, with A50/A51/A52 stored at context offsets
`140/144/148`.  On the PCIe-capable path, surface memory type 4 clears both
location bits and sets A51 bit 20 (`0x196802..0x196814`); the older path sets
bit 21 instead (`0x196066..0x196078`).  Thus the successful `51100000` word is
the vendor's translated SF encoding, not an experimental alias for raw system
memory.

## Live result

The guarded live test mapped three pages at SF PTEs 1--3 and rendered to the
middle page at GPU VA `00002000`.  It changed only the local target's
destination words to:

```text
A50 = 50000020
A51 = 51100000
```

Without CPU cache maintenance, the CR retired, `0400=0`, `040c=0`, and both
guards remained intact, but a CPU read initially observed only 272 of 1,024
expected pixels.  This is the signature of stale CPU WB lines: engine
retirement and intact bounds rule out a still-running or out-of-range draw.

With cache-line writeback/invalidation before GPU ownership and again after
retirement, the same low-DMA coherent allocation, PTEs, and exact 188-dword
draw produced all 1,024 pixels as `000000ff`, zero bad guard words, and a
passing local-VRAM 2D regression.  The passing module is:

```text
/root/tmp/chrome9/chrome9-h6-sf-target-clflush.ko
SHA-256 0093be37b4b33185d922d1d4055577af2e7f0f8604dd55fb1f7c5635e43b90bd
```

### GEM shmem/high-PFN cache-policy discriminator

The result above does not yet generalize to arbitrary GEM shmem pages.  On a
clean boot, shmem PTEs `000ddf92/93/94` exposed 144 pixels.  A second run with
`shmem->map_wc=true`, PTEs `000de4fe/ff/e500`, complete GTI-flush waiting, and
the same pre/post synchronization exposed only 96 pixels.  Both runs retired
with engine/error zero and intact guards.  These PFNs correspond to physical
addresses near 3.72--3.73 GiB, whereas the passing coherent allocation used
PFN `00001088`, near 16.5 MiB.

The PTE-only discriminator subsequently passed.  Ordinary cached GEM shmem at
DMA `ddcae000`, represented by internal PTEs
`020ddcae/020ddcaf/020ddcb0`, rendered all 1,024 expected pixels with guards
and the independent local-VRAM 2D regression intact.  A larger six-page
cached-shmem texture-plus-target job at DMA `ddcca000`, PTEs
`020ddcca..020ddccf`, also passed all 1,024 pixels, red/green/blue/white
samples, source verification, four guards, and the 2D regression.  Both use
PFNs in the same upper-RAM region as the failing plain-PFN cases.  Therefore
bit 25 is required for the tested WB shmem mapping, and no sub-4-GiB physical
ceiling below these addresses is indicated.

The internal RTSF table format is not in doubt: the old Linux KMD writes
`page_to_pfn(page) & 3fffffff` directly (`via_chrome9_dma.c:178..179` and
`444..446`), and the passing coherent test uses the same PFN encoding.  Do not
replace it with a Windows AGP PTE containing a shifted physical address or
cache flags.

The Windows internal mapper independently reads the MDL PFN array, masks each
entry with `3fffffff`, and writes it to the primary table
(`VTGKModeDX32A.sys:0x0002de8e..0x0002df2c`).  Its optional `02000000` OR is
not a universal valid bit: the allocation call chain proves that it is the
RTSF cached/snooped attribute.  The Linux and live low-PFN noncached/coherent
paths therefore work without it.  After populating the table, a device flag
can make the Windows KMD execute WBINVD (`0x0002dfe6..0x0002e00c`) before its
GTI flush sequence.

The proof is bit exact.  The PCIe/nonlocal heap allocation callback at
`0x0002e3b0` allocates an MDL below physical address `ffffffff`, then passes
the allocation request's `MEMORY_CACHING_TYPE` field at `request+20` unchanged
to `MmMapLockedPagesSpecifyCache` for both its optional user mapping
(`0x0002e4cf..0x0002e4dc`) and kernel mapping
(`0x0002e515..0x0002e522`).  It immediately passes the same MDL to the
internal RTSF mapper at `0x0002e587`/`0x0002e5a4`.  The mapper's final Boolean
argument is exactly `(request->CacheType == MmCached)`: the caller selects it
at `0x0002e568..0x0002e5a4`, and the mapper ORs `02000000` into every PTE when
that byte is nonzero (`0x0002df01..0x0002df20`).  `MmNonCached` (0) and
`MmWriteCombined` (2) take the plain-PFN path unless a separate adapter
capability override is set.  Thus the vendor-supported pairings are:

| CPU mapping requested by Windows | internal RTSF PTE |
|---|---|
| `MmNonCached` (0) | `PFN` |
| `MmCached` (1) | `PFN | 02000000` |
| `MmWriteCombined` (2) | `PFN` |

This makes a plain PTE for an ordinary WB shmem page a policy mismatch, not a
vendor-equivalent cached mapping.  It also explains why setting only
`shmem->map_wc=true` is not conclusive: pre-existing kernel/direct-map or
userspace WB aliases are not converted by that flag, and the PTE still lacks
the Windows cached attribute.

The same mapper contains an explicit software PFN-remap branch
(`0x0002de9c..0x0002ded1`).  If a saved threshold word has bit 31 set, the
call-mode byte is zero, and the source PFN is at or above the remaining
31-bit threshold, it writes:

```text
pte_pfn = source_pfn - threshold_pfn + remap_base_pfn
```

Adapter initialization obtains both values from a checksummed shadow table:
`remap_base_pfn = u16[table+26] << 4`, while `threshold_flags =
u32[table+28]` (`0x000125e3..0x00012614`).  The earlier description of this as
"VBIOS file offset `10000`" was wrong.  `10000` is an offset into a temporary
128-KiB physical mapping, not into the PCI option-ROM image.

For `7122`, PCI discovery sets the KMD's 64-bit-resource flag
(`0x00114d54..0x00114d9e`).  Both possible shadow paths then map the last
128 KiB ending at the detected framebuffer length and set `table = mapping +
10000`:

```text
mapping_pa = resource_base + framebuffer_length - 0x20000
table_pa   = mapping_pa + 0x10000
           = resource_base + framebuffer_length - 0x10000
```

In this binary the ten-entry access-range template at `0x0012f000` has zero
bases for the framebuffer entries, and its range-population hook at
`0x0005d700` is a no-op.  `state+6f8`, populated from the detected physical
memory length at `0x00115179..0x00115197`, supplies the length when the modern
path is selected.  Thus the target's measured 128-MiB framebuffer size makes
the exact Windows-KMD candidate physical address `07ff0000`, not VGA shadow
aperture `c0000 + 10000`.  The latter contains ordinary real-mode code as the
live dump correctly showed.

There is no magic signature check.  The KMD accepts the candidate only when:

```text
length = table[2]
length > 0
sum(table[0:length]) != 0
sum(table[0:length]) & 0xff == 0
```

It later requires `length >= 2c` before reading the remap fields.  The
non-zero-sum condition matters: an all-zero page is rejected even though its
8-bit checksum is zero.  This is the strongest static evidence for
address-dependent PTE transformation.  Exact target bytes are still required
before applying it to PFN `ddf92`.

The package's `VTROM.bin` is only `9000` bytes and is not this private shadow
table.  With the live 128-MiB size, the narrow read-only target capture is:

```sh
dd if=/dev/mem of=/root/tmp/chrome9/vbios-rtsf-table-64k.bin \
   iflag=skip_bytes,count_bytes skip=134152192 count=65536 status=none
sha256sum /root/tmp/chrome9/vbios-rtsf-table-64k.bin
xxd -g1 -l 0x40 /root/tmp/chrome9/vbios-rtsf-table-64k.bin
```

This reads physical `07ff0000..07ffffff`; it does not touch PCI ROM-enable or
GPU register state.  First confirm that `/proc/iomem` still reports the live
framebuffer topology and that `SR68=c0` / PCI `B2=60` still decode to 128 MiB.
If strict `/dev/mem` rejects the read, stop rather than enabling the PCI ROM
sysfs file: that ROM is a different object anyway.  Decode and checksum the
saved bytes with:

```sh
python3 tools/chrome9_vbios_rtsf.py \
  /root/tmp/chrome9/vbios-rtsf-table-64k.bin \
  --pfn 0xddf92 --pfn 0xde4fe --pfn 0x1088
```

The target performed exactly that narrow absolute-PA read.  Linux rejected it
with `EPERM` because `07ff0000` is System RAM; no alternate address was read.
This is a measured safety boundary, not evidence that the table is absent.
Windows consumes the boot copy during adapter start, before general allocation
can reuse it.  A post-boot Linux kernel must not bypass `CONFIG_STRICT_DEVMEM`
or map an owned RAM page merely to recover this transient metadata.

There is a safer authoritative source for the underlying firmware decision.
The [VX900 Series System Programming Manual, revision
1.41](https://bitsavers.computerhistory.org/components/viaTechnologies/VX900_System_Programming_Manual_Rev_1.41_20111004.pdf)
defines the memory controller as PCI device `D0F3` (`1106:3410`) and exposes
all relevant values through read-only PCI configuration access:

| D0F3 bytes | field | exact decode |
|---:|---|---|
| `84..85` | Low Top Address | little-endian word bits `15:4` are address `31:20`; bit 0 selects whether the framebuffer size is subtracted for DRAM decode |
| `A0..A1` | CPU direct framebuffer control | little-endian word bits `14:12` select `0,8,16,32,64,128,256,512` MiB; `101b` is 128 MiB |
| `F8..FB` | DRAM memory remapping | dword bit 0 enables remap; bits `11:2` are beginning address `35:26`; bits `23:14` are ending address `35:26`; both use 64-MiB units |

The remap range is half open: `begin <= address[35:26] < end`.  The manual says
cycles in that system-address range are converted to the DRAM displaced by the
hardware hole immediately below 4 GiB.  A safe target capture is therefore:

```sh
lspci -s 0000:00:00.3 -nn -xxx
```

Use the BDF that reports `1106:3410` if firmware enumerated it elsewhere.  This
reads PCI configuration space only; it does not map System RAM, touch graphics
MMIO, or submit GPU work.  The register values can determine the active
hardware remap and its boundaries even if the KMD's transient table is gone.
The target's read-only dump returned:

```text
D0F3 84..85 = 01 e8       u16 e801
D0F3 a0..a1 = 02 df       u16 df02
D0F3 f8..fb = 01 82 21 00 u32 00218201
```

The exact decode is:

```text
nominal_low_top = (e801 & fff0) << 16 = e8000000
framebuffer selector = (df02 >> 12) & 7 = 5 = 128 MiB
effective DRAM top below 4G = e8000000 - 08000000 = e0000000

hardware remap enabled = 00218201 & 1 = 1
remap begin = ((00218201 >> 2)  & 3ff) << 26 = 200000000
remap end   = ((00218201 >> 14) & 3ff) << 26 = 218000000
```

Thus the chipset moves the 384-MiB DRAM range displaced by the hardware hole
at `e8000000..ffffffff` to the half-open system-address range
`200000000..218000000`.  The 128-MiB UMA framebuffer is immediately below the
hole at `e0000000..e7ffffff`.

These values reconstruct the Windows scratch-table fields without accessing
the discarded copy:

```text
table+26 raw       = e800
remap_base_pfn     = e800 << 4 = e8000
table+28 raw       = 80000000 | (200000000 >> 12) = 80200000
threshold_pfn      = 200000
```

This is an exact field conversion, not a name-only guess: the table's base
word is the low-top register with its control nibble removed, and its enabled
threshold is the hardware remap-begin address in PFNs.  Applying the KMD rule
would map source PFN `200000` to `e8000`, precisely the start of the displaced
below-4G range.

The live render-target PFNs do **not** cross it:

| source PFN | byte address | below effective DRAM top `e0000000` | KMD remap |
|---:|---:|---:|---:|
| `00001088` | `01088000` | yes | identity |
| `000ddf92` | `ddf92000` | yes | identity |
| `000de4fe` | `de4fe000` | yes | identity |

All three are below threshold PFN `200000`; the Windows branch is enabled in
firmware but cannot explain the partial high-shmem render writes.  Do not add a
Linux PFN transform for these pages.

The PTE-only result identifies the CPU/RTSF cache-policy mismatch as causal.
It rejects a GPU/GTI reachability ceiling at the tested 3.72--3.73-GiB
addresses, a PFN-versus-DMA-address error for these objects, and incomplete SF
write draining as explanations of the earlier partial output.  The firmware
remap hypothesis is independently rejected for the measured PFNs because its
active threshold is 8 GiB.  A driver must pair WB shmem with
`PFN | 02000000`; plain PFNs remain correct for the vendor NC/WC allocation
paths.  This is a cache-type contract, not a general valid bit.

## Required ownership transition

For a CPU-mapped WB BO which the GPU will write, use this ordering:

1. wait for prior CPU/GPU users under the BO reservation;
2. finish all CPU writes, then clean and invalidate every cache line which the
   GPU may access; this pre-clean is mandatory because a later eviction of a
   dirty old CPU line could overwrite newer GPU output;
3. perform the DMA-API device transition and a DMA write barrier;
4. publish the command only after command bytes, PTEs, and resource bytes are
   visible;
5. wait for the owned PAUSE/fence and complete engine idle;
6. perform the DMA-API CPU transition, then explicitly invalidate/flush the
   range before any CPU read on this machine;
7. only then signal CPU-visible completion or permit mmap/fault access.

The current synchronous proof conservatively uses `DMA_BIDIRECTIONAL`,
`dma_sync_sgtable_for_device()`, and `dma_sync_sgtable_for_cpu()`, bracketed by
the explicit x86 cache operation.  A future direction-specific implementation
may use `DMA_FROM_DEVICE` for a pure GPU destination, but it must preserve the
pre-clean when dirty CPU aliases or partially shared cache lines are possible.
The live platform has already shown that the generic "coherent" allocation
label and barriers alone do not make the internal SF client snoop CPU WB
cache; the explicit architecture operation cannot yet be removed.

The transitions must also follow the actual owner; they are not harmless
cache-flush synonyms.  Chrome9 eagerly creates the persistent DMA mapping in
`GEM_CREATE`, before userspace mmap writes.  With SWIOTLB, a redundant
sync-for-CPU at the start of submission copies stale bounce contents back over
newer shmem contents.  A live degenerate D16 test exposed this exactly: all
userspace `ffff` samples became create-time zero without possible raster
coverage.  Transfer the eager mapping to CPU ownership once before returning
the GEM object, then use only sync-for-device on submission and only
sync-for-CPU after retirement.  Never perform a second sync-for-CPU merely as
the prelude to a device transition.

That ownership sequence subsequently passed live: the degenerate control
changed no D16 sample, a Z=0.75 triangle wrote exactly 528 `c000` samples, and
the full 64x64 and 257x129 depth suite plus prior regressions passed.  The
verified module is `chrome9-d16-ownership-v8.ko`, SHA-256
`a66a5ee422bd2ac0cd942569c48fbeea91c82856c76db3d146f346c9f12a5f4a`.

Do not test a post-only flush after CPU prefill.  CLFLUSH writes back a dirty
line before invalidating it, so that experiment can destroy correct GPU output
with stale CPU contents.  The two-sided transition is the smallest safe
protocol.

## Legacy Linux corroboration

The old KMD implements the same CPU-side operation in
`via_chrome9_clflush_pages()`: on x86 it executes `mb()`, CLFLUSH for every
cache line of every selected page, then another `mb()`; without CLFLUSH it
falls back to WBINVD (`via_chrome9_dma.c:44..75`).  Branch command pages are
flushed before H6/H5S2VP1 execution (`via_chrome9_dma.c:1812..1824`).

The GL UMD exposes the resource form directly:
`__glDpMemPcieFlushCache:0x1e8dd0` subtracts the shared PCIe heap CPU base from
its pointer, constructs the eight-byte `{offset,size}` record, and sends DRM
command `0c`.  `via_chrome9_ioctl_flush_cache()` converts that range to SG
pages and calls the same CLFLUSH helper.  Its direct internal caller
`__glS3InvFlushVertexBufferPcieCache:0x2314f0` uses it before PCIe vertex-buffer
consumption for chip selectors 7, 8, and 14.  The KMD's inclusive-end page
calculation flushes one extra page for a page-aligned end and must not be
copied.

More importantly for allocation policy, the legacy PCIe scatter/gather heap
never starts as normal WB shmem.  `via_chrome9_drm_sg_alloc()` allocates it
with

```text
__vmalloc(pages << PAGE_SHIFT,
          GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL_NOCACHE)
```

at `via_chrome9_dma.c:323..390`, zeroes it only through that noncached vmalloc
alias, obtains each backing page with `vmalloc_to_page()`, and marks each page
reserved.  The UMD copy/memset ioctl likewise accesses payload only through
`pcie_vmalloc_nocache` (`via_chrome9_mm.c:846..881`).  The direct-map alias may
still exist architecturally, but this driver does not populate or expose it as
an ordinary WB object.  Consequently the legacy driver's plain RTSF PTE and
its noncached CPU allocation form a matched pair.

## Windows adjacent cache-policy evidence

The Windows 7 KMD avoids treating ordinary cached AGP aperture memory as
implicitly coherent.  Its OS AGP-interface reservation routine at
`VTGKModeDX32A.sys:0x0005a660` accepts these `MEMORY_CACHING_TYPE` values and
stores a cache flag in the low PTE nibble:

| OS cache type | value | reserved PTE low nibble | mapped PTE low nibble |
|---|---:|---:|---:|
| `MmNonCached` | 0 | `8` | `9` |
| `MmWriteCombined` | 2 | `4` | `5` |
| `MmHardwareCoherentCached` | 3 | `2` | `3` |

The mapped forms are constructed by `0x0005ace0`: the page PFN occupies bits
31:12 and bit 0 is added to the reserved cache flag.  Ordinary `MmCached`
(value 1) falls into the "Unknown cache type" failure path.  Hardware-coherent
cached memory is accepted only when device capability byte `+0x2e` is set;
otherwise the KMD reports `Coherent memory not supported` at
`0x0005a78f`.  The binary imports no `KeFlushIoBuffers`, consistent with WDDM
using noncached/WC mappings or a genuinely coherent mapping instead of
allowing an unqualified WB CPU alias.

These are **not** proven encodings for the internal RTSF page table used by
`SR6A..SR6C`; applying them there would corrupt the Linux-proven PFN.  They are
adjacent evidence only: the Windows stack also treats cache policy as separate
from address translation.  Calls to `MmAllocatePagesForMdl` in the KMD pass a
highest acceptable address of `00000000:ffffffff`, proving a below-4-GiB
allocation constraint, while the live PTE25 tests prove operation through
about 3.72 GiB but no higher.  This is the vendor-supported production
boundary: set an explicit 32-bit DMA mask for RTSF backing and reject any
mapped segment whose DMA address crosses 4 GiB.  Do not infer a wider limit
from the PTE's `3fffffff` PFN mask, because cached attribute bit 25 overlaps
nominal physical-address bit 37.  The target's active `SRB0..SRB8` remap begins
at 8 GiB and does not extend this tested contract.

There are two additional direct Windows allocation paths.  The PCIe/nonlocal
heap callback described above maps the actual RTSF MDL using the request's
cache type and encodes cached policy in internal-PTE bit 25.  Separately,
`IOCTL_DMA_LOCK_CREATE` at `0x0002ab00` allocates below 4 GiB and maps its MDL
to user space explicitly as `MmNonCached` at
`0x0002ac09..0x0002ac16`.  That DMA-lock buffer drives the descriptor-DMA
register path rather than proving the RTSF encoding, but it corroborates the
vendor's preference for deliberately selected NC/WC/cache-aware mappings over
an unqualified WB alias.

## Four distinct ordering and cache operations

These operations are all necessary at different boundaries and are not
substitutes:

| Operation | State affected | Required event |
|---|---|---|
| CPU CLFLUSH / DMA ownership sync | CPU data cache versus SF payload | before GPU access and after GPU retirement |
| `SR6F.bit7` GTI flush | cached SF virtual-to-physical translations | after PTE changes, before publishing an SF address |
| H0 `006c=00800000` and decoded pipe ordering | internal 2D/3D/render visibility | at the vendor draw/order boundaries |
| H2 subtype-FE `A00.bit0=1` | texture consumer cache | after producer completion and texture rebinding, before sampling |

The exact one-texture Xorg packet uses A00=`00000011`: one active texture in
bits 7:4 and the one-shot clear in bit 0.  Even its zero-texture state emits
A00=`00000001`, so this is the normal invalidation path; the per-stage A18
force-miss bit remains clear.  Keep H1 A01.bit8 enabled and issue the A00 clear
after the rendered resource is ordered complete and rebound for sampling.

No extra SF-specific render-cache packet was needed in the measured job: its
exact 232-dword texture body already contained the A00 clear.  Changing A51
bit 20, performing the two-sided CPU-cache transition, matching internal PTE
bit 25 to the CPU mapping, and using the vendor ordering/cache packets made
both low coherent memory and high-PFN cached GEM shmem pass.  This establishes
the tested linear SF target and texture path for GEM shmem; tiled resources
and cache-policy transitions of a live mmap remain separate work.
