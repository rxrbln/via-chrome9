# Windows VX900 driver reverse engineering

Status: static extraction in progress.  All addresses in this file are specific
to the binaries identified by their hashes; they are analysis addresses, not
GPU register addresses.

## Verdict

The programming manuals are not complete enough to implement a programmable
Chrome9 HD 3D driver.  They are sufficient to begin display, 2D, fixed-function
3D, register-state, and carefully bounded command experiments, but not to build
a correct NIR backend or production command-submission ABI.

The decisive gap is the shader ISA.  The VX900 CR table assigns program upload
subtypes, while the detailed chapter calls those values reserved.  HC3/HCM add
program-store sizes but never define instruction words.  The manuals also do
not completely specify address patching, tiling, coherency, fencing, context
state, or recovery.

The supplied Windows package is therefore relevant and is an exact VX900
reference: its INF binds PCI `1106:7122`, and the binaries contain explicit
`7122` branches.  Static analysis has already recovered useful transport,
relocation, fence, program-upload, and partial shader-encoding details.  The
OpenGL and Direct3D binaries contain byte-identical compiler backend routines,
so static comparison of the two frontends is now the primary ISA-recovery path.
The driver's shader-binary and command-dump switches remain useful optional
confirmation, but Windows execution is not required for the next phase.

## Binary provenance

Package INF version: `03/03/2015, 8.14.14.0235`.

| File | Role | PE timestamp | SHA-256 |
|---|---|---|---|
| `VTGUModeDX32A.dll` | 32-bit Direct3D UMD; exports `OpenAdapter` | 2015-03-03 03:58:08 | `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| `VTGOGL32A.dll` | 32-bit OpenGL ICD; exports the Windows `Drv*` entry points | 2015-03-03 03:56:27 | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` |
| `VTGKModeDX32A.sys` | 32-bit WDDM kernel-mode display driver | 2015-03-03 03:55:19 | `618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56` |
| `ucb_32a.sys` | display/firmware support module | -- | `ad00c56d8532ad6fd8886a70a040f6d149dbf16c4904c27b15e8d17f758be442` |
| `VTROM.bin` | VGA option ROM for device `7122` | -- | `d6ddebe6b103b0c506a8420ca09dd9599d7fd02289f06fa5304f6614afbf9a3c` |

The embedded PDB paths identify the original tree as
`d:\sw\s3gdrv\uc_whql22\uma`.  The OpenGL ICD identifies renderers for Chrome9
HC, HC3, HCM, and "VIA Chrome9 HD Pro IGP" and carries the version string
`1.4 24.16.01j`.  Its embedded extension/function names are compiler evidence,
not by themselves proof that each feature is exposed on VX900.

## Command-buffer grammar recovered from the KMD

The KMD contains a command-buffer decoder at analysis address `0x0009fd60`.
Its top-level dispatch masks the first dword with `0xffff0000` and recognizes:

| Header | Signature | Decoder at | Directly observed behavior |
|---:|---:|---:|---|
| 0 | `fe000000` | `0x000a08a0` | register/value pairs using a 16-bit MMIO offset |
| 1 | `fe010000` | `0x000a0a90` | values written to a port at `0x200000 + low16(header)` |
| 2 | `fe020000` | `0x000a0c30` | begins with an offset/value write, then consumes compact words until a new header |
| 3 | `fe030000` | `0x000a0db0` | counted payload; selector written to `043c`, then data streamed in groups of at most four through `0440`-`044c` |
| 4 | `fe040000` | `0x000a0f90` | counted payload streamed in groups of at most four through a four-register window |
| 5 | `fe050000` | `0x000a1130` | register/value pairs using 16-bit offsets |
| 7 | `fe070000` | `0x000a1240` | register/value pairs; structurally similar to header 0 |

Header 3 is counted transport, not native-shader validation.  Its decoder
honors the payload length and streams opaque dwords in groups of at most four;
it does not recognize VS/PS opcodes, registers, branch targets, or program
limits.  The KMD supplies no semantic backstop; shader acceptance must be
enforced before this transport packet is built.

The decoder also recognizes `cc000000`, `cd000000`, and `ce000000` classes as
single words, special class `fef60000`, and exact marker `fef65333`.  Header 2's
inner scan treats `fe060000` as a header boundary, although the top-level
decoder has no header-6 handler.  This is a real asymmetry and should not be
papered over by assuming header 6 has header-7 semantics.

Other directly recovered constraints and values:

- the decoder diagnoses a DMA buffer that is not 256-byte aligned;
- KMD command construction uses `cc000000` and `cc0000cc` padding;
- a synchronization sequence contains `fe020420`, parameter `041c`, and
  additional `69xxxxxx`, `6axxxxxx`, and `6bxxxxxx` address/length words;
- header 0/7 handlers special-case MMIO offsets `0e90` and `0e94` with value
  `3` and acknowledge DMA status through MMIO `0200` bits `20` and `80`;
- the main WDDM render path has an explicit PCI-device comparison against
  `7122` and uses a different padding/metadata path for it.

The decoder calls `WRITE_REGISTER_ULONG` directly.  Its exact runtime purpose
still needs a call-table trace, so it is evidence for packet semantics but not
yet proof that every normal rendering submission is software-decoded this way.

## Exact VX900 command-reader initialization

