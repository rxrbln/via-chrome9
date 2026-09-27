<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 multi-job in-flight queue

Update 2026-09-27: merged as kernel `73e7e08`; `6c823ec` made
`max_inflight=8` the default.  The design notes below predate the merge.

Status: implemented as an unapplied kernel patch,
`~/tmp/agent-mq/multi-inflight.patch` (`-p1` from the `linux-7.2` root,
against the chrome9 tree at `38e43f1` including the uncommitted segment
state-cache work present on 2026-09-25; only `chrome9_engine.c` changes).  Built out of tree with `W=1`
without warnings; the existing host self-tests and a new cookie-window
self-test pass.  **Not silicon-tested.**  The default `max_inflight=1`
keeps the current single-job behaviour.

Motivation (see [chrome9-performance-20260924.md](chrome9-performance-20260924.md),
[chrome9-present-path.md](chrome9-present-path.md) section C and
[chrome9-per-draw-state.md](chrome9-per-draw-state.md)): ioquake3 GL1 runs at
40 fps with the system 65 % idle.  A `DRAW_VERTEX_PROGRAMS` job (about 25
draws, about 4 jobs per frame) costs about 1.2 ms of CPU (entry 239, compile
303, map 149, gart 270, flush 39, build 75, append 138 us) and about 3.25 ms
of GPU time.  With one job in flight, the CPU work of job N+1 overlaps job N
only until `submit_lock`, which first retires job N.

## 1. Baseline: how one job is retired today

* `chrome9_submit_lock*()` retire `engine->inflight` (a single pointer)
  before any other work under `submit_lock`.
* `chrome9_append_segment()` requires the reader at its owned PID00 PAUSE
  (`0444.bit31`) and non-CR idle (`0400 & CHROME9_ENGINE_WORK_MASK == 0`)
  before copying the next segment to `engine->append` and writing only
  A64/A65.  `append` is `ALIGN(ring_stop + CHROME9_CR_READ_AHEAD, 128)`, so
  the published PAUSE/STOP closure and its read-ahead are never rewritten
  within a ring epoch.  A segment that does not fit takes the wrap: PID10
  STOP, full idle, dummy fill, A60..A68 reinstall
  ([monotonic-command-ring.md](monotonic-command-ring.md),
  [cr-pause-stop-lifecycle.md](cr-pause-stop-lifecycle.md)).
* Every segment starts with H0 `006c=00800000` (3D wait,
  `chrome9_fragment_segment_begin()`) and ends with `chrome9_emit_3d_tail()`:
  optional FE33 system write (`fe33_fence=1`, wait mask `00200000`), H0
  `006c=00800000`, the A1C/A1D occlusion-counter reset, an H2 dummy and the H0
  `DST_POS` cookie ([fe33-persistent-ring-production.md](fe33-persistent-ring-production.md)).
* `chrome9_wait_job()` proves retirement in this order: exact FE33 ID in the
  single coherent slot (when enabled), exact cookie in `DST_POS`,
  `0444.bit31`, non-CR idle.  Only then are guards verified, mappings
  released and the fence signalled.
* The cookie alternates `00010001`/`00020002`, and the FE33 slot is poisoned
  with `~id` for every job.  Both are unique only for one outstanding job,
  so at most one job can be in flight by construction
  ([chrome9-throughput-re.md](chrome9-throughput-re.md) K13).

## 2. Design

### 2.1 Parameters

| parameter | default | range | meaning |
|---|---|---|---|
| `max_inflight` (0400) | 1 | 1..64 | published fragment/vertex-program jobs outstanding before a submitter waits |
| `ring_pages` (0400) | 16 | 16..256 | command ring size in pages (64 KiB .. 1 MiB) |

Engine init logs `ring=<dma>/<KiB> KiB fe33=on|off max_inflight=N`.  Values
outside the range are clamped.

A roughly 35 KiB ioquake3 segment wraps the 64-KiB ring every second job, and
a wrap drains the queue (section 2.3), so `max_inflight > 1` is only useful
together with a larger ring; `ring_pages=64` (256 KiB, about 7 such jobs per
epoch) is the suggested first setting.

