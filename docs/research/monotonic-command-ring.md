# Chrome9 monotonic multi-command ring

This note turns the now live-proven PID00/PID10 lifecycle into the smallest
multi-command design which does not replace the command ring for every draw.
The first implementation deliberately does **not** reuse bytes within one ring
epoch.  Normal submissions append monotonically and update only PID00
A64/A65.  Exhaustion performs the already proven PID10/full-idle transition,
then reinstalls the same address range from offset zero.

This choice removes the risky part of the old circular allocator—relying on an
occasionally unreliable hardware current pointer—while still amortizing one
full replacement over many draws.

## Exact vendor evidence

### Bounds are start-inclusive and end-exclusive

The old Linux Chrome9 ring initialization assigns:

```text
pBeg = addr_linear
pEnd = addr_linear + dmasize
```

for ring mode (`via_chrome9_dma.c:725..737,757..761`).  `dmasize` has already
been converted to dwords.  `set_agp_ring_cmd_inv()` translates `pBeg` directly
to A60/A61 and `pEnd` directly to A62/A63
(`via_chrome9_dma.c:539..543,565..583`).  Therefore A62/A63 is the first byte
after the legal ring range, not the address of its last dword or quadword.

The Windows VX900 executable independently constructs a four-MiB range as
`start=P`, `end=P+0x400000` in `VTGKModeDX32A.sys:0xa17a0` and installs it in
the VX900 initializer `0xc7c00..0xc7f6f`.  The recovered words are:

```text
A60/A61 = start, A60.bit0 set for SF
A62/A63 = end-exclusive
A64/A65 = initial PID00 pause
A66/A67 = fffffff0 jump sentinel
A68     = 680a0001
```

For the current Linux SF aperture, a 64-KiB ring at SF offset zero is thus:

```text
A60 = 60000001
A61 = 61000000
A62 = 62010000
A63 = 63000000
A66 = 66fffff0
A67 = 670000ff
A68 = 680a0001
```

A64/A65 depend on the staged initial pause.  Start, end, pause, and stop are
16-byte aligned.  The ring allocation and all of its PTEs remain live until a
PID10 STOP has cleared the full engine wait mask.

### Ordinary append changes only A64/A65

Old Linux `kickoff_dma_ring_inv()` pads the new CPU cursor to 128 bytes for
H5S2/H5S2VP1, computes `pause = cursor - cpu_base + gpu_base - 16`, selects
T10, and writes only A64 then A65 with PID00
(`via_chrome9_dma.c:1021..1035,1053..1073`).  It records the new software
cursor afterward.  It does not rewrite bounds, jump, or trigger state.

The VX900 Windows path at `VTGKModeDX32A.sys:0xc8190..0xc834b` matches this
exactly.  Its current CPU cursor is adapter `+0x22fa8+0x30`, its last published
cursor is `+0x22fa8+0x28`, CPU base is adapter `+0x22ec0`, and GPU base is
adapter `+0x22eb8`.  If no bytes were appended it returns.  Otherwise it pads
to a 128-byte cursor, translates cursor minus 16, then writes only PID00
A64/A65.  No A60--A63, A66/A67, or A68 write occurs.

The resulting invariant for an already owned PID00 reader is:

```text
old_pause = P
append     = P + 0x10             # next command begins here
new_pause  > P, aligned to 0x10
distance   >= 0x20                # manual minimum two 128-bit quadwords
publish    = T10 select, A64 low, A65 high|PID00
```

The existing builder's stronger 128-byte cursor alignment should remain.

### Why not copy the legacy circular reuse yet

`get_space_ring_inv()` reserves the requested bytes plus
`INV_CMDBUF_THRESHOLD * 8`, i.e. 64 bytes, and compares them with MMIO's
current address (`via_chrome9_dma.c:1234..1243`).  On wrap it also reserves
`INV_QW_PAUSE_ALIGN * 16 = 0x400` bytes
(`via_chrome9_dma.c:1259..1264`).  Every reuse decision polls the current
pointer without a timeout, and the source explicitly says its value can be
unreliable and therefore reads it twice (`:1247..1258,1270..1284,
1300..1316,1326..1339`).

Its `rewind_ring_agp_inv()` additionally writes A66/A67 while the old range is
owned and on H5S2VP1 points JUMP 64 bytes before the aligned tail because the
410 jump command requires `4*128` alignment (`:1145..1189`).  A65 itself stays
PID00; this is an A66/A67 tail-jump protocol, not an explicit PID01 write.
None of that protocol has been live-proven on this VX900.  Do not import it
into the first monotonic implementation.