The exact 7122-family command-reader initializer is the VtgKMode path at
analysis `0x000c7c00..0x000c7f6f`; it explicitly compares device `7122` at
`0x000c7cf9`.  The generic helper at `0x000a1440` describes a related newer
CR layout but is not the authoritative VX900 initializer.  The 7122 path calls
the shared STOP/replacement helper at `0x0001b690..0x0001b85c`, which first
reads MMIO `0444`
and tests bit 31.  If a reader is already active, it appends an aligned stop
trailer to the current stream, updates only subtypes `64/65` with pause ID
`10b`, and waits until MMIO `0400 & 00101ff3` becomes zero.  The appended
32-byte form is:

```text
fe020420 0000041c 00300000 cc000000
ccccccc0 ddd00000 ccccccc0 ddd00000
```

The final two quadwords are the region to which the new stop address points.
This agrees with the manual's minimum two-quadword kickoff distance and with
the older Linux driver's practice of placing termination inside the ring.

If no reader is active, the callback translates CPU pointers to GPU addresses
as `cpu_pointer - cpu_mapping_base + gpu_mapping_base`, writes selector
`00100000` to `041c`, and streams these words through `0420`:

| Subtype | Value |
|---:|---|
| `60/61` | start address low 24/high 8 bits |
| `62/63` | end address low 24/high 8 bits |
| `64/65` | pause address low 24/high 8 bits plus pause ID |
| `66/67` | disabled jump address `fffffff0` |
| `68` | trigger/control word `680a0001` |

This Windows path's generated start address has location bits `00b`, consistent
with its private mapping being in the SL domain.  It must not be generalized to
a raw Linux DMA address: the live VX900 instead fetched a coherent DMA page
through the KMD's SF translation table with location `01b`.  The manual defines
pause ID `00b` as pause, `01b` as jump, and `10b` as stop.  Live lifecycle tests
also corrected the earlier STOP assumption: changing an idle paused reader to
STOP without immediately replacing its ring left `0400.bit4` asserted.  The
safe current behavior is to replace an idle paused ring in place and leave the
reader paused after completion.

This initializer is generation-specific.  The legacy VIA driver uses
`043c/0440` and an older combined-high-byte format, so transplanting its MMIO
sequence to VX900 is incorrect even though its ordering rules remain useful.

## Type-03 program-upload ABI

Static tracing from both UMDs into the KMD decoder establishes the following
type-03 packet layout:

```text
fe03043c
payload_dword_count
(subtype << 24) | (03 << 16) | HParaOS:HParaAdr
cc000000
payload...
```

The packet is preceded by `cc000000` words when alignment is needed.  Recovered
subtypes are `20` for 128-bit pixel ALU instructions, `21` for 32-bit pixel
texture-address instructions, `22` for pixel float constants, `31` for 128-bit
vertex instructions, `32` for vertex float constants, and `33` for packed
vertex integer constants.  The selectors and widths are identical in the
OpenGL and Direct3D code.

The D3D vertex integer/Boolean updater is now bounded exactly at
`VTGUModeDX32A.dll:0x101593b0..0x10159507`.  At
`0x10159413..0x1015948c` it emits
`fe03043c,00000010,33030000,cc000000` followed by all 16 integer constants.
Each source int4 occupies a 20-byte record and its X/Y/Z/W low bytes are packed
to one little-endian dword.  At `0x101594aa..0x10159500`, 16 canonical Boolean
bytes at six-byte stride are shifted into a 16-bit mask and stored into the
low half of saved type-04 A61 state; the upper control half is preserved.
This independently confirms both Linux 5.75 upload formats.  The Windows path
does not dirty-slice integer uploads, while Linux can start on a four-constant
boundary with selector `33030000 | (first_index >> 2)`.

The complete field evidence, compiler-object layout, matching backend
addresses, and current partial ALU decode are maintained in
[shader-isa.md](shader-isa.md).

## Relocation and resource addressing

`S3gPatch` is at analysis address `0x000921c0`.  Patch-list entries are 24 bytes.
The patch helper at `0x00093390` derives an address-format selector from the low
byte of patch-entry dword 2.  Recovered transformations include:

| Format selector | Transformation observed in the UMA patcher |
|---:|---|
| `1`-`3` | split address: low 24 bits in one command word, high address bits in the following word |
| `4` | address shifted right by 1 |
| `5`, `9`, `10` | address shifted right by 3 |
| `13`, `15` | full address written directly |
| `14` | address with low four bits cleared |
| `16` | low address nibbles merged into two existing command fields; likely tiled/swizzled addressing |
| `138` | low 24 bits in one word and upper byte placed in the following word's bits `8:15` |

Unknown selectors fail with `STATUS_INVALID_PARAMETER`; the table is therefore
not merely diagnostic text.  Format 16 needs instruction-level rechecking and
controlled samples before assigning bitfield names.

## System-memory page table

The KMD's map and unmap routines are at analysis addresses `0x0002ddf0` and
`0x0002db10`.  They establish a 4 KiB page size and construct the primary PTE
from the physical page/PFN, masked with `3fffffff`.  Cached/snooped mappings
add `02000000`; an invalid entry is `80000000`.  A parallel residency array is
written with `1` for resident and `0` for absent, and an optional secondary
table stores a low 20-bit value.  After changing entries the KMD executes the
graphics translation/cache-flush path, including the sequencer `SR6F` flush
control reached through helpers around `0x0001ad10`/`0x0001adc0`.

