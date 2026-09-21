# Single 32-KiB list: minimum kernel patch audit

## Scope and result

The minimum safe first performance patch is command `0x0f`, capability bit
22: one D16 RHW immediate triangle list containing `1..272` triangles.  It
reuses the current structured 128-byte state envelope, the existing
post-viewport RHW vertex record, the current two-object reservation/fence/GART
lifetime, and the live-proven monotonic ring.  Its only hardware-visible
change is A24 plus more consecutive immediate vertex words in one otherwise
identical state-complete packet.

Do not combine persistent object mappings, changed cache ownership,
asynchronous retirement, textured lists, native topology, or the new
semantic compiled-fragment command in this patch.  In particular, current
private command `0x19` / capability bit 41 remains limited to the proven
34-triangle program batch; `0x0f` is fixed Gouraud only.

The large list needs a second, CPU-only 32-KiB staging allocation.  The
existing 64-KiB coherent ring and its GART mapping do not change.  The exact
maximum copied closure is `0x5290`, leaving `0x2d70` bytes of staging
headroom.  One run therefore replaces up to eight current 34-triangle
submissions with one job, one command publication, and one retirement while
retaining the same failure/quarantine boundary.

This audit is against the current T2 kernel source at
`src.linux.../linux-7.2/drivers/gpu/drm/chrome9/chrome9_engine.c`, including
the later native-PS encoder work.  It does not edit the shared kernel or Mesa
trees.

## Exact current cost of a full glxgears batch

The live 1024x768 direct-SF layout is:

```text
XRGB destination payload       4096 * 768 = 3,145,728 bytes
guarded destination object                   3,153,920 bytes = 770 pages
D16 payload                    2048 * 768 = 1,572,864 bytes
guarded depth object                         1,581,056 bytes = 386 pages
combined transient SF mapping                4,734,976 bytes = 1,156 pages
```

For one maximum current 34-triangle blend batch, the native body is
`174 + 18*34 = 786` dwords and the complete ring copy is only `0x0f90`
bytes.  The command itself is not the bulk of the CPU/memory work.  The
current submit executes:

| operation | exact count/volume per batch |
|---|---:|
| copied userspace RHW vertices | `34*3*20 = 2,040` bytes |
| PTE CPU allocation/image | `1,156*4 = 4,624` bytes |
| GART bind writes | 1,156 MMIO PTE dwords |
| GART invalidate writes | 1,156 MMIO PTE dwords |
| VGA legacy-I/O acquisitions | 2 |
| `SR6F` translation flush pulses | 2 |
| explicit `chrome9_cache_flush()` | `2*4,734,976 = 9,469,952` bytes |
| guard writes | 4 pages = 16,384 bytes |
| guard reads | 4 pages = 16,384 bytes |
| normal SG ownership calls | 2 device + 2 CPU |
| active-XRGB scanout resync | 1 additional device sync, 3,153,920 bytes |
| reservation/fence jobs | 1 |
| cookie/PAUSE/work-idle retirements | 1 |
| vmaps/vunmaps | 2 / 2 |
| transient `drm_mm` nodes | 1 insert + 1 remove |

Each `chrome9_gart_set()` obtains VGA arbitration, polls `SR6F.bit7` clear,
unprotects SR6C, performs all PTE writes, and calls `chrome9_gart_flush()`.
That helper polls `SR6F.bit7` clear a second time, pulses bit 7, and polls for
completion.  Across bind and unbind this is two VGA acquisitions, 2,312 PTE
writes, four pre-pulse idle-poll loops, and two post-pulse completion loops.

Static operation counts cannot prove whether uncached PTE writes, VGA
arbitration, `SR6F` polling, explicit cache flush, or DMA/SWIOTLB ownership
dominates wall time.  They do establish the two high-volume classes:
2,312 MMIO PTE writes and 9.47 MiB of explicit cache traversal for a command
smaller than 4 KiB.  Instrument those separately before changing either.
The single-list patch amortizes all of them over up to 272 triangles but does
not remove them.  A later persistent mapping cache removes the PTE/VGA/SR6F
class on hits.

## UAPI patch

The allocation ledger already reserves:

```c
#define DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN      0x0f
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN  (1ULL << 22)
#define DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES     272U
```

