# VIA 5.75.32.87a Chrome9 KMD hardware extraction

This note extracts the hardware-facing behavior in
`binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31`.  It concentrates on
VX900 (`PCI 1106:7122`, internal `CHIP_H5S2VP1`) and treats the source as an
encoding and sequencing oracle, not as code suitable for reuse.  The supplied
2015 Windows 7 KMD is used as an independent, explicitly identified static
cross-check where its 7122 path is stronger or newer.  Line anchors refer to
the Linux files in that directory; Windows anchors are PE analysis addresses.

## Confidence and generation boundaries

`CHIP_H5S2VP1` is explicitly annotated as device `7122` in
`via_chrome9_drm.h:160-178`; `7122` is in the PCIe group in
`via_chrome9_drv.c:194-201`, and the AGP feature is removed for `7122` in
`via_chrome9_drm.c:1029-1037`.  The KMD nevertheless trusts userspace's
`chip_agp` and `chip_sub_index` values at initialization
(`via_chrome9_drm.c:768-783`) instead of deriving them from the PCI ID.  A new
driver must identify VX900 in-kernel and never expose that choice to userspace.

The following facts are direct VX900 evidence:

- the H5S2VP1 branches in the page-table, ring, fence, resume, interrupt, and
  engine-default paths;
- the new `FE40` branch and `FE30` fence encodings used only by the H5S2VP1
  submission routine;
- the PCIe system-framebuffer address path used by both H6S2 and H5S2VP1.

Definitions used only by H5/H5S1 or H6S1/H6S2 are retained below as
cross-generation evidence and labeled accordingly.

Confidence tags used in the bit tables below are: **P** = observed working on
the live VX900; **D** = stated by the VX900 programming manual but not yet
exercised by this driver; **S** = recovered from the 5.75 KMD or binary driver
and therefore source evidence rather than a hardware guarantee; **I** = an
inference supported by more than one observation/source; **U** = unknown or
contradictory.  Compound tags such as D/S mean that independent documentation
and source agree.  A field is not considered safe for userspace merely because
it is P or D.

## Minimal safe VX900 command-reader bring-up

The source establishes the following order.  This is also the sequence already
used by the gated `chrome9_selftest_pcie_ring()` in the new driver at
`src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/chrome9/chrome9_drv.c`.

1. Reserve a page-table region in VRAM and a system-memory ring.  Do not copy
   the old driver's userspace-selected VRAM offset (`via_chrome9_dma.c:613-620`).
2. Clear GART-table write protect with `SR6C.bit7=0`, program the table base and size,
   initialize unused PTEs to `80000000`, install valid PTEs, execute the
   `SR6F.bit7` GTI-cache flush protocol, then set `SR6C.bit7=1`
   (`via_chrome9_dma.c:256-301`, `430-454`).
3. Publish and flush the command bytes before giving the reader an address.  The
   old driver explicitly flushes cached SG pages for branch buffers
   (`via_chrome9_dma.c:1812-1824`), although its implementation has a boundary
   bug described below.
4. Write CR type `11h` to `041c`, then `06000000` and `07100000` to `0420` for
   H5S2VP1.  Select type `10h` and set readback group CR with `02000000`
   (`via_chrome9_dma.c:640-659`).
5. Build the initial ring prefix as header 2 / CR dummy, followed by alternating
   `CCCCCCC0`, `DDD00000` until the next 128-byte boundary.  The PAUSE address
   is 16 bytes before that boundary (`via_chrome9_dma.c:546-560`).
6. Program the start/end/pause/jump words through `041c/0420`, OR bit 0 into
   the start address to select SF on H5S2VP1, and trigger with threshold 10 plus
   bit 0 (`via_chrome9_dma.c:565-591`).
7. Bound every poll.  The vendor code does not: several page-table and ring
   loops can spin forever.

Live tests have confirmed fetch from SF virtual address zero, pause, idle,
header-0 dispatch, a guarded off-screen 2D fill, a complete H6 3D draw to
local VRAM, and a guarded H6 3D render target mapped through SF PTEs.  The SF
target requires explicit CPU cache writeback/invalidation before and after GPU
access; barriers alone exposed stale CPU lines.  See `hardware-lab.md`.

An important teardown correction came from the live target: changing the paused
reader's pause ID to STOP without installing a replacement leaves CR busy on
VX900.  The vendor routine uses STOP only while replacing an already paused
ring and then immediately programs and triggers the new ring
(`via_chrome9_dma.c:489-591`).  Keep an idle reader paused, or replace it as one
atomic, bounded operation; do not treat STOP as a general reset.

### Cold takeover and two-phase reader priming

The vendor implementations do not make their first newly installed ring carry
ordinary rendering work.  Linux `set_agp_ring_cmd_inv()` constructs a CR-dummy
header and pads the new H5S2VP1 ring to a 128-byte boundary, puts PAUSE at the
last 16-byte slot, and triggers that dummy-only ring.  Normal submissions are
made later by appending commands and updating only T10 A64/A65 with the new
pause (`via_chrome9_dma.c:546-591,1003-1078`).

The 2015 Windows KMD independently has the same lifecycle in its exact
7122-family initializer at analysis `0x000c7c00..0x000c7f6f` (the explicit
`7122` comparison is at `0x000c7cf9`).  Subject to two driver-state flags it
first calls the shared stop helper `0x0001b690..0x0001b85c`.  If `0444.bit31`
says the old reader is paused, that helper:

1. takes the driver-owned ring's software write cursor, aligns it, and appends
   `fe020420 0000041c 00300000 cc000000` plus alternating
   `ccccccc0/ddd00000` words;
2. points A64/A65 with ID `10b` (STOP) at the trailer and waits for
   `0400 & 00101ff3` to clear (`0x0001b6bd..0x0001b859`).

The 7122-family initializer then writes a fresh CR-dummy packet, pads the new
ring to 128 bytes for device IDs including 7122, puts PAUSE at the final
16-byte slot, masks start low bits and ORs bit 0 for SF, installs
A60..A67, and triggers with `680a0001`
(`0x000c7d28..0x000c7f60`).

The Windows wait is unbounded and its old stream is driver-owned, so this is
not permission to patch a firmware ring at an unknown CPU address.  Its value
is the independent proof that STOP is an in-place ownership transition which
must be followed immediately by replacement, and that a newly installed ring
is established before normal rendering submission.

A fresh-boot live run exposed why this distinction matters.  After the full
address-free 3D initialization and CR setup, the first pre-populated work ring
advanced `043c` from `00000000` to `00000180`, reached PAUSE at `00000170`, and
became engine-idle, but its guarded 2D fill had no register or pixel side
effects.  A following no-upload baseline ring executed, and repetitions of the
same store-only workloads then executed normally.  This proves fetch and pause
control in the anomalous transaction, but one run does **not** prove a silicon
rule that “the first ring is discarded.”  Its sampled `current-pause=0x10`
also differs from the later steady `0x210` request-FIFO lead.  That is
consistent with an incompletely filled first request FIFO or simply an earlier
sample, not proof that read-ahead depth controls dispatch.  Plausible causes
still include:

- takeover of firmware's pre-existing paused reader without the vendor's
  owned-ring STOP/replacement transition;
- stale request-FIFO/dispatch state that is initialized by the first trigger;
- state cleared by the full T11 address-free sweep and restored only partly by
  the known CR setup;
- an ordering requirement stronger than the currently used coherent-memory
  barrier, although successful fetching makes a basic GART/PTE publication
  failure less likely.

The first explicit two-phase attempt then refined the PAUSE criterion.  Its
dummy-only phase reached `pause=00000070`, `0444=c0000000`, and
`043c=00000280`, exactly the previously inferred `0x210` request-FIFO lead,
but `0400=00000010`.  The probe incorrectly required the combined busy mask to
be zero, timed out before exposing phase 2, restored all guarded state, and was
followed by a healthy controlled reboot.  No guarded phase-2 work was exposed
or dispatched and no guard changed.  This is evidence that CR bit 4 can mean
an **armed reader held at PAUSE**, not necessarily a hang, when every non-CR
engine bit is clear.

Both vendor implementations agree with that interpretation.  Linux
`set_agp_ring_cmd_inv()` triggers the initial PAUSE ring and does not wait for
CR idle; `kickoff_dma_ring_inv()` later advances only A64/A65 and likewise has
no intervening idle requirement (`via_chrome9_dma.c:546-591,1003-1078`).  The
exact Windows 7122-family initializer returns immediately after writing
`680a0001` (`0x000c7f49..0x000c7f6f`).  Windows waits for CR bit 4 to clear only
after changing an owned old ring to STOP, before installing its replacement
(`0x0001b7d9..0x0001b859`).  PAUSE and STOP therefore have different readiness
criteria.

Until those are discriminated, use this safe two-phase sequence after every
reset, resume, translation rebuild, or change of ring ownership:

1. Allocate and map one persistent coherent ring.  Initialize the entire
   reachable ring plus at least `0x220` bytes after each pause with legal
   padding.  Publish PTEs and complete the bounded GTI flush.
2. Run all address-free 2D/3D initialization **before** installing CR, because
   the H5S2VP1 3D sweep writes T11 indexes `00..20` and destroys CR-side state.
3. Reapply `06000000`, `07100000`, and readback selector `02000000` through the
   CR port.  Install a dummy-only prefix ending at a 128-byte-aligned PAUSE;
   program A60..A68 once and trigger with `680a0001`.
4. Require bounded confirmation of `0444.bit31`.  Record `045c`/`0460`/`0464`
   only as raw diagnostics; their HC3 address-readback meanings have not
   produced changing or layout-valid values on live VX900 silicon.  Require
   all **non-CR** ordinary busy bits to be clear:
   `0400 & 00101fe3 == 0`.  Accept CR bit 4 as either zero or one; value
   `00000010` with matching PAUSE is an extendable armed state.  Treat `043c`
   only as a fetch/request pointer and retain all prefetched storage.
5. Append the first real, address-free regression packet after the initial
   tail, publish it with the DMA ordering operation appropriate to the ring,
   and update only A64/A65 to a later PAUSE.  Do not rewrite A60..A63 or retrigger
   A68 for ordinary submissions.
6. Confirm the later PAUSE, clear non-CR busy mask `00101fe3`, and a real side
   effect (initially a saved/restored 2D latch or guarded 2D fill), not merely
   pointer motion.  A completion fence remains necessary before general BO
   reuse.  Only then admit 3D state, memory-writing work, or interrupts.

If the old paused ring is already owned and mapped by this driver, the Windows
STOP trailer plus bounded idle wait may be used before step 3.  If ownership is
unknown, never derive a CPU pointer from firmware's `043c`; install the new
dummy ring using the already live-tested direct replacement path and regard
phase 1 as the ownership boundary.  Keep the old translation and storage pinned
until replacement is confirmed, because the request FIFO can be `0x220` bytes
ahead of PAUSE.

| Reader state | Observable minimum | Permitted transition | Forbidden assumption |
|---|---|---|---|
| foreign paused | `0444[31]=1`, ownership/readbacks unknown | install owned dummy ring with complete A60..A68 tuple; preserve old resources if known | `043c` is a safe CPU offset or STOP is teardown |
| owned prime paused | `0444[31]=1`, `0400 & 00101fe3=0`, owned software epoch/pause tuple; bit 4 may remain set | append/publish work, then advance A64/A65 only | CR bit 4 alone is a hang, pointer movement proves dispatch, or raw `045c`/`0464` validates ownership |
| running/fetching | pause bit not yet observed and/or non-CR busy mask nonzero | wait for fence/pause under deadline | PTE, end, or ring storage can be reclaimed |
| fetch paused, engine busy | `0444[31]=1`, but `0400 & 00101fe3 != 0` | retain all mappings and collect diagnostics; invoke only a proven full recovery | PAUSE means delivered 2D/3D work retired |
| owned completed paused | `0444[31]=1`, non-CR mask zero, completion fence/side effect observed; bit 4 may remain set | submit by later A64/A65 or perform atomic owned STOP/replacement | `043c` equals retirement or is behind PAUSE |
| STOP requested | A65 ID=`10b` | require full `0400 & 00101ff3=0`, then immediately install replacement | PAUSE's permitted CR bit applies to STOP, or STOP is stable teardown |

## PCIe GART / system-framebuffer translation

### Table format and registers

The KMD accepts a framebuffer-relative page-table offset and size from
userspace (`via_chrome9_dma.c:613-624`).  It maps that VRAM range at
`fb_base_address + pagetable_offset` (`via_chrome9_dma.c:235-254`).  Hardware
programming is:

| Field | Encoding | Source |
|---|---|---|
| table base bits `19:12` | `SR6A[7:0]` | `via_chrome9_dma.c:264-266` |
| table base bits `27:20` | `SR6B[7:0]` | `via_chrome9_dma.c:268-270` |
| table base bit `28` | `SR6C.bit0` | `via_chrome9_dma.c:272-275` |
| table-size/protection code | `SR7B[3:0]` | `via_chrome9_dma.c:277-281` |
| GART table write protect | `SR6C.bit7`; clear while CPU updates the table, then set | `via_chrome9_dma.c:259-262,296-299`; Part I, internal p.46 |
| GTI-cache flush | wait for `SR6F.bit7=0`, then write it as 1; hardware self-clears | `via_chrome9_dma.c:285-294`; Part I, internal p.47 |

`protect_size_value()` maps a table size in `(2^(i+12), 2^(i+13)]` to `i+1`
for `i=0..7` (`via_chrome9_dma.c:81-90`).  Thus a 64 KiB table selects code 4.
The KMD does not itself require 64 KiB; that size contains 16,384 PTEs and maps
64 MiB with 4 KiB pages.  Keep table size and GPU-VA aperture size distinct in
the new VM code.

An invalid PTE is exactly `80000000` (`via_chrome9_dma.c:283-284`).  A valid
entry is `page_to_pfn(page) & 3fffffff`, with no valid bit added
(`via_chrome9_dma.c:444-446`).  The value is therefore a 4 KiB physical page
number with 30 retained bits.  This source does not prove that all resulting
42 physical-address bits work on VX900; production allocations must stay
within a DMA mask confirmed on hardware.

The Windows KMD map path at analysis `0x0002ddf0` identifies bit 25 exactly.
Its nonlocal allocator passes one MDL both to
`MmMapLockedPagesSpecifyCache` and to this mapper; it sets the mapper Boolean
iff the requested CPU type is `MmCached`, and the mapper then ORs
`02000000`.  `MmNonCached` and `MmWriteCombined` normally retain a plain PFN.
Live high-PFN WB shmem confirms the contract: plain PTEs produced partial
CPU-visible results, while `PFN | 02000000` passed guarded render-target and
texture-plus-target jobs around 3.72 GiB.  This is a cached/snooped attribute,
not a valid bit.  It overlaps nominal PFN bit 25, so the supported mapping
policy must stay below the vendor allocator's explicit 4-GiB ceiling rather
than assuming all 30 PFN payload bits are usable with caching.

Table updates are performed with GART-table write protect cleared, after
waiting for an earlier flush to finish, followed by a new GTI-cache flush and
re-protection
(`via_chrome9_dma.c:430-454`).  The KMD has no timeout, no rollback, and no
serialization against another VGA user.  A modern driver needs a VM lock, VGA
arbitration for indexed sequencer access, readbacks/barriers, and timeout/error
recovery.

The manual explicitly calls `SR6C.bit7` *GART Table Write Protect Enable*, not
translation enable (`docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`,
internal p.46).  The KMD's update sequence proves that clearing and restoring
it around PTE writes works, but does **not** prove that clearing it disables
address translation.  Likewise `SR6F.bit7` is documented as GTI cache flush;
calling it a TLB invalidate is a reasonable implementation inference, not a
documented guarantee.  No actual VX900 translation-disable bit has been
identified.

The legacy `RING_BUFFER_CLEANUP_FLAG` path only clears `SR6C.bit7` and returns;
it neither stops CR nor invalidates mappings (`via_chrome9_drv.c:116-141`).
If the manual description is literal, this merely removes table write
protection.  It must not be copied as a “GART disable” or teardown sequence.

### GART/PT bit-exact audit

| State | Exact encoding | Confidence | Constraint |
|---|---|---|---|
| table base | `A[19:12]=SR6A[7:0]`, `A[27:20]=SR6B[7:0]`, `A[28]=SR6C[0]`; low 12 bits implicit zero | D/S; P for current live base | table must be 4 KiB aligned and below 512 MiB of VRAM-relative address space |
| table write protection | `SR6C[7]`; `1` prevents hardware writes, `0` disables protection | D/S | preserve SR6C bits 6:1; this is not proven to disable translation |
| SL system-memory base | `A[28:21]=SR6D`, `A[36:29]=SR6E`, `A[43:37]=SR6F[6:0]` | D/U | KMD preserves/restores but does not program it for SF GART |
| cache flush | set `SR6F[7]`; GTI clears when complete | D/S; P in initialization | bounded pre-poll and post-poll required; source only pre-polls |
| PTE invalid | exactly `80000000` | S; present in the live table but fault semantics untested | remaining bits must be zero until fault semantics are known |
| PTE valid | `PFN & 3fffffff`; address is nominally PFN bits `[29:0]` / PA `[41:12]`, but Windows allocations are capped below 4 GiB | S; P through about 3.72 GiB | use a 32-bit DMA mask; no permission/writable bit is recovered |
| Windows PTE bit 25 | cached/snooped attribute: set exactly for `MmCached`, clear for NC/WC; live WB-shmem pass | S/P | set for cached GEM shmem, clear for deliberately NC/WC mappings; not a valid bit |
| page size | one 32-bit PTE per 4 KiB CPU page | S/P | no large-page encoding recovered |
| table-size code | KMD writes `SR7B[3:0]=i+1` for `2^(i+12) < bytes <= 2^(i+13)`, `i=0..7` | S/P for current setup | VX900 manual marks all SR7B bits reserved: generation discrepancy, never infer beyond tested codes |

The SR7B conflict is material: the KMD's `protect_size_value()` and programming
are explicit (`via_chrome9_dma.c:81-90,132-137,277-281`), while the VX900
manual calls `SR7B[7:0]` reserved (Part I, internal p.49).  The current live
mapping proves the selected value works on this machine, not what the field
means or whether larger codes are valid.  Record the firmware value, change
only `[3:0]`, restore it on unload, and hard-limit the aperture to a live-tested
table/code pair.

PTE publication requires CPU/DMA ordering even on the x86 target: write all
PTEs, issue the appropriate write/DMA barrier, trigger `SR6F.bit7`, wait for it
to self-clear with a deadline, and only then publish a CR address that can reach
the mapping.  The old KMD waits only for the *previous* flush to clear and does
not wait for the newly requested flush (`via_chrome9_dma.c:142-150,285-299,
430-454`).  It also has no hardware fault register or recoverable invalid-PTE
path; deliberately fetching an invalid PTE is therefore not a bounded probe.

The source contains a base-programming bug: both initialization paths OR
`SR6C.bit0` into the old value rather than clearing bit 0 first
(`via_chrome9_dma.c:127-130`, `272-275`).  Moving a table from a base with bit
28 set to one with it clear can retain the stale bit.  The new driver correctly
uses replace semantics, not OR semantics.

### GPU virtual layout and memory domains

For PCIe, the command ring begins at SF GPU virtual address zero.  The CPU SG
allocation covers `DMA_size + agp_tex_size` and is installed from PTE zero
upward (`via_chrome9_dma.c:669-676`, `157-189`).  `agp_offset` is set to
`DMA_size`, so the texture/branch heap begins immediately after the ring in the
same SF address space (`via_chrome9_dma.c:617-620`).  Branch addresses are
`branch_heap_offset + agp_offset` (`via_chrome9_dma.c:1655-1658`).

The old allocation model exposes four names—video, AGP, PCIe-video, and PCIe
(`via_chrome9_drm.h:341-363`)—but collapses PCIe to the AGP heap internally
(`via_chrome9_mm.c:661-683`).  VRAM allocations are returned relative to
`back_offset`; PCIe allocations are offsets in the post-ring SF heap
(`via_chrome9_mm.c:713-745`).  This is useful for reconstructing old addresses,
but a new driver should expose GEM BOs and per-BO GPU VAs, not heap offsets or
physical addresses.

The SG allocation is a `PAGE_KERNEL_NOCACHE` vmalloc range whose individual
pages are mapped into consecutive PTEs (`via_chrome9_dma.c:322-395`).  Branch
payload pages are explicitly `clflush`ed on x86 before submission
(`via_chrome9_dma.c:42-74`, `1812-1824`).  Fence storage instead comes from
`kmalloc`, is converted with `virt_to_phys`, and is handed directly to hardware
(`via_chrome9_mm.c:333-386`).  This is an x86/32-bit coherency assumption, not
a portable DMA contract.  Use `dma_alloc_coherent()` or `dma_map_*()` and the
matching sync operations.

### Production GART implications

- Reserve and pin the table as a kernel-owned VRAM BO; never accept its offset
  from userspace.
- Use invalid PTEs everywhere before publishing any reachable SF address and
  flush the GTI cache after each committed update.
- Keep the ring and every referenced BO mapped until the last fence completes.
- Never disable/replace translation while a non-idle reader can fetch SF.
- Enforce the confirmed aperture limit and prevent PTE-index arithmetic
  overflow.
- Keep distinct location tags for SL, SF, and any raw/system domain.  On
  H5S2VP1, ring start bit 0 is the confirmed SF selector
  (`via_chrome9_dma.c:572-574`).

## Command-reader lifecycle

### Registers and status

The header defines these command-reader registers
(`via_chrome9_3d_reg.h:83-135`):

| MMIO | Meaning |
|---:|---|
| `041c` | CR transfer/type selector |
| `0420` | CR streamed parameter data |
| `0438` | CR transfer end |
| `043c` read | current command-request/fetch address, not proven retirement PC |
| `0444` | command-reader status; bit 31 means PAUSE |
| `045c` | HC3 pause-address readback; not live-validated on VX900 |
| `0460` | HC3 jump-address readback; not live-validated on VX900 |
| `0464` | HC3 start-address/location readback; not live-validated on VX900 |
| `0468` | HC3 end-address readback; not live-validated on VX900 |
| `0448` | VQ status; bit 30 means full |
| `0400` | engine status |

At `0400`, CR busy is bit 4, 2D busy is bit 1, and the 3D mask is `00001fe1`.
The vendor's combined idle mask is `00001ff3`
(`via_chrome9_3d_reg.h:103-118`).  It does not include the separately named VQ
or MPEG bits.  The old polling helper performs only 50,000 tight reads and the
ioctl discards its timeout result (`via_chrome9_dma.c:1080-1099`,
`1846-1855`).  Use time-based bounded polls and report all status registers.

The VX900 manual omits the general `0400` bit-name table, but the HC3 manual's
same-layout table, the H5 KMD mask, and the Windows VX900 masks permit the
following evidence-bounded decode.  Names marked HC3 are cross-generation
evidence, not a promise that every auxiliary engine is unchanged on VX900.

| Bit | Mask | HC3/manual name | VX900 evidence and rule |
|---:|---:|---|---|
| 0 | `00000001` | overall 3D engine | included in Linux H5 3D and Windows masks; busy if set |
| 1 | `00000002` | 2D engine | explicitly named by Linux H5 source |
| 2 | `00000004` | CMDQ/VQ | separately named by Linux H5 source; excluded from its normal combined idle mask |
| 3 | `00000008` | reserved / MPEG in KMD | separately named MPEG by Linux; do not infer 3D pipeline state |
| 4 | `00000010` | command regulator | explicitly named by Linux H5 source; may remain set while ordinary PAUSE is armed, but must clear after STOP before replacement |
| 5 | `00000020` | vertex processor | HC3 name plus inclusion in H5 3D mask |
| 6 | `00000040` | clipping engine | HC3 name plus inclusion in H5 3D mask |
| 7 | `00000080` | setup engine | HC3 name plus inclusion in H5 3D mask |
| 8 | `00000100` | primitive/rasterization engine | HC3 name plus inclusion in H5 3D mask |
| 9 | `00000200` | read-color engine | HC3 name plus inclusion in H5 3D mask |
| 10 | `00000400` | pixel shader | HC3 name plus inclusion in H5 3D mask |
| 11 | `00000800` | pixel engine | HC3 name plus inclusion in H5 3D mask |
| 12 | `00001000` | burst engine | HC3 name plus inclusion in H5 3D mask |
| 13 | `00002000` | GEMI | HC3 only; notably excluded from Linux `00001fe1` and Windows `00101ff3` |
| 14 | `00004000` | reserved | never assign |
| 15 | `00008000` | sequencer/T-arbiter 1 | HC3 only; not in recovered VX900 idle masks |
| 16 | `00010000` | sequencer/T-arbiter 0 | HC3 only; not in recovered VX900 idle masks |
| 17 | `00020000` | HQV-V3 | HC3 name; Linux calls this HQV1; Windows reset hang mask includes it |
| 18 | `00040000` | HQV-V1 | HC3 name; Linux calls this HQV0; Windows reset hang mask includes it |
| 19 | `00080000` | reserved | excluded from both Windows masks; report raw, never call it an engine busy bit |
| 20 | `00100000` | four-channel DMA | included in Windows `00101ff3` idle mask and `00161fff` reset-hang mask |
| 21 | `00200000` | LCD down-scaling | HC3 only; outside recovered render-idle masks |
| 22 | `00400000` | CR auto-framebuffer-swap | HC3 only; outside recovered render-idle masks |
| 31:24 | `ff000000` | readback-group ID | depends on readback selection; mask before interpreting status |

