# Chrome9 pixel-shader component-allocation boundary

Status: exact Linux 5.75 proprietary compiler disassembly and source-exact
ioquake3 default-lightall resource replay, 2026-09-24.  The six-coordinate,
ten-temporary fallback bucket was separately checked with a production Linux
compiler object.  This is an offline compiler boundary, not a silicon
qualification and not evidence for a memory spill ABI.

## Result

The proprietary compiler does not reserve all four lanes for every semantic
temporary.  It schedules an instruction dependency graph first and then
places each masked destination group in a physical register whose requested
lane intervals are free; unrelated values may share its other lanes.
The current Chrome9 kernel compiler instead maps each semantic TEMP id to one
whole physical vec4.  Closing that mismatch will be useful for larger
shaders.  The original ioquake3 lightall order has a 35-channel peak, a
theoretical minimum of nine packed vec4 registers.  That no longer proves
packing alone insufficient: the earlier claim that six coordinates force a
six-register maximum was incorrect.  The zero resource bucket is a valid
fallback, selected by the production compiler for six coordinates and ten
temporaries.

The implementation boundary is therefore scheduler first, then an exact
whole-register allocation replay.  The first scheduler candidate reaches ten
live semantic vectors and may fit the current 12-register compiler bound
without component packing.  If no dependency-legal whole-vector schedule
fits, channel packing is a second route to investigate.  Neither route has
yet been compiled into a complete native lightall object or run on silicon.

A dependency-checked candidate schedule uses the one spare ALU record to
clone one ADD, sinks three independent subtrees, and reduces the conservative
peak from 35 to 21 live channels and from 15 to ten live vectors.  This is
below the current twelve-register bound, but is not yet a kernel allocation
replay or proof that the native object executes.  Channel packing may reduce
pressure further; 21 live channels alone does not prove a legal group-aware
assignment.

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
and `+0xe26e` and compares them against the exact resource thresholds in
`stmUpdateConfig`: 4 coordinates / 3 temps, 5 / 4, and 8 / 6.  If none
matches, the resource field is zero; that is a real fallback bucket, not a
rejection.  The current kernel allocator is independently bounded to twelve
physical temporaries.  Its `chrome9_fragment_resource_bucket()` returns
zero for the fallback and `chrome9_fragment_encode()` uploads it.

The missing mixed-resource differential was run through the production Linux
5.75 pixel compiler, configuration two, with six direct full-float inputs
`t0..t5` and ten simultaneously live full-vec4 temporaries `r0..r9`.
The 71-dword PS 2.0 token stream consists of ten `MOV r_i, t_i/c_j`, nine
accumulating `ADD r0, r0, r_i`, and `MOV oC0, r0`.  Its space-separated
lowercase hexadecimal dwords have SHA-256
`b7b72c3ac5e7a91b540d2425da8ec8f88dec11fc4f3d51adddc46eb38a25054d`.
The exact offline result is:

```text
active-resource-counts: 6 10 6 0
ALU count=20; TAU count=0
metadata +2c=00040000, +40=0000003f, +44=000003ff
controls A90=9004003f A91=91000000 A92=92000014
```

Native TEMP IDs occupy four bits and the vendor compiler emits sixteen
simultaneously live IDs `r0..r15`; its `r16` overflow silently aliases r0 and
is not a usable seventeenth register (see
`chrome9-ps-temp-lifetime-finalizer.md`).  The current kernel deliberately
caps its first allocator at twelve (`CHROME9_FRAGMENT_PHYS_TEMPS=12`), so the
actionable limit for this driver is twelve, not the raw encoding's sixteen.
The zero A90 bucket is still valid with six coordinate inputs and ten of
those twelve temps.