Add a distinct `struct drm_chrome9_draw_triangle_d16_rhw_run` even though it
is intentionally byte-identical to the old batch envelope.  This avoids
silently changing the maximum of command `0x0c`:

```text
offset  size  field
0       4     destination_handle
4       4     depth_handle
8       4     flags
12      4     triangle_count (1..272)
16      4     width
20      4     height
24      4     destination_pitch
28      4     depth_pitch
32      4     destination_format
36      4     reserved0 = 0
40      40    blend
80      12    scissor
92      4     pad = 0
96      8     vertices_ptr
104     24    reserved[3] = 0
```

The structure is 128 bytes on 32- and 64-bit userspace; the x86 ioctl value
is `0x4080644f`.  Add `static_assert(sizeof(...) == 128)` in the kernel and
Mesa copies.  Command `0x0c` retains maximum 34 forever.  Advertise bit 22
only in the same kernel commit which installs the complete handler and ioctl
table entry.

The accepted flags and state are exactly the existing perspective D16 batch:
DEPTH mandatory, optional BLEND, implicit/explicit LESS, LEQUAL, EQUAL or
ALWAYS only through their existing capability gates, optional read-only
depth, current destination formats/blend factors/alpha test/scissor, and all
other bits zero.  The run adds no state enum.

## Engine allocation and teardown

Add:

```c
#include <linux/vmalloc.h>
#define CHROME9_RUN_STAGING_SIZE SZ_32K

struct chrome9_engine {
        ...
        void *staging_cpu;      /* existing one-page stage */
        void *run_staging_cpu;  /* new 32-KiB CPU-only stage */
        ...
};
```

Allocate `run_staging_cpu = vzalloc(CHROME9_RUN_STAGING_SIZE)` immediately
after the existing `get_zeroed_page()`.  A vmalloc address is page aligned,
which preserves the emitters' absolute-pointer 16/128-byte alignment.  It is
never DMA mapped and never receives a GART PTE.

There are exactly two freeing locations to update:

1. the `err_free_ring:` initialization unwind, before `free_page()` and
   `dma_free_coherent()`; and
2. the successful `chrome9_engine_fini()` tail, after the proved STOP/GART
   restore and before freeing the engine.

Use `vfree(NULL)` safely in the unwind.  Do **not** free it on any branch that
sets `wedged`, takes the quarantine module/device hold, and returns for reboot.
Those branches deliberately retain the whole engine, ring, stage, and
reachable mappings.  Likewise unsafe `engine_fini()` returns retain both
staging allocations.

## Sized finisher and append

Factor the current finisher without changing its emitted words:

```c
static int chrome9_finish_ring_sized(u32 *base, u32 *p, size_t capacity,
                                     u32 *pause, u32 *stop);

static int chrome9_finish_ring(u32 *base, u32 *p,
                               u32 *pause, u32 *stop)
{
        return chrome9_finish_ring_sized(base, p, PAGE_SIZE, pause, stop);
}
```

The sized helper must reject a non-dword/eight-byte-aligned capacity, `p`
before `base`, cursor arithmetic overflow, and a cursor beyond
`capacity - CHROME9_CR_READ_AHEAD` before writing closure words.  Its end
pointer is `base + capacity/4`; every pair write must be proven to fit.  Keep
the current do/while behavior: even an already 128-byte-aligned cursor emits
one dummy pair before testing alignment.  STOP must remain greater than
PAUSE, at least `0x20` bytes later, and at most
`capacity - CHROME9_CR_READ_AHEAD`.

Extend append explicitly rather than storing a mutable global current stage:

```c
static int chrome9_append_segment(struct chrome9_engine *engine,
                                  struct chrome9_job *job,
                                  const void *source,
                                  size_t source_capacity,
                                  u32 segment_pause,
                                  bool *must_quarantine);
```

Require `segment_stop + 0x220 <= source_capacity` with checked addition, then
copy from `source`.  Existing call sites pass
`engine->staging_cpu, PAGE_SIZE`; command `0x0f` passes
`engine->run_staging_cpu, SZ_32K`.  Retain the independent
`copy_len <= CHROME9_RING_SIZE` check.  The ring wrap, cache flush, A64/A65
publication, ownership update, and quarantine rules remain byte-for-byte
unchanged.