The Windows KMD uses two distinct masks.  STOP/recovery-idle polling uses
`00101ff3`: DMA plus 3D stages, CR, and 2D, excluding CMDQ, MPEG, and bit 19.
Its reset helper first diagnoses a hang with `00161fff`, additionally including
CMDQ/MPEG and both HQV engines, then polls `00101ff3` after reset.  These are
useful scope definitions, not justification for copying its unbounded loops or
requiring CR bit 4 to clear while an ordinary PAUSE remains armed.  PAUSE-ready
polling must instead ignore bit 4 and use `00101fe3`, together with
`0444.bit31`.  Do not require a matching `045c` value on VX900: four live
streams with different pauses returned the same `045c/0460/0464` tuple
`5778e8b1 bbfee773 1f2a0f31`, including low bits which contradict the HC3
reserved/location fields.  The old Linux ring code consumes `043c` and `0444`
but never these three address readbacks.  Until a VX900-specific readback
enable sequence is proved, retain them as raw diagnostics only.

The observed failed quad value `00080031` therefore decomposes exactly as bits
19, 5, 4, and 0.  Bit 5 localizes the active pipeline stage to VP/front-end;
bits 4 and 0 show CR/overall-3D backpressure.  Pixel shader, setup, clipping,
primitive, read-color, pixel, and burst-engine bits are all clear.  Bit 19 is
reserved in HC3 and deliberately ignored by both Windows masks, so it must stay
unclassified.  `0444.bit31` being set at the same time means fetch reached a
PAUSE; it does not mean already delivered vertex work retired.  Recovery must
not reclaim mappings or overwrite the prefetched ring merely because CR is
paused.

### Ring-control parameter words

With CR type `10h` selected, ring control uses the following streamed words
(`via_chrome9_3d_reg.h:155-187`):

| Prefix | Meaning |
|---:|---|
| `60xxxxxx` / `61xxxxxx` | start low 24 / high 8 |
| `62xxxxxx` / `63xxxxxx` | end low 24 / high 8 |
| `64xxxxxx` / `65xxxxxx` | pause low 24 / high 8 plus ID |
| `66xxxxxx` / `67xxxxxx` | jump low 24 / high 8 |
| `68xxxxxx` | threshold/control/trigger |

Pause IDs are PAUSE=`000`, JUMP=`100`, STOP=`200` in bits covered by the
`65` word.  Control bits defined for the `68` word are clear=`8`, restore AGP
=`4`, restore all=`2`, trigger=`1`; only trigger is used by this KMD.  The ring
starts with threshold 10 (`000a0000`) plus trigger
(`via_chrome9_dma.c:565-591`).  The reset/restore definitions are therefore
unverified constants, not a recovered reset procedure.

For H5S2VP1, start is ORed with 1 for SF.  End/pause/jump are 16-byte address
encodings, while initial and subsequent pause positions are padded to a
128-byte boundary (`via_chrome9_dma.c:546-574`, `1021-1054`).  The jump sentinel
is `fffffff0` / high `ff` (`via_chrome9_dma.c:557,584-587`).

When advancing the ring, the driver pads with a CR-dummy header plus
`CCCCCCC0`/`DDD00000` pairs to 128-byte alignment, sets pause to the aligned
pointer minus 16, and writes only the new `64/65` words
(`via_chrome9_dma.c:1003-1078`).  When wrapping, it writes a dummy region at the
tail, programs jump and the new head pause, and fills the head to a 128-byte
boundary (`via_chrome9_dma.c:1125-1190`).  The VX900 tail jump is calculated as
the aligned pointer minus 64, with a comment claiming a `4*128` alignment need
(`via_chrome9_dma.c:1157-1164`).  That comment and arithmetic do not agree;
preserve the observed 128-byte/pointer-minus-64 behavior as an experiment item,
not a specification.

The ring-space algorithm reserves 1 KiB at wrap (`INV_QW_PAUSE_ALIGN * 16`),
double-reads the current pointer because the authors found it unreliable, and
busy-waits without a timeout (`via_chrome9_dma.c:1193-1343`).  This is evidence
for conservative head/tail sampling and guard space.  It is not acceptable
scheduler logic.

For H5/H6S2/H5S2VP1 ring submissions, `release_space_inv()` appends a dummy
header and 256 `CC000000` words (`via_chrome9_dma.c:1345-1374`).  Space
reservation additionally includes `256 + 4 + 256` dwords as a “VT3293 ring
buffer stability” patch (`via_chrome9_dma.c:1418-1425`).  The exact amount is
probably an overfetch/FIFO workaround and should be reduced only after guarded
VX900 tests.

### CR internal-register bit matrix

Words streamed after selecting HParaType `10h` or `11h` encode the sub-address
in bits `31:24` and the value in bits `23:0`.  The following is the subset that
can affect fetching, arbitration, addresses, or completion.  Manual anchors are
internal pages 74-84 of Part II.

| Type/sub-address | Exact bits | Meaning | Confidence / use |
|---|---|---|---|
| T10 A02 | `[20:12]` MMIO readback address `[10:2]`; `[11]` enable debug-port readback; `[7:0]` readback group (`0` CR, `1` FE, `2` PE, `3` RC, `4` PS, `5` XE, `6` BE, `7` counters) | read-register-back selector | D; KMD writes `02000000`, selecting CR without debug readback (S) |
| T10 A04 | `[2:1]`: `00` video GFX priority, `01` normal GFX priority, `1x` round-robin; `[0]` route video-related GFX to HQV data path | command dispatch | D/U; untouched by KMD |
| T10 A08 | `[19:0]=A[43:24]` | preemption fence high address | D/U; privileged |
| T10 A09 | `[23:4]=A[23:4]` | preemption fence low address | D/U; privileged |
| T10 A0A | `[23:0]=preemption ID[31:8]` | preemption ID low word | D/U |
| T10 A0B | `[15:8]=preemption ID[7:0]`; `[2:1]` mode (`00` system write, `01` IRQ+queue, `10` both, `11` reserved); `[0]` trigger | preemption completion/trigger | D/U; no live reset recovery exists |
| T10 A0C | `[23:16]` request kill count default `32h`; `[15:8]` main-buffer priority mask `FEh`; `[7:0]` VQ mask `FCh` | CRMI arbitration | D/U; preserve firmware default |
| T10 A0D | `[23:16]` sub-main mask `F8h`; `[15:8]` video mask `F0h`; `[7:0]` GFX-command mask `E0h` | CRMI arbitration | D/U |
| T10 A0E | `[23:16]` decoder mask `C0h`; `[15:8]` GFX-data mask `80h`; `[7:0]` 3D-writeback mask `00h` | CRMI arbitration | D/U |
| T10 A60 | `[23:4]=start A[23:4]`; `[1:0]` location `00` SL, `01` SF, `10` SM, `11` LL | main-buffer start | P for SF; D all codes |
| T10 A61 | `[7:0]=start A[31:24]` | main-buffer start high | P/D |
| T10 A62/A63 | A62 `[23:4]=end A[23:4]`; A63 `[7:0]=end A[31:24]` | main-buffer end | P/D |
| T10 A64/A65 | A64 `[23:4]=pause A[23:4]`; A65 `[9:8]` ID (`00` pause, `01` jump, `10` stop, `11` reserved), `[7:0]=A[31:24]` | main-buffer pause | P/D; STOP is not a general reset |
| T10 A66/A67 | A66 `[23:4]=jump A[23:4]`; A67 `[7:0]=A[31:24]` | main-buffer jump | D/S; not live-proven |
| T10 A68 | `[21:16]` read threshold (default 8); `[5]` H1 data width (`0` 128-bit, `1` 32-bit); `[4]` clear fence queue; `[3]` clear AGP cycle; `[2]` restore command cycle; `[1]` restore 3D registers; `[0]` trigger | main-buffer control | P for threshold 10 + trigger; D otherwise |
| T10 A70/A71/A72 | command-queue start/end: A70 `[23:4]` start low and `[1:0]` location; A71 `[23:4]` end low; A72 `[15:8]` end high, `[7:0]` start high | VQ/CMDQ address | D/U; unused by KMD |
| T10 A73 | `[23:0]` command-queue length in 128-bit units; minimum `0x800` | VQ/CMDQ size | D/U |
| T10 A74 | `[21:16]` write threshold; `[13:8]` read threshold; `[7:4]` read-vs-write arbitration; `[2]` source (`0` PCI, `1` AGP); `[1]` variable request length; `[0]` enable | VQ/CMDQ control | D/U; branch forbidden while VQ1 enabled |
| T11 A00 | `[17]` hardware mutual exclusion of 2D/3D (default 1); `[16]` request-lock mode | 2D/3D arbitration | D/U; firmware state must be sampled |
| T11 A03 | `[23]` wait 3D, `[22]` 2D, `[21]` VD, `[20]` HQV0, `[19]` HQV1, `[18:15]` DMA3..0, `[14]` HQV0 EOF, `[13]` HQV1 EOF, `[8:4]` idle count, `[0]` all engines | traditional fence wait | D/U |
| T11 A04/A05 | if mode bit 2 is zero: 32-bit fence type; if one: system-write address A04 `[19:0]=A[43:24]`, A05 `[23:4]=A[23:4]` | traditional fence payload/address | D/U |
| T11 A06 | `[23:0]=fence ID[23:0]` | traditional fence ID low | D/S |
| T11 A07 | `[22]` queue-full skip; `[21]` interrupt wait; `[20]` trigger; `[19:16]` mode 0..7; `[15:8]=ID[31:24]` | traditional fence control | D/S |
| T11 A08/A09 | A08 `[23:4]=save A[23:4]`, `[1:0]` location; A09 `[8]` store 3D-register writes, `[7:0]=A[31:24]` | context-save buffer | D/U; required size/layout unknown |
| T11 A0B | `[0]` safe trigger for saving 3D writeback registers | context save | D/U; has priority over A09 bit 8 |

The KMD's initialization pair is now decoded, not opaque: `06000000` writes
T11 A06 fence ID zero, and `07100000` writes A07 with trigger set, mode zero,
recording fence ID zero (`via_chrome9_dma.c:640-659`; Part II, internal
pp.82-83).  This likely initializes fence readback state; it does not initialize
the 3D engine.  The source names A68 bit 1 `RestoreAll`, while the VX900 manual
specifically says “Trigger Restore 3D Register Cycle”
(`via_chrome9_3d_reg.h:183-187`; Part II, internal p.78); the narrower manual
meaning is authoritative for VX900.

Two exclusions are release-critical.  Enabling T10 A04 command switching
disables traditional wait/fence and traditional branch, and enabling VQ1 makes
FE8x branches illegal because hardware cannot stop framebuffer queue reads
(Part II, internal pp.74,80).  Neither mode may be changed while jobs are live,
and the initial driver should leave both at their firmware/default setting
until readback or a known-good initialization sequence is available.

### Live pointer anomalies are request-FIFO read-ahead

The live pauses and `043c` samples were:

| Stream | pause | early current | later current | later delta |
|---|---:|---:|---:|---:|
| first dummy stream | `0070` | `0080` | `0280` | `0210` |
| H6 fill stream | `00f0` | `0180` | `0300` | `0210` |

The repeatable steady delta is strong evidence that `043c` is the next
request/fetch position, not the last retired instruction.  The source defines
the request FIFO as 34 entries of 128 bits, or `0220` bytes
(`via_chrome9_dma.c:36-38`).  A pointer `0210` bytes past the programmed pause
is exactly one 16-byte slot short of that capacity: the pause slot plus the
following 33 prefetched slots account for all 34 entries.  The early `0010` and
`0090` deltas are naturally explained by sampling while that FIFO was still
filling.  This interpretation is an inference from the live values plus the
source constant; the manual only calls `043c` the current address.

The old allocator already treats this register cautiously.  It reads it twice
because the value is “sometimes ... unreliable” and waits against regions that
must not be overwritten (`via_chrome9_dma.c:1241-1258,1270-1284,1300-1316,
1324-1339`).  Branch submissions additionally append 17 aligned dummy records,
which consume 34 128-bit FIFO entries (`via_chrome9_dma.c:1519-1550,
1596-1629,1662-1693`).  Safe implications are:

- use `0444.bit31`, engine-idle state, or a fence for completion; never interpret
  `043c == pause` as a completion requirement;
