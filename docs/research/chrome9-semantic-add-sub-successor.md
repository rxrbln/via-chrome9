# Chrome9 semantic compiler ADD/SUB successor

Status: patch-ready encoder and lifetime design from byte-identical
Linux/Windows compiler records.  ADD and SUB arithmetic remain numerical
silicon candidates, not advertised runtime operations.

## ADD is the first source-B operation

The smallest ADD key is terminal full-vector `oC0 = v0 + c0`:

```text
ALU       00800530 30140924 00f04005 00000820
A90-A93   90058000 91000000 92000001 93000000
```

Chrome9's underlying form is `B + A*C`.  ADD places logical source 0 in A,
logical source 1 in B, and installs the indivisible literal C=ONE.  The base
record before packing A, B, and the destination is:

```text
00000000 00140924 00f00000 00000820
```

The first Mesa key should keep the exact operand order A=v0, B=c0.  The
reversed compiler record is exact but does not need to be admitted until a
separate signed-zero/rounding oracle.

## Exact source-B packer

Source B is entirely in dword 0:

| value | d0 bits |
|---|---:|
| modifier | `[3:0]` |
| W selector | `[6:4]` |
| Z selector | `[9:7]` |
| Y selector | `[12:10]` |
| X selector | `[15:13]` |
| six-bit id | `[21:16]` |
| three-bit file | `[24:22]` |
| terminal-read/release | `[25]` |

The structured encoder is therefore:

```c
record[0] &= ~0x03ffffff;
record[0] |= modifier |
             swizzle_w << 4 |
             swizzle_z << 7 |
             swizzle_y << 10 |
             swizzle_x << 13 |
             id << 16 |
             file << 22 |
             last_use << 25;
```

For identity sources this yields TEMP r0=`00000530`, INPUT v0=`00400530`,
and CONSTANT c0=`00800530`.  The first one-record ABI continues to require
id zero, NONE modifier, and identity XYZW, but the encoder must still mask and
range-check every field rather than OR into a caller-provided word.

The literal C word `00140924` is part of the ADD template.  It is not c0 with
a ONE swizzle and must never consume a logical source or constant mask.

## Physical lifetime mapping

The current semantic finalizer counts logical sources and assigns the
terminal marker by source order because MOV/MUL happen to map source 0=A and
source 1=C.  ADD maps source 0=A and source 1=B.  It therefore remains correct
only if the encoder explicitly associates each logical operand with its
physical slot before duplicate arbitration:

```text
ADD r0,c0   final r0 in A -> d2.bit17
ADD c0,r0   final r0 in B -> d0.bit25
ADD r0,r0   final in A+B  -> A wins; only d2.bit17
```

The exact production terminal records after a `mov r0,v0` producer are:

```text
00800530 30140924 00f20005 00000820  # ADD r0,c0
02000530 30140924 00f08005 00000820  # ADD c0,r0
00000530 30140924 00f20005 00000820  # ADD r0,r0
```

A robust compiler should store a physical-slot tag for each scheduled native
operand and let the second pass choose A over C over B for duplicate final
reads.  Do not extend the existing `s==0` shortcut as a general rule.

## Why SUB must follow the slot refactor

SUB uses the same A/B packers but reverses logical placement:

```text
logical src0 -> B
logical src1 -> A
C            -> NEG_ONE
native form  -> B + A*(-ONE)
```

Its base record is:

```text
80000000 00140924 00f00000 00000820
```

Thus `SUB r0,c0` releases B, while `SUB c0,r0` releases A.  When both operands
are r0, A belongs to logical source 1 and must win.  A logical-source-index
finalizer would choose source 0/B and silently emit the wrong record.  SUB is
therefore forbidden until operand-to-slot mapping is a first-class scheduling
result.

## Fail-closed ADD classifier

The first NIR/runtime admission should require:

- one terminal `nir_op_fadd`, source 0 exactly v0 and source 1 exactly c0;
- FP32 vec4, identity swizzles, NONE modifiers, no SAT/scale, full oC0;
- no temp, texture, discard, depth output, control flow, second output, or
  explicit non-default float mode;
- each c0 sign-cleared bit pattern at most `0x7effffff` (FLT_MAX/2).

The constant bound is necessary because v0 is finite in `[0,1]`, while an
uncharacterized positive rounding mode could turn `FLT_MAX+1` into infinity.
The symmetric FLT_MAX/2 bound makes the result finite under every IEEE
rounding direction.  It does not prove exact rounding, denormal handling, or
signed-zero behavior.

Reuse of the existing fixed-size structured request is possible because it
already has two source records.  As with MIN, use a separately coordinated
capability rather than silently broadening capability 41's documented
MOV/MUL meaning.  The kernel rebuilds the exact native record; raw dwords and
literal-C controls never cross UAPI.

## Silicon gate

Use a guarded D16/RHW program batch bracketed by known-good MOV/MUL draws.
Choose asymmetric exactly representable values, for example primary bytes
`(32,64,96,128)` and c0 `(0.25,0.5,0.125,0.0)`, then compare against the exact
rounded normalized BGRA result.  Add zero, negative bounded constants, and
the FLT_MAX/2 endpoint as separate rows.  Require exact target pixels, intact
guards, ordered retirement, clean engine status, no reset count change, and a
successful following known-good draw at 1, 64, and 1024 iterations.

SUB gets a separate noncommutative oracle only after the slot-aware finalizer
and ADD gate pass.

## Evidence

The byte-matched one-record rows are in
[chrome9-next-alu-one-record-corpus.tsv](chrome9-next-alu-one-record-corpus.tsv).
The A/B terminal-read differentials are in
[chrome9-nested-terminal-alu-production-corpus.tsv](chrome9-nested-terminal-alu-production-corpus.tsv).
The bounded constant proof is independently recorded in
[chrome9-min-add-rung.md](chrome9-min-add-rung.md).

