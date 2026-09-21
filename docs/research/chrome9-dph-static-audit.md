# Chrome9 pixel DPH static one-record audit

Pixel DPH is structurally one native record in both proprietary emitters, but
it is reachable only through the internal OpenGL grammar and has not yet been
run with a production allocator/finalizer.  This note derives its candidate
records from exact emitter masks plus byte-matched DP4 records.  They are
static predictions, not dynamic compiler byte contracts.  Provenance and
derived vectors are retained in
[`chrome9-dph-static-corpus.tsv`](chrome9-dph-static-corpus.tsv).

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

## Mechanically derived records

The exact byte-matched terminal DP4 records are already known for v0/c0
topologies.  Applying the emitters' only DPH-specific rewrite changes d1's A
W selector from 3 to literal ONE selector 4, i.e. high nibble `3` to `4`:

```text
DP4 v0,c0  01c00530 30080053 00f04005 00030820
DPH v0,c0  01c00530 40080053 00f04005 00030820  (static derivation)
```

The four derived rows decode as native A=`xyz1`, C=`xyzw`, subop 3, terminal
full XYZW, SAT clear, and scale zero.  No scheduler dependency is expected
for a terminal direct-input record, but this must be confirmed dynamically;
the static derivation does not establish Linux configuration-2 output or
control words.

## Fail-closed policy and possible future key

Keep DPH rejected.  The current Windows D3D tokenizer exposes no public DPH
token, and an internal emitter address plus predictable packing is not an ABI.
Before any promotion, invoke both internal paths with production register
management, require exact agreement with the derived row, capture Linux
configuration 2 and controls, then prove scalar-result replication and
numerics on silicon.

If those gates pass, `DPH(v0,v0)` is the simplest no-constant key.  It is
finite for normalized v0 and computes `x^2+y^2+z^2+w`, though it can exceed
one and be masked by UNORM clamping.  Use flat v0=`(.25,.25,.25,.25)` for an
in-range result `.4375`, then v0=`(.5,.25,.125,.25)` for `.578125`; require
the same scalar result in all four output lanes.  Mesa must recognize the
exact fdot4 of `(v0.xyz,1)` and `v0`, not arbitrary dot/reassociation shapes.

Every live experiment uses command 19's mandatory fresh D16 target, depth
clear 1, LESS/write at z=0.5, COPY, exact command-word checks, target/depth
guards, fences, reset/error counters, and known-good draws before and after.
Modifiers, swizzles, constants, partial masks, SAT/scale, temporary consumers,
and explicit FP controls remain rejected even after the first dynamic row.