- keep at least the full `0220`-byte read-ahead window mapped and initialized
  beyond a pause, with a larger guard until repeated wrap tests justify reducing
  the vendor's 1 KiB/extra-padding policy;
- do not reclaim or overwrite ring storage solely from one `043c` sample;
- retain the source's double sampling, but replace its unbounded waits with a
  timeout and hang recovery.

One independent stream-construction defect was exposed by reconstructing the
fill offsets.  After the final six-dword header-0 sync, the experimental stream
placed its manual dummy header 2 at byte `00d8`, not on a 16-byte boundary.
The relevant construction is in the new driver's
`chrome9_drv.c:520-565` as currently analyzed.
Every vendor packet builder aligns a header first; in particular,
`addcmdheader2_invi()` pads until 16-byte aligned
(`via_chrome9_3d_reg.h:401-412`).  The fill still succeeded because its command
had already been delivered and this malformed packet was trailing padding, but
future branch/fence streams must use the shared aligned builder.

### Why the first exact H6 fill latched but did not draw

The original packet was not missing Quick Start.  The unstripped Xorg binary's
H6 solid path emits the same seven header-0 pairs in this order: mode `0004`,
destination base `0014`, pitch `0008`, destination position `0010`, dimension
`000c`, fixed-pattern color `0058`, and command `0000`
(`bin/via_drv.so:viaAccelSolidHelper_H6+0x64..+0x108`, virtual addresses
`0002abc4..0002ac68`).  It encodes destination base in eight-byte units and
pitch as `(bytes >> 3) << 16`.  The XAA setup constructs the command as
`ROP << 24 | 00002001` (`bin/via_drv.so:VIASetupForSolidFill_H6+0x60..+0x70`,
`00025ac0..00025ad0`), so `f0002001` is the correct GXcopy/fixed-color BitBLT.
Bit 23 Quick Start is deliberately clear: the command register is written last
and that write starts the operation.  The manual says Quick Start instead starts
on a write to destination-position register `0010`.

The ring and the 2D destination use independent domains.  Ring-start bit 0
selects translated SF, but destination-location `00b` in pitch register
`0008[31:30]` selects SL/local framebuffer.  The successful live values make
the encoding exact: `base=00ffc008 << 3` equals BAR2 offset `07fe0040`, and
`pitch=00100000` decodes as `(128 >> 3) << 16`.  The manual assigns location to
`0008[31:30]` and base payload to `0014[25:0]`
(`docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`, internal pages 99-100).
Do not set the 2D destination to SF merely because the command stream itself is
fetched from SF.

The missing prerequisite was `VIAInitialize2DEngine()`.  For the VX900 path it
first clears the address-free 2D register range, selects pre-CR transfer type
`00100000`, streams `00000000,00333004,60000000,61000000,62000000,63000000,
64000000,7d000000`, emits `fe020000/00000000`, then selects `00fe0000` and
streams `00000004,40008c0f,44000000,45080c04,46800408`
(`bin/via_drv.so:VIAInitialize2DEngine+0x00..+0x84,+0xfb..+0x284`,
`000223a0..00022624`).  Installing this sequence changed the same translated
ring fill from a latch-only result to 1,024 correct pixels with intact guards
(`docs/research/hardware-lab.md:188-218`).  Treat those opaque words as required
VX900 2D initialization, not as general reset state, until their fields are
decoded.

The module-switch sequence was already right.  `Command_CRSync_2D3D()` emits a
one-pair header 0 writing `006c=00400000` for its H6 path
(`bin/via_drv.so:Command_CRSync_2D3D+0xe0..+0x184`,
`00084d20..00084dc4`), and `viaExaSolid_H6()` places it both before and after
the solid packet (`bin/via_drv.so:viaExaSolid_H6+0x71,+0x111`,
`0002b351..0002b40b`).  The KMD comments that module-switch tracking belongs to
the userspace context and only requires kernel-wrapped DMA buffers to retain the
proper 3D boundary (`via_chrome9_dma.c:1213-1219`); it does not inject a hidden
2D switch.

Nor does this KMD enable the 2D clock or initialize 2D registers.  Its hardware
setup programs GART/CR/ring state, and its only indexed-VGA change here is
`SR6C.bit7` GART-table write-protect control (`via_chrome9_dma.c:430-454`,
`via_chrome9_drv.c:116-139`).  The VX900 manual identifies `SR3F[3:2]` as the 2D
clock control and `SR4E[3:2]` as the 2D engine/register reset
(`docs/OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf`, PDF pages 36 and 39).  A
production driver must audit/preserve that firmware/display-driver state and
define a bounded reset sequence; the successful live fill proves no extra clock
write was needed on the current boot, not that it is universally unnecessary.

## Packet grammar

All legacy packet builders align headers to 16 bytes and fill leading words
with `CC000000` (`via_chrome9_3d_reg.h:375-504`).  Recognized old signatures are
`FE00`, `FE01`, `FE02`, `FE03`, `FE04`, `FE05`, `FE06`, `FE07`, `FE09`, and
`FE0A`; newer branch forms include `FE40`, `FE41`, `FE43`, `FE45`, `FE47`, and
`FE4A` (`via_chrome9_3d_reg.h:56-81`).

### Header 0: 2D register/value pairs

```
d0 = FE000000
d1 = number of register/value pairs
d2 = 00000000
d3 = CC000000 software flag
then d1 pairs: MMIO offset, value
```

This comes directly from `addcmdheader0_invi()` and `add2dcmd_invi()`
(`via_chrome9_3d_reg.h:375-386,553-557`) and is mirrored by the BCI fallback
(`via_chrome9_dma.c:823-835`).  Saved/restored latch writes and an initialized
H6 solid fill are now confirmed on the live VX900
(`docs/research/hardware-lab.md:188-218`).

### Header 1: 2D host BitBLT stream

```
d0 = FE010000 | dwAddr
d1 = data dword count
d2 = 00000000
d3 = CC000000 software flag
then d1 data dwords
```

The software fallback ignores `dwAddr` and streams payload dwords to successive
offsets beginning at zero in the separate host-BLT aperture
(`via_chrome9_dma.c:838-848`).  Thus the low field's hardware meaning is U; the
builder name alone is not proof that it is a byte offset.  Do not enable this
packet for untrusted users until that field, start/length, and
destination-state effects are fully validated.

### Header 2: typed CR/3D parameter stream

```
d0 = FE020000 | (data_port + 4)
d1 = data_port
d2 = parameter type/subtype/offset/address selector
d3 = CC000000 software flag
then parameter words until the next recognized header
```

For CR, the port pair is `041c/0420`; for 3D it is `043c/0440`.  The first word
is written to the selector port, subsequent words to the stream port
(`via_chrome9_3d_reg.h:401-425`, `via_chrome9_dma.c:860-877`).

Parameter types are Vdata=`00000000`, Attr=`00010000`, Tex=`00020000`,
Pal=`00030000`, FVF=`00040000`, PreCR=`00100000`, CR=`00110000`,
Cfg=`00fe0000`, Dummy=`00300000` (`via_chrome9_3d_reg.h:189-217`).  Texture
subtypes 0 through 7 select stages, `20h` selects sampler state when ORed into
the subtype, and `feh` is general texture state.

### Header 3: bounded 3D stream

```
d0 = FE03043c
d1 = payload dword count
d2 = type | (start & ffff)
d3 = CC000000 software flag
then d1 parameter words written to 0440
```

See `via_chrome9_3d_reg.h:428-452` and the fallback at
`via_chrome9_dma.c:880-895`.  Unlike header 2, header 3 carries an explicit
count.  The legacy verifier does not honor it and cannot be reused.

### Headers 4, 5, 6, and 7

The builders identify header 4 as a sequential video-MMIO payload, header 5 as
video register/value pairs, header 6 as debug, and header 7 as DMA
(`via_chrome9_3d_reg.h:454-504`).  The CPU fallback handles headers 4 and 5 but
does nothing for 6 and 7 (`via_chrome9_dma.c:825-858,898-902`).  The verifier
rejects 6/7 and weakly parses 4/5.  They are outside initial 3D allowlists.

### VX900 new branch packet (Header 4X)

For H5S2VP1, the KMD emits:

```
d0 = FE400000 | hierarchy | NEW_BRANCHLOC_SF[bit0] | optional_video[bit7]
d1 = command_size_dwords << 2       /* encoded byte count for aligned sizes */
d2 = SF GPU address & fffffff0
d3 = 00000000
```

The builder is at `via_chrome9_3d_reg.h:536-551`; the VX900 call site uses
first-level hierarchy and the branch-buffer SF address
(`via_chrome9_dma.c:1642-1687`).  Addresses are at least 16-byte aligned.
Although the macro comment describes the size as a 128-bit-unit field, the
call site passes a dword count and the macro shifts it by two, yielding bytes.
The comment places the field at packet bits `[59:36]`, i.e. `d1[27:4]`; thus
`units=d1>>4=size_dwords>>2` and each unit is 16 bytes.  `cmd_size` must
therefore be divisible by four dwords.  The KMD validates only nonzero and at
most 1 MiB/4 dwords and does not enforce that alignment
(`via_chrome9_dma.c:1705-1759`).  A nonmultiple sets reserved `d1[3:2]` through
the macro.  Treat aligned call-site behavior as authoritative and reject
unaligned or field-overflowing sizes before hardware.

The hierarchy encodings are first=`00`, second=`10`, third=`20`; location is
SL=`0`, SF=`1` (`via_chrome9_3d_reg.h:258-265`).  Only the first-level SF form
is demonstrated for VX900 here.

### VX900 new fence packet (Header 3X)

The four dwords encode:

```
d0 = fence header | mode flags | optional_video[bit7]
d1 = wait-engine mask | (fence_id[7:0] << 24)
d2 = fence_id[31:8] | (address[11:4] << 20)
d3 = address[31:12]
```

This is exact from `ADDCmdHeader3X_INVI()`
(`via_chrome9_3d_reg.h:520-534`).  Reassembly is:

```
id   = ((d2 & 00ffffff) << 8) | (d1 >> 24)
addr = (d3 << 12) | ((d2 >> 24) << 4)
```

Headers are 2D=`FE300000`, 3D=`FE330000`, HQV=`FE350000`, DMA=`FE370000`, and
VD=`FE3A0000`.  Wait masks are 2D=`00100000`, 3D=`00200000`, HQV=`000c0000`,
VD=`00020000`, DMA=`0001e000`, all=`003fe000`; mode flags are interrupt
generation=`2`, system write=`4`, interrupt wait=`10`, queue skip=`20`
(`via_chrome9_3d_reg.h:267-291`).

The header also defines unused “fence command follow” values in `d0[11:8]`:
3D=`1`, 2D=`2`, DMA=`3`, scaler=`4`, HQV=`5`, VD=`6`
(`via_chrome9_3d_reg.h:293-298`).  No call site in this KMD ORs them into a
packet, so their timing semantics are U and the new driver must keep the field
zero until another binary or hardware probe proves it.

The actual VX900 submission selects the 2D fence header and sets interrupt
generation, queue skip, system write, and interrupt wait, but sets the wait
mask to zero (`via_chrome9_dma.c:1669-1687`).  With both H5S2VP1 capabilities
enabled by `set_device_capablity()`, dword 0 is therefore `FE300036`.  This
proves the encoding used by
the shipping stack, but not that a zero wait mask is correct for every 3D
completion.  A first new-driver fence test should use a kernel-owned coherent
word, one branch packet, one fence packet, bounded IRQ/poll completion, and no
untrusted payload.

The Windows 7 KMD's production builder gives the missing workload-specific
policy (`VTGKModeDX32A.sys:0x000b755f..0x000b85f4`).  It starts normal fences
at mode `22` (interrupt generation plus queue skip), conditionally adds
interrupt-wait `10` and system-write `4`, and can suppress interrupt generation
under a separate adapter flag.  It selects the header and wait mask from the
submitted engines:

| submitted work | header | wait contribution |
|---|---:|---:|
| 3D | `FE330000` | `00200000` |
| 2D | `FE300000` | `00300000` (2D plus 3D) |
| DMA | `FE370000` | `0031e000` (DMA plus 2D/3D) |
| HQV | `FE350000` | `000c0000` |
| VD | `FE3A0000` | `00020000` |
| no recognized engine / wait sentinel | `FE300000` fallback | `003fe000` |

Contributions are ORed when a submission names multiple engines.  The 2D and
DMA values are deliberately composite dependencies, not alternate definitions
of the individual wait bits.  The same builder uses `d0[11:8]` to identify the
previous/following engine when that scheduler mode is enabled: 3D=`100`,
2D=`200`, DMA=`300`, HQV=`500`, VD=`600`
(`0x000b7639..0x000b77ad`).  Its final four stores at
`0x000b8540..0x000b85f4` independently confirm every ID and address split
shown above.  Therefore a persistent 3D submission should use `FE33` and at
least wait contribution `00200000`; the Linux `FE300036`/wait-zero form is a
legacy implementation fact, not the preferred 3D completion packet.

