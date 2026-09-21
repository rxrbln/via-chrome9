# Chrome9 pixel DPH internal-emitter audit

Pixel DPH is structurally one native record in both proprietary emitters. It
is reachable through internal opcode 102 rather than a public D3D token. The
original static derivation has now been confirmed by direct Windows and Linux
internal-emitter invocation plus each production configuration-two finalizer.
Exact dynamic rows are retained in
[`chrome9-dph-production-corpus.tsv`](chrome9-dph-production-corpus.tsv).

The entries in the older
[`chrome9-dph-static-corpus.tsv`](chrome9-dph-static-corpus.tsv) remain
static predictions, not dynamic compiler byte contracts by themselves; the
new production corpus is authoritative for the tested identities.

## Cross-driver emitter structure

The Windows internal emitter is `0x102163b0`.  It converts the destination
and two sources, clears first-source selector bits with `and eax,0xfffff9ff`
at `0x10216425`, forces selector value 4 with `or eax,0x00000800` at
`0x1021642a`, constructs one DP4-family record, calls the ALU append routine
once at `0x1021652d`, and returns at `0x1021653d`.  It does not allocate or
release a temporary.

The Linux twin is `stmPPS_DPH_ICD_PS20_invi` at `0x00164750`.  Its equivalent
first-source selector rewrite is at `0x00164809..0x0016482f`, including
`or edx,4` at `0x00164827`; it builds d3 from base `0x00030020` at
`0x001648ee`, appends exactly once at `0x0016491b`, and returns at
`0x0016492c`.  It likewise has no temporary allocation.

Both paths therefore force the first logical operand from identity XYZW to
XYZ1 and retain the second operand as identity XYZW.  With the native DP4
datapath this implements:

```text
dot4((src0.x, src0.y, src0.z, 1), src1)
 = src0.x*src1.x + src0.y*src1.y + src0.z*src1.z + src1.w
```

## Derived records, now dynamically confirmed

The exact byte-matched terminal DP4 records are already known for v0/c0
topologies.  Applying the emitters' only DPH-specific rewrite changes d1's A
W selector from 3 to literal ONE selector 4, i.e. high nibble `3` to `4`:

```text
DP4 v0,c0  01c00530 30080053 00f04005 00030820
DPH v0,c0  01c00530 40080053 00f04005 00030820  (static derivation)
```

The four rows decode as native A=`xyz1`, C=`xyzw`, subop 3, terminal full
XYZW, SAT clear, and scale zero. Direct invocation proves that Windows and
Linux emit those exact records. Configuration two leaves all four terminal
records unchanged and closes controls at A90=`90058000`, A92=`92000001`,
with A91 and A93--A9a zero.

## Production internal invocation

The Windows wrapper tokenizes an ordinary DP4-shaped operand record, then
changes only normalized opcode 9 to internal opcode 102 before the production
dispatcher. It retains the recovered H5 register-table builder, physical
remap, scheduler, and configuration-two pass. Wrapper SHA-256 is
`642b1df45ec7eb075a32543b80daad8aa05f64714a6421ed73b10c962427d4c4`;
the eight-case differential JSON SHA-256 is
`5f1f65f16302bb3b41ffc565890913ea79055fc6570c28d67d6cdb2bb95e6a0c`.

Linux exports `stmPPS_DPH_ICD_PS20_invi` at VA `00164750`. An initialized
instance with production `stmPPSSetFunctionsPS20_invi` and
`stmPPSSetRegManager_invi` accepts the same normalized 36-byte instruction
record directly. The bounded helper SHA-256 is
`1a953c770d096e60ba31d87af8381d10edd3ec1d3bc4439bf9ed050335b8cea8`.
Calling `stmPPS_PostTranslationSetting_invi(instance+0x10,2)` after the
emitter closes the same controls and bytes.

Calling `stmPPS_SDGOptimization_invi` directly on that emitter-only object
segfaults because the direct path has not built the optimizer's graph state.
This is an exact harness boundary, not a DPH emitter failure. The Windows path
runs the production scheduler. Linux terminal DPH is one record, so no order
is ambiguous; arbitrary DPH scheduling remains unproved.

## TEMP producer and last-use finalization

A same-shape production DP4 compile was used only to initialize the Linux H5
register table and retain the first MOV producer. Its terminal DP4 record was
discarded, then the exported DPH emitter appended the replacement record and
configuration two ran normally. Windows uses the complete internal
dispatcher/scheduler path. Both drivers agree exactly:

```text
mov r0,v0          01c00530 301c0053 00f04005 000d0020
dph oC0,r0,c0      01c00530 40080053 00f20005 00030820  # A last use
dph oC0,c0,r0      01c00530 40200053 00f08005 00030820  # C last use
dph oC0,r0,r0      01c00530 40000053 00f20005 00030820  # A wins
```

Raw A/C/duplicate records have the respective marker clear. Configuration
two uses the ordinary physical-TEMP rules: A d2.bit17, C d1.bit21, and the
A-over-C duplicate priority. All rows keep physical r0, TEMP write mask 1,
A90=`90058000`, and A92=`92000002`. DPH needs no special lifetime rule beyond
its A/C operand assignment.

## Fail-closed policy and possible future key

Keep DPH rejected. The compiler-byte, register-management, and
configuration-two gates are now closed, but the Windows D3D tokenizer still
exposes no public DPH token and internal opcode 102 is not a userspace ABI.
Promotion still requires scalar-result replication and numerical proof on
silicon, plus a structured semantic key rather than native instruction words.

If those gates pass, `DPH(v0,v0)` is the simplest no-constant key.  It is
finite for normalized v0 and computes `x^2+y^2+z^2+w`, though it can exceed
one and be masked by UNORM clamping.  Use flat v0=`(.25,.25,.25,.25)` for an
in-range result `.4375`, then v0=`(.5,.25,.125,.25)` for `.578125`; require
the same scalar result in all four output lanes.  Mesa must recognize the
exact fdot4 of `(v0.xyz,1)` and `v0`, not arbitrary dot/reassociation shapes.

Every future live experiment uses command 19's mandatory fresh D16 target, depth
clear 1, LESS/write at z=0.5, COPY, exact command-word checks, target/depth
guards, fences, reset/error counters, and known-good draws before and after.
Modifiers, swizzles, constants, partial masks, SAT/scale, arbitrary TEMP
graphs, and explicit FP controls remain rejected even after the first live
row. Compiler invocation alone is not silicon proof.