The mapper reads those values directly from the MDL PFN array and masks them
with `3fffffff` (`0x0002de8e..0x0002df2c`), matching the old Linux internal
RTSF table rather than the separate OS AGP-interface PTE format.  Internal PTE
bit 25 (`02000000`) is now identified as the cached/snooped attribute, not a
universal valid bit.  The PCIe/nonlocal allocator at `0x0002e3b0` passes
`request+20` unchanged as the `MEMORY_CACHING_TYPE` to
`MmMapLockedPagesSpecifyCache`, passes that same MDL to `0x0002ddf0`, and sets
the mapper's last Boolean exactly when the cache type equals `MmCached` (1).
The mapper then ORs bit 25 into every PFN at `0x0002df01..0x0002df20`.
`MmNonCached` (0) and `MmWriteCombined` (2) normally retain the plain PFN; an
adapter capability can independently force bit 25.  Under another device flag
the mapper executes WBINVD after table population
(`0x0002dfe6..0x0002e00c`).  The KMD's `MmAllocatePagesForMdl` call sites use
`00000000:ffffffff` as the highest acceptable physical address, establishing
a 32-bit allocation ceiling but no tighter statically visible limit.  Live
cached-PTE operation near 3.72 GiB reaches close to that boundary.  A Linux
driver should therefore set a 32-bit DMA mask for all RTSF backing and FE3x
fence words; neither the 30-bit PFN mask nor the chipset's higher remap range
is evidence that the vendor stack supports allocations above 4 GiB.

There is also an address-dependent remap before the PTE write.  When a saved
threshold has bit 31 set, the call-mode byte is zero, and `PFN >= threshold`,
`0x0002de9c..0x0002ded1` substitutes
`PFN - threshold + remap_base`.  Adapter initialization reads the base and
threshold from a checksummed private shadow table: the base is `u16[+26] << 4`
PFNs and the threshold/enable word is `u32[+28]`
(`0x000125e3..0x00012614`).  `10000` is not a PCI-ROM file offset; it is the
second half of a 128-KiB mapping whose physical start is framebuffer end minus
`20000`.  Therefore the table is at `resource_base + framebuffer_length -
10000`.  The access-range template has a zero framebuffer base in this binary,
so the measured 128-MiB target points to physical `07ff0000`.  Validation has
no magic signature: byte 2 is the length, the sum over that many bytes must be
nonzero and congruent to zero modulo 256, and length must be at least `2c` for
the fields.  Exact target bytes are required before this transform can be
applied on Linux.

On the target, the exact read of physical `07ff0000` was rejected with `EPERM`
because Linux owns the range as System RAM.  It is unsafe to bypass strict
`/dev/mem` policy for a Windows boot-time scratch copy.  The chipset exposes a
non-invasive alternative at PCI `D0F3` (`1106:3410`): configuration bytes
`84..85` contain Low Top Address, `A0..A1[14:12]` contain framebuffer size,
and `F8..FB` contain the enabled flag plus 64-MiB-granularity beginning and
ending addresses for hardware DRAM remapping.  A read-only `lspci -s
0000:00:00.3 -nn -xxx` captures these values without touching the GPU.

The target values are `e801`, `df02`, and `00218201`, respectively.  They
decode to nominal low top `e8000000`, 128-MiB framebuffer and effective DRAM
top `e0000000`, plus an enabled remap from `200000000` through `218000000`.
They also reconstruct the KMD table exactly: `+26=e800` gives base PFN
`e8000`, while `+28=80200000` gives enabled threshold PFN `200000`.  Therefore
the KMD maps pages at or above 8 GiB down to the displaced range beginning at
`e8000000`.  PFNs `ddf92`, `de4fe`, and `1088` are all below the threshold and
remain unchanged; this rule is not the cause of the current high-shmem SF
failure.

The PTE attribute is now confirmed dynamically as well as statically.  Cached
GEM shmem at DMA `ddcae000` passed a guarded render-target test with internal
PTEs `020ddcae/020ddcaf/020ddcb0`; cached shmem at `ddcca000` passed a
six-page texture-plus-target test with PTEs `020ddcca..020ddccf`.  Plain PTEs
in that upper-RAM region had produced partial CPU-visible results.  This proves
bit 25 is the required cached/snooped contract on the target and rejects a
lower physical-address ceiling there.

The VX900 manual independently defines `SR6A:SR6C` as the RTSF/GART table base
in SL address bits `28:12`, `SR6C.bit7` as write protect, `SR6D:SR6F` as the SL
system-memory base in bits `43:21`, and `SR6F.bit7` as cache flush.  What is
still missing is the exact PTE bit naming, table-size programming, and the
allocation/enable sequence around these routines; Linux must not enable a
page table until those are traced and tested.

The separate OS AGP interface gives adjacent CPU-cache policy evidence.
Its reservation routine at `0x0005a660` accepts `MmNonCached` (0),
`MmWriteCombined` (2), and conditionally `MmHardwareCoherentCached` (3),
encoding them as reserved PTE low nibbles `8`, `4`, and `2`.  The mapper at
`0x0005ace0` combines the physical PFN in bits 31:12 with mapped low nibbles
`9`, `5`, and `3`, respectively.  Ordinary `MmCached` (1) is rejected as an
unknown cache type.  Type 3 is rejected with `Coherent memory not supported`
unless device capability byte `+0x2e` is set.  This independently confirms
that address translation is not an implicit CPU-WB coherency contract.  These
low-nibble flags do not belong to the internal RTSF table controlled by
`SR6A..SR6F`; applying them there would be unsafe.  The internal table instead
uses the PFN plus the cached bit 25 described above.  The distinction and the
resolved high-PFN GEM experiment are in
[sf-render-target-coherency.md](sf-render-target-coherency.md).

