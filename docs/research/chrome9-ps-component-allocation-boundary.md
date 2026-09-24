# Chrome9 pixel-shader component-allocation boundary

Status: exact Linux 5.75 proprietary compiler disassembly and source-exact
ioquake3 default-lightall resource replay, 2026-09-24.  This is an offline
compiler boundary, not a silicon qualification and not evidence for a memory
spill ABI.

## Result

The proprietary compiler does not reserve all four lanes for every semantic
temporary.  It schedules an instruction dependency graph first and then
places each masked destination group in a physical register whose requested
lane intervals are free; unrelated values may share its other lanes.
The current Chrome9 kernel compiler instead maps each semantic TEMP id to one
whole physical vec4.  Closing that mismatch is necessary for large shaders,
but component packing alone is not sufficient for the exact ioquake3
lightall order: its peak live-channel count still has a theoretical minimum
of nine vec4 registers while its six-coordinate hardware resource bucket
allows only six.

The implementation boundary is therefore scheduler first, component
allocator second.  The compiler must shorten the semantic live intervals to
at most 24 simultaneously live channels, then pack those channels and prove
that the resulting physical-register high-water is no greater than six.
Simply raising the current 12-register allocator limit or packing the current
order cannot make the object legal.

A dependency-checked candidate schedule now reaches that necessary capacity
bound without inventing a memory spill: it uses the one spare ALU record to
clone one ADD, sinks three independent subtrees, and reduces the conservative
peak from 35 to 21 live channels.  It still has ten simultaneously live
semantic values, so it also proves why whole-vec4 allocation remains
insufficient.  Twenty-one channels fitting into 24 available lanes is not by
itself a group-aware interval-coloring proof; the component allocator must
still produce and validate an actual six-register assignment.

## Proprietary allocator evidence

The source binary is:

```text
binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so
SHA-256 fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
ELF32 i386, debug symbols present
```

The relevant exported functions are:

| symbol | virtual address | role established by disassembly |
|---|---:|---|
| `FindNextDirectType` | `0x001c1e60` | select the coordinate/temp resource envelope |
| `TryAllocChn_invi` | `0x001c1f00` | test a requested channel list against per-lane intervals |
| `RenameSrcChannel_invi` | `0x001c2090` | rewrite source swizzles after lane assignment |
| `stmPPS_ShrinkSrcChn_invi` | `0x00161a40` | reduce/remap pixel-shader source channels |
| `PSOPT_InstReorder_invi` | `0x001c4140` | reorder the instruction dependency graph |
| `InitChannelUsageInfo_invi` | `0x001c8e70` | initialize physical-register lane usage |
| `PSOPT_RenamingTREG_invi` | `0x001ca1a0` | temporary-renaming pass |
| `RequestChns_invi` | `0x001ca800` | allocate the destination writemask's channels |
| `RenameALU_Dst_invi` | `0x001cae20` | allocate and rewrite an ALU destination |
| `RenameDstChannel_invi` | `0x001cb440` | rewrite destination lane mapping |
| `stmPixelShaderSDGOptimize_invi` | `0x001cd1b0` | construct/schedule the shader dependency graph |
| `PSOPT_RenameRWRegs_invi` | `0x001cd450` | initialize usage, then rename sources/destinations |

`InitChannelUsageInfo_invi` at `0x1c8ea8..0x1c8fb0` initializes interval
state for 16 physical registers times four channels, plus a separate
four-register pool.  `TryAllocChn_invi` chooses one of three tables:

```text
type 0: context + 0xd9a4, 16 candidate registers
type 1: context + 0xdaa4, 16 candidate registers
type 8: context + 0xdba4,  4 candidate registers
```

For each candidate register, `0x1c1f58..0x1c1f95` walks the requested channel
indices and compares their existing interval endpoints with the requested
start.  On success, `0x1c2054..0x1c2087` returns the physical register and
writes the new endpoint only into the requested lanes.  This is direct
evidence for component-granular interval allocation, not merely a vec4
first-fit allocator.

`RequestChns_invi` at `0x1ca83e..0x1ca859` turns the four-bit destination mask
into a requested-channel list.  It enumerates remapped type priorities and
candidate registers with the same per-channel interval test.  Its caller
`RenameALU_Dst_invi` invokes it at `0x1cb150`, writes the selected physical id
and destination mapping, while `RenameSrcChannel_invi` rewrites later source
selectors to the allocated lanes.

Scheduling is a distinct earlier phase.  `stmPixelShaderSDGOptimize_invi` at
`0x1cd230..0x1cd259` constructs a dependency graph and calls
`PSOPT_InstReorder_invi`; `PSOPT_RenameRWRegs_invi` later initializes channel
usage at `0x1cd4bc` and performs register/lane renaming.  Therefore the
proprietary sequence is:

```text
semantic dependency graph -> instruction reorder -> lane allocation/remap
```

It is not whole-register allocation followed by an optional peephole pack.

`FindNextDirectType` reads two resource counts at context offsets `+0xe26c`
and `+0xe26e` and compares them against the exact envelopes visible in the
current kernel resource table: 4 coordinates / 3 temps, 5 / 4, 8 / 6, and
the largest 16 / 12 envelope.  For six active direct coordinates, the
qualified limit is consequently six physical temporary registers.

## Exact ioquake3 lower bound

The source-exact default lightall replay is retained as:

```text
/tmp/chrome9-lightall-resource-probe.cpp
SHA-256 8d1970cb5492653f517869911276d4da2acce23e7287bae5a63075ec537aa0a0
/tmp/chrome9-lightall-resource-probe
SHA-256 02e2d2b112829051b72d13e266bd9c59ed1729c34ef14e70fe211ce78f33dcfb
/tmp/ioq-default-lightall-semantic-liveness.txt
SHA-256 99cfd830a061fe5ad848c206e6470e665dac44ebd666bbaa75f4dfed34da6cb1
```

The transformed program has 100 records: 95 ALU and five TEX, six direct
varying coordinates, five dense sampler slots, and 96 virtual SSA temps.  The
current whole-vec4 allocate-before-release replay fails at semantic record 34
and has a peak of 15 simultaneously live values at record 67.

The stronger component count still disproves a packing-only fix.  At record
34 the live values are:

```text
t3 t5 t10 t12 t13 t21 t22 t25 t29 t35 t49 t50 t53
```

Their live destination masks total 34 channels.  Even an ideal component
allocator therefore needs `ceil(34 / 4) = 9` physical registers.  At the
record-67 peak, the 15 live values total 35 channels, again requiring at
least nine registers.  Both exceed the six-register envelope before any
first-fit fragmentation is considered.

Trying generic `nir_schedule` before the Chrome9 compiler did not change the
semantic sequence for pressure thresholds 4, 6, 8, 12, or 32, in either its
normal or fallback mode.  This is explained by the current Mesa backend:
`chrome9_compile_ps_nir()` recursively lowers the color-output DAG through
`chrome9_ps_lower_def()`, emitting its own depth-first/postorder semantic
stream.  It does not preserve NIR block order.  A generic NIR scheduling pass
therefore cannot solve this specific backend order; the scheduler must act on
the Chrome9 semantic DAG/emission order or on an equivalent backend-owned
dependency graph.

### Bounded schedule discriminator

The retained offline reorder checker is:

```text
/tmp/chrome9-lightall-reorder.pl
SHA-256 1a390a973837061b2a51343c14a22b81bf1b95b6dbf9d68fb8444df2f8247d15
```

It rejects an ordering that reads an undefined semantic temp and measures
both live semantic values and the conservative union of their destination
channels.  Two results matter:

- sinking records 23..29, the roughness POW denominator chain, immediately
  before its first late consumer at old record 66 removes the earliest
  12-temp allocation failure at record 34 at zero instruction cost, but does
  not change the later peak;
- additionally sinking the independent lightmap/normalization and
  reflectance subtrees, and cloning the ADD at old record 3 for its late
  branch, produces 101 total semantic records: 96 ALU plus five TEX.  Its
  peak is ten live values and 21 conservatively live channels at new record
  54 (old record 67).

The exact moved groups are:

```text
old 23..29                    immediately before old 66
old 17,20..22                 immediately before old 82
old 0..2, cloned 3, old 4,
    old 12..16                immediately before old 83
```

The complete old-record order is retained here so the result does not depend
on the temporary checker surviving a reboot:

```text
003 005 006 007 008 009 010 011 018 019 030 031 032 033 034 035
036 037 038 039 040 041 042 043 044 045 046 047 048 049 050 051
052 053 054 055 056 057 058 059 060 061 062 063 064 065 023 024
025 026 027 028 029 066 067 068 069 070 071 072 073 074 075 076
077 078 079 080 081 017 020 021 022 082 000 001 002 100 004 012
013 014 015 016 083..099
```

Record 100 is a duplicate of old record 3 writing new `t96`; old record 4 is
rewritten to read `t96`.

This is an implementation discriminator, not a final schedule.  It preserves
the parsed TEMP definition/use dependencies and remains within the 96-ALU
store, demonstrating that the current program is not ruled out by channel
capacity alone.  A production scheduler must additionally model TEX/KILL
ordering, native source-slot/port constraints, partial-component definitions,
and actual lane interference before the candidate can become executable.

The instruction budget is tight: the exact program already uses 95 of 96 ALU
records, and the discriminator spends the sole spare record on one cloned
ADD.  Arbitrary recomputation is not a general spill strategy; further clones
would require algebraic/coissue reductions.  Scheduling and definition
sinking must be attempted before broader recomputation.

## Patch-ready compiler contract

A safe implementation sequence is:

1. Build explicit dependencies and per-component use intervals for the
   Chrome9 semantic program.  Preserve TEX/KILL ordering and every native
   operation's already-qualified source-slot constraints.
2. Schedule ready records using live-channel pressure, with a hard target of
   24 channels for a six-temp bucket.  Reject if no legal order meets the
   program's resource envelope and 96-ALU/32-TEX store limits.
3. Allocate each destination component to a physical `(register, lane)`.
   Rewrite destination masks and every consumer swizzle; do not assume a
   semantic vec3 or vec4 remains in consecutive physical lanes.
4. Recompute physical-register lifetime after lane packing.  Hardware release
   markers remain per physical vec4, not per component.  Mark exactly one
   final source occurrence for each physical register, using the proven
   same-record precedence A over C over B.
5. Recompute physical high-water and select the resource bucket only after
   scheduling and allocation.  Reject rather than alias or silently select a
   larger unsupported envelope.
6. Add byte-level selftests for disjoint scalar lifetimes, mixed vec3/scalar
   packing, source-swizzle rewriting, same-record repeated physical ids, and
   the complete 100-record lightall semantic object.  The lightall test must
   assert both final physical high-water `<= 6` and unchanged ALU/TEX result
   dependencies, not merely that allocation returns success.

No proprietary scratch load/store or memory-spill packet was recovered in
this audit.  Calling the proposed scheduling/recomputation work a spill
implementation would overstate the evidence.  Until a legal six-register
schedule exists and the complete native object is byte-reviewed, this shader
must remain rejected before submission.