### Packet grammar audit matrix

This table separates what is known about packet framing from what is safe to
accept.  Every legacy builder aligns `d0` to 16 bytes; header 2 has no payload
count and is delimited only by the next recognized header, which makes complete
buffer pre-validation mandatory (`via_chrome9_3d_reg.h:375-551`,
`via_chrome9_verifier.c:633-978`).

| Signature | Bit-exact framing / destination | Confidence | Initial policy |
|---|---|---|---|
| `FE00_0000` | `d1` pair count, `d2=0`, `d3=CC00_0000`; then `{MMIO,value}` pairs | P/S | kernel-generated 2D allowlist only |
| `FE01_xxxx` | low bits of `d0` are builder `dwAddr` but ignored by fallback; `d1` data-dword count, `d2=0`, `d3=CC00_0000`; sequential host-BLT payload | S/U | reject; low field and destination extent are unknown/state-dependent |
| `FE02_xxxx` | low bits are `data_port+4`; `d1=data_port`, `d2=type/subtype/offset`, `d3=software flag`; payload until next header | P for CR dummy, D/S otherwise | only exact kernel templates; never user-delimit |
| `FE03_043c` | `d1` explicit payload-dword count, `d2=type|(start&ffff)`, `d3=software flag`; data to `0440` | S | reject until counted parser and no-draw probe agree |
| `FE04_xxxx` | sequential video MMIO: `d0` embeds start address, `d1` count, `d2=id`, `d3=0` | S | privileged/reject |
| `FE05_0000` | video `{register,value}` pairs; `d1` QW count, `d2=id`, `d3=0` | S | privileged/reject |
| `FE06_0000` | four-dword DEBUG header, all remaining words zero | S | reject |
| `FE07_0000` | DMA header; `d1` QW count, `d2=id`, `d3=0` | S/U | reject; no recovered side-effect model |
| `FE09`, `FE0A` | recognized/skipped by verifier, no builder or semantics recovered here | U | reject |
| `FE82_xxxx` | legacy branch uses header-2-like `d0=(addr+4)`, `d1=addr`, `d2=type`, `d3=CC00_0000` | S | reject on VX900 |
| `FE40_....` | `d0[5:4]` hierarchy (`00/01/10` = first/second/third), `d0.bit0` location (`0` SL, `1` SF), `d0.bit7` video-related; `d1[27:4]=size/16B` (macro is `size_dw<<2`), `d2=A[31:4]<<4`, `d3=0` | S; first-level SF used by vendor | kernel branch only, aligned size, one level initially |
| `FE30/33/35/37/3A` | new fence class in `d0[23:16]`; flags `d0[5]=queue-skip`, `[4]=interrupt-wait`, `[2]=system-write`, `[1]=interrupt`; ID split across `d1[31:24]` and `d2[23:0]`; address split `d2[31:24]=A[11:4]`, `d3=A[31:12]` | S | kernel-generated only |

`FE41/43/45/47/4A`, `FE20`, `FE25`, `FEE2`, and `FEE3` are constants in the
header but lack a recovered VX900 builder/consumer in this KMD
(`via_chrome9_3d_reg.h:69-81`).  They are unknown opcodes, not variants that a
parser may accept by masking loosely.  The old `is_agp_header()` omits all new
branch and fence forms (`via_chrome9_3d_reg.h:356-373`), so its delimiter logic
cannot safely parse a mixed VX900 stream.

The H3 verifier is also not a shader validator.  Despite H3 carrying an
explicit count in dword 1, `via_chrome9_check_header3()` advances four dwords
at a time until the next header-looking value and never reads that count or
examines a native instruction (`via_chrome9_verifier.c:801-812`).  The CPU
fallback does use the count, but only to copy that many opaque dwords to
`0440` (`via_chrome9_dma.c:880-895`).  The proprietary user-mode VS/PS
"validate" routines perform resource remapping/allocation and return success;
they do not validate native opcodes, branch targets, nesting, or length.  A new
DRM parser must bound H3 exactly and must not accept arbitrary user-supplied
native code on the strength of this legacy verifier.

## Branch buffers and completion

The old driver allocates sixteen 1 MiB branch buffers, each 256-byte aligned
(`via_chrome9_mm.h:30-40`, `via_chrome9_mm.c:495-504`).  On PCIe they are CPU
addresses inside the SG allocation and GPU addresses in the SF heap after the
ring (`via_chrome9_mm.c:343-389`).

VX900 submission is:

1. validate only `0 < cmd_size <= 1 MiB / 4` dwords;
2. assign a monotonically increasing 31-bit fence ID;
3. flush payload cache lines;
4. append one FE40 first-level SF branch packet to the main ring;
5. append one FE30 fence packet;
6. append FIFO padding and advance PAUSE.

The code is at `via_chrome9_dma.c:1705-1836`; fence-ID generation is
`via_chrome9_drm.h:436-442`.  The branch buffer is returned to the free pool in
an interrupt-scheduled worker after the readback ID passes its fence ID
(`via_chrome9_mm.c:69-204`).

H5S2VP1 capabilities enable system-memory fence write and interrupt-wait, with
`0494` retained as the fallback/current-fence readback register
(`via_chrome9_mm.c:395-423`).  The IRQ status is MMIO `0204.bit5`; VX900 clears
it by writing one through an old-status OR mask (`via_chrome9_drm.c:1040-1086`).
Global graphics interrupt enable is `0200.bit31`; branch initialization writes
only that bit and therefore disables the other listed interrupt enables
(`via_chrome9_mm.c:515-530`, `via_chrome9_3d_reg.h:300-340`).

`CAPS_FENCE_INT_WAIT` is described as pausing CR until the CR interrupt is
cleared (`via_chrome9_drv.h:52-60`).  Consequently an IRQ test must install the
handler and enable/ack path before submitting an interrupt-wait fence.  Losing
or masking the IRQ can stop the command reader.

Completion comparison is not wrap safe: it tests `submitted <= readback`, but
IDs wrap from `7fffffff` to zero (`via_chrome9_mm.c:147-150`,
`via_chrome9_drm.h:436-442`).  Use DRM sequence-number helpers or signed modular
comparison.  Also add DMA read barriers before consuming hardware-written
fence memory.

### Fence and interrupt bit matrix

| Interface | Bits / operation | Confidence | Safe implication |
|---|---|---|---|
| traditional T11 A03 wait | `[23]` 3D, `[22]` 2D, `[21]` VD, `[20]` HQV0, `[19]` HQV1, `[18:15]` DMA3..0, `[14:13]` HQV0/1 EOF, `[8:4]` idle count, `[0]` all | D | do not mix with command-switch mode |
| traditional T11 A07 mode | mode `[19:16]`: 0 record; 1 record after all-idle; 2 IRQ+queue; 3 IRQ+queue after idle; 4 system write; 5 system write after idle; 6 system write+IRQ+queue; 7 same after idle | D/S | `[20]` triggers; `[21]` can pause CR until IRQ ack; `[22]` overwrites/skips full queue |
| new FE3x engine class | `FE30` 2D, `FE33` 3D, `FE35` HQV, `FE37` DMA, `FE3A` VD | S | use the class matching the submitted workload; vendor's VX900 path always uses 2D despite arbitrary branch payload |
| new FE3x wait | `d1[21]` 3D, `[20]` 2D, `[19:18]` HQV, `[17]` VD, `[16:13]` DMA; `003fe000` means all listed engines | S | Linux vendor path sets zero; Windows uses `00200000` for 3D and composite `00300000`/`0031e000` for 2D/DMA dependencies |
| new FE3x mode | `d0[5]` queue skip, `[4]` interrupt wait, `[2]` system write, `[1]` interrupt generate | S | stage each side effect independently before combining |
| IRQ global | MMIO `0200[31]` global interrupt enable | D/S | preserve only documented enable fields with `(old & f7ff0800) | 80000000`; a literal RMW echoes mixed RW1C status, while the KMD's `80000000` destroys display/video enables |
| IRQ status/ack | MMIO `0204[5]` CR interrupt, RW1C | D/S | ack only the sampled RW1C bit; do not OR unclassified status bits |
| fence readback | MMIO `0494` for H6S1/H6S2/H5S2VP1 | S | fallback/debug observation, not enough to prove memory visibility |
| interrupt wait | T11 A07 bit21 or new FE3x bit4 | D/S | handler and ack must be live first; lost IRQ otherwise leaves CR paused |

The KMD's IRQ acknowledgement writes `interrupt_status2 | 0x20`
(`via_chrome9_drm.c:1048-1058`).  The manual documents all other `0204` bits as
RO, so that happens to be benign on VX900, but the robust pattern is still to
write only `0x20` after confirming it was asserted (Part II, internal p.11).
The global-enable bug is more serious: branch setup writes `0200=80000000`
instead of preserving other enable fields (`via_chrome9_dma.c:198-217`,
`via_chrome9_mm.c:515-530`).  A naive read/OR/write is also wrong because
`0200` interleaves enables with RW1C status.  VX900's documented enable mask is
`f7ff0800` and its RW1C mask is `0800f3fd`; write
`(old & f7ff0800) | 80000000`, not `old | 80000000`.  See
`chrome9-fe3x-completion.md` for the serialized KMS/render ownership rule.

Windows confirms the IRQ ordering and the VX900-specific acknowledgement.
Its ISR at `0x00077a60` samples `0200`, `0204`, `1280`, and `c500`; on
entry it rejects the interrupt if `0200==ffffffff` or `0200.bit31` is clear.
Thus bit 31 is an enforced global gate in the live handler.  On `0204.bit5`
it calls `0x00077830`.  That helper first invokes chip callback
slot `+e08`, then processes current fence IDs through `0x00076ad0`, and only
then requests the DPC when progress was found.  Device selection maps PCI
`7122` to callback-table index 4 (`0x0007806b..0x000780e9`); table slot 11 is
`0x000c8360`, exactly the `+e08` slot.  That function reads `0204`, ORs `20`,
and writes it back when adapter flag `+9c4.bit0` is set
(`0x000c8360..0x000c83ac`).  Thus the vendor order is **sample, acknowledge,
read/process fence, schedule DPC**.  A new ISR should use the narrower RW1C
write `0204=20`, preserve unrelated `0200` enables, and publish/consume the
fence word with DMA ordering before waking waiters.

Bring up FE30 in four bounded stages: (1) a system-write-only fence to a
coherent, 16-byte-aligned word, polling memory plus `0494`; (2) interrupt
generation without interrupt-wait, checking `0204[5]` and handler accounting;
(3) repeat with the IRQ intentionally masked but still **without** wait to prove
timeout/error reporting; (4) only after those pass, enable interrupt-wait.  Use
a distinct sentinel and DMA barriers at each stage, preserve MMIO `0200`, and
leave no outstanding fence before advancing.

For a 3D job, the exact staged dwords are:

```text
d0 = FE330004                         polling system-write-only
     FE330006                         + interrupt, no interrupt-wait
     FE330016                         + interrupt-wait after IRQ/ack proof
     FE330036                         Windows-normal + queue-skip
d1 = 00200000 | ((id & 000000ff) << 24)
d2 = ((id >> 8) & 00ffffff) | ((addr & 00000ff0) << 20)
d3 = (addr >> 12) & 000fffff
```

Only one fence may be outstanding in the first three stages, so queue-skip is
unnecessary there.  Keep follow-tag `d0[11:8]=0` until its scheduling effect is
independently needed.  Initialize the coherent word to a value different from
`id`, flush any stale `0204.bit5` before unmasking, and require the memory ID,
`0494`, and engine-idle observation to agree under a deadline.  The production
choice after the staged proof is `FE330036` with the same `00200000` wait mask;
never enable interrupt-wait unless `0200.bit31`, ISR acknowledgement, and DPC
wakeup have already passed independently.

Because FE30 carries only `A[31:4]` in the recovered macro while the traditional
fence registers document up to 44 address bits, the first coherent fence word
must be allocated under an explicit 32-bit DMA mask and checked for
`dma_addr >> 32 == 0`.  Passing a normal 64-bit kernel virtual/physical address
as the old KMD does with `virt_to_phys()` is unsafe on an IOMMU or above 4 GiB
(`via_chrome9_mm.c:333-386`, `via_chrome9_3d_reg.h:520-534`).

## 3D engine default-state oracle