The legacy Linux PCIe heap supplies the complementary allocation proof:
`via_chrome9_drm_sg_alloc()` uses `__vmalloc(..., PAGE_KERNEL_NOCACHE)`, and
the UMD copy/memset ioctl reaches it only through `pcie_vmalloc_nocache`.
Windows `IOCTL_DMA_LOCK_CREATE` independently maps its allocated MDL with
`MmNonCached` at `0x0002ac09..0x0002ac16`.  A normal WB shmem page with a
plain internal PTE therefore matches neither vendor strategy.  The exact
PTE-only test has now passed at high PFNs: cached WB shmem requires internal
PTE bit 25 while the PFN, GTI flush, command, and all completion checks remain
unchanged.

The KMD's patch diagnostics expose these resource IDs:

| ID/case | Diagnostic name |
|---:|---|
| `0` | `RID_MapBase` |
| `4` | `2D_SRC` |
| `5` | `2D_DST` |
| `37`-`40` | `RID_RT0` through `RID_RT3` |
| `44` | `RID_TXB` |
| `45`-`50` | `RID_TXB_Cube` cases |
| `51` | `RID_TXL` |

Each logged patch also has width, height, stride, and format metadata.  These
IDs explain how the Windows UMD describes render targets and texture resources
to the KMD, but they must not become a Linux UAPI verbatim.

## Fences, submission, and reset

The KMD installs WDDM callbacks corresponding to `S3gRender`, `S3gPatch`,
`S3gSubmitCommand`, `S3gQueryCurrentFence`, `S3gPreemptCommand`, and
`ResetFromTimeout`.

For device IDs `1122`, `2122`, `5122`, `7122`, `8122`, and `a122`,
`S3gQueryCurrentFence` reads the current fence from MMIO `0494`; older paths
read `0474`.  An alternate enabled mode reads a per-engine memory fence instead.
This establishes `0494` as the VX900 fallback/current-fence register.

The producer is recovered at `0x000b66d0`, with the exact FE3x assembly at
`0x000b755f..0x000b85f4`.  Normal mode begins at `22` (interrupt generation
plus queue skip), conditionally adds interrupt-wait `10` and system-write `4`,
and chooses `FE33/00200000` for 3D, `FE30/00300000` for 2D,
`FE37/0031e000` for DMA, `FE35/000c0000` for HQV, or `FE3A/00020000` for VD.
The 2D and DMA masks intentionally include dependent engines.  A no-engine
wait sentinel uses `003fe000`.  Scheduler-enabled `d0[11:8]` follow tags are
3D=`100`, 2D=`200`, DMA=`300`, HQV=`500`, and VD=`600`.  This is stronger than
the old Linux VX900 path, which emits `FE300036` with a zero wait mask even
after arbitrary 3D payloads.

The ISR at `0x00077a60` samples `0200`, `0204`, `1280`, and `c500`.  It rejects
the interrupt before device dispatch when `0200==ffffffff` or `0200.bit31` is
clear, proving bit 31 is the global gate in the actual handler rather than
merely an initialization convention.  It then dispatches `0204.bit5` to
`0x00077830`.  For PCI `7122`, chip callback table index 4 selects slot 11
function `0x000c8360`; it acknowledges by reading `0204`, ORing `20`, and
writing it back when the adapter capability is enabled.  The dispatcher then
processes current fence IDs in `0x00076ad0` and schedules the DPC only if work
advanced.  The exact CR order is global-gate check, status sample,
acknowledge, process fence, then schedule DPC.  Linux can safely narrow the acknowledgement to the documented
RW1C value `0204=20` rather than copying the read/OR/write literally.

`S3gSubmitCommand` records the requested fence ID, distinguishes present from
non-present work, and dispatches through a chip-selected submission function.
`ResetFromTimeout` logs the current hardware fence, last requested fence, and
last submitted fence, clears four arrays for three engines/queues, cancels
driver notifications/timers, and invokes lower reset helpers.  Static analysis
now establishes the core of the lower reset helper at `0x0008a500`:

1. treat `MMIO[0400] & 00161fff` as a hang;
2. write zero to MMIO `0200`;
3. write `0f` then `00` to sequencer `SR4E`;
4. pulse sequencer `SR1A.bit6`, the documented PCI-bus software reset;
5. for device `7122`, clear a driver-side word and pulse `SRC7.bit7`;
6. poll `MMIO[0400] & 00101ff3`; revision 0/1 may also receive `20` at
   MMIO `0204` while polling;
7. call a substantial reinitialization helper at `0x000896b0`.

The last helper is now fully bounded at `0x000896b0..0x0008a4f5`.  It zeros
MMIO dwords `0000..0038` and `1e00..1fe0`, replays the complete transfer-port
3D default-state sweep, and ends with CR-port priming.  It does not restore
`0200`, the RTSF table, translation registers, ring state, or outstanding
fences.  The sequencer pulses use `10000h`-iteration delay loops and the final
engine-idle poll is unbounded.  Consequently this is destructive reset plus
default initialization, not context restoration; a Linux reset must separately
rebuild GART/ring/IRQ state and fail all affected scheduler fences.

