# Chrome9 guarded MIN and ADD one-record rungs

## Result

The first candidate after MOV/MUL should be exactly `MIN oC0,v0,c0`:

```text
ALU       01c00530 30080053 00f04005 000c0820
A90-A9b   90058000 91000000 92000001 93000000
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000 9b000000
```

Both proprietary compilers byte-match that record.  It uses the already
recovered A/C source packing, takes only finite v0/c0 inputs, and cannot create
an infinity or NaN.  It is still compiler evidence, not permission to execute
until the guarded silicon oracle below passes.

ADD with exactly one v0 and one c0 is the second candidate.  Its B source
packing and literal-one C slot are compiler-exact, but it exercises more
native fields than MIN.  Arbitrary constant/constant ADD is outside this
rung.

The additional eight Windows/Linux compiler differentials are retained in
[`chrome9-min-add-rung-corpus.tsv`](chrome9-min-add-rung-corpus.tsv).

## First MIN validator contract

The first semantic key must be narrower than the hardware record:

- one `nir_op_fmin`, with source 0 exactly v0 and source 1 exactly c0;
- FP32 lanes, identity XYZW swizzles, NONE source modifiers, full XYZW oC0
  write, no saturation, and no destination scale;
- a terminal one-record fragment program with no temporary, texture, discard,
  depth write, control flow, or second output;
- all four c0 dwords finite by exponent-`ff` rejection before packet creation;
- default shader FP32 execution mode and no non-default per-instruction float
  controls for the first gate;
- the exact kernel-owned ALU and twelve-word control tuple above.

Reject source reversal initially even though the compilers byte-match it.
Reject `.aaaa`, NEG, partial masks, SAT, literal swizzles, and every other
operation.  Their encodings do not expand the semantic key until separate
numerical gates pass.  Validation failure must occur before reservation,
publication, fence allocation, or command-counter advance.

## Guarded live oracle

Use the already proven D16/RHW triangle envelope, BGRA8888 target, COPY blend,
full color mask, alpha test disabled, and fixed common state.  Surround the
candidate with a known-good MOV/MUL draw and require the following draw to
retire.  Canary-fill target padding and adjacent guarded pages.  Record fence
retirement, command-error state, reset count, target/depth guards, and the
exact submitted ALU/control words for every case.

Use a flat primary v0 logical RGBA of `(32,96,160,224)/255`.  The decisive
finite rows are:

| c0 logical RGBA | expected logical RGBA bytes | purpose |
| --- | --- | --- |
| `(1,0,0.5,0.5)` | `(32,0,128,128)` | alternates the winning source by lane; distinguishes MIN from either MOV |
| `(-1,2,0.5,-0)` | `(0,96,128,0)` | negative clamp, source selection, and signed-zero safety without claiming sign preservation |
| `(FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX)` | `(32,96,160,224)` | finite-endpoint closure and v0-winning path |
| `(-FLT_MAX,-FLT_MAX,-FLT_MAX,-FLT_MAX)` | `(0,0,0,0)` | negative finite endpoint and c0-winning path |

Run `known-good -> candidate -> known-good` for each row, then repeat the
alternating-source row for 1, 64, and 1024 draws.  Require exact pixels (with
the established BGRA channel conversion), unchanged guards, ordered fence
retirement, no reset or error-counter increment, and a succeeding known-good
draw.  A timeout, guard change, unexpected reset, or stale result rejects the
key and leaves MIN unadvertised.

A later source-order oracle may add `MIN oC0,c0,v0`.  A separate alpha-test
oracle is required for denormal preservation; normalized target readback
cannot distinguish a preserved subnormal from zero.  Neither is part of the
first gate.

## ADD source-B packing

ADD implements `B + A*C` with A=source 0, B=source 1, and canonical C=ONE.
For B, dword 0 contains:

| Field | Bits |
| --- | ---: |
| modifier | `3:0` |
| X/Y/Z/W three-bit selectors | `15:13`, `12:10`, `9:7`, `6:4` |
| six-bit id | `21:16` |
| three-bit class | `24:22` |
| finalized dependency flag | `25` |

Thus B=`c0.xyzw` contributes `00800530`; B=`c0.aaaa` contributes
`00806db0`.  C=ONE is the indivisible literal/special-class word
`30140924`, not a c0-class `.1111` source.  The full compiler records are:

```text
ADD v0,c0       00800530 30140924 00f04005 00000820
ADD c0,v0       00400530 30140924 00f08005 00000820
ADD v0.aaaa,c0  00800530 b0140924 00f0406d 00000820
ADD v0,c0.aaaa  00806db0 30140924 00f04005 00000820
```

Windows and Linux byte-match both `.aaaa` rows.  They do not byte-match NEG:
the Windows programmable ADD path sets d3.bit20 for either modified source,
while Linux clears it.  This differs from the common MIN path, where both
drivers agree and keep d3.bit20 clear.  Every ADD source modifier therefore
remains rejected; the companion bit is operation/path-specific.

## ADD finite constant domain

No magnitude cap beyond the existing finite-c0 check is required for the
exact v0+c0 rung under the required default round-to-nearest behavior.  v0 is
D3DColor-normalized in `[0,1]`.  The worst positive case is
`FLT_MAX + 1`, still far below the round-to-nearest overflow threshold
`FLT_MAX + 2^103`, so it rounds to finite `FLT_MAX`.  Adding nonnegative v0
to a negative c0 moves toward zero.  Every other finite c0 has still more
headroom.

This proof is deliberately tuple-specific.  It does not admit c0+c1,
NEG, a temporary, an unbounded float varying, an explicit non-default
rounding mode, or multi-record use.  Signed zero, denormal rounding, exact
addition, and endpoint safety still need live numerical coverage before ADD
becomes a runtime key.
