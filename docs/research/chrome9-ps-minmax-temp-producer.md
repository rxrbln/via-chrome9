# Chrome9 multi-record MIN/MAX TEMP producer boundary

Status: exact production-initialized Windows D3D and Linux 5.75 differential
for MIN/MAX writing an ordinary TEMP and a later MOV/MUL consuming it.  All
eight finalized programs match byte for byte.  This closes the native
destination and lifetime fields but does not admit MIN/MAX to the first
structured multi-ALU UAPI.

The Windows wrapper and production allocator sequence are documented in
[`chrome9-ps-temp-swizzle-neg.md`](chrome9-ps-temp-swizzle-neg.md).  The
retained differential JSON SHA-256 is
`12442307caf0ef6d654f20d001b4b5ff9e7d41ca1fe652b219bba803dd629a15`.

## TEMP destination encoding

Terminal and TEMP-destination MIN/MAX use the same arithmetic templates.
Changing oC0 to physical r0 changes only the destination class/id field:

```text
MIN oC0,v0,c0  01c00530 30080053 00f04005 000c0820
MIN r0,v0,c0   01c00530 30080053 00f04005 000c0020
MAX oC0,v0,c0  01c00530 30080053 00f04005 000b0820
MAX r0,v0,c0   01c00530 30080053 00f04005 000b0020
```

The subops remain MIN=`c` and MAX=`b`; r0 is ordinary destination class 0,
id 0.  A later `mov oC0,r0` finalizes to
`01c00530 301c0053 00f20005 000d0820`, with the TEMP last-use marker in
A d2.bit17.  Neither MIN nor MAX needs a special producer dependency bit.

## MUL consumers and slot-specific last use

Both arithmetic producers feed either native MUL source slot without a
schedule or template change:

```text
mul oC0,r0,c1  0140b6d0 30081053 00f20005 00000820  # A last use
mul oC0,c1,r0  0140b6d0 30200053 00f08105 00000820  # C last use
```

Windows raw output has the respective marker clear; both Windows
configuration two and Linux configuration two set exactly A d2.bit17 or C
d1.bit21.  MIN and MAX produce identical consumer records, confirming that
the last-use pass follows physical source operands rather than producer
opcode.

## Two-TEMP three-record chain

The matrix also covers a producer that consumes r0 and writes r1, followed
by a terminal read of r1:

```text
mov r0,c0
min r1,r0,v0
mov oC0,r1
```

Configuration two marks r0's last use in the middle MIN A slot and r1's last
use in the terminal MOV A slot:

```text
01c00530 301c0053 00f08005 000d0020
01c00530 30040053 00f20005 000c0060
01c00530 301c0053 00f20105 000d0820
```

The MAX variant deliberately puts r0 in C and proves the other slot:

```text
01c00530 301c0053 00f08005 000d0020
01c00530 30200053 00f04005 000b0060
01c00530 301c0053 00f20105 000d0820
```

The physical destinations are r0=`...0020` and r1=`...0060`.  The compiler
does not reuse r0's physical slot for the non-overlapping r1 value, consistent
with the previously recovered sorted logical-id allocation.

## Resource controls

Every two-record row has:

- TEMP write mask `00000001`;
- A90 `90058000`;
- A92 `92000002`.

Every three-record row has:

- TEMP write mask `00000003`;
- A90 `90058000`;
- A92 `92000003`.

A90 therefore stays in the same resource bucket when the chain grows from
one to two ordinary physical TEMPs and from two to three ALU records.  A92 is
the instruction count, not the TEMP count.  A91 and A93--A9a remain zero.

## Fail-closed consequence

The first structured multi-record UAPI remains MOV/MUL-only.  MIN/MAX TEMP
producers must stay rejected until all of these are independently true:

- one-record MIN/MAX pass their guarded numerical and NaN/signed-zero gates;
- the structured ABI names MIN/MAX rather than accepting native dwords;
- full XYZW, identity sources, no modifiers/SAT/scale, no WAW, and complete
  TEMP definitions are enforced;
- multi-record pixels validate both A-slot and C-slot consumption.

Once those gates pass, no new physical-allocation or last-use mechanism is
needed: the existing A/C slot rules and dense r0/r1 destination encoding are
byte-exact for these chains.

Exact tokens, raw Windows records, both finalized streams, and controls are
retained in
[`chrome9-ps-minmax-temp-producer-corpus.tsv`](chrome9-ps-minmax-temp-producer-corpus.tsv).