The Windows continuation is explicit.  Reset sets adapter byte `+22f01=1`;
the next submission path at `0x00083dbb..0x00083e0b` sees it and performs
`0200 |= 80a00000` before clearing the byte.  The vendor header decodes the
mask as global interrupt enable plus DMA1 and DMA0 enables.  IRQ restoration
is therefore deferred until submission and still precedes any newly emitted
interrupt-wait fence; a Linux scheduler may restore only the global bit for
CR fences, but must preserve unrelated display/video enables.

That same deferred path calls the chip-table slot at device `+0xdf4`.  For
PCI `7122` this is `0x000a17a0`, which reconstructs the software description
of a `0x100000`-dword (4 MiB) ring and calls `0x000c7c00` to restart the
hardware command reader.  With `V` the CPU mapping at adapter `+22ec0` and
`P` its SF/GPU address at `+22eb8`, the vendor code sets `cursor=V`,
`end=V+0x400000`, pads the head to 128 bytes, and directly emits:

```text
MMIO 041c = 00100000                 # T10 transfer-port select
MMIO 0420 = 60 | ((P         ) & 00fffff0) | 00000001
MMIO 0420 = 61 | ((P         ) >> 24)
MMIO 0420 = 62 | ((P+00400000) & 00ffffff)
MMIO 0420 = 63 | ((P+00400000) >> 24)
MMIO 0420 = 64 | ((P+head_pad-10) & 00ffffff)
MMIO 0420 = 65 | ((P+head_pad-10) >> 24)
MMIO 0420 = 66ffffff
MMIO 0420 = 67ffffff
MMIO 0420 = 680a0001                 # threshold 0a, trigger
```

The A60 low bit selects SF.  The `A66/A67` value is the split
`0xfffffff0` jump sentinel, not a valid restart destination.  For the VX900
family the initial padding is one aligned `FE020420,0000041c,00300000,
CC000000` dummy followed by `CCCCCCC0,DDD00000` pairs until the CPU cursor is
128-byte aligned; pause is the translated cursor minus 16.  Thus the exact
first-submit recovery order is: restore `0200`, rebuild/restart the CR ring,
clear `+22f01`, then invoke the ordinary submission callback.  Replaying only
the 3D default-state sweep after reset leaves the reader unstarted.

The manual independently identifies `SR4E[5:4]` as 3D reset and `[3:2]` as 2D
reset, but marks its CR reset bits `[7:6]` read-only.  Consequently, pulsing a
guessed CR bit is not recovery.  The new Linux driver currently exposes only a
2D-reset lab gate; a full GPU reset must wait until the `SRC7`, `0200/0204`,
and `0x000896b0` state-restoration effects are completely traced.

`S3gPreemptCommand` is exact at `0x000837d0..0x00083ac0`.  It selects T10 with
`041c=00100000` and writes the four preemption words for completion address
`A` and fence ID `F`:

```text
08 | ((A >> 24) & ff)
09 | ( A        & fffff0)
0a | ( F        & ffffff)
0b | ((F >> 16) & 00ff00) | 000005
```

The fixed low value `5` is mode `10b` (IRQ, fence queue, and system-memory
write) plus trigger.  `A` is a per-queue 32-byte record at device `+0xee8`,
with the completion word at record offset `+0x10`; optional multi-queue mode
selects the record from the current queue index.  The implementation has
unbounded software-counter waits before it writes the packet, so those loops
are not suitable Linux scheduler behavior.

Windows also proves the previously missing hardware-context allocation and
restore geometry.  On `7122`/`a122`, initialization reserves `0x70000` bytes
from the top of local framebuffer memory: 16 slots of exactly `0x7000` bytes,
aligned down to 32 bytes.  A context switch emitted by the VX900 packet
builder at `0x000b6aab..0x000b70cf` first clears FVF declaration words
type-04 `A04..A07`, then saves old slot `n` with:

```text
FE020440 0000043c 00010000 CC000000
04000000 05000000 06000000 07000000
... alignment ...
FE020420 0000041c 00110000 CC000000
08 | ((C+n*7000) & ffffff)
09 | ((C+n*7000) >> 24) | 000100
CE000000 0B000001
... alignment ...
FE020420 0000041c 00300000 CC000000
(CC000000 CD000000 CE000000 CF000000) repeated four times
```

The address location bits are zero, hence local framebuffer (SL), and A09
bit 8 asks hardware to store 3D register writes; A0B bit 0 is the safe-save
trigger.  The four zero writes are specifically vertex declarations D3..D6,
not generic dummy words.  The `CE000000` word and four `CC/CD/CE/CF` quads are
vendor synchronization/dummy-class padding retained by this builder.  To
restore new slot `m`, Windows does not use T10 A68 bit 1.  It executes a
traditional restore branch into the saved slot:

```text
FE020420 0000041c 00110000 CC000000
68000001
69 | ((C+m*7000+1b0) & ffffff)
6a | ((C+m*7000+1b0) >> 24)       # SL location
6b00045e                           # 0x45e * 16 = 0x45e0 bytes
... alignment ...
FE820420 0000041c 00300000 CC000000
... alignment ...
FE020420 0000041c 00300000 CC000000
(CC000000 CD000000 CE000000 CF000000) repeated four times
```