For reproduction, begin with `ffff0200`; for `i=0..9` append
`03000001, 000f0000+i, (i<6 ? 30e40000+i : 20e40000+i-6)`; for
`i=1..9` append `04000002, 000f0000, 00e40000, 00e40000+i`; finish with
`03000001, 000f0800, 00e40000, 0000ffff`.  Pass the words to
`tools/re/chrome9_pps_probe.c` under `qemu-i386` with
`CHROME9_POSTPROCESS=2`, `CHROME9_RESOURCE_COUNTS=1`, and
`CHROME9_DUMP_CONTROLS=1`.  This proves the fallback encoding for the mixed
count, not six-varying/five-TEX lightall silicon correctness.

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

The component count shows that packing the original order needs at least
nine registers, although the corrected fallback may encode that.  At record
34 the live values are:

```text
t3 t5 t10 t12 t13 t21 t22 t25 t29 t35 t49 t50 t53
```

Their live destination masks total 34 channels.  Even an ideal component
allocator therefore needs `ceil(34 / 4) = 9` physical registers.  At the
record-67 peak, the 15 live values total 35 channels, again requiring at
least nine registers.  Nine fits the zero-bucket/12-register candidate
envelope, but the original whole-register order still fails at 13 live values
at record 34 and later peaks at 15.

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
  54 (old record 67); whole-register allocation now deserves a direct replay.

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

## Patch-ready compiler contract and regressions

A safe implementation sequence is:

1. Build explicit dependencies for the Chrome9 semantic program.  Preserve
   TEX/KILL ordering, terminal full-RGBA output, and each operation's
   qualified A/C/B source-slot constraints.  Do not rely on `nir_schedule()`:
   recursive semantic lowering discards its block order.
2. Schedule ready records to shorten whole-vector lifetimes.  Try the retained
   101-record discriminator first; replay the kernel's allocate-before-release
   rule, require physical high-water `<=12`, 96 ALU, five TEX, and the expected
   masks `varying=0x3f`, `sampler=0x1f`.  Check that zero-bucket A90 is selected
   for a ten-register result; zero must not be confused with failure.
3. If no valid whole-vector schedule fits, add grouped per-channel intervals
   and physical `(register,lane)` remapping.  Rewrite masks and every consumer
   swizzle, then rerun the same lifetime and native-port checks.  Physical
   high-water remains `<=12`; use a smaller bucket only if the final mixed
   resource counts qualify for it.
4. Recompute physical-register lifetime after either allocation.  Hardware
   release markers remain per physical vec4, not per component.  Mark exactly
   one final source occurrence per physical register using A over C over B
   same-record precedence.  Reject undefined reads and any dead temp.
5. First add an offline resource-bucket regression: ten full-vec4 MOV
   definitions from six distinct varyings and four constants, then a fresh-SSA
   ADD reduction and one terminal RGBA MOV.  Assert all six varying routes,
   no dropped source, physical high-water between ten and twelve, and A90
   `9004003f` (the vendor WAW program above supplies the resource-field
   oracle, not an exact ALU-byte golden for the SSA rewrite).  Then add a
   synthetic scheduler regression: thirteen full-vec4 MOV
   definitions from constants, followed by an ADD reduction and one terminal
   RGBA MOV.  The deliberately eager semantic order exceeds twelve live
   temps; an interleaved dependency-preserving schedule needs only a small
   number.  It must not be accepted by aliasing r12 to r0 or truncating masks.
   Add a separate allocator-unit test with thirteen pinned scalar definitions
   and later uses to isolate channel packing from scheduling.  Neither test
   authorizes silicon submission; compare native bytes and run small guarded
   per-lane oracles before enabling the packed path.
6. The complete default-lightall regression must assert semantic output
   dependencies, exact ALU/TEX counts, all five sampler IDs, six varying
   routes, final native source/destination and release markers, and physical
   high-water `<=12`.  Only then consider a bounded distinct-color silicon
   oracle, after the separate alpha-mode and FACE proofs.

No proprietary scratch load/store or memory-spill packet was recovered in
this audit.  Calling the proposed scheduling/recomputation work a spill
implementation would overstate the evidence.  Until a legal twelve-register-
or-less schedule exists and the complete native object is byte-reviewed,
this shader must remain rejected before submission.