### 2.2 Queue and locking

* `engine->queue` is a list of published, unretired jobs in ring order
  (`job->queue_link`), with `engine->queued` and the limit
  `engine->max_inflight`.  No fixed-size array is used.
* `chrome9_submit_lock()` / `chrome9_submit_lock_interruptible()` now
  **drain** the whole queue (`chrome9_engine_retire_all_locked()`).  Every
  synchronous path (COPY_RECT, triangle ioctls, fixed quad, SF pin/unpin,
  KMS display begin, timeout-segment read, teardown) therefore still sees an
  idle engine, exactly as before.
* Only `chrome9_submit_fragment_programs()` (DRAW_FRAGMENT_PROGRAM,
  DRAW_VERTEX_PROGRAM, DRAW_VERTEX_PROGRAMS) uses
  `chrome9_submit_lock_queue_interruptible()`: it retires already completed
  jobs without waiting, then blocks only while `queued >= max_inflight`,
  retiring the oldest job.  Only the last segment of a job whose objects all
  have a persistent kernel mapping (the existing `async` condition) is
  queued; every other segment keeps the synchronous path after a drain.

### 2.3 Ring space and live append

* With an empty queue `chrome9_append_segment()` keeps the proven
  precondition (owned PAUSE, non-CR idle).
* With queued jobs the append is a *live* A64/A65 advance: the reader may
  still be executing an earlier segment.  The new bytes are copied to
  `engine->append`, which lies beyond the previous STOP trailer plus
  `CHROME9_CR_READ_AHEAD`, flushed, ordered by `dma_wmb()` and published by
  A64/A65 only.  No byte the reader may have fetched is written.