Do not replace `engine->staging_cpu` globally: the prime builder and all
existing one-page commands are part of the live no-wedge proof and should
remain unaffected.

## Builder change

Factor only the storage and limit parameters of
`chrome9_build_depth_triangle()`:

```text
base       old page or run_staging_cpu
capacity   PAGE_SIZE or SZ_32K
maximum    34 or 272
```

The existing wrapper passes the old values.  A new run wrapper passes the
large values and requires internal kind
`CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN`.  It must not accept
`CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH`.

Everything else stays common: H6 template, optional four-dword closed blend
block, D16 tuple, A20 `20000000`, A23 `23004f02`, A24
`24000000 | 3*T`, clip/scissor, 6-dword hardware vertex record, post-body
sync/status/dummy/cookie, and finisher.  Stream converted vertices directly
from the copied UAPI records into the 32-KiB stage; do not allocate a second
native array.

For the worst blend row:

```text
body_dw       = 174 + 18*T
T=272         = 5070 dwords
body end      = 0x4f58
post-body end = 0x4fc8
PAUSE         = 0x4ff0
STOP          = 0x5070
copied end    = 0x5290
A24           = 0x24000330
```

No-blend T=272 has body 5066 dwords and lands at the same PAUSE/STOP/copy
closure.  Runtime assertions should verify the calculated body length, A20,
A23, A24, immediate-data end words, post-body header words, PAUSE, STOP, and
copy bound.  A failure returns before append/publication.

## Handler and submit reuse

Refactor `chrome9_draw_triangle_batch()` to take an internal maximum and
fixed draw kind, or add a small common parser which both old and new handlers
call.  Do not cast the new public structure merely because its first version
has the same layout.  The run handler must complete these operations before
GEM lookup:

1. all fixed state/flag/pitch/format/blend/scissor/reserved validation;
2. `triangle_count` in `1..272` and nonzero pointer;
3. checked destination/depth guarded sizes and combined aperture size;
4. checked `triangle_count*3` and `vertex_count*20`;
5. one `memdup_array_user()` (maximum 16,320 bytes);
6. coordinate, Z, finite-positive RHW, and one nondegenerate test per three
   records.

The new kind then enters the existing `chrome9_submit_depth_triangle()`.
Select the large builder/stage only for that exact kind.  The submit helper
still performs one two-object `drm_exec`, one fence, one contiguous transient
SF mapping, one guard/cache ownership interval, one append, and one
`chrome9_wait_job()`.  There is no segment loop, partial-prefix success, or
multiple-cookie state to unwind.  Every existing prepublication failure
invalidates the one transient mapping; every post-publication timeout retains
the complete job and reachable mapping for reboot exactly as today.

## Exact static layout verifier

Add a host-side verifier before the kernel implementation is deployed.  Its
model must reproduce, in order:

1. initial H0 sync and body alignment at `0x20`;
2. `174 + 18*T` worst-case body;
3. 16-byte alignment plus H0 sync (24 bytes), H2 status (32), H2 CR dummy
   (16), and H0 cookie (24);
4. the finisher's do/while 8-byte dummy pairs and 128-byte alignments;
5. `copy_end = STOP + 0x220`.

Required golden rows are:

| T | body dw | body end | post-body end | PAUSE | STOP | copy end |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 192 | `0x0320` | `0x0388` | `0x03f0` | `0x0470` | `0x0690` |
| 2 | 210 | `0x0368` | `0x03d8` | `0x03f0` | `0x0470` | `0x0690` |
| 34 | 786 | `0x0c68` | `0x0cd8` | `0x0cf0` | `0x0d70` | `0x0f90` |
| 35 | 804 | `0x0cb0` | `0x0d18` | `0x0d70` | `0x0df0` | `0x1010` |
| 64 | 1326 | `0x14d8` | `0x1548` | `0x1570` | `0x15f0` | `0x1810` |
| 128 | 2478 | `0x26d8` | `0x2748` | `0x2770` | `0x27f0` | `0x2a10` |
| 272 | 5070 | `0x4f58` | `0x4fc8` | `0x4ff0` | `0x5070` | `0x5290` |
| 273, reject in handler | 5088 | `0x4fa0` | `0x5008` | `0x5070` | `0x50f0` | `0x5310` |