`Initialize3DEngine()` contains an H6S2/H5S2VP1 state sweep and default table
load (`via_chrome9_drm.c:82-565`).  Nothing in this source calls the function.
It is therefore valuable as a list of legal state ranges and default values,
but it is not proven to be a required kernel initialization sequence.  The UMD
or Xorg driver likely owns this setup.

The VX900 branch writes through `043c/0440`:

| Type/subtype | Initialized indexes or payload | Source |
|---|---|---|
| Attr `01/00` | indexes `00..9a`, value `index << 24` | `329-335` |
| texture stage `02/0..f` | each `00..30`, value `index << 24` | `337-347` |
| texture sampler `02/20..2f` | each `00..36`, value `index << 24` | `349-359` |
| texture general `02/fe` | `00..13`, value `index << 24` | `361-368` |
| palette/table `03/15` | five packed de-gamma dwords | `378-391` |
| palette/table `03/17` | packed gamma table | `393-457` |
| texture palette `03/00` | sixteen zero dwords | `479-485` |
| 4x4 coefficient `03/11` | thirty-two zero dwords | `487-494` |
| stipple palette `03/14` | eight zero dwords for five entries plus pad | `496-503` |
| FVF `04/00` | indexes `00..62`, value `index << 24` | `505-511` |
| config `fe/00` | indexes `00..47`, value `index << 24` | `513-519` |
| CR misc `11/00` via 3D port | indexes `00..20`, value `index << 24` | `520-527` |

It then overrides config indexes 40=`0000840f`, 47=`00000404`, 44=`0`, and
46=`5`, followed by config values 00=`1004`, 08=`249`, 0a=`2c9`, 0b=`2fb`,
0c=`0`, 0d=`2cb`, 0e=`9`, 10=`49`, 11=`2ff`, 12=`8`, 13=`2db`
(`via_chrome9_drm.c:528-563`).  Values include the index in bits 31:24.

The comments say palette/table write-through state cannot be partially updated,
must be submitted as AGP commands, and must be padded to multiples of four
128-bit entries (`via_chrome9_drm.c:460-477`).  The function itself uses MMIO,
so this is internally inconsistent.  For live bring-up, submit full table
blocks through the working ring and do not infer that partial MMIO writes are
safe.

There is a confirmed de-gamma typo at `via_chrome9_drm.c:389-391`: the final
dword ORs both `119 << 20` and `884 << 20`, overlapping the same field.  The
unstripped DRI builder `stmFullUpdateHardware_invi:0x177bf9..0x177c3d`
emits the authoritative five dwords
`41e03c05,07714434,11b36ca5,2176e565,374bd680`; the last word is solely
`(884 << 20) | (757 << 10) | 640`.  Never copy the extra `119` from the old
KMD source.  The packet-exact sRGB sampler oracle is in
[fixed-function-texture-sync.md](fixed-function-texture-sync.md).

## Suspend, resume, reset, and 2D/3D interaction

Resume restores PCI D0/config/BME, repeats the CR `06000000`, `07100000`, and
readback-group setup, then restores sequencer registers `SR66..SR6F` and
`SR79..SR7B` (`via_chrome9_drm.c:567-621`).  Suspend saves those thirteen
registers, disables both display channels, blanks both screens, and clears
IGA FIFO-depth fields to avoid a documented “PCIE series S3 hang”
(`via_chrome9_drm.c:624-710`).

This is not a complete modern PM sequence:

- it does not wait for jobs or stop the ring;
- it does not rebuild PTEs, ring contents, or fence state explicitly;
- it programs CR before restoring the translation registers;
- it does not invoke the 3D default-state function;
- it does not restore the display values it destructively changed.

The register header defines CR restore/clear bits but the source never uses
them.  There is no hangcheck or reset routine.  Reset scope, clock ordering,
and post-reset state remain release-blocking unknowns.

The Windows KMD supplies a recovery oracle, but not a sequence safe to copy.
Its helper at analysis `0x0008a500` tests
`0400 & 00161fff`, disables interrupts with `0200=0`, writes `0f` then `00` to
`SR4E`, pulses `SR1A.bit6` (documented PCI-bus software reset), and on 7122
also pulses `SRC7.bit7`.  It then polls `0400 & 00101ff3`, optionally writes
`0204=20` on revision 0/1 of 7122 or a122 while busy, and finally calls the
large reinitialization function at `0x000896b0`.  The delays around every
sequencer pulse are software loops of exactly `10000h` iterations, not a
documented duration.  The idle loop after reset has no timeout.

`0x000896b0..0x0008a4f5` is now bounded precisely.  It zeros MMIO dwords
`0000..0038` and `1e00..1fe0`, then writes a full transfer-port default-state
sweep through `043c/0440`: chip-specific preamble, the attribute-register
range, all 16 texture-stage banks, palettes/tables, FVF/config ranges, and
final CR-port priming
`041c=00100000; 0420=680a0000`, then
`041c=00100000; 0420=02000000`.  It is the Windows analogue of the old Linux
`Initialize3DEngine()`, not a restoration of the pre-hang context.  It does
not restore `0200`, RTSF PTEs, translation registers, ring bounds/content, or
outstanding fence IDs.  Several facts prevent reducing
this to “toggle SR4E”:

- `SR4E=0f` does not select the documented 3D field `[5:4]`; it affects the
  2D/video fields and includes encodings not independently validated here;
- the PCI-bus and 7122-specific reset pulses have unknown collateral effects
  on display, GART, CR, and memory-controller state;
- `0200=0` destroys all graphics/display interrupt enables rather than masking
  only CR;
- completion depends on destructive 2D/video clearing plus a substantial 3D
  state replay, not merely waiting for idle;
- `0200` remains zero on return from these two helpers, so the caller must
  rebuild interrupt state before any interrupt-wait fence;
- the reset discards submitted work and hardware fence state, requiring the
  software scheduler to error all affected fences and start a new sequence
  epoch.

Windows defers that IRQ rebuild rather than performing it inside the reset.
After `0x000896b0`, `0x0008a500` sets adapter byte `+22f01=1`
(`0x0008a769..0x0008a77c`).  The next submit sees this byte, read-modify-writes
`0200 |= 80a00000`, invokes an additional setup helper, and clears the byte
(`0x00083dbb..0x00083e0b`).  The old header decodes that mask as global enable
`80000000` plus DMA1 `00800000` and DMA0 `00200000`; there is no separate CR
enable bit in it.  Linux does not need to enable DMA interrupts for a CR fence,
but it must restore global bit 31 with a preserving RMW before submitting any
interrupting fence.

The setup helper in that deferred path is now decoded too.  For PCI `7122`,
device callback `+0xdf4` is `0x000a17a0`; it resets the software ring
descriptor to `0x100000` dwords (4 MiB), CPU base/cursor from adapter
`+22ec0`, physical/SF base from `+22eb8`, then calls `0x000c7c00`.  That
helper pads the head to 128 bytes and writes T10 A60--A68 directly through
`041c/0420`:

```text
A60 = (P          & 00fffff0) | 1   # SF location
A61 =  P          >> 24
A62 = (P+00400000) & 00ffffff
A63 = (P+00400000) >> 24
A64 = (P+head_pad-10) & 00ffffff    # pause = translated cursor - 16
A65 = (P+head_pad-10) >> 24
A66 = 00ffffff; A67 = 00ffffff      # split fffffff0 jump sentinel
A68 = 000a0001                      # read threshold 0a, trigger
```

For this chip family the head padding begins with the aligned dummy
`FE020420,0000041c,00300000,CC000000` and repeats
`CCCCCCC0,DDD00000` to a 128-byte boundary.  The exact Windows recovery order
is therefore IRQ-enable RMW, complete CR ring reconstruction/restart, clear
the deferred flag, then ordinary submission.  The default-state sweep alone
does not restart command fetching.

A Linux hang recovery path must quiesce display access, save the minimal
coexistence state, apply the reset pulses, replay the now-recovered default
3D sweep, then independently rebuild translation, ring, and interrupt state.
Until that complete sequence and bounded timeouts are implemented, controlled
reboot remains safer than a partial reset for a VP/CR wedge.

The common engine status and queue mean 2D, 3D, and CR cannot be treated as
independent for synchronization.  Header 0 and header 1 are consumed by the
same command reader, header 2 targets CR or 3D ports, and the new fence packet
has per-engine wait bits (`via_chrome9_3d_reg.h:281-298`).  A safe progression
is: prove header-0 latch writes, prove an off-screen 2D fill, then branch to a
no-draw 3D state block, then add an engine-specific fence.  Save/restore every
observable latch and keep scanout ranges out of all tests.

### Context, reset, arbitration, and clock matrix

| Mechanism | Bit-exact behavior | Confidence | Driver rule |
|---|---|---|---|
| 3D context save address | T11 A08 `[23:4]=A[23:4]`, `[1:0]` location; A09 `[7:0]=A[31:24]`; 128-bit alignment | D/S | Windows `7122` uses SL and one `0x7000`-byte slot |
| store written 3D registers | T11 A09 `[8]=1` | D/S | Windows sets it for every old-context save |
| safe save trigger | T11 A0B `[0]=1`; manual says it saves 3D writeback registers then CR continues and has priority over A09 bit8 | D/S | Windows emits `CE000000,0B000001` after A08/A09 |
| clock control | `SR3F[7:6]` CR, `[5:4]` 3D, `[3:2]` 2D, `[1:0]` DVD; each `0x=off`, `10=always on`, `11=idle-gated`; reset default `AAh` | D | snapshot/preserve; never gate an active engine |
| transfer-port clock wake | CR programming through `041c/0420` does not enable 3D clock; identical programming through `043c/0440` does | D | use the 3D port for 3D state; CR dummy does not prove 3D clock/state |
| software reset | `SR4E[5:4]` 3D and `[3:2]` 2D: `01` engine reset, `10` register reset; `[1:0]` HQV/DVD/capture same; `[7:6]` CR reset is documented RO | D/U | no CR software reset is recovered; do not pulse until assertion/deassertion timing is known |
| CR recovery controls | T10 A68 `[4]` clear fence queue, `[3]` clear command cycle, `[2]` restore command cycle, `[1]` restore 3D-register cycle, `[0]` trigger | D/U | not a complete reset; exact prerequisites/state source unknown |
| 2D/3D mutual exclusion | T11 A00 `[17]=1` hardware locks engines; `[16]=0` prevents delivery to the other busy engine, `1` permits delivery but blocks its memory access | D/U | sample or initialize deliberately before mixed workloads |
| sync latch | 2D header-0 write `006c=00400000` before and after H6 operations in Xorg | S/P | retain at 2D/3D boundaries until field semantics are decoded |
| status | `0400[4]` CR busy, `[1]` 2D busy, 3D mask `00001fe1`; combined vendor mask `00001ff3` | S/P; armed PAUSE with bit 4 only is now live-observed | use non-CR `00101fe3` for PAUSE-ready, full `00101ff3` after STOP; report raw status, `0444`, `043c`, `045c`, `0460`, `0464` on timeout |

The KMD performs no context switching.  Suspend stores only sequencer
`SR66..6F` and `SR79..7B`, destructively disables both displays, and does not
save the main ring, FE/PE/PS state, fence queue, T11 state, or clock/reset state
(`via_chrome9_drm.c:567-710`).  `Initialize3DEngine()` is dead code.  Therefore
neither KMD suspend/resume nor that state sweep is evidence for a complete
context image.

The Windows binary now bounds a context slot even though the manual omits the
write length.  `7122`/`a122` reserve `0x70000` bytes of local framebuffer for
16 slots of `0x7000` bytes, aligned down to 32 bytes.  The old-context packet
at `0x000b6aab..0x000b6caf` first writes type-04 `A04..A07=0` (FVF vertex
declarations D3..D6), then selects T11 and emits A08/A09 for
`C + old_index*0x7000`, with SL location, A09 bit 8 set, dummy-class
`CE000000`, then safe trigger `0B000001`.  It follows the trigger with an
aligned CR-port type-30 header and four repetitions of
`CC000000,CD000000,CE000000,CF000000`.

Restore is a traditional branch, not the T10 A68 restore-register bit.  At
`0x000b6e40..0x000b70cf` the new-context packet selects T11 and emits
`68000001`, A69/A6A for `C + new_index*0x7000 + 0x1b0`, and `6B00045E`, then
an aligned `FE820420,0000041c,00300000,CC000000` trigger.  It then emits an
ordinary aligned type-30 CR header and the same four `CC/CD/CE/CF` quads.  The
branch length
is exactly `0x45e * 16 = 0x45e0` bytes, covering slot offsets
`0x1b0..0x478f`.  The first `0x1b0` bytes are hardware-save
metadata/preamble whose internal layout remains unknown.  Although the manual
says the safe trigger saves all 3D writeback registers, the generated replay
format has not yet proved whether/how H3 VS/PS program stores are represented;
do not replace the hardware image with a guessed partial shader restore.  A later isolated
experiment must still use a guard-padded non-scanout allocation and pre- and
post-fences; no production UAPI should expose these raw controls.