* Space accounting is unchanged and monotonic: `ring_size - append`.  If the
  segment does not fit, the queue is drained first (retire all, which proves
  the tail's PAUSE and idle), then the existing wrap runs with its unchanged
  STOP/full-idle/reinstall preconditions.  Ring bytes are never reused while
  any job is queued.
* The single 64-KiB `run_staging_cpu` stays sufficient: a segment is built
  and copied into the ring under `submit_lock`; queued jobs live in the ring,
  not in staging.
* `ring_pages` replaces the compile-time ring size wherever the ring is
  addressed (GART ring PTEs, A62 end, dummy fill, cache flush, draw-cache
  page bound, `sf_mm` start, diagnostics).  The per-ioctl object-size
  pre-checks keep the 64-KiB constant; they are upper bounds only and
  `drm_mm` insertion remains authoritative.

### 2.4 Per-job completion

**FE33.**  With `fe33_fence=1` each ID uses its own 16-byte slot in the
coherent fence page, `slot = id % fence_slots` with
`fence_slots = max_inflight` (one slot, identical to today, for
`max_inflight=1`).  IDs stay monotonic and never wrap.  Before poisoning a
slot, `chrome9_emit_fe33()` checks the queue for a job using the same slot
(possible only when IDs were consumed by unpublished segments) and drains in
that case.  The canary words stay per slot.

**Cookie.**  Cookies are `n | n << 16`, `n` in `1..max_inflight + 1`.  The
next cookie follows the newest *published* cookie (queue tail, or
`last_cookie` when empty), so the last retired cookie and all queued cookies
form a contiguous run of at most `max_inflight + 1` distinct values.  For
`max_inflight=1` this reproduces the legacy alternation exactly, including
`00010001` after any unrelated initial `DST_POS` value.  The init-time
`chrome9_cookie_selftest()` models the window for depths 1, 2, 3, 8 and 64.

**Proof per job** (`chrome9_job_done_locked()`, non-blocking):

1. FE33 enabled: the job's own slot holds exactly its ID;
2. `DST_POS` equals the cookie of this job or of a later queued job;
3. tail job only: `0444.bit31` and non-CR idle.

Step 2 is sound because the ring executes in order, the cookie window is
unique, and `DST_POS` can only hold `last_cookie` (nothing queued has
reached its tail) or a queued cookie; the 2D paths that also write
`DST_POS` (COPY_RECT) run only with an empty queue.  A non-tail job cannot
show PAUSE/idle because later work may already run; its proof is the tail
order FE33 (3D wait) -> `006c` 3D wait -> cookie, i.e. the job's 3D work
drained before its cookie write executed (hypothesis H-MQ2).

**Retirement.**  Jobs retire strictly from the head:
`chrome9_job_retire_locked()` does the existing post-job work
(`chrome9_fragment_job_finish_locked()`: CPU sync, guard flush/verify,
occlusion snapshots, mapping release), then signals the fence.
`chrome9_engine_retire_all_locked()` waits for the tail with the unchanged
bounded `chrome9_wait_job()` and then retires every job in order.

**Worker.**  For `max_inflight=1` `chrome9_retire_work()` is unchanged.
Otherwise it polls without holding `submit_lock` between polls, so
submitters append while the GPU runs.  Since v2 the sleep is adaptive
(`chrome9_retire_delay_us()`): the worker sleeps through most of the
expected head-job time (`job_us_avg`, a running average of the time a job
spends at the head of the queue), then backs off with `elapsed / 8`, bounded
to 50..2000 us.  A late completion is therefore noticed within about 1/8 of
its runtime, with two to three wake-ups per job.  A head job older than
`CHROME9_HW_TIMEOUT_US` is handed to the blocking, logged path.  A submitter
that finds the queue full sleeps on `retire_wq` without `submit_lock`
(woken on every retirement, 100 ms timeout loop) instead of polling.

### 2.5 Timeout and quarantine

* A timeout proves nothing about the unfinished jobs: the tail is waited
  with full diagnostics (`job timeout ... queued=N`, FE33 slot dump,
  captured segment), the proven prefix still retires normally, and the first
  unproven job and every job behind it are quarantined with `-ETIME`
  (objects, mappings and ring retained until reboot, as today).
* A non-tail job that fails its proof after the tail was proven logs
  `queued job unproven seq=... fe33=... cookie=.../... tail=0` and is
  quarantined with everything behind it.
* After quarantine the engine refuses new submissions, so no job is ever
  appended behind an unproven one.

### 2.6 Dependencies

**GPU after GPU.**  With `max_inflight > 1` the fragment path's dependency
wait (`chrome9_resv_wait_deps()`) skips fences of the engine's own
`fence_context` and waits only for foreign fences.  Ordering then relies on:

* in-order ring execution;
* `006c=00800000` at the start of every segment and in every tail;
* the type-FE A00 group with bit 0 set (texture cache clear), emitted by
  `chrome9_emit_fragment_draw()` before the primitive of every draw, and with
  the segment-local state cache (`state_cache=1`, in development) at least
  by the first draw of every segment, since that cache is reset at each
  segment start
  ([chrome9-desktop-r2t-ordering-audit.md](chrome9-desktop-r2t-ordering-audit.md));
* the read-colour and reading-Z caches left disabled per
  [resource-layout-clear-cache.md](resource-layout-clear-cache.md) (driver
  policy, not re-verified by this patch).

Whether this suffices for render -> sample without a CPU retirement in
between is hypothesis H-MQ4 (the present-path H4).  An own fence is always
either published before the new job (it was added under `submit_lock` in the
same critical section as its publication, while the submitter held the
object's reservation) or already signalled with an error.

**CPU preparation of a submission.**  Guard rewrites, dirty/implicit cache
flushes and streaming DMA syncs of bounced pages (`dma_need_sync()`) must not
run while a queued job writes the object.  Before those steps, every object
that needs any of them (not explicit, CPU-dirty, guard cache not valid,
bounced, or a vertex buffer) and still has an unsignalled WRITE fence causes
a drain.  In the steady ioquake3 state (explicit targets and textures with
valid guard caches, vertex buffers without write fences) nothing drains.

**GART.**  A draw-cache miss (bind and eviction) drains the queue first, so
PTE writes and SR6F flushes keep the proven idle-engine precondition, and
`chrome9_gart_kick()` returns `-EBUSY` while jobs are queued.  PTE updates
while the engine runs other jobs are therefore never required.

**Occlusion snapshots.**  Queued jobs keep their VRAM snapshot slots until
retirement; a submission drains first when fewer free slots remain than it
has draws.

**CPU access.**  `GEM_CPU_ACCESS` still waits on the object's reservation.
The worker signals each job as soon as its own proof holds, so a CPU access
waits for exactly the jobs that reference the object, not for the queue.

**Synchronous ioctls.**  COPY_RECT and all other synchronous paths drain at
`submit_lock`; their reservation waits (before the lock) wait on queued
fences, which the worker signals without needing any reservation lock.

### 2.7 dma_fence order

* `dma_resv_add_fence()` replaces an earlier fence of the same context when
  the new one is later and of equal or stronger usage.  That is correct only
  if fences of one context signal in seqno order.
* Seqnos are therefore now assigned under `submit_lock`
  (`chrome9_job_fence_init_locked()`, all three submit paths), so seqno
  order equals publication order.  Before this change the seqno was taken
  before the lock, so two submitters could publish out of seqno order (a
  latent issue with the existing asynchronous retirement as well).
* Queued jobs signal from the head only.  Synchronous jobs drain first.
* A fragment job that fails before publication may already have replaced a
  queued job's fence in a reservation; its error fence is signalled only
  after draining (`CHROME9_DRAIN_ERROR`).
* Jobs without an initialized fence (failure before `submit_lock`) are
  freed by `chrome9_job_put()`.

### 2.8 max_inflight=1

The queue never holds more than one job, `chrome9_submit_lock_queue_*`
retires it with the same `chrome9_wait_job()` oracle, the append
precondition is always checked, the cookie alternates as before, one FE33
slot is used, the own-fence skip is off and the worker is the old one.
Remaining differences: seqnos are taken under the lock, and a completed
in-flight job may be retired by one non-blocking check before the blocking
wait.

### 2.9 Diagnostics

With `profile=N` and `max_inflight > 1`, every N jobs a `profile queue:` line
reports totals since load: live appends, waits for a free queue entry, and
drains by cause (`lock`, `gart`, `prepare`, `wrap`, `sync`, `slot`, `query`,
`error`).  Compare two consecutive lines for per-interval numbers.

## 3. Safety arguments

**S1 Ring bytes.**  A byte is written only at `>= engine->append`, which is
beyond every published PAUSE, STOP trailer and read-ahead window of the
current epoch; a new epoch starts only after the drain proved the tail's
PAUSE and idle and PID10 STOP proved full idle.  Hence no fetched or
fetchable byte is modified, with or without queued jobs.

**S2 Control registers.**  With queued jobs only A64/A65 are written, always
forward and at least one closure beyond the previous PAUSE.  A60..A63,
A66..A68 are written only in the wrap, which runs with an empty queue.

**S3 Completion uniqueness.**  At any time the values
`{last_cookie} ∪ {cookies of queued jobs}` are pairwise distinct (contiguous
run of length at most `max_inflight + 1` in a cycle of that length; host and
init self-test).  FE33 slots of queued jobs are pairwise distinct (checked
before every poison).  A job is retired only on its own FE33 ID and on a
cookie that the in-order ring can write only after that job's tail.

**S4 Memory ownership.**  Objects of a queued job stay referenced
(`fsub->objects`), mapped in the draw cache (`users > 0`, not evictable) and
kernel-mapped until its retirement; a GART change happens only with an empty
queue.  Post-job CPU work of job N touches a buffer that a later queued job
may write only as guard reads, guard-line flushes of clean lines and
no-op streaming syncs; bounced objects drain (section 2.6).  Guard failures
are still detected, but a corruption by job N+1 may be attributed to job N
when both write the same object (both receive `-EIO` in the worst case).

**S5 CPU visibility.**  A CPU access waits for all fences of the object; a
fence signals only after its job's proof and post-job CPU sync.  Because
fences of the context signal in seqno order and seqno order equals ring
order, a replaced (older) fence in a reservation is always implied by the
remaining newer one.

**S6 Failure containment.**  No job is published behind an unproven job; a
timeout quarantines the unproven suffix without releasing any of its
resources; the proven prefix is complete by S3.

**S7 Liveness.**  Every loop that waits for a free entry removes the head
(retired or quarantined); every hardware wait is bounded by
`CHROME9_HW_TIMEOUT_US` per phase; the worker exits when the queue is empty.

## 4. Hardware hypotheses (to be proven on silicon)

| id | hypothesis | risk | evidence so far |
|---|---|---|---|
| H-MQ1 | A PID00 A64/A65 advance while the reader is still executing earlier segments (not yet at PAUSE) continues seamlessly into the new segment | medium | the 5.75 Linux KMD advances only A64/A65 with no idle requirement (`kickoff_dma_ring_inv()`, [via-5.75-kmd.md](via-5.75-kmd.md)) and keeps several submissions outstanding behind monotonic fence IDs; this driver has so far advanced only from an owned PAUSE |
| H-MQ2 | For a non-tail job, FE33 (wait `00200000`) and/or the cookie after `006c=00800000` prove that its colour, depth and snapshot writes are in memory | medium | vendor uses FE33 with the 3D wait as the production fence; `006c` is the vendor "CR wait chip idle" packet |
| H-MQ4 | Render -> sample across jobs needs no CPU retirement: in-ring order, `006c` and the A00 texture-cache clear suffice | medium | the audited vendor paths emit no further barrier ([resource-layout-clear-cache.md](resource-layout-clear-cache.md)) |
| H-MQ5 | `DST_POS` stores and returns `00030003` .. `00410041` exactly | low | only `00010001`/`00020002` observed |
| H-MQ6 | Ring sizes above 64 KiB (A62 end, more ring PTEs) behave like the 64-KiB ring | low | the A62/A63 end encoding carries 32 address bits; not exercised above 64 KiB |

H-MQ3 (GART updates while the engine runs) is avoided by the GART drain.

## 5. Host verification

```sh
source /etc/profile
make -C <linux-7.2> M=~/tmp/agent-mq/linux-7.2/drivers/gpu/drm/chrome9 \
	CONFIG_DRM_CHROME9=m W=1 modules          # no warnings
bash tools/re/chrome9_kraster_selftest.sh ~/tmp/agent-mq/linux-7.2
                                           # tcl raster 0, fe config 0
~/tmp/agent-mq/mq_selftest.sh ~/tmp/agent-mq/linux-7.2
                                           # cookie window 0
```

The cookie harness was checked to fail (`-22`) when the span is reduced to
`max_inflight`.  `patch -p1 --dry-run` applies cleanly to the current tree.

## 6. Silicon test plan

Rules: every GPU client runs as
`flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ...`; module changes go
through `tools/chrome9_module_handoff.sh` (hash-gated, `NEW_PARAMS`,
`flock -x -o`); no forced unbind after a timeout, cold boot (NanoKVM) instead.
Before and after each step record `dmesg` delta, module Build ID and Xorg
PID.  Client environment as in `tools/chrome9_glmark_matrix.sh` plus
`CHROME9_TCL=1`.  Stop at the first failure signature.

Short form used below:

```sh
G="flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7"
```

`chrome9_mq_bench W H quads reps targets [explicit]`
(`~/tmp/agent-mq/tools/chrome9_mq_bench.c`, built on the target with
`cc -O2 -D__user= -I<linux>/include/uapi/drm`) submits back-to-back fills
into `targets` destinations in turn, each job with its own colour, then
reads every target back through `GEM_CPU_ACCESS` and compares 16 pixels with
the colour of the last job that wrote it.  With `explicit=1` (default) the
targets use the explicit CPU access protocol, so jobs can queue; `explicit=0`
is the draining control.  `chrome9_fill_bench` uses implicit objects and
therefore always drains.

| step | module parameters (`NEW_PARAMS`) | command | expected |
|---|---|---|---|
| M0 | `hw_tcl=1 fe33_fence=1` (defaults `max_inflight=1 ring_pages=16`) | `$G ./chrome9_gl_rtt_smoke`, `$G ./chrome9_gl_mip_smoke`, `$G ./chrome9_gl_occlusion_smoke`, `$G ./chrome9_fill_bench 800 600 16 50`, `$G ./chrome9_mq_bench 800 600 16 100 2` | init line `max_inflight=1`, results identical to the unpatched module (`rtt: 0 mismatches`, `occlusion: 0 mismatches`, same Mpixel/s ±5 %), `0 mismatches` |
| M1 | `hw_tcl=1 fe33_fence=1 ring_pages=64 profile=50` | `$G ./chrome9_mq_bench 800 600 2 200 2` | init line `ring=.../256 KiB`; no timeout, `0 mismatches`, us/job within 5 % of the same command with `ring_pages=16` (H-MQ6) |
| M2 | `hw_tcl=1 fe33_fence=1 max_inflight=2 ring_pages=64 profile=50` | `$G ./chrome9_mq_bench 800 600 2 200 1`, then `... 2 200 2`, then `... 2 200 2 0` | `0 mismatches`; `profile queue:` live appends growing for the explicit runs and not for the `explicit=0` run; no `job timeout`, no `queued job unproven`; us/job below M1 for the explicit runs (H-MQ1, H-MQ2) |
| M3 | as M2 | `$G ./chrome9_gl_rtt_smoke`; `$G ./chrome9_gl_mip_smoke`; `$G ./chrome9_gl_cube_dxt_smoke` | 0 mismatches each (H-MQ4) |
| M4 | as M2 | `$G ./chrome9_gl_occlusion_smoke`; `$G ./chrome9_gl_stencil_smoke`; `$G ./chrome9_gl_topology_smoke` | 0 mismatches; occlusion counts equal M0 |
| M5 | `hw_tcl=1 fe33_fence=1 max_inflight=2 ring_pages=16 profile=50` | `$G ./chrome9_mq_bench 800 600 64 40 2` (about 90 ms GPU per job) | `drains wrap` growing, `0 mismatches`, no timeout (drain-then-wrap path) |
| M6 | `hw_tcl=1 fe33_fence=0 max_inflight=2 ring_pages=64 profile=50` | repeat M2 and M3 | same results with the cookie-only proof |
| M7 | `hw_tcl=1 fe33_fence=1 max_inflight=8 ring_pages=64 profile=50` | `$G ./chrome9_mq_bench 800 600 4 400 4`; M3 smokes | `full waits` growing (queue reaches 8), cookies up to `00090009` retire (H-MQ5), `0 mismatches` |
| M8 | best of M2/M7 | `OUT=... LIB=... VTIME=7 FTIME=7 DUR=4 tools/chrome9_glmark_matrix.sh` (default scene list) | validation results equal to the `max_inflight=1` matrix; no `STOP` line |
| M9 | best of M2/M7 | ioquake3 q3dm1 GL1 and GL2 via `~/tmp/c9bench.sh` (`CENV="CHROME9_TCL=1"`, `SEC` shortened to fit a 7 s bound per run) with `profile=40` | screenshots identical in content to `max_inflight=1`; fps above 40 (GL1); profile shows few drains per frame |

Failure signatures and their meaning:

| signature | meaning / next step |
|---|---|
| `job timeout phase=cookie ... cookie=<old>/<new> queued=N` with `cr=60000000`-like active state | reader did not continue after a live append (H-MQ1 false); rerun M2 with `max_inflight=1` to confirm, keep default |
| `job timeout phase=cookie` with `DST_POS` holding a truncated value (for example `00030000`) | `DST_POS` does not retain `n >= 3` (H-MQ5 false); limit `max_inflight` to 1 or change the cookie encoding |
| `job timeout phase=fe33` plus `FE33 id=... slot=... [~id ...]` | FE33 for a queued job not written; check slot address in the dump; compare with M6 |
| `queued job unproven seq=... fe33=... cookie=.../... tail=0` | tail proven but an earlier job's FE33 slot or cookie is inconsistent (slot aliasing or FE33 ordering); keep log and captured segment |
| `FE33 id=... wrote beyond its ID dword` | canary damage in a neighbouring slot; FE33 write width larger than assumed |
| `PID10 STOP timeout` after `drains wrap` | wrap after a drained queue failed; compare with the unpatched wrap path (M5 with `max_inflight=1`) |
| smoke mismatches only with `max_inflight > 1` | GPU->GPU ordering insufficient (H-MQ4 or H-MQ2); next discriminator: force a drain on render-target changes |
| guard `-EIO` (fence error) only with `max_inflight > 1` | post-job work raced a later job (S4); report object kind and job pair |

## 7. v2: silicon results of v1 and changes

First silicon run of v1 on driver `98fa4fd` (coordinator report,
2026-09-25): M0..M4 passed (all smokes 0 mismatches, no timeouts); with
`max_inflight=2 ring_pages=64` `chrome9_mq_bench` showed 196 live appends in
200 jobs.  ioquake3 GL1/GL2 stayed at 41/29 fps.  Two defects:

**NULL dereference at ioquake3 exit.**  `drm_gem_shmem_vunmap_locked+0x5`
with `RDI=0` from `chrome9_submit_fragment_programs()`.  In the
asynchronous path v1 released `submit_lock` after adding the job to the
queue and only then unmapped the job's temporary kernel mappings through
`job->fragment_objects[i]`.  The polling worker could retire and reap the job
in between: `chrome9_engine_reap()` clears `fragment_objects[]`, drops the
object references and the last fence reference, which frees the job.  The
submitter then passed `NULL` (or freed memory) to the unmap.  The same window
existed before this patch with `async_retire`, but the old worker only ran
after the unmap and any other retirer had to wait for the mutex, so it was
not observed.  v2 unmaps before the job becomes visible in the queue; the
persistent kernel mapping keeps `fragment_maps[].vaddr` valid for the
post-job work, as before.  After `chrome9_submit_unlock()` the submitter no
longer touches the job.

**CPU load.**  With `max_inflight=8` the system was fully busy.  Two
pollers: the worker (20..50 us sleeps) and, more expensively, every
submitter that found the queue full, which polled the head job every
3..10 us under `submit_lock` (`read_poll_timeout(..., 10, ...)` in
`chrome9_engine_retire_head_locked()`); ioquake3 reported 3061 such waits.
v2 replaces both (section 2.4).  Estimated cost: about three wake-ups per
3 ms job, each a mutex, one coherent read and at most three MMIO reads,
well below 1 % of one core (**E**, to be measured with `perf stat`).

### Interrupt-driven retirement (design, not implemented)

Documented source (static RE, not live-proven):
FE33 mode bit 1 (`FE330006` = 3D wait + system write + interrupt, no
interrupt-wait) raises the CR interrupt `0204.bit5` (RW1C, acknowledge by
writing only `0x20`); the global enable is `0200.bit31`, written as
`(old & f7ff0800) | 80000000` because `0200` mixes enables with RW1C status;
the Windows ISR samples, acknowledges, then reads the fence IDs
([via-5.75-kmd.md](via-5.75-kmd.md), [chrome9-fe3x-completion.md](chrome9-fe3x-completion.md)).
The driver has no interrupt handler today, and the IGA vblank interrupt is
also unproven.

Proposed design, behind `fe33_irq=0` default:

1. `request_irq(IRQF_SHARED)` handler: read `0200`; return `IRQ_NONE` unless
   bit 31 is set and `0204.bit5` is set; write `0x20` to `0204`; wake the
   retire worker (`queue_work`).  No fence or queue state in hard IRQ
   context.
2. Enable only after init proved `0204.bit5` clear or acknowledged it once in
   process context.
3. Queued jobs use `FE330006`; never interrupt-wait (bit 4) or queue-skip
   (bit 5), so a lost interrupt cannot stop the reader: the adaptive poll of
   v2 stays as the fallback with a longer first sleep (for example twice the
   expected job time).
4. Silicon ladder: (a) interrupt count equals FE33 completions for 100
   isolated jobs with the queue disabled; (b) masked global enable produces
   no interrupt and no reader stall; (c) queued jobs with the worker woken
   only by the interrupt; (d) `perf stat` CPU and fence latency against v2
   polling.

Until (a)..(c) pass on silicon, adaptive polling is the production path.

### Why ioquake3 did not speed up

Kernel side the queue works (8289 live appends), but the CPU is still
serialised with the GPU by Mesa (static audit of
`src/gallium/drivers/chrome9/`, Mesa 26.2.2 tree):

1. **Client-array uploads, every draw (main cause).**  `chrome9_tcl.c`
   uploads client arrays through `u_upload_data_ref()` and unmaps after
   every draw.  The uploader maps `WRITE|UNSYNCHRONIZED`, but
   `chrome9_transfer_map()` (`chrome9_resource.c`) still calls
   `chrome9_resource_cpu_access(WRITE)`, i.e. `GEM_CPU_ACCESS PREPARE_WRITE`
   over the whole 1 MiB upload buffer.  The kernel ioctl waits for **all**
   fences of the object (`DMA_RESV_USAGE_BOOKKEEP`) before looking at the
   range, and every submitted TCL job holds a fence on that buffer.  So the
   first draw after each batch submit blocks until the previous batch has
   retired: the CPU builds batch N+1 only after batch N finished, and at most
   one job of the client is really in flight.  The counters show the queue
   still filled at times (3061 full waits); which submitter filled it
   (Xorg, swap flush, or a batch without client arrays) is not visible in
   the counters.
2. **Swap throttle.**  `dri_drawable.c` throttles every SwapBuffers with
   `fence_finish`, which `chrome9_fence_finish()` implements as
   `GEM_CPU_ACCESS WAIT` on `last_target` (all fences).  With DRI3 back-buffer
   rotation this allows one frame in flight; with a private guarded shadow
   target it drains the GPU every frame.
3. Texture `transfer_map`/subdata and mipmap generation flush and wait, but
   not per frame in normal play.

Required changes (not part of this kernel patch):

* Mesa: skip the waiting `cpu_access` for `PIPE_MAP_UNSYNCHRONIZED` maps of
  buffers (the uploader never overwrites a range still referenced), but
  still record the CPU-dirty range for the flush.
* Kernel UAPI: a `GEM_CPU_ACCESS` flag that records the dirty range and does
  the cache maintenance without the reservation wait (for unsynchronized
  sub-allocation), and let `PREPARE_READ` wait only for WRITE fences
  (`dma_resv_usage_rw(false)`).  Both are small, but they change the UAPI
  contract and need their own review.
* Mesa: keep the stream uploader mapped across draws to save one ioctl per
  draw.

Until these land, measure the kernel queue with `chrome9_mq_bench` and
glmark2, not with ioquake3.  The profile `head job` time from v2 gives the
GPU time per job directly, so it shows whether a frame is GPU-bound
(8 jobs x head time close to the frame time).

## 8. Expected effect and limits (estimates)

* Up to about 1.2 ms of CPU per job leaves the serial path, about 4.8 ms per
  ioquake3 frame of about 25 ms (**E**); the gain is bounded by wraps,
  draw-cache misses, CPU accesses and Xorg's synchronous paths, all of which
  drain.  The profile drain counters measure these directly.
* Polling costs a few MMIO reads every 20..50 us while jobs are queued; an
  FE33 interrupt path would replace it.
* Implicit (non-explicit) render targets, such as Xorg's, drain on every
  job and gain nothing.
* Follow-ups: IRQ completion (FE330006), segment-local state caching to
  shrink segments, wrap without a drain once PID01 JUMP is proven.