The Windows executable independently contains the same circular-space
algorithm at `VTGKModeDX32A.sys:0xa1c50..0xa1f46`.  Its recovered ring fields
are CPU base `+0x1c`, published/current CPU cursor `+0x30`, and CPU end `+0x34`.
For a request count in dwords it computes:

```text
request_bytes = request_dwords * 4
NextSW        = gpu(current_cpu + request_bytes + 0x40)
BoundaryCheck = gpu(base_cpu + request_bytes + 0x400)
```

At `0xa1ced..0xa1d51`, `0xa1d7c..0xa1de0`, and
`0xa1ebf..0xa1f33` it polls MMIO `043c` with the same range comparisons twice
before permitting overwrite or invoking the rewind callback.  Every loop is
unbounded.  Thus the 64-byte request reserve, 0x400-byte wrap reserve,
double-read policy, and current-pointer dependency are not Linux-source
accidents; they are also in the shipping Windows ring manager.  This is useful
cross-checking, but it strengthens the reason to defer byte reuse until Linux
has a bounded silicon oracle for `043c`.

The adjacent VX900 128-byte rewind implementation at
`VTGKModeDX32A.sys:0xc7f80..0xc817a` exposes a generation-sensitive conflict
which makes reuse still less suitable for initial bring-up.  It emits the
dummy H2 prefix, pads the tail with `ccccccc7` through a 128-byte cursor,
sets A66/A67 JUMP to translated tail cursor minus **16**, resets its software
cursor to the CPU base, sets PID00 A64/A65 to translated base minus 16, and
writes A66, A67, A64, A65 in that order without A68.  The old Linux H5S2VP1
path instead subtracts **64** for its JUMP address.  Both use PID00 in A65;
neither proves that one tail offset is safe for the other subgeneration.

The first driver should therefore defer both the A66/A67 tail-jump rewind and
any explicit PID01 experiment.  PID10 full idle plus a complete reinstall has
none of this unresolved `-16` versus `-64` dependency.

## Implementation-ready layout

Use two allocations with different lifetimes:

* `ring_cpu/ring_dma`: initially 16 coherent pages (64 KiB), SF-mapped at
  offsets `[0, CHROME9_RING_SIZE)` and visible to the command reader;
* `staging_cpu`: one page-aligned CPU-only page used by the existing per-draw
  builders and never entered in the GART.

Keep every builder limited to one page for now.  This preserves all existing
packet-size checks and makes the ring change independent of texture/depth
packet growth.  Initialize the entire active ring to the legal alternating
dummy pattern and flush it before the first A60--A68 installation.

The staging base must be 128-byte aligned because the current emitters align
the absolute CPU cursor, not merely its offset.  Use a page allocator such as
`get_zeroed_page()`/`free_page()` (or another explicitly page-aligned
allocation); do not assume an arbitrary heap allocation preserves the packet
offsets when copied to the page-aligned ring.

Replace slot state with offsets relative to ring start:

```text
engine->append          next 128-byte-aligned destination for a segment
engine->active_pause    currently published PID00 pause
engine->active_stop     immutable PID10 trailer for that publication
engine->ring_epoch      diagnostic replacement counter

job->segment_pause      staging-relative pause returned by finish_ring
job->segment_stop       staging-relative stop returned by finish_ring
job->ring_pause         absolute ring-relative pause after staging
job->ring_stop          absolute ring-relative stop after staging
```

`active_pause` and `active_stop` should always be ring-relative addresses;
because the command ring occupies SF offset zero, these are also the current
T10 address payloads.  Keeping one convention removes the current
`slot * PAGE_SIZE` arithmetic from STOP, waits, and diagnostics.

### Preserve the existing segment closure exactly

`chrome9_finish_ring()` currently:

1. appends at least one `ccccccc0,ddd00000` pair and aligns the cursor to 128;
2. records PID00 pause at cursor minus 16;
3. emits the immutable STOP trailer header and dummy pairs through the next
   128-byte cursor alignment;
4. records STOP at that cursor minus 16;
5. fills the rest of the page with legal dummy pairs.

Build this unchanged into `staging_cpu`.  If payload ends at byte offset `E`,
the mandatory first pair makes the aligned cursor
`Q = ALIGN(E + 8, 128)`, so:

```text
segment_pause = Q - 16
segment_stop  = Q + 112
```

With active `append` already 128-byte aligned, copying staging offset zero to
that location preserves every packet, pause, and STOP alignment.  Copy exactly
through the existing read-ahead closure:

```text
copy_len = segment_stop + CHROME9_CR_READ_AHEAD
ring_pause = append + segment_pause
ring_stop  = append + segment_stop
```

The 128-byte boundary also places `append` beyond the cache line containing
the old pause word (`old_pause=append-16` on x86).  Flushing the new suffix
therefore does not evict or rewrite the cache line which holds the still-owned
pause address.

