# Chrome9 pixel temporary lifetime finalizer

Status: exact Linux 5.75 compiler evidence for the native TEMP destination
field and configuration-2 lifetime markers.  This closes a correctness gap in
the proposed Mesa encoder: the three source flags previously called
"dependency bits" are not producer-edge flags.

This result supersedes the older A/B/C wording in
`general-shader-runtime-contract.md`, `shader-upload-oracles.md`, and
`via-5.75-shader-re.md`.  It does **not** rename TAU phase/order fields or
claim that every bit historically called a dependency is a lifetime marker;
the correction is limited to ALU d0.bit25, d1.bit21, and d2.bit17.

## Result

The exact physical TEMP destination is:

```text
d3[11:10] = 0              # TEMP destination class
d3[9:6]   = physical id    # 0..15 only
```

With the neutral low bits used by the MOV emitter, r0 through r15 therefore
produce d3 values `000d0020`, `000d0060`, ... `000d03e0`.  A production
compiler run with all sixteen temporaries simultaneously live maps every
logical id to the same physical id and emits all sixteen values.  There is no
extra high-bank flag for r12--r15.

r16 is a fail-open vendor frontend boundary, not a seventeenth register.  The
same 17-live input returns success, encodes its r16 destination as
`000d0020` (aliasing r0), and retains source id 16 in the later B operand as
`02100000`.  Mesa must reject when physical allocation would exceed sixteen;
it must not rely on vendor-style masking.  The first encoder may retain the
more conservative r0--r11 policy until r12--r15 execute on silicon.

## Source lifetime markers

Configuration-2 owns one flag adjacent to each encoded source:

| parallel source | flag |
|---|---:|
| B | d0.bit25 (`02000000`) |
| C | d1.bit21 (`00200000`) |
| A | d2.bit17 (`00020000`) |

Differential programs prove these are last-use/release markers for physical
TEMP-like values, not indications that an SSA producer exists:

- `mov oC0,r0` sets A.bit17 even though r0 has no producer;
- direct `mov oC0,t0` also sets A.bit17 without TEXLD;
- when r0 is read by two records, only its terminal read is marked;
- three distinct dying temporaries in B, C, and A set all three flags;
- when the same physical id occupies multiple slots in its last record, it is
  marked exactly once, avoiding a double release.

The observed same-record winner is A over C over B.  `mul oC0,r0,r0` marks
only A; `add oC0,r0,r0` marks only A; and a MAD with r0 in B+C marks only C.
This is consistent with the finalizer scanning B, then C, then A and replacing
the saved last-use location for an equal physical id.  The scan-order
explanation is an inference; the emitted winner and the requirement to mark
only one slot are exact.

Lifetime is per physical vec4, not per component.  A partial-write program
that reads r0.x into oC0.x and then r0.y into oC0.y leaves the first read
unmarked and puts A.bit17 only on the second.  This is one more reason for the
first backend to reject partial temporary merging: component SSA does not
remove the need to preserve whole-register liveness after packing.

Do not replace this pass with either of the tempting shortcuts:

```text
mark every TEMP source                       # double-releases repeated ids
mark every source having an earlier producer # misses accepted undefined/direct forms
```

For valid Mesa SSA, the safe algorithm is to schedule and allocate first,
then find the final scheduled source occurrence for each physical temporary.
When one instruction contains that id more than once, select A, else C, else
B.  Set only that slot's marker.  Reject a read-before-definition even though
the proprietary compiler accepts and marks it.

## Dead destinations

Configuration-2 sets d3.bit29 (`20000000`) on a temporary destination that is
never read.  For example:

```text
mov r0,c0       raw/final d3 000d0020 -> 200d0020
mov oC0,c1
```

The reused-source discriminator is stronger: in

```text
mov r0,c0
add r1,r0,c1   # r0 is read again later; r1 is never read
mul oC0,r0,c2
```

the ADD's r0 source remains unmarked, its unused r1 destination changes from
`00000060` to `20000060`, and only the terminal MUL source gets A.bit17.
Thus d3.bit29 is a destination-unused/dead marker, not a high-temp selector.
Its silicon effect remains untested.  A Mesa backend should run DCE and reject
or eliminate dead temporary writes rather than deliberately emitting bit29 in
its first executable multi-ALU rung.

The vendor pass is register-id based rather than a substitute for SSA.  A
write overwritten by another write to the same id before the id's eventual
read is not independently recognized as dead.  Mesa should preserve SSA until
allocation and avoid WAW forms in the initial backend.

## Patch-ready bounded compiler contract

The first multi-ALU successor can remain fail-closed with this order:

1. Lower to one acyclic block; copy-propagate and DCE.
2. Accept only already proven ALU operations and full-width FP32 values.
3. Build explicit SSA use lists; reject read-before-definition, indirects,
   partial-register merging, control flow, and WAW after allocation.
4. Schedule native ALUs/TAUs, then allocate at most 12 physical temps for the
   initial gate (16 is the encoding limit).
5. Rebuild source ids, destination ids, and resource masks from structured IR.
6. Mark exactly one final source occurrence per live physical temp, using the
   exact A/C/B same-record precedence above.
7. Require no dead temp destination after DCE; d3.bit29 must therefore be zero
   in the first accepted objects.
8. Compare the complete ALU/TAU/control object against offline goldens.  A90--
   A9b remain closed scheduler tuples and kernel-owned; no raw program upload
   follows from recovering these fields.

For NIR, `nir_op_mov`, `nir_op_fmul`, and any later admitted `fadd`/`fsub`/
`ffma` map to the already recovered native templates only after SSA sources
have been assigned physical ids.  The final-use pass is independent of opcode
but depends on actual parallel-slot assignment: SUB places its minuend in B
and subtrahend in A, while MUL places operands in A/C.  Therefore NIR source
number is not enough to choose a marker bit.

## Evidence and reproduction

The exact compact rows are in
[chrome9-ps-temp-lifetime-finalizer-corpus.tsv](chrome9-ps-temp-lifetime-finalizer-corpus.tsv).
They were generated with the checked-in
`tools/re/chrome9_pps_probe.c` using production H5 callbacks and both raw and
`CHROME9_POSTPROCESS=2` modes.  Probe source SHA-256 is
`f73e238a22ec86ab342c1b27ce339cd2dcc1796ba5c7e09681d8cab601f103f3`.

The complete 16-live configuration-2 trace is retained at
`/tmp/chrome9-linux-temp16-config2.txt`, SHA-256
`f9440622d387c352f2f2f51cd48c9a8afb2c6ff81ad0531da10eaa237749ce64`.
The 17-live overflow trace is
`/tmp/chrome9-linux-temp17-overflow.txt`, SHA-256
`c6cf97683c90a77cc035dff726534c4ca58e7c15f409e09442b41c739b5f86a8`.
These paths are reproducibility artifacts, not runtime dependencies.

This is compiler/static evidence.  r12--r15, dead-destination bit29, and all
multi-ALU schedules still require guarded silicon gates before capability
advertisement.