For reset, begin read-only: record `SR3F`, `SR4E`, T11 A00 if a reliable
readback route is proven, and all engine status registers across boot, first
2D, first 3D-state packet, and idle.  A first write probe may pulse **2D engine
reset only** after display exclusion, idle, state snapshot, and an already
working 2D reinitialization path.  Do not test 3D reset until the minimal 3D
init can be replayed, and do not test CR reset—the manual marks its field RO and
the board has no software recovery from a stopped fetcher.

## Verifier-derived address fields

The verifier tables are useful as an index map even though the verifier is
unsafe.  Commands are indexed by their top byte.

For Attr type `01`, the address-related indexes are
(`via_chrome9_verifier.c:105-207`):

| Index | Inferred field |
|---:|---|
| `10`, `11`, `12` | Z base part 0, Z base part 1/location, Z/stencil mode |
| `21`, `22` | Z-occlusion base parts |
| `29`, `2a` | coarse-Z base parts |
| `50/51/52` | render target 0 base/mode/base-high-or-pitch |
| `58/59/5a` | render target 1 equivalent |
| `60/61/62` | render target 2 equivalent |
| `68/69/6a` | render target 3 equivalent |

For texture-stage type `02`, index `18` contains six two-bit memory-location
fields checked against system-memory value 2 (`via_chrome9_verifier.c:209-232`,
`500-512`).  For FVF type `04`, index `00` has a two-bit memory-location field
at bits `16:15` (`via_chrome9_verifier.c:234-306`, `491-499`).

For CR type `11`, the older/AGP table treats indexes `01`, `02`, `03` as fence
address/trigger components; the PCIe table uses `04`, `05`, and `07`
(`via_chrome9_verifier.c:307-373`).  These correspond to the older H5/H6 fence
parameter encodings, not VX900's FE30 packet.

The hazard logic exposes these location fields
(`via_chrome9_verifier.c:434-543`):

- render-target mode bits `21:20`, system value 2;
- Z location bits `1:0`, system value 2;
- stencil location bits `6:5`, system value 2;
- Z-occlusion location bits `23:22`, system value 2;
- coarse-Z location bits `21:20`, system value 2;
- FVF location bits `16:15`, system value 2;
- texture index-18 locations at bits `1:0`, `3:2`, `5:4`, `7:6`, `15:14`,
  and `17:16`, each with system value 2.

### Address and memory-domain matrix

The manual consistently assigns the two-bit location code `00=SL`, `01=SF`,
`10=SM`, `11=LL` to CR ring, command queue, save buffer, and display-swap
addresses (Part II, internal pp.77,79,83-84).  The verifier independently
treats value `2` as dangerous direct system memory.  SL is local framebuffer;
SF is the translated system-framebuffer/GART space and is live-proven for the
ring; SM is raw system memory.  The exact transport/coherency semantics of LL
remain unknown.  These codes appear global, but only SL for 2D destinations and
SF for CR fetch have live proof.

| Parameter/index | Exact address/location extraction in old verifier | Confidence | Missing validation |
|---|---|---|---|
| Attr `01`, A10/A11/A12 | A10 starts Z sequence; A11 location=`word[1:0]`; A12 stencil location=`word[6:5]` | S; domain code D | address assembly, pitch, tiling, size |
| Attr `01`, A21/A22 | Z-occlusion sequence; A22 location=`word[23:22]` | S | address assembly and extent |
| Attr `01`, A29/A2A | coarse-Z location=`A29[21:20]`; A2A is remaining address state | S | bit split and extent |
| Attr `01`, RT0 A50/A51/A52 | low/base accumulator=`A50_word<<8`; location=`A51[21:20]`; verifier stores `pitch=(A52&0x1ff)>>5` | S/U | high address, units, tiling, compression, extent |
| RT1/2/3 | identical index triplets `58/59/5A`, `60/61/62`, `68/69/6A` | S/U | same as RT0 |
| Tex `02`, A18 | six locations at `[1:0]`, `[3:2]`, `[5:4]`, `[7:6]`, `[15:14]`, `[17:16]`, apparently cube faces | S/I | face/base address registers and mip extents |
| FVF `04`, A00 | vertex/FVF source location=`word[16:15]` | S | stream addresses, strides, vertex count bounds |
| traditional fence, old table | T11 A01 low 24 bits and A03 low 8 bits assemble 32-bit address; A03 bit18 (`0x40000`) means system write | S, older generation | not the VX900 layout |
| traditional fence, PCIe table | T11 A04 low byte is address `[31:24]`; A05 low 24 bits are `[23:0]`; A07 bit18 selects system write | D/S | KMD's parser truncates manual 44-bit address to 32 bits |
| FE30 fence | `A[11:4]=d2[31:24]`, `A[31:12]=d3[19:0]`, low 4 implicit zero | S | macro and API use 32-bit physical address despite manual 44-bit traditional fence path |
| FE40 branch | `A[31:4]=d2[31:4]`, low 4 implicit zero; `d0.bit0` SL/SF only in source | S | no SM/LL encoding in new packet recovered |
| T10 preemption | 44-bit raw address split A08 `[19:0]=A[43:24]`, A09 `[23:4]=A[23:4]` | D | completely unverified/privileged |
| T11 save buffer | 32-bit address split A08/A09 plus location in A08 `[1:0]` | D | allocation size and written layout unknown |
| T11 auto-swap | display bases begin at A10/A11 with location in A10 `[1:0]` | D | display-owned and must never be render-accessible |

The old verifier does not reconstruct or bound any render target, Z, texture,
or vertex address.  Its RT “pitch” extraction drops bits `[4:0]` after masking
only nine low bits and is not sufficient evidence for a real pitch formula
(`via_chrome9_verifier.c:434-512`).  Treat every semantic label in this table as
an index map for reverse engineering, not a submission ABI.  Safe validation
must compute complete byte ranges from format, dimensions, mip level, tiling,
compression, multisample state, and pitch, then prove those ranges belong to
the submitting VM.

The tables' nominal allowlists are:

- Attr: padding `cc/cd/ce/cf/dd/ee`; `00,01`; `10..2a`; `33..3f`;
  `50..53`, `58..5b`, `60..63`, `68..6b`; `70..78`; `80..8a`;
  `90..9b`; `aa`.
- Texture stage: padding plus `00..05`, `18`, `20..22`, `30`, `50..51`,
  `9b`.
- FVF: padding plus `00..05`, `08..2f`, `40..48`, `50..52`, `60..62`,
  `9b`, `aa`.
- CR/older AGP: padding plus `00..03`, `10..14`, `18..1c`, `20`, `aa`, `ab`.
- CR/PCIe: padding plus `00..12`, `18..1a`, `30..32`, `68..6b`, `aa`, `ab`.

These are discovery hints, not a VX900 allowlist.  Unknown/default header-2
types, including Cfg and PreCR, are skipped without inspection
(`via_chrome9_verifier.c:751-780`).

## Verifier and source security defects

The following must be treated as explicit “do not copy” findings.

1. **VX900 branch payloads are completely unverified.**  The H5S2VP1 branch
   path skips `via_chrome9_verify_command_stream()` by design
   (`via_chrome9_dma.c:1750-1759`).  This exposes arbitrary GPU commands.
2. **The nominal forbidden-index mechanism is broken.**  Unlisted indexes are
   initialized to `forbidden_command` (`via_chrome9_verifier.c:981-992`), but
   `investigate_hazard()` has no forbidden case and its default returns success
   (`via_chrome9_verifier.c:544-547`).
3. **Only fence addresses are range checked.**  Render target, Z, coarse-Z,
   occlusion, FVF, and texture sequences are collected or location-filtered but
   never checked against owned allocations.  The source admits that AGP access
   is considered safe (`via_chrome9_verifier.c:398-417`).
4. **Header 3 ignores its declared count and all payload semantics.**  It steps
   four dwords at a time until a header-like value appears
   (`via_chrome9_verifier.c:800-812`).
5. **Header 2 default types are unchecked.**  Cfg, PreCR, palette, vertex data,
   sampler, and unknown types are advanced until the next header with no field
   checks (`via_chrome9_verifier.c:751-780`).
6. **Header 9/A payloads are unchecked; headers 6/7 are merely rejected.**
   (`via_chrome9_verifier.c:928-939`).
7. **Truncated terminal headers can pass.**  The state machine consumes a loop
   iteration only to classify a header; if the buffer ends there, it exits
   without running that header's length checker and accepts any non-error state
   (`via_chrome9_verifier.c:902-977`).  Header-0/1 parsers also read the count
   before first proving four header dwords exist (`633-683`).
8. **New VX900 FE30/FE40 packets are outside `is_agp_header()`.**  The helper
   recognizes only old headers 0-7/9/A (`via_chrome9_3d_reg.h:356-373`).
9. **No relocations or ownership checks exist.**  Userspace supplies final GPU
   and physical-looking addresses directly.
10. **The PCIe-memory-control ioctl has no offset/size bounds checks.**  It adds
    user-controlled values to the kernel SG base and copies or memsets there
    (`via_chrome9_mm.c:850-885`).
11. **Branch cache flush has an off-by-one condition.**  It flushes only when
    `sg_entry_idx + sg_page_num < pages`, so a valid range ending exactly at
    the allocation boundary is not flushed; if the check fails it submits
    anyway rather than rejecting (`via_chrome9_dma.c:1815-1829`).
12. **All important hardware loops lack timeouts.**  GTI-cache flush and ring
    head/tail waits can wedge a CPU indefinitely (`via_chrome9_dma.c:142-150`,
    `1239-1339`).
13. **Fence completion is wrap-unsafe and lacks DMA barriers.**  It uses a
    plain cached load and numeric `<=` (`via_chrome9_mm.c:120-150`).
14. **The global branch-buffer pools are file-scope singleton arrays.**  They
    are not per-device (`via_chrome9_mm.c:38-39`).
15. **Several UAPI paths are effectively unauthenticated.**  The driver marks
    every open authenticated (`via_chrome9_drv.c:204-209`), and DMA init,
    branch query/request/flush, cache flush, and direct PCI config reads have
    zero ioctl flags (`via_chrome9_drv.c:146-190`).
16. **The 32-bit compatibility init leaves fields uncopied.**  For example,
    `front_offset`, `depth_offset`, and several handles never reach the native
    struct (`via_chrome9_ioc32.c:43-110`).  This code was not 64-bit clean.
17. **GART base bit 28 is ORed instead of replaced** as described above.
18. **Initialization error handling and allocation checks are weak.**  One path
    dereferences `event_tag_info` with `memset` before checking it for NULL
    (`via_chrome9_dma.c:700-704`), and cleanup does not first prove the reader
    has stopped (`via_chrome9_drm.c:835-895`).
19. **Cleanup can leave live translations into freed memory.**  It frees the SG
    pages and unmaps the page table without stopping CR, invalidating the PTEs,
    flushing GTI, or restoring the prior `SR6A/B/C/7B` values
    (`via_chrome9_drm.c:835-895`).  A paused reader may already have prefetched
    beyond PAUSE, so merely observing `0444[31]` is not enough before teardown.
20. **VX900 branch length alignment is unchecked.**  FE40 encodes 16-byte
    units in `d1[27:4]`, but the macro shifts an arbitrary dword count by two
    and the ioctl validates only range.  A count not divisible by four sets
    reserved `d1[3:2]` (`via_chrome9_3d_reg.h:536-551`,
    `via_chrome9_dma.c:1705-1759`).
21. **Header-0 validation is an address mask, not an allowlist.**  It accepts
    every register word with `(word & 0xfe00)==0`, including unaligned offsets
    and every address through `01ff`; values and state-dependent memory effects
    are unrestricted (`via_chrome9_verifier.c:633-661`).
22. **Header 1 never validates its embedded `dwAddr` field.**  The parser skips
    the four-dword header and trusts only the count, so an arbitrary low header
    field plus prior destination state reaches hardware even though the CPU
    fallback ignores that field
    (`via_chrome9_verifier.c:664-683`).
23. **Video-MMIO validation is a broad deny-range filter.**  It permits all
    offsets outside five gaps, including display/video control regions, and
    does not validate values or ownership (`via_chrome9_verifier.c:551-575,
    815-883`).  Such packets cannot be exposed to a render client.
24. **Count arithmetic can wrap before bounds checks.**  Header 0 calculates
    `dword=qword<<1`, header 5 does `data<<1`, and header 1 forms `buf+dword`
    from attacker counts (`via_chrome9_verifier.c:639-643,670-674,862-880`).
