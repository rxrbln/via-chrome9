# Chrome9 vendor persistence and state-reuse audit

> **Erratum (2026-09-24):** `__glS3InvDrawPrimitiveTriangleList` (`0x209d10`) is the software-VS (pre-transformed) emitter; VX900 (UMD family 8) uses the hardware-VS `__glS3H5DrawPrimitiveTriangleList` (`0x1e7490`).  See [chrome9-throughput-re.md](chrome9-throughput-re.md#3-linux-575-stack-vx900--umd-chip-family-8).

## Result

The proprietary drivers do **not** rebuild SF translations or replay the
complete 3D state for every primitive.  They use three independent lifetime
domains:

1. SF/GART translations remain resident for the allocation lifetime;
2. the software hardware-state image remains valid until an explicit context
   or ownership loss, while successful draws clear only dirty metadata;
3. command and resource cache ownership is synchronized when bytes are
   published or consumed, independently of the translation lifetime.

The modern driver can therefore safely remove per-draw PTE allocation,
`drm_mm` insertion/removal, VGA arbitration, PTE writes and `SR6F`
invalidation on a persistent-mapping cache hit.  It can also emit invariant
state once inside a validated multi-triangle run.  It must **not** yet skip
CPU/device ownership transitions, dependency waits, retirement, guard
verification, or state replay across independent ioctls.  Cross-ioctl state
omission requires a kernel-owned state generation and owner token because
another context, a reset, or another engine path may have changed the global
3D state after `submit_lock` was released.

This note supplements
[persistent-batched-submit-performance-plan.md](persistent-batched-submit-performance-plan.md).
It adds exact vendor state-shadow and branch-list evidence and separates
translation reuse from state and cache coherency.  It does not change the
current 32-KiB run-list safety limit.

## Reproducible evidence

The read-only checker is
[`tools/chrome9_vendor_reuse_audit.py`](../../tools/chrome9_vendor_reuse_audit.py).
It pins the unstripped Linux UMD by hash, checks its symbol addresses and
disassembly invariants, and verifies the old KMD source constants and
publication order:

```sh
python3 tools/chrome9_vendor_reuse_audit.py
```

Expected result:

```text
old KMD: PASS (16 resident branch buffers x 1024 KiB, 16 MiB total)
Linux UMD: PASS (0x5f0-dword full reserve, 416 dirty-range records, dynamic draw chunks with 0x600-byte reserve)
Chrome9 vendor persistence/state-reuse evidence: PASS
```

The principal binary identities are:

| image | SHA-256 |
|---|---|
| Linux `via_chrome9_dri.so` | `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` |
| Windows `VTGUModeDX32A.dll` | `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| Windows `VTGKModeDX32A.sys` | `618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56` |
| Windows `VTGOGL32A.dll` | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` |

## Persistent SF translations in the old Linux KMD

`via_chrome9_dma_init_inv()` in the retained 2.6.31 driver first invalidates
the complete GART table, pulses `SR6F.bit7`, then maps the SG allocation that
contains both the DMA ring and AGP texture heap.  Its exact mapped size is:

```c
lpcmdmamanager->dmasize * sizeof(unsigned int) + dev_priv->agp_size
```

It clears `SR6C.bit7`, waits for an earlier `SR6F.bit7` operation to finish,
writes each SG page PFN to the PTE array, pulses `SR6F.bit7` once, and restores
`SR6C.bit7`.  Later allocations are stable offsets in that already mapped
heap.  There is no translation rewrite at a primitive boundary.

The branch mechanism is even more explicit:

| property | recovered value |
|---|---:|
| branch buffers | 16 |
| bytes per buffer | `0x100000` (1 MiB) |
| total resident branch heap | 16 MiB |
| alignment | 256 bytes |
| maximum submitted command dwords | `0x100000 / 4 = 262144` |
| lifecycle | prepared -> allocated -> flushed -> prepared |

`via_chrome9_branch_buf_init()` returns immediately when already enabled,
allocates the 16-MiB heap once, and assigns every buffer a stable SF offset,
CPU address and fence readback word.  The request path waits only when all 16
prepared buffers are unavailable.  The flush path validates the ID and
nonzero dword count, assigns a 31-bit fence, cache-flushes only the pages
covering the written command payload, and then publishes one FE40 branch and
FE30 fence into the resident main ring.  The interrupt bottom half returns a
completed buffer to the prepared list.

This establishes allocation-lifetime mapping and multiple outstanding lists.
It does **not** make that KMD safe to copy:

- its command verifier is not a suitable modern security boundary;
- buffer acquisition can wait indefinitely;
- completion accepts `submitted_fence <= readback_fence`, which is not a
  wrap-safe 31-bit sequence comparison;
- error recovery does not provide the current quarantine guarantees.

Use the architecture, not its unchecked waits or verifier.

## Exact Linux UMD state shadow

The unstripped Linux UMD exposes the following relevant entry points:

| symbol | address |
|---|---:|
| `cmKickoffRingAGP_inv` | `0x000b0fb0` |
| `cmReleaseSpace_inv` | `0x000b2690` |
| `cmGetSpace_inv` | `0x000b2730` |
| `stmRestoreHwContext_invi` | `0x00175f70` |
| `stmSaveHwContext_invi` | `0x00175f80` |
| `stmHWContextSwitch_inv` | `0x00176660` |
| `stmFullUpdateHardware_invi` | `0x00177490` |
| `stmResetHwToken_invi` | `0x001783f0` |
| `stmUpdateHardware_inv` | `0x0017a770` |
| `__glS3InvDrawPrimitiveTriangleList` | `0x00209d10` |

### Full versus incremental emission

`stmFullUpdateHardware_invi:0x1774d3` requests exactly `0x5f0` dwords from
the command manager.  It serializes the complete software hardware-state
image and releases only the dwords actually used.

`stmUpdateHardware_inv:0x17a7ac` compares its second argument with one.  A
value of one branches to the full update at `0x17b4c7`; the ordinary path
instead builds dirty-range records.  The scratch object layout is:

| offset | per-record role |
|---:|---|
| `+0x06cc` | dirty bit mask or leading control word |
| `+0x0d4c` | source pointer into the persistent state image |
| `+0x13cc` | H2/H3 selector/control |
| `+0x1a4c` | range size/class |
| `+0x20cc` | state-bank byte offset |
| `+0x274c` | pending record count |

The five arrays are separated by `0x680` bytes, so each holds exactly 416
dword entries.  This same `0x680` spacing appears in the Windows scratch
layout at `+0x0000`, `+0x0680`, `+0x0d00`, `+0x1380` and `+0x1a00`, with its
count at `+0x2080`; see
[windows-draw-path.md](windows-draw-path.md#dirty-state-serialization).

The Linux serializer at `0x17ae48..0x17afcd` aligns each header to 16 bytes,
chooses H2 or H3 from the record class, and uses bit-scan-forward to find
contiguous runs inside a 32-bit dirty mask.  It copies only those contiguous
dwords.  A fully dirty 32-bit group becomes one 128-byte copy, while a small
change need not replay the group.

### Successful draw clears metadata, not hardware state

After incremental emission, `stmUpdateHardware_inv:0x17ad7e` calls
`stmResetHwToken_invi`.  That function conditionally clears group dirty
tokens, zeroes all five scratch arrays through the pending count, then clears
the pending count and summary words at `+0x274c`, `+0x2750`, `+0x2754` and
`+0x2758`.  It does not erase or restore the hardware-state image.

`stmRestoreHwContext_invi` and `stmSaveHwContext_invi` are literal no-op
returns, and `stmHWContextSwitch_inv` returns the constant two.  The real
ownership mechanism is software: `stmForceContextSwitch` clears the global
previous-state pointer, and `stmSetCurrentState` invokes the state callback
for the selected context.  Thus a full replay follows an explicit ownership
loss; it is not part of every draw.

The Windows UMD independently has the same architecture.  Its Direct3D and
OpenGL paths gather dirty state at `0x10150be0` / `0x101fc050`, serialize the
ranges, and clear their emitted records at `0x10150850` / `0x101fbcc0`.

## Primitive/list batching limits

`__glS3InvDrawPrimitiveTriangleList` asks the command manager for available
capacity with `cmGetDMAInfo(..., 6)`, subtracts `0x600` bytes, divides by the
active vertex stride, and chunks an API draw only when it cannot fit.  Each
chunk has one A24 containing its complete vertex count.  There is no fixed
three-vertex submission rule.

At `0x209f2d` and the corresponding loop sites the draw checks bit zero of
the context word at `+0x264`.  Only when set does it clear the bit, call
`stmForceContextSwitch`, and call `stmSetCurrentState`; otherwise the next
chunk reserves command space and emits vertices without a full state replay.

The recovered capacities describe different boundaries and must not be
confused:

| boundary | value | meaning |
|---|---:|---|
| old Linux branch-list allocation | 1 MiB / 262144 dwords | vendor KMD accepted size per resident list |
| old Linux branch pool | 16 lists | vendor maximum outstanding allocation count |
| Linux UMD dirty scratch | 416 records | maximum queued state ranges before serialization |
| Linux UMD full-state reserve | `0x5f0` dwords | command-manager reservation, not actual list maximum |
| Windows resident ring | 4 MiB | ring address-space size, not a safe single-list limit |
| modern validated run staging | 32 KiB | current bounded proof and implementation target |

The A24 count field is 24 bits, but neither its width nor the vendor 1-MiB
allocation proves that a list near those limits is safe under VX900 command
read-ahead, current validators, or timeout policy.  Keep the exact modern
32-KiB closure and per-path triangle maxima until progressively larger lists
pass guarded silicon oracles.

## Four distinct coherency operations

The vendor code distinguishes operations that the current synchronous path
currently performs together:

1. **PTE publication.** Write or clear PTEs, then pulse `SR6F.bit7`.  This is
   required only when translations change.
2. **Command publication.** Flush only newly written command-list pages before
   adding the FE40 branch or advancing the resident ring tail.
3. **Resource ownership.** Make texture/target/depth contents visible in the
   DMA direction required for the job.
4. **Retirement and CPU inspection.** Wait for proven completion, return
   GPU-written targets/depth to CPU ownership, and inspect guards.

The old KMD's explicit `via_chrome9_ioctl_flush_cache(offset, size)` rounds a
persistent heap range to pages and flushes only that range.  It does not
unmap/retranslate the range.  Likewise branch publication flushes only the
pages covering `cmd_size`, then publishes the branch.  These paths prove that
`SR6F` is a translation-cache operation, not a required per-draw texture or
command cache flush.

In the current modern multi-object submit path, by contrast, every draw still
allocates one contiguous SF node, calls `chrome9_gart_set()` to bind it,
prepares guards, flushes and DMA-syncs every complete object, submits and
waits, syncs all objects back to CPU, flushes them again, checks guards, and
calls `chrome9_gart_set()` again to erase the PTEs.  Direct scanout already
has a narrower persistent exception in `chrome9_sf_pin()`; ordinary draw
objects do not yet use it.

## Safe reuse matrix

| operation/state | same validated run | later ioctl, same context | another context | reset/timeout |
|---|---|---|---|---|
| one A24-counted invariant state image | yes | only with kernel owner+generation | replay | replay / prohibit |
| local GEM SF mapping | yes | yes while cache entry is pinned | yes; global address space | quarantine on uncertain retirement |
| PTE allocation and PFN array | reuse | reuse | reuse | retain if possibly referenced |
| VGA acquisition and `SR6F` | no repeat without PTE changes | no repeat on mapping hit | no repeat on mapping hit | required for proven rebuild only |
| command payload flush | once for written interval | each newly written interval | each newly written interval | do not republish timed-out bytes |
| texture sync to device | once before run | required until CPU-write tracking exists | required | required after safe recovery |
| target/depth sync to CPU | once after retirement | required after each GPU write | required | forbidden without retirement |
| dependency waits/reservation lock | once per job | required | required | required |
| guard prepare/verify | once per job/run | required | required | preserve and quarantine |

Two tempting shortcuts remain unsound:

- A persistent PTE does not imply persistent DMA ownership.  Userspace mmap
  writes are not tracked today, so a mapped texture still needs a
  sync-to-device before a job.
- A matching Mesa state key does not prove hardware state ownership.  The
  engine is global and the kernel submit lock is released between ioctls.
  Only a kernel-maintained owner and generation can establish that no other
  path changed the state.

## Exact keys and invalidation rules

### Persistent SF mapping key

Key by kernel GEM object identity and an internal mapping generation, not a
userspace handle.  A cache entry must own:

- a GEM reference;
- the stable SGT/PFN vector;
- its `drm_mm_node` and SF base;
- page count and object size;
- active-job reference count, LRU link and quarantine state;
- the engine address-space generation in which the PTEs were published.

Imported objects should remain excluded from the first tranche because their
attachment/mapping lifetime differs.  Evict only an inactive entry after all
reservations and fences prove retirement.  Batch all eviction clears and new
PTE writes under one `vm_lock` and one VGA ownership interval, then issue one
`SR6F` pulse.  If publication or cleanup fails after a command may have become
visible, retain the mapping and its object reference in quarantine.

### Hardware-state residency key

The native key must cover every word whose omission would change a later
draw, including:

- render target and depth SF bases, pitches, formats and dimensions;
- texture SF bases, all level offsets, formats, pitches, sizes, wrap/filter
  and border state;
- blend, channel masks, depth/stencil, alpha test, scissor and raster state;
- FVF/varying linkage, topology and vertex payload stride;
- VS/PS selectors, instruction bodies and all referenced constant banks;
- destination format and any program-specific flags;
- SF mapping generations for every referenced object.

A state match is usable only when the kernel owner token and engine generation
also match.  Increment that generation and force a full replay after:

- engine initialization, ring reinstall/wrap recovery, reset, timeout or
  quarantine;
- a file/context ownership change;
- any 2D/raw/direct-MMIO path that can alter shared 3D registers;
- mapping eviction or SF-base change for a referenced object;
- suspend/resume or power-state loss.

For the next performance tranche, avoid this cross-ioctl complexity: emit one
complete state image followed by all triangles in the immutable run.  Add
cross-ioctl incremental state only after every state-changing entry point is
under the generation protocol.

## Patch order and executable gates

1. **Instrumentation only.** Add counters for `sf_hit`, `sf_miss`,
   `sf_evict`, `pte_write_pages`, `sr6f_pulses`, `vga_acquires`,
   `command_flush_bytes`, resource sync bytes by role, full-state uploads,
   run triangles, guard scans and quarantined mappings.  Confirm counters do
   not change on rejected UAPI input.
2. **Single large list.** Land the already designed 32-KiB state-complete run
   with one reservation, mapping interval, retirement and guard scan.  This
   requires no cross-ioctl state trust.
3. **Persistent local-GEM mappings.** Add the object-keyed cache with bounded
   entry/page limits and batched PTE publication.  Keep the existing whole-
   object cache ownership and guard protocol unchanged initially.
4. **Role-aware DMA ownership.** Only after CPU access hooks exist, retain
   read-only textures device-owned across jobs; targets and depth still return
   to CPU after GPU writes.  This is a separate correctness change.
5. **Kernel state generation.** Add owner/generation tracking before any
   vertex-only cross-ioctl packet.  A forced-generation debug knob must make
   every job replay full state and produce identical pixels.

Minimum static and hardware checks:

```sh
python3 tools/chrome9_vendor_reuse_audit.py
python3 tools/chrome9_run_layout.py
```

- Submit one maximum run and require exactly one bind interval, one `SR6F`
  pulse for initial cache miss, one command interval flush, one retirement and
  one guard scan.
- Repeat the identical run and require an SF hit with zero PTE writes, zero
  VGA acquisitions and zero `SR6F` pulses; resource ownership and guard
  counters must still advance.
- Alternate two contexts with identical public state but different texture,
  constants and target.  Require a full replay on every owner transition and
  exact, non-leaking pixels.
- Evict one inactive mapping, then reuse its former SF range for a different
  BO.  Require one batched clear/publish invalidation and no stale pixels.
- Force a ring wrap and engine-generation bump.  Require the first later draw
  to replay full state even when its user-visible key matches.
- Inject timeout after publication.  Require the list and all referenced SF
  mappings to remain quarantined, with no eviction, PTE clear, CPU readback or
  later reuse.
- Run 2D/presentation between two matching 3D jobs.  Until shared-register
  effects are proven absent, require the intervening path to invalidate the
  3D state generation.

The vendor evidence supports large performance gains, but the safe unit is
specific: persist address translations, batch immutable primitives, and
continue synchronizing actual data ownership.  State omission across jobs is
the last step, not a prerequisite for the first two.