The 35-row deliberately proves why the old page path cannot merely raise its
maximum: copied end `0x1010` exceeds 4 KiB.  The 273-row still fits 32 KiB,
but must fail because 272 is the public audited maximum, not because an
accidental buffer boundary stops it.

An offline kernel-builder self-test should fill the large stage with poison,
build each accepted boundary, verify every keyed word/offset above, ensure
the legal alternating dummy pattern continues from the finisher through the
full staging capacity, and ensure allocation canaries outside the staging
range remain unchanged.  The append verifier separately checks that only the
exact `STOP+0x220` prefix is copied into the ring.  Run it under KASAN once.
Separately compare every existing one-page builder before/after the
sized-finisher refactor; their complete copied byte ranges must be identical.

## Live oracle update

Extend the standalone batch tool with a distinct command-`0x0f` mode; do not
make the old command silently use the new maximum.  Generate a 17-by-16 grid
of 272 nonoverlapping right triangles, each with a deterministic per-index
color, Z=0.5, RHW=1.0, and alternating far/near D16 prefill cells.  Sample one
interior pixel per triangle, an uncovered pixel, all four guard pages, and
require the expected alternating depth pass/fail result.

Use this promotion ladder, one new maximum/state per reboot initially:

```text
2               distinguish independent-list color/vertex latching
34              old maximum equivalence
35              first command which crosses the old staging page
64, 128         intermediate front-end/list-count gates
272             audited maximum, A24=0x24000330
273             reject before append/PTE counters change
```

Then submit four maximum runs after a fresh module load.  Three fit the
64-KiB monotonic epoch; the fourth must take exactly one bounded PID10/full
wrap and retire.  Require append `+4`, wrap/epoch `+1`, exact image/depth,
guards unchanged, `040c=0`, a successful legacy command-`0x0c` triangle,
and clean unload/reload.  Repeat the final gate for LESS/write,
LEQUAL/read-only, source-over blend, alpha test, BGRA destination, and the
widest accepted scissor, changing only one state per initial run.

## Minimum phase counters

Before interpreting FPS, add these counters/timers under `submit_lock`:

```text
command and triangle count
job_allocs, pte_alloc_bytes, sf_node_insert/remove
gart_bind/unbind calls and PTE dwords
vga_get_ns total/max
gart preflush-idle ns, PTE-write ns, SR6F-completion ns
explicit_cache_flush calls/bytes/ns
dma_sync device/CPU/scanout calls/bytes/ns
guard prepare/verify bytes/ns
ring copy bytes, cookie wait ns, PAUSE/work-idle ns, wraps
```

Give `chrome9_gart_set()` an internal reason enum (ring, scanout, submit bind,
submit unbind, eviction) so initialization/KMS activity does not pollute draw
counts.  Keep timing disabled by default if its overhead is visible.  Expose
a lockless/read-only debugfs snapshot or print one aggregate line at safe
engine teardown; reading counters must never take VGA, VM, or submit locks.

For one successful command-`0x0f` run in transient mode, expected deltas are:
one job/fence, one bind and one unbind, 2,312 PTE writes on the live layout,
two VGA gets/SR6F pulses, one cache/guard ownership round, one cookie/PAUSE
retirement, and one append.  A later persistent-cache hit must reduce PTE
writes, VGA gets, SR6F pulses, and SF-node/PTE allocations to zero while
leaving one cache/guard ownership and one retirement until separately proven
safe to remove.

## Commit/build/deploy order

1. UAPI command/capability/structure/maxima and static size assertions.
2. Kernel counters only; build and capture a 34-triangle glxgears baseline.
3. Kernel run stage, sized finisher/append, and byte-equivalence/static layout
   tests, with capability still absent.
4. Kernel run handler/builder/ioctl/capability; `W=1` module build,
   `checkpatch`, UAPI 32/64-bit size/ioctl checks, KASAN host self-test.
5. Standalone live oracle ladder and wrap/reload gate.
6. Mesa UAPI copy/wrapper/outer-draw accumulation only after kernel silicon
   validation.
7. Re-measure phase counters; persistent SF/GART mapping is the next isolated
   kernel patch, not part of command `0x0f`.

This order leaves command `0x0c`, the one-page staging builder, transient
mapping, and current cache/guard behavior intact as a complete fallback and
post-failure health oracle.
