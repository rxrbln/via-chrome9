# Chrome9 one-record source swizzle and NEG boundary

## Result

A 72-case production differential covers terminal MOV plus MUL, MIN, MAX,
ADD, SUB, and DP3 with identity files v0/c0.  For every source position it
tests `.xxxx`, `.yyyy`, `.zzzz`, `.wwww`, and `.wzyx`; it also tests the two
previously missing DP3 NEG positions.  All 70 swizzle rows byte-match between
the proprietary Windows scheduler and Linux configuration-2 compiler.  Both
DP3 NEG rows disagree in instruction count and encoding and are hard rejects.
Exact tokens and complete records are in
[`chrome9-swizzle-neg-corpus.tsv`](chrome9-swizzle-neg-corpus.tsv).

This is compiler evidence only.  No new swizzle or modifier enters the
structured runtime allowlist until its exact numerical oracle passes.

## Scalar replication is source state

Replicating one channel does not narrow or otherwise alter the destination.
The destination remains a full XYZW oC0 write (`d2[23:20]=0xf`) and the four
source selectors carry the replication.  For MOV(v0):

```text
v0.xxxx  01c00530 001c0053 00f04000 000d0820
v0.yyyy  01c00530 901c0053 00f04024 000d0820
v0.zzzz  01c00530 201c0053 00f04049 000d0820
v0.wwww  01c00530 b01c0053 00f0406d 000d0820
v0.wzyx  01c00530 801c0053 00f04068 000d0820
```

The same selector tuples byte-match in A, B, and C across the relevant
operations.  `.wwww` means source selectors `(3,3,3,3)` and produces four
replicated result lanes because the source is replicated, not because of a
scalar destination mode.  `.wzyx` is `(3,2,1,0)` and proves independent lane
routing.

DP3 is the deliberate exception in its fourth source selector, not its
destination.  It consumes the first three post-swizzle token lanes and forces
native A.w/C.w to ZERO selector 5.  Thus DP3(v0.wzyx,c0) uses
`(w,z,y,0)=(3,2,1,5)` while still writing all four destination lanes.  The
operation's scalar dot result replication remains a silicon semantic to test;
the compiler record proves only source routing plus a full destination mask.

## Physical slot coverage

The byte-matched matrix independently exercises:

| operation | logical source 0 | logical source 1 |
| --- | --- | --- |
| MOV | A | - |
| MUL, MIN, MAX, DP3 | A | C |
| ADD | A | B; C is literal ONE |
| SUB | B | A; C is literal NEG_ONE |

Every listed slot preserves X/Y/Z/W and WZYX exactly, except DP3's forced
fourth ZERO.  This closes the three user-source selector packers for these
one-record shapes.  It does not license literal ONE/ZERO source-class aliases,
partial destination masks, arbitrary source files, temporaries, or multi-
record use.

## NEG cross-driver boundary

Combining this differential with the earlier exact corpora gives:

| operation/form | Windows/Linux result | policy |
| --- | --- | --- |
| MOV -A | byte-match | numerical gate required |
| MUL -A,C | byte-match | numerical gate required |
| MUL A,-C | disagree on d3.bit20 | reject |
| MIN/MAX -A,C or A,-C | byte-match; C NEG has no d3.bit20 | numerical gate required |
| ADD/SUB either logical source negated | disagree on d3.bit20 | reject |
| DP3 either logical source negated | wholesale record mismatch | reject |

The DP3 discrepancy is not the familiar bit-20 companion difference:

```text
DP3 -v0,c0
Windows  00962490 30140924 00f04005 00002020
         01c00530 301c0053 00f08005 000d0020
         00962490 30140924 00f00005 00002020
         01c00530 30000053 00f00005 00020820
Linux    01c00530 58080055 00f04005 00020820

DP3 v0,-c0
Windows  same four-record expansion
Linux    81c00530 50080055 00f04005 00020820
```

Windows emits the same four-record expansion for both positions: two subop-0
records, an intermediate MOV, and a final subop-2 record.  Linux instead
retains one subop-2 record and places NEG in the requested physical source.
Windows changes its pretarget A92 count/control from `92000001` to
`92000004`; Linux retains the ordinary executable `92000001`.  There is no
closed cross-driver native, count, or control-word contract.  Reject both
forms before allocation and do not canonicalize one into the other based only
on dot-product algebra.

## Safest structured swizzle rung

The smallest useful key is terminal `MOV oC0,v0.wwww`.  v0 is finite and
normalized, there is no constant-domain or arithmetic hazard, the result is
four equal lanes, and the current command-19 source descriptor already has
four explicit swizzle selectors.  Require:

- exactly one terminal MOV, source v0 index zero, `.wwww`, NONE modifier;
- full XYZW oC0 write, no SAT/scale, default FP32 controls;
- no texture, temporary, discard, depth output, control flow, or second
  output; and
- the exact ALU/control tuple above, constructed by the kernel.

Use flat logical v0 `(32,96,160,224)/255`.  Expected logical output bytes are
`(224,224,224,224)`.  Test `.xxxx`, `.yyyy`, `.zzzz`, and `.wzyx` as separate
later keys with exact expected rows `(32)*4`, `(96)*4`, `(160)*4`, and
`(224,160,96,32)`.  Use command-19's mandatory fresh D16 clear=1, LESS/write,
z=0.5, COPY, full target mask, guarded pages, and known-good draws before and
after.  Check color/depth guards, fences, reset/error counters, exact words,
and 1/64/1024 repetitions.

Constant MOV swizzles rank after v0 and require finite c0.  Swizzled MIN/MAX
remain finite-selection operations and rank before arithmetic swizzles.
MUL(v0,c0) remains finite for finite c0; ADD/SUB retain the conservative
per-lane `abs(c0) <= 0x7effffff` bound.  Mixed v0/c0 DP3 retains the prior
`abs(c0) <= 0x7dffffff` bound and needs separate scalar-result proof.

All NEG forms remain outside the runtime even when byte-matched.  Their live
tests must observe negative pre-target values with fixed A33, because ordinary
UNORM readback would clamp them and can falsely resemble a broken modifier.
