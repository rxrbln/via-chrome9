# Chrome9 HD FE3x completion and IRQ design

This note defines a kernel-owned completion path for VX900/Chrome9 HD jobs.
It is deliberately independent of the current `DST_POS` cookie oracle: the
first implementation keeps both and uses the new record only as an additional
proof.  No userspace command may choose an FE3x packet, completion address,
wait mask, ID, or IRQ mode.

## Evidence and confidence

The bit encoding is source-exact in the supplied 5.75 KMD macro
`ADDCmdHeader3X_INVI()` at
`binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/via_chrome9_3d_reg.h`.
The supplied Windows 7 KMD
`binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGKModeDX32A.sys`, SHA-256
`618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`,
independently builds the same packet at image offsets
`0x000b755f..0x000b85f4`.  Its workload policy supplies the important fact
missing from the old Linux path: 3D uses header `FE33` and wait mask
`00200000`.

The Windows conditions are bit-exact.  The builder starts mode at `22`; adapter
`+9bc.bit10` adds interrupt-wait `10`, `+9bc.bit11` adds system write `4`, and
`+9c4.bit30` suppresses interrupt generation by clearing `2`
(`0x000b755f..0x000b75d4`).  Thus these are independent controls rather than a
single opaque “fence enabled” mode.

The old Linux VX900 path at `via_chrome9_dma.c:1640..1694` emits an FE40
branch followed by FE30, but uses a zero wait mask.  It proves that the chip
accepts the new fence grammar, system-memory write, interrupt generation and
interrupt-wait; it does **not** prove that its zero mask orders arbitrary 3D
stores.  Windows' nonzero, engine-specific masks are the authority for a new
driver.

Older H5/H6 branches in the same source are not alternative VX900 encodings.
They write traditional CR fence subaddresses A01/A02/A03 or A04--A07, with
trigger `00100000`, idle mode `00010000`, interrupt `00020000`, system write
`00040000`, interrupt-wait `00200000`, and queue disable `00400000`.  The
chip-selection code deliberately routes H5S2VP1/VX900 to the FE40/FE3x
builder instead.  Reusing A01--A07 on 7122 would combine a different fence
queue and wait model with the new command reader without vendor evidence.
The VX900 Part II manual, internal pages 74 and 83, makes the exclusion
architectural: enabling command switch disables traditional wait/fence and
traditional branch.  It also defines traditional interrupt-wait as CR pausing
until software clears `0204.bit5`, corroborating the 5.75 capability comment
and the lost-IRQ deadlock risk even though FE3x itself is omitted from the open
manual.

The four dwords, on a 16-byte boundary, are:

```text
d0 = FE33_0000 | mode
d1 = 0020_0000 | ((id & 0000_00ff) << 24)
d2 = ((id >> 8) & 00ff_ffff) | ((addr & 0000_0ff0) << 20)
d3 = (addr >> 12) & 000f_ffff

id   = ((d2 & 00ff_ffff) << 8) | (d1 >> 24)
addr = (d3 << 12) | ((d2 >> 24) << 4)
```

Mode bits are interrupt generate `00000002`, system-memory write `00000004`,
interrupt-wait `00000010`, and queue-skip `00000020`.  The address has four
implicit zero bits and only 32 encoded bits.  The completion allocation must
therefore be 16-byte aligned, coherent, covered by a 32-bit DMA mask, and
rejected if `upper_32_bits(dma_addr)` is nonzero.
The current probe already restricts `chrome9_dma_bits` to 24--32 and calls
`dma_set_mask_and_coherent()`, so a `dma_alloc_coherent()` fence page naturally
uses the correct device-visible domain; retain the explicit upper-bit check as
an encoding assertion.
Pass the returned DMA/bus address, not `virt_to_phys(cpu_pointer)` and not an
SF virtual address.  FE3x performs a direct PCI-master system-memory write;
the completion page must not consume transient GART PTEs whose invalidation it
is intended to authorize.  The old KMD's `virt_to_phys(kmalloc_pointer)` works
only under its historical no-IOMMU/low-memory assumptions and is not a Linux
DMA API contract.

Other Windows wait policies are useful cross-checks but not substitutes:

| work | header | wait mask |
|---|---:|---:|
| 3D | `FE330000` | `00200000` |
| 2D | `FE300000` | `00300000` (2D plus 3D dependency) |
| DMA | `FE370000` | `0031e000` (DMA plus 2D/3D dependencies) |
| HQV | `FE350000` | `000c0000` |
| VD | `FE3A0000` | `00020000` |
| all/sentinel | `FE300000` | `003fe000` |

The current Chrome9 draw builders submit 3D plus a final 2D cookie, so the
first FE3x oracle belongs immediately after the 3D body and before that cookie.
Keep the existing cookie and owned PAUSE after it:

```text
3D body -> FE330004 completion -> DST_POS cookie -> dummy closure -> PID00 PAUSE
```

The memory record proves the selected 3D wait completed; the later cookie and
owned PAUSE retain the already-live command-order and reader-ownership proof.
`ADDCmdHeader3X_INVI()` may insert up to three `CC000000` alignment dwords, so
the kernel helper must run the existing `segment_stop + 0x220 <= PAGE_SIZE`
check after insertion.  Do not subtract a nominal 16 bytes from the maximum
batch and assume alignment: reject `-E2BIG` if the completed immutable
read-ahead closure no longer fits.

## Why the first production mode should be `FE330006`, not `FE330036`

The old Linux and Windows drivers normally enable interrupt-wait and
queue-skip.  Those are scheduler features, not prerequisites for completion.
Interrupt-wait couples CR progress to successful interrupt acknowledgement:
a masked/lost IRQ can leave the reader stopped inside the fence rather than at
the driver's immutable PID00 PAUSE.  Queue-skip handles vendor fence-queue
pressure whose depth and overflow behavior have not been measured on VX900.

After separate polling and IRQ oracles pass, use:

```text
FE330006 = 3D fence + system write + interrupt generation
```

The reader remains free to advance to the existing PID00 PAUSE even if Linux
loses the interrupt.  A timeout worker can then recover completion by polling
the coherent word plus the existing cookie/PAUSE/work predicates.  Keep bits
`00000010` and `00000020` clear until queue depth, ack, masking and recovery
are each validated.  Windows' `FE330036` is a useful final policy reference,
not the safe bring-up value.

## Four-stage hardware oracle

Allocate one coherent page and initially use one 16-byte slot.  Use an ID
different from both the zero-filled slot and `MMIO[0494]`; store a poison
value, execute `dma_wmb()`, and append exactly one packet to a proven no-write
or fixed 3D oracle.  At every stage retain the ordinary cookie and PAUSE polls.

1. **System write only:** `d0=FE330004`.  Poll the coherent word with
   `dma_rmb()` and exact ID equality.  Also record `0494`, but do not require it
   for memory safety until its relation to memory write is measured.
2. **Interrupt, no wait:** install the shared handler, clear only stale
   `0204.bit5`, preserve all other `0200` *enable* bits while setting bit 31
   without echoing its RW1C status bits, and use
   `FE330006`.  Require the memory ID, handler count, exact RW1C ack and normal
   PAUSE.
3. **Intentionally masked IRQ, still no wait:** use `FE330006` with the global
   gate masked for this controlled test.  The IRQ wait must time out, but the
   memory record, cookie and PAUSE must complete.  Poll recovery must classify
   this as lost notification rather than engine busy.
4. **Optional interrupt-wait:** only after the first three pass, test
   `FE330016` once per boot with a remote recovery path.  The handler must ack
   before the deadline and the reader must subsequently reach PID00 PAUSE.
   This is an oracle, not yet a production requirement.

Hard-stop on any unexpected `040c`, wrong memory ID, nonzero 3D work after the
record, `0444=60000000` after the no-wait stages, or an IRQ storm.  Do not
retry by changing FE33 to FE30 or clearing engine state.

## IRQ register contract

Both old Linux and Windows use:

```text
MMIO 0200 bit31  global graphics-interrupt gate
MMIO 0204 bit5   command-reader interrupt status, write one to clear
MMIO 0494        VX900 current/fallback fence-ID readback
```

VX900 Part II, internal pages 10--11, independently documents `0200.bit31` as
the global interrupt enable and `0204.bit5` as RW1C CR interrupt status.  This
is primary hardware documentation for the gate/ack pair; the Windows ISR is
the executable ordering evidence.

Windows' progress routine at `0x00076ad0` confirms that `0494` and the memory
record are alternative primary paths.  With adapter `+9bc.bit11` set it reads
the per-engine memory array (`0x00076b8a..0x00076bd1`); without that capability,
device `7122` reads `0494` for engine zero
(`0x00076c5c..0x00076cea`).  Other branches take additional `0494/049c` samples
for diagnostics/workarounds, but static code does not establish whether
`0494` updates before, with, or after an enabled system write.  This is why
Linux must use the coherent word as the primary record and keep `0494`
diagnostic until the hardware oracle orders them.

The Windows ISR at `0x00077a60` first rejects an all-ones `0200` or a clear
global bit, samples `0204`, dispatches bit 5 to the 7122 callback at
`0x000c8360`, acknowledges, processes fence progress and finally schedules
the DPC.  The callback reads `0204`, ORs `20`, and writes it back.  Because the
other documented status bits are read-only this happened to work, but Linux
must write exactly `BIT(5)` and must never echo unclassified RW1C status.

Initialization/teardown order is:

1. allocate and poison coherent slots;
2. `request_irq(..., IRQF_SHARED, ...)`;
3. sample and acknowledge stale `0204.bit5` with exactly `BIT(5)`;
4. update the `0200.bit31` gate through the mixed-register helper below,
   preserving display/video enables without acknowledging their statuses;
5. publish only non-waiting FE3x packets;
6. on teardown stop submissions, prove all records or quarantine, mask only
   the bit owned by this driver, `synchronize_irq()`, then free the IRQ and
   coherent page.

In the handler, sample `0200` and `0204`; return `IRQ_NONE` unless the global
gate and bit 5 are both set.  Ack bit 5, execute `dma_rmb()`, then compare the
coherent record with the exact expected ID.  A mismatched record increments a
diagnostic counter and wakes a threaded/bottom-half recheck but never signals
a fence.  Hard IRQ context must not take `submit_lock`, `vm_lock`, a GEM
reservation, or invalidate PTEs.

`0200` cannot use a literal read/OR/write.  VX900 Part II page 10 interleaves
enable bits with RW1C status bits.  Writing a sampled one back would
acknowledge an unrelated vblank, DMA, capture, HQV, DVI or LVDS event.  The
documented masks are:

```text
0200 enable fields = f7ff0800
0200 RW1C fields   = 0800f3fd
```

For a gate update, read once for diagnostics, then write
`(old & ~0800f3fd) | 80000000`; all pending status positions are deliberately
written as zero while every enable field is retained.  Disabling follows the
same rule and clears bit 31 only when the driver's central IRQ-user accounting
proves no display/video consumer still needs the global gate.  Do not copy the
old Linux `0200=80000000` (destroys other enables) or Windows' unmasked OR
(can clear sampled RW1C events).  Serialize every `0200` update through one
driver helper so KMS and render IRQ users cannot race.

## Kernel lifetime and completion state machine

Use a per-engine coherent page with 16-byte slots.  The first implementation
allows one outstanding hardware job, matching today's synchronous
`submit_lock` path, so exact equality is sufficient and queue depth is not an
unknown.  Allocate a monotonically increasing nonzero 32-bit hardware ID and
store the full 64-bit Linux fence sequence plus `ring_epoch` beside it in
software.  Do not use the old KMD's `submitted <= readback` comparison: its
31-bit wrap to zero is not wrap safe.

Before command publication:

```text
WRITE_ONCE(*slot, poison_not_equal_to_id)
dma_wmb()
emit FE330004/006 with exact slot DMA address and ID
emit existing cookie and closure
record id, slot, publish_epoch in chrome9_job
publish A64/A65
```

Completion has two levels:

* **notification completion:** after `dma_rmb()`, the coherent slot exactly
  equals this job's ID.  This proves FE33 executed after its 3D wait mask.  It
  may wake the waiter, but it does not alone authorize PTE removal because the
  reader can still be consuming the following cookie/closure.
* **release completion:** exact slot ID, exact `DST_POS` cookie,
  `0444.bit31`, `!(0400 & 00101fe3)`, matching ring epoch/PAUSE tuple, CPU DMA
  transition, and successful GTI invalidation.  Only this level removes the
  SF node and releases BO references.

The synchronous first implementation may let the IRQ wake a wait queue and
then perform the release proof in process context.  On IRQ timeout, poll the
slot once under the same bounded deadline:

| memory ID | cookie/PAUSE/work | classification |
|---|---|---|
| exact | exact/set/idle | lost or delayed IRQ; complete normally and count it |
| exact | incomplete | FE33 completed, CR tail still active; wait only to the bounded tail deadline |
| wrong | exact/set/idle | packet/system-write failure; keep current cookie proof but disable FE3x capability |
| wrong | incomplete or work busy | true/unclassified engine timeout; quarantine |

Never call an engine busy merely because the IRQ counter did not advance, and
never call an IRQ lost merely because `0204.bit5` is pending.  The memory ID is
the independent completion record.  `0494` remains diagnostic until a live
oracle proves whether it updates before, with, or after the coherent write.

## Integration boundary

The first code tranche should add only the coherent allocation, kernel packet
helper, polling stage and counters.  It should not alter dma-fence signaling or
advertise asynchronous submissions.  The second tranche adds `FE330006`, the
shared handler and wait queue while retaining synchronous release.  Only after
masked-IRQ recovery passes should the handler replace the 10-microsecond MMIO
poll loop as the normal wakeup.

Required counters are packets emitted, memory completions, IRQs handled,
spurious bit-5 IRQs, ID mismatches, poll recoveries after missing IRQ, tail
timeouts, and true engine timeouts.  Log packet dwords, slot DMA, ID, `0494`,
`0200`, `0204`, cookie, `0444`, `0400`, `040c`, `043c`, and `0440` on the first
failure.  This makes a notification defect distinguishable from a shader/CR
wedge without weakening the existing no-late-DMA quarantine rules.