T11 A68 bit 0 marks a restore branch, which waits for prior work; `FE82` is
the branch trigger.  The branch region is therefore exactly `+0x1b0` through
`+0x478f` within each `0x7000`-byte slot.  The leading `0x1b0` bytes are
hardware-save metadata/preamble; their internal image format remains opaque,
but allocation size and replay bounds are no longer unknown.  The manual says
the safe trigger saves all 3D writeback registers, but neither the manual nor
the KMD enumerates which H3 shader-store records appear in that generated
replay region.  Shader program preservation across this hardware context
switch is therefore plausible, not yet bit-exactly proven; software must not
substitute a partial `A60..A62` replay for the vendor branch image.

## Shader compiler and diagnostic controls

The OpenGL ICD includes an ARB vertex/fragment-program parser and backend, as
shown by complete internal `!!ARBvp1.0` and `!!ARBfp1.0` programs, parser error
messages, optimizer code, and hardware upload controls.  This is stronger
evidence than exported GL entry-point strings: the compiler itself is present.

The Direct3D UMD exports only the standard WDDM `OpenAdapter` entry point, but
contains the same Halcyon shader dump, optimizer, tiling, hardware vertex/index
buffer, and command-capture option set as the ICD.  This is evidence that the
two frontends share a hardware backend.  Direct3D bytecode and ARB programs can
therefore provide two independent source languages for differential native
binary tests.  The byte-identical constructor, scheduler, and metadata-finalizer
routines now make this shared-backend conclusion direct code evidence.

Both userspace binaries contain command-manager paths named
`cmLH_KickoffDMA_h6` and `cmLH_KickoffDMA_inv`; the KMD similarly contains
`PatchDmaBuffer_h6` and `PatchDmaBuffer_inv`.  The names establish multiple
chip-specific implementations, but static strings alone do not identify which
one is selected for `7122`.