The capacity test is therefore exact and overflow-safe:

```text
segment_stop <= PAGE_SIZE - CHROME9_CR_READ_AHEAD
append <= CHROME9_RING_SIZE - copy_len
```

Equivalently, `append + segment_stop + CHROME9_CR_READ_AHEAD <= ring_size`
after checked arithmetic.  Do not test only the new PAUSE against A62: the
STOP trailer and observed read-ahead closure must also remain inside the
range.

After successful publication, the next append cursor is
`ring_pause + 0x10`, which equals `append + Q` and is 128-byte aligned.  Bytes
through that cursor are committed and are never reused within the epoch.  The
STOP/read-ahead closure begins at that cursor and is intentionally provisional:
the next legal PID00 append replaces it with new commands and constructs a new
STOP/read-ahead closure farther forward.  Between publications, however, the
current closure is immutable because PID10 may need to consume it.

## Normal submission transaction

All steps remain under `submit_lock`, so only one command segment can be
staged or published at a time.

1. Prove the previously published tuple is at owned PID00 PAUSE and that
   non-CR work is idle: `0444.bit31 == 1` and
   `!(0400 & CHROME9_ENGINE_WORK_MASK)`.  The preceding synchronous retirement
   already proved its exact cookie as well.
2. Build the complete new segment in the unmapped staging page.  Validation or
   builder failure cannot corrupt the active ring.
3. If its checked `copy_len` does not fit at `append`, use the bounded wrap
   transaction below **before modifying the active ring**.  The current STOP
   trailer is still required by that transaction.  Do not split a segment over
   the end.
4. Copy `[0, copy_len)` from staging to `ring_cpu + append`, flush that copied
   destination range, and execute `dma_wmb()`.
5. Record the pending absolute `ring_pause` and `ring_stop` in the job.
6. Select T10 with `041c=00100000`; write
   `64000000 | (ring_pause & 00fffff0)`, then
   `65000000 | (ring_pause >> 24)`, then read back the transfer port to drain
   the posted MMIO writes.  Do not touch A60--A63, A66/A67, or A68.
7. The A65 write publishes the segment.  Immediately set `job->published`,
   `engine->active_pause=ring_pause`, `engine->active_stop=ring_stop`, and
   `engine->append=ring_pause+0x10`.  These fields must describe what hardware
   may now own even if retirement later times out.
8. Retire with the existing bounded three-stage oracle: exact per-job cookie,
   `0444.bit31`, then `!(0400 & CHROME9_ENGINE_WORK_MASK)`.
9. Only after all three pass may the job's data BO PTEs be invalidated and its
   fence signalled.  Executed command bytes through `ring_pause+0x10` remain
   allocated and immutable until epoch replacement; only the provisional
   closure at and after that cursor may become the next append.

If any post-publication wait times out, make no further CR writes, mark the
engine wedged, and retain the ring, GART, job PTEs, and BO mappings for reboot.
The ring cursor metadata still names the newly published tuple for useful
diagnostics; it must not be rolled back to the previous tuple.

An append helper corresponding to the vendor operation should have a narrow
interface, for example:

```text
chrome9_append_segment(engine, job, segment_pause, segment_stop)
```

It owns capacity checking, copy/flush, A64/A65 publication, and absolute cursor
updates.  It must not install or trigger a ring.

## Bounded exhaustion and wrap transaction

The first version uses no live circular free-space calculation.  Available
space is simply `ring_size - append`.  When the staged segment does not fit:

1. The current tuple must first satisfy the same owned-PID00 and non-CR-idle
   preconditions.
2. Call the live-proven `chrome9_stop_active_ring()`: publish the immutable
   later trailer through only A64/A65 with PID10, then poll until
   `(0400 & 00101ff3) == 0` using the existing bounded timeout.
3. Only after this full-idle proof may any byte in the old epoch be overwritten
   or A60--A68 be reinstalled.
4. Reinitialize the entire ring to the legal alternating dummy pattern.  This
   prevents an unexpected over-fetch in the new epoch from decoding stale 3D
   commands left later in the old epoch.
5. Copy the already-built staged segment at ring offset zero, including its
   STOP/read-ahead closure; flush the full reinitialized ring and execute
   `dma_wmb()`.
6. Install the full constant bounds plus its new PID00 pause, disabled jump,
   and A68 trigger exactly once.  Set `append=segment_pause+0x10`, update the
   active tuple, and increment `ring_epoch`.
7. Retire the job with the same cookie/PID00/non-CR checks.

Thus a wrap costs one proven PID10 STOP and one full install, not another prime
submission.  The pending segment is built before STOP, but it is copied into
the active ring only after full idle.