25. **Verifier state is device-global, not per-job.**  Each invocation mutates
    `dev_priv->hc_state` and preserves the mutations on success, without a lock
    in this code (`via_chrome9_verifier.c:885-977`).  Concurrent submitters can
    race sequence/address tracking even if the individual parser bugs were
    repaired.
26. **The DMA cleanup ioctl does not clean up DMA.**  It only clears
    `SR6C.bit7`, which the VX900 manual calls table write protection, and
    returns without stopping CR, flushing GTI, or invalidating PTEs
    (`via_chrome9_drv.c:116-141`).
27. **Branch-buffer ownership is only a global numeric ID.**  Request moves any
    free global record to the allocated list, and flush removes a record by ID
    without recording or checking `drm_file`; a different client can consume
    it (`via_chrome9_mm.c:69-157,888-943`,
    `via_chrome9_dma.c:1728-1737`).
28. **The flush path has a cross-call static pointer.**  `p_kmd` is a
    function-static local even though concurrent/device state is otherwise
    expected, adding another race across clients or devices
    (`via_chrome9_dma.c:1705-1737`).
29. **PCIe heap offsets are word-size dependent.**  After `dmasize` is converted
    from bytes to dwords, allocation and PCIEMEMCTRL paths multiply it by
    `sizeof(unsigned long)` instead of four.  On a 64-bit kernel they place the
    userspace-visible heap base twice as far as intended
    (`via_chrome9_dma.c:726`, `via_chrome9_mm.c:784-799,850-880`).
30. **The CPU/BCI decoder is not memory safe for malformed streams.**  It reads
    header/count words and executes count-driven loops without proving the
    remaining buffer length; its header-2 loop dereferences
    `pcmddata[swpointer]` before checking `swpointer < swpointerend`
    (`via_chrome9_dma.c:820-905`).  It is only a reverse-engineering oracle,
    never a fallback for untrusted jobs.

The new submission ABI therefore needs kernel-owned command buffers, strict
packet bounds, per-opcode allowlists, BO-handle relocations or a validated VM,
ownership checks for every address range, and rejection of all privileged CR,
display, DMA, and VGA state from render clients.

## Legacy ioctl map and what it reveals

The ABI numbers and structures are in `via_chrome9_drm.h:67-158,187-249` and
the dispatch permissions in `via_chrome9_drv.c:146-190`.

| Number | Name | Dispatch flags | Audit disposition |
|---:|---|---|---|
| `00` | ALLOCMEM | AUTH | obsolete heap allocator; replace with GEM |
| `01` | FREEMEM | AUTH | obsolete heap free |
| `02` | FREE | AUTH | legacy free path |
| `03` | ALLOCATE_EVENT_TAG | AUTH | shadow/event-tag allocator; internal fence replacement only |
| `04` | FREE_EVENT_TAG | AUTH | shadow/event-tag free |
| `05` | ALLOCATE_APERTURE | AUTH | stub/obsolete |
| `06` | FREE_APERTURE | AUTH | stub/obsolete |
| `07` | ALLOCATE_VIDEO_MEM | AUTH | legacy VRAM allocation wrapper |
| `08` | FREE_VIDEO_MEM | AUTH | legacy VRAM free wrapper |
| `09` | WAIT_CHIP_IDLE | AUTH | unbounded/tiny-spin wait whose result is discarded; internal only |
| `0A` | PROCESS_EXIT | AUTH | stub |
| `0B` | RESTORE_PRIMARY | AUTH | stub |
| `0C` | FLUSH_CACHE | none | user-directed cache flushing; delete |
| `0D` | INIT | AUTH, MASTER, ROOT | installs user-chosen mappings/chip fields; replace with probe |
| `0E` | FLUSH | AUTH | raw command submission/BCI fallback; replace with validated scheduler job |
| `0F` | CHECKVIDMEMSIZE | AUTH | heap query; obsolete |
| `10` | PCIEMEMCTRL | AUTH | unchecked SG pointer arithmetic/copy; delete |
| `11` | AUTH_MAGIC | none | redundant authentication shim; delete |
| `12` | GET_PCI_ID | none | reads PCI identity/config; use kernel PCI core, no ioctl |
| `13..15` | unassigned | — | reject |
| `16` | INIT_JUDGE | none | global initialization-state query; delete |
| `17` | DMA | none | ring/GART initialization reachable without ioctl flags; internal only |
| `18` | QUERY_BRANCH | none | exposes global branch state; delete |
| `19` | REQUEST_BRANCH_BUF | none | exposes global pool/offset; replace with kernel job BO |
| `1A` | BRANCH_BUF_FLUSH | none | unverified FE40 payload execution; delete from UAPI |

`DRM_AUTH` is not a meaningful boundary in this driver because every opener is
marked authenticated (`via_chrome9_drv.c:204-209`).  The modern UAPI must not
retain any of these numbers or structures merely for compatibility; there is
no safe command/address ownership model to preserve.

| Function | Hardware knowledge retained | Modern disposition |
|---|---|---|
| INIT/CLEANUP | map set, chip/bus, GART location/size, ring size, heaps | replace with PCI probe and kernel BO/VM setup |
| FLUSH | BCI fallback vs copied ring submission and wait | replace with GEM submit + scheduler fence |
| DMA init/cleanup | toggles `SR6C.bit7` table protection and constructs ring | internal only |
| ALLOCMEM/free | VRAM vs SF heap offsets and alignments | replace with GEM create/map |
| PCIEMEMCTRL | old non-mappable SF copy path | delete |
| event tags | shadow-memory completion convention | replace with `dma_fence` |
| branch query/request/flush | FE40 branch-buffer pool and FE30 completion | internal command batching only |
| wait idle/cache flush | status mask and x86 coherency assumptions | internal bounded waits/DMA sync |
| aperture | stub in this KMD | delete |
| process exit/restore primary | stubs | delete |

The BCI fallback is still valuable as a software decoder for headers 0-5
(`via_chrome9_dma.c:785-910`).  It gives exact MMIO side effects and can seed a
userspace command disassembler/emulator.  It must not become a raw MMIO
submission ioctl.

## Bounded live-probe plan for unresolved fields

Every write probe below requires the existing module parameter/gate, saved
registers, off-screen guarded allocations, a hard timeout, remote logging, and
a single command-stream change per reboot.  “Defer” means the current driver
cannot recover safely enough to make the experiment bounded.

| Unknown | Lowest-risk discriminator | Success evidence | Failure bound |
|---|---|---|---|
| first post-init dispatch / priming | on each fresh reboot install a dummy-only ring, accept PAUSE when `(0400 & 00101ff3)` is zero or `10`, then expose one saved/restored H0 latch by changing A64/A65 only; compare separately against a pre-populated first ring | phase-2 latch changes on every boot while phase 1 reaches `current-pause=0x210` and has no non-dummy side effects | no draw or GPU memory address; leave reader paused and ring/GART pinned |
| `043c` fetch/read-ahead window | repeat the aligned CR-dummy stream at several pause distances with at least `0x400` initialized bytes after pause | stable final `current-pause=0x210`, `0444[31]=1`, non-CR mask clear; CR bit 4 may remain set | no addresses unmapped; timeout leaves known pause |
| FE40 size units/limits | branch to a read-only payload containing one harmless H0 latch write, vary encoded size only across `16/32/64` bytes | exactly one chosen latch changes and CR returns to main pause | mapped 1 KiB payload/padding; no nested branch |
| FE40 hierarchy | first prove first-level SF; defer second/third level until parser can reject recursion and total byte budget is enforced | bounded nested no-draw latch chain | reject/defer now; uncontrolled recursion can wedge |
| FE30 system write | system-write-only new fence to coherent sentinel, no IRQ or wait | sentinel/`0494` ID and idle become consistent | poll deadline; ring remains able to reach pause |
| FE30 engine ordering | after a proven no-draw 3D state write, compare zero wait, `WAIT3D`, and `WAITALL`, one per boot | fence visibility only after intended engine quiescence | no draw/memory target; timeout diagnostics |
| IRQ and interrupt-wait | staged four-step sequence described above | `0204[5]`, handler count, RW1C ack, then continued CR fetch | never set wait before handler/ack proven |
| H2 3D-port clock behavior | sample `SR3F`; emit one documented harmless state word through aligned `043c/0440`; read back via debug route only if already proven | state readback changes, no busy bits persist | no vertex HParaType 00; ring and guards remain mapped |
| T11 A00 arbitration | run initialized 2D fill, fence, harmless 3D state, fence serially with default; only later compare explicitly programmed documented default (`bit17=1`) | both operations complete, guard/state intact | never overlap engines in first probe |
| SR3F clocks | read-only samples first; if firmware left a clock off, program only documented `10` for that engine while idle and restore afterward | no status regression and known operation succeeds | do not test clock-off or idle-gating during bring-up |
| SR4E reset | after mature replay path, pulse 2D engine-reset encoding and immediately restore/reinitialize, with scanout excluded | known guarded fill succeeds post-reset | defer 3D/CR reset; remote reboot is not software recovery |
| T10 A68 clear/restore | clear fence queue only after ring pause, engines idle, and no pending fences; observe status/readback before next trivial job | next fence starts from deliberate software epoch | restore-cycle bits deferred until state source is understood |
| context-save size/layout | vendor exact SL slot is `0x7000` bytes; restore replays `+0x1b0..+0x478f` | finite changed span within one guarded local-VRAM slot, reproducible header image | defer live write; generated save format and exact maximum written extent remain unverified |
| SF aperture/SR7B code | retain current proven table size; test a larger aperture only with all new PTEs valid and no commands crossing old limit | fetch within added last page plus intact neighboring guards | never deliberately fetch invalid PTE; one code per reboot |
| SM and LL domain codes | recover actual allocator/path from Windows UMD first | valid coherent access with known DMA mapping semantics | defer raw SM/LL live access; can DMA arbitrary RAM |
| texture/RT address splits | disassemble UMD command generation and compare multiple BO addresses/strides/formats before any draw | changing one input changes predicted bits only | no live draw until complete range validator exists |

The immediate safe path is therefore FE40 first-level SF with a harmless,
aligned payload, followed by FE30 system-write completion, followed by a 3D
port no-draw state word.  Invalid-PTE faults, SM/LL, context save, preemption,
CMDQ/VQ, clock-off, and CR reset are explicitly outside current live scope.

## Concrete integration order

1. Preserve the currently proven SF ring/GART self-test and saved/restored
   header-0 latch packet.
2. Convert the test into a persistent kernel-owned ring and pinned, reserved
   GART BO.  Make dummy-only install/prime and later pause advancement two
   separate bounded phases; the first real job must not share the A60..A68
   installation transaction.  Add head/pause sampling and guard padding
   matching the vendor stream.
3. Preserve the now-confirmed header-0 2D fill and its guard verification as a
   regression test.  Keep display and VRAM allocator ownership explicit.
4. Add one kernel-generated FE40 branch to a coherent, read-only dummy payload.
5. Add one FE30 system-write-only fence and prove polling first.  Then install
   the IRQ and test interrupt generation/ack independently; enable
   interrupt-wait only after both paths work.
6. Build a new parser from the packet grammar and address-field table above.
   Reject header 1, video/DMA headers, CR PreCR/Cfg, nested branches, and all
   unknown fields initially.
7. Replay only the minimum DRI-binary-derived 3D state needed for a no-draw
   state change, then a clear, then a triangle.  Do not execute the dead
   `Initialize3DEngine()` sweep wholesale.
8. Before exposing a render node, implement hangcheck, a measured reset path,
   per-file VM/BO ownership, fence-error propagation, and suspend/resume ring
   reconstruction.

## Remaining source-level uncertainties

- FE40 size-field limits and the meaning/availability of second- and
  third-level branches;
- whether FE30 with wait mask zero is a full 3D completion or merely ordered
  behind the branch fetch;
- the VX900-required ring tail/head guard and overfetch amount;
- the contradictory jump-alignment comment and pointer-minus-64 arithmetic;
- reset/restore bit scope and safe clock/power sequencing;
- whether `SR6C.bit7` has any translation gating side effect beyond its
  documented GART-table write protection, and the undocumented VX900 meaning
  of `SR7B[3:0]`;
- context-save buffer length/layout and whether T11 A09 bit8 is incremental or
  a complete writable-register image;
- exact semantics and coherency of SM and LL location codes;
- CMDQ/VQ read/write pointer and overflow recovery behavior;
- whether every H6S2-shared default and hazard field is identical on H5S2VP1;
- tiled/compressed address extent rules absent from this KMD;
- complete 3D state, texture format, and shader ISA encodings, which must come
  from the debug-rich DRI binary, Windows UMD, manuals, and bounded hardware
  probes.