The UMD/ICD reads DWORD options from the active primary display adapter's
registry key.  On Windows 7 the code enumerates the active display device,
removes the `\Registry\Machine\` prefix from its `DeviceKey`, and opens that
path under `HKEY_LOCAL_MACHINE`.  This normally resolves to:

```text
HKLM\SYSTEM\CurrentControlSet\Control\Video\{adapter-guid}\0000
```

The following relevant controls are initialized to zero and then overridden
from that key:

| Control | Evidence-backed use |
|---|---|
| `HalcyonMSPSSBinaryDumpEnable` | pixel-shader binary dump switch; exact file format TBD |
| `HalcyonMSVSSBinaryDumpEnable` | vertex-shader binary dump switch; exact file format TBD |
| `HalcyonMSPSDumpEnable` | pixel-shader dump switch; prefix semantics TBD |
| `HalcyonMSVSDumpEnable` | vertex-shader dump switch; prefix semantics TBD |
| `HalcyonFFPSDumpEnable` | fixed-function pixel-shader dump switch |
| `HalcyonPPSDumpEnable` | programmable pixel-shader dump switch |
| `HalcyonPSDumpStyle` | pixel-shader dump formatting/style selector |
| `HalcyonVSDumpEnable` | vertex-shader dump switch |
| `S3GDW_DumpBinaryFile` | binary-file dump switch |
| `S3GDW_CommandCapture` | full command-capture switch |

The same option block exposes useful experiment controls, including
`HalcyonEnableVS`, `EnableTnL`, `NullVS`, `NullPS`, `SVSEnable`, `SPSEnable`,
`1ALU1CACHE`, shader optimization passes, hardware vertex/index buffers,
multiple streams, tiling for display/Z/RT/textures, and tile-swizzle disable.
Those names establish controllable branches, not their precise semantics.

## Windows H6 vertex-declaration mapper

`VTGOGL32A.dll:0x10204320..0x102043ea` converts the ICD's internal attribute
type byte into the low eight bits of an H6 `HnVBDm` declaration word and adds
that attribute's packed size to the total input length.  The jump selector at
`0x1020442c` is 52 bytes, so all accepted and rejected internal types can be
enumerated exactly:

| internal type(s) | hardware format byte(s) | dwords consumed | decoded hardware type |
|---:|---|---:|---|
| `00..03` | `02 42 82 c2` | `1 2 3 4` | float32, dimensions 1..4 |
| `04` | `d1` | 1 | normalized 4D color/BGRA |
| `05..07` | invalid | - | rejected |
| `08` | `8c` | 1 | 3D unsigned 10-bit (`UDEC3`) |
| `09` | `8f` | 1 | normalized 3D signed 10-bit (`DEC3N`) |
| `0a` | `44` | 1 | 2D float16 |
| `0b` | `c4` | 2 | 4D float16 |
| `0c..0f` | `06 46 86 c6` | `1 1 1 1` | unsigned byte, dimensions 1..4 |
| `10..13` | invalid | - | rejected |
| `14..17` | `08 48 88 c8` | `1 1 2 2` | unsigned short, dimensions 1..4 |
| `18..1b` | `0a 4a 8a ca` | `1 1 2 2` | signed short, dimensions 1..4 |
| `1c..23` | invalid | - | rejected |
| `24..27` | `07 47 87 c7` | `1 1 1 1` | normalized unsigned byte, dimensions 1..4 |
| `28..2b` | invalid | - | rejected |
| `2c..2f` | `09 49 89 c9` | `1 1 2 2` | normalized unsigned short, dimensions 1..4 |
| `30..33` | `0b 4b 8b cb` | `1 1 2 2` | normalized signed short, dimensions 1..4 |

The decode follows the VX900 manual exactly: bits `7:6` are dimension minus
one, bits `5:1` select float32, float16, u8, u16, s16, unsigned decade, signed
decade, or color, and bit 0 enables normalization.  The table is therefore an
independent Windows confirmation of the public H6 format layout, not a set of
names inferred only from the internal enum.

The caller at `0x10204470..0x1020498b` builds the full declarations.  For each
active 16-byte element record it:

- places the mapped format in bits `7:0`;
- places the selected VS input register in bits `11:8`;
- places the dword offset in bits `19:12`;
- records per-stream base/stride state and advances the packed input length;
- forms declaration word 00 with `((element_count - 1) & 0xf) << 8` and
  `total_input_dwords & 0x7f`;
- writes `(active_stream_count - 1) << 20` into type-04 subaddress 62 while
  preserving its other fields.

This matches the Linux Xorg rectangle declaration: three active elements,
six input dwords, and two encoded resource units produce `00000206` and the
zero-texture resource word `622081ff`.  It also explains why the rejected H5
prefix could not feed the H6 VS even though the instruction upload itself was
accepted.

## Static extraction plan

Windows execution is not required to continue.  The primary path is to recover
the shared compiler backend, chip-selected callback tables, instruction
finalizers, program-control words, and the `7122` command-submission branches
directly from the binaries.  The staged plan and current field table are in
[shader-isa.md](shader-isa.md).

The KMD work proceeds independently: complete the format-16 relocation decode,
trace the `7122` selection to `h6` or `inv`, identify fence production and
acknowledgement, and reduce reset/cache/tiling paths to auditable state
transitions.  Every recovered field should be captured in a decoder before it
is used in an encoder or proposed Linux UAPI.

## Headless Wine compiler oracle

The private Direct3D shader backend can run under Wine without the Windows
KMD, a display device, or VX900 hardware.  This was demonstrated with Wine
11.10's WoW64 mode in a disposable prefix.  `regsvr32` first loaded the PE32
UMD successfully and reached the expected `DllRegisterServer not implemented`
diagnostic, proving that normal DLL initialization and imports work.

The checked-in [`../../tools/wine_shader_probe.c`](../../tools/wine_shader_probe.c)
then exercised three progressively deeper private entry points in the exact
hashed `VTGUModeDX32A.dll`:

| Image RVA | Operation exercised | Result |
|---:|---|---|
| `001ddc20` | construct the `05ac`-byte compiler object | success; ALU and TAU vectors allocated with zero initial counts |
| `001dbd30` | tokenize a D3D `ps_2_0` shader | success; one 36-byte IR record generated for the test MOV |
| `001dca80` | programmable instruction dispatcher | success; dispatched the IR to the native MOV emitter |
| `00203530` | scheduling and stream rewrite | success; final ALU/TAU vectors and metadata produced |

For the built-in `mov r0.xyzw, c0.xyzw` test, the raw emitter output was:

```text
01c00530 301c0053 00f08005 000d0020
```

This independently confirms source class 2 for a float constant, destination
class 0 for a temporary, identity swizzle, XYZW writemask, and dword-3
sub-operation `d` for MOV.  The apparently populated source-B slot is a fixed
unused template in this unary instruction and must not be interpreted as a
second MOV source.

The probe accepts an optional D3D token stream at
`Z:\tmp\chrome9-shader.bin`, tokenizes every instruction, supplies minimal
identity register-allocation callbacks, and writes
the exact selected input back to `Z:\tmp\chrome9-shader-used.bin`, plus
`Z:\tmp\chrome9-alu-raw.bin`, `Z:\tmp\chrome9-tau-raw.bin`, and corresponding
`*-final.bin` post-scheduler/finalizer streams.  Its private RVAs and object
layouts are tied to the documented DLL hash.  Raw and final forms are retained
separately so instruction rewriting can be distinguished from emitter output.

The current wrapper builds as a PE32 console executable with SHA-256
`332d9d7adfb2c6f66b20a000fb29bc691ebef06d21b0e1eb2233ed8946b798ec`.
With no optional input present, its built-in `ps_2_0; mov r0,c0; end` returns
zero and produces one ALU record, no TAU records.  Raw and finalized ALU are
byte-identical, 16 bytes, SHA-256
`2434244abe46d8eba8a130b151a7272cb55a25d29c991f108e10e620fff34cd1`.

For a TEX-then-MUL stream, input SHA-256
`4f0ccbe39de980f4d4a7f1d56146dd736093053881a7ea664de5623b287dbe55`,
the final stores are one ALU record and one TAU word:

```text
ALU: 0140b6d0 00040053 00f00000 00000820
TAU: 201001b0
```

Their respective SHA-256 values are
`d24f6a9bfe123b76f274ed7673ab27e3e7e597672bc82cc8adc4e476f4ddf475`
and `6bc2d9f1a19245224609b68f16f1fede46da85317e0860339a6c34e2d1aa980d`.
Raw and final stores are also identical in this small sample; nontrivial
programs must still use the finalized streams and metadata.  Compiler-object
files contain process pointers and consequently are not suitable hash-stable
golden blobs.

This makes large differential sweeps of opcodes, indices, swizzles,
writemasks, and modifiers possible entirely on Linux.  It does not replace
real-hardware tests for numerical behavior, scheduling hazards, cache
visibility, faults, or hangs.

The native vertex compiler is independently callable.  The checked-in
[`../../tools/wine_vs_shader_probe.c`](../../tools/wine_vs_shader_probe.c)
loads the same UMD but invokes only the H5/H6 VS 2.0 table selected by the
normal compile/cache manager:

| Image RVA | Linux symbol analogue | Stage |
|---:|---|---|
| `002407b0` | `PVSCP_MSVSPARSER_VS20` | parse D3D tokens into a `3a0c`-byte object |
| `002415f0` | `PVSCP_VALIDATE_VS20_H5` | stage immediate constants and remap inputs |
| `0023f4b0` | `PVSCP_DISPATCHER_VS20_H5` | emit 16-byte native instructions |
| `00241680` | `PVSCP_POSTPROCESS_H5` | resolve flow-control fixups |

The table order is parser, validator, dispatcher, postprocess, as proved by
the indirect calls at `0x10207519..0x1020753b`; validator and dispatcher were
easy to transpose when considering their addresses alone.  The dispatcher
needs only a software chipset descriptor reached through wrapper `+0c`,
driver view `+9c3c`, descriptor `+1c`.  Value `8` selects the same H5/H6
finalization branch as the normal UMD.  A deliberately incomplete first run
parsed successfully and faulted only at the final descriptor read
`0x1023fdfd`; after providing that field, all four stages completed with
status zero under Wine 11.10.

For the built-in `vs_2_0` DEF/ADD/MOV stream, the compiler reports nine
native instructions at object `+424`, stored at `+42c`, and writes a bounded
`0x4bc`-byte native blob.  The wrapper accepts an optional token stream at
`Z:\tmp\chrome9-vs-shader.bin` and writes parsed and native results to
`Z:\tmp\chrome9-vs-parsed.bin` and
`Z:\tmp\chrome9-vs-native.bin`.  It never calls OpenAdapter, creates a D3D
device, submits commands, or loads `VtgKModeDX32A`; consequently it is a safe
offline compiler oracle, not a GPU execution probe.

The corresponding static upload path at `0x1015abb0` reads native count
`+424`, copies `count * 16` bytes from `+42c`, and selects `31030000` at
`0x1015af74`.  Vertex float and integer constants use separate
`3203xxxx` and `33030000` uploads; compiling a shader does not itself touch
those MMIO-backed command paths.

Pixel constants are likewise outside the offline compiler object.  The D3D
update/build routine at `0x1015b000` proves the complete PS 2.0 source layout:

| Selector | Dwords | Source in UMD software state |
|---:|---:|---|
| `22030000` | 64 | driver view `+4bd8`, API `c0..c15` |
| `22030080` | 64 | driver view `+4cd8`, API `c16..c31` |
| `22030040` | 28 | `[driver view+9c34] +100`, internal ids 16..22 |
| `220300c0` | 28 | `[driver view+9c34] +300`, internal ids 48..54 |
| `2203005c` | 4 | `[driver view+9c34] +2e54`, narrow last-vector update |
| `220300dc` | 4 | `[driver view+9c34] +2e64`, narrow last-vector update |

The PS 2.0 full-bank branch is at `0x1015c117..0x1015c43b`; it copies IEEE-754
dwords directly.  The older branch at `0x1015be4b` calls conversion routine
`0x1020ab00` for each component before uploading a 64-dword bank.  Internal
bank dirty flags are at `+400/+404`; the narrow-update gate is at `+2e50`.
These are packet-builder facts only: the headless compiler wrapper never calls
`0x1015b000`, because doing so would enter the command-buffer allocation path.

## Optional golden-vector confirmation

If a suitable Windows installation later becomes available, the driver's own
dumps can cheaply confirm static conclusions.  A disposable 32-bit Windows 7
installation with a VX900 board would use this procedure:

1. save the adapter registry key and a baseline Process Monitor trace;
2. first set `HalcyonMSPSSBinaryDumpEnable`,
   `HalcyonMSVSSBinaryDumpEnable`, and `S3GDW_DumpBinaryFile`, leaving optimizer
   controls at defaults; enable source-level dump switches only if needed;
3. compile one minimal ARB or D3D shader at a time and preserve source, dump,
   command capture, and rendered result;
4. vary exactly one opcode, register, writemask, swizzle, modifier, immediate,
   texture target, branch condition, or program length per sample;
5. repeat selected samples with one optimizer disabled to distinguish ISA bits
   from instruction scheduling;
6. enable `S3GDW_CommandCapture` for a clear, triangle, texture sample, fence,
   and present, then correlate packets with the KMD grammar above;
7. snapshot MMIO and framebuffer data before/after each test and retain the
   exact driver hashes.

Before bulk capture, Process Monitor should identify the dump filenames and
directory because those strings are assembled dynamically.  Do not expose the
test machine to untrusted shaders or command streams; these binaries predate
modern GPU isolation expectations.

## What can be implemented now

Work can start on a read-only probe, register database, KMS/2D investigation,
packet decoder, relocation model, static shader disassembler, and a
fixed-function off-screen bring-up harness.  A production render node, stable
submit UAPI, and tiling modifiers should wait for measured command, memory,
fence/reset, and tiling behavior.  NIR code generation should wait for a
complete, cross-checked static ISA field table; hardware golden vectors are
then required for correctness validation rather than initial discovery.