On a STOP timeout, retain the old epoch untouched and quarantine for reboot.
On a timeout after the new full install, retain the new epoch and all job
mappings for reboot.  Never attempt A66/A67 tail-jump rewind or PID01 as a
fallback.

Teardown remains identical to the current live-proven path: owned PID00 and
non-CR idle, then PID10/full wait-mask idle, then and only then invalidate ring
PTEs and restore the GART.

## Source-level patch map for the current driver

This map refers to `chrome9_engine.c` SHA-256
`8adfd76ea4d6c6a8f3ec0b7ce56876f96bb16d3628e5795fe7ef23f70dcdd1be`.

1. Lines 34--36: increase `CHROME9_RING_PAGES` from 2 to 16; retain the
   measured `CHROME9_CR_READ_AHEAD=0x220`.
2. Lines 73--99: replace `job->slot` and the staging-relative-only `stop` with
   the four segment/ring offsets above.  Update timeout logging accordingly.
3. Lines 160--192: add the staging allocation, `append`, and `ring_epoch`;
   remove `active_slot` after all diagnostics use absolute offsets.
4. Lines 363--397: make `chrome9_finish_ring()` operate on the staging page.
   Preserve the present mandatory pair, 128-byte alignments, STOP distance,
   legal tail fill, and page-size/read-ahead bound.
5. Builder bases at lines 403, 429, 595, 751, 908, 1044, and 1099: point all
   builders at `staging_cpu`, not `ring_cpu + slot * PAGE_SIZE`.
6. Lines 1120--1142: retain `chrome9_install_stopped_ring()` but restrict it to
   cold init and post-PID10 wrap.  For the 64-KiB epoch its start/end are
   `1` and `CHROME9_RING_SIZE`; A62 remains end-exclusive.
7. Lines 1144--1182: remove slot multiplication from STOP address generation;
   `address=active_stop`.  Preserve its A64-before-A65 order and full
   `CHROME9_ENGINE_WAIT_MASK` bounded poll.
8. Add the normal append helper next to those lifecycle helpers.  It must
   implement the transaction above and never write A68.
9. Lines 1220--1265 already bind all `CHROME9_RING_PAGES`; a 16-element stack
   PTE array is small.  Continue programming contiguous PTEs at SF page zero.
10. Lines 1640--1703: allocate staging separately, initialize/flush the full
    active range with legal dummies, build the prime in staging, copy its
    closure at offset zero, install bounds `[0, CHROME9_RING_SIZE)`, and set
    `append=pause+0x10`.
11. Submission blocks at lines 1805 and 1860--1915, plus the parallel block at
    lines 2120 and 2169--2217: remove slot flipping and per-job STOP/install.
    After building staging, call append; if `-ENOSPC`, run the bounded wrap
    helper.  Continue to wait and retire exactly as today.
12. Limits using `SZ_64M - CHROME9_RING_SIZE` and `drm_mm_init()` already scale
    when the constant changes.
13. Lines 2812--2814 are a correctness trap after enlarging the ring: the
    initializer sets only two invalid PTEs and leaves the rest zero.  Fill all
    `CHROME9_RING_PAGES` entries with `CHROME9_GART_INVALID` in a loop before
    `chrome9_gart_set()`.  Teardown must invalidate every ring page.
14. Allocate staging with explicit page alignment because builder alignment is
    based on the absolute pointer.  Free it on every init error and normal fini
    path.  Never free either allocation on a quarantine/reboot-required path.

The two submission functions should share the append/wrap helper so their
lifecycle cannot diverge as packet types are added.

## First live validation

Keep all current packet/state oracles unchanged and vary only lifecycle:

1. log one cold A60--A68 install and assert subsequent non-wrap submits write
   only T10 A64/A65;
2. run 1000 tiny submissions and assert pause addresses are strictly
   increasing and every next append equals prior pause plus 16;
3. use a deliberately small debug ring to force several wraps, and assert one
   PID10/full-idle plus one full install per epoch;
4. run the full smoke suite and at least ten three-gear frames;
5. unload/reload and verify all enlarged ring PTEs were invalidated before GART
   restoration;
6. inject a pre-publication builder failure and verify the active tuple is
   unchanged; inject a post-publication timeout only on a disposable boot and
   verify the new tuple and all reachable mappings are quarantined.

Useful counters are `pid00_appends`, `pid10_wraps`, `full_installs`, and
`ring_epoch`.  In an ordinary 1000-submit run, `full_installs` must be far
smaller than submissions and `pid10_wraps == full_installs - 1` (excluding a
cold-install counter convention).

Only after this monotonic design is stable should PID01 and within-epoch byte
reuse be considered.  That later optimization needs its own bounded,
live-proven current-pointer protocol; it is not required to eliminate the
per-triangle replacement bottleneck.
