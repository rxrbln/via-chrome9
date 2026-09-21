# Chrome9 pixel depth-output compiler boundary

The D3DSPR_DEPTHOUT destination token `100f0800` is accepted by both
production compilers and sets their depth-output-present metadata.  A terminal
depth MOV uses native destination word `000d0920`.  Depth-only records
byte-match across Windows and Linux; a combined color-plus-depth program has
the same two records but the compilers choose opposite record order.  Packet
encoding is therefore closed, but context sideband ordering and silicon depth
export remain unproved, so depth output stays rejected.

## Token and records

`MOV oDepth,c0.wwww` compiles identically in both families:

```
01c00530 b01c0053 00f0806d 000d0920
```

`MOV oDepth,v0.zzzz` also byte-matches:

```
01c00530 201c0053 00f04049 000d0920
```

Both are phase 1 and set depth-output-present.  Windows reports color-output
count zero.  Windows uses A90=`90040000`; Linux configuration 2 uses
A90=`90058000`; both have A92=`92000001` and all remaining transition controls
zero.  The metadata acceptance and exact native destination distinguish the
real depth register from invalid `oC4`, which can alias the same native word
without setting depth-output-present.

For source-order token program `MOV oC0,v0; MOV oDepth,c0.wwww`, Windows emits
depth then color:

```
01c00530 b01c0053 00f0806d 000d0920
01c00530 301c0053 00f04005 000d0820
```

Linux configuration 2 emits color then depth:

```
01c00530 301c0053 00f04005 000d0820
01c00530 b01c0053 00f0806d 000d0920
```

Both set depth-present, stay in phase 1, and use A92=`92000002`; Windows also
reports one color output.  This order mismatch means the unordered record set
is compiler evidence, not a cross-driver executable byte sequence.  A new
backend must select and validate one target schedule deliberately.

The three-case differential JSON SHA-256 is
`d73508d0d93b8351533b1f0a0bba754051d3f0abd3313348941a15f622079a99`.
Exact tokens, controls and metadata are retained in
[`chrome9-depth-output-corpus.tsv`](chrome9-depth-output-corpus.tsv).

## Required A13 depth-source state

The compiler object depth flag is not descriptive metadata only.  The
recovered update path copies it into context-state byte `+0x4e.bit7` and
dirties group `0x00080000`.  The context image stores type-01 A13 at `+0x4c`,
so byte `+0x4e.bit7` is exactly A13 payload bit 23.  Its native name is
`HZSrcPS`: it selects pixel-shader Z instead of vertex/raster Z.  Therefore a
depth-writing shader must OR A13 with `0x00800000`:

```
LESS:   13010000 -> 13810000
ALWAYS: 13070000 -> 13870000
```

This bit is independent of the existing compare function in A13 bits 18:16
and the normalization factor in bits 7:0.  The VX900 A13 table labels bit 23
reserved, but the manual's fog constraint names `HZSrcPS` and states that Z is
calculated in the pixel shader when it is one.  The UMD dataflow plus that
manual cross-reference resolves the previously unknown emitted H2 word.
Uploading `000d0920` without A13.bit23 does not select shader depth; conversely
A13.bit23 must be cleared when returning to a color-only shader.  Fixed fog
from Z is forbidden by the same manual note, so the first key requires fog
disabled.

Keep depth output outside the structured UAPI until the kernel derives this
A13 delta from a bounded semantic key, validates one exact record ordering,
and restores/clears it transactionally on error and reset.  Do not expose raw
`oC4`/destination words or a user-controlled depth-source bit.

## Guarded live oracle

The safest first shape is full color `v0` plus terminal depth from finite
job-local `c0.w` constrained to [0,1], using the Linux order above and
A13=`13810000`.  Use command 19's mandatory fresh D16, depth clear 1,
LESS/write, and guarded color/depth
destinations.  First draw red at geometric z=0.75 with shader depth 0.25.  Then
draw green at z=0.5 with the already proven color-only shader.  Correct depth
export leaves red because 0.25 blocks the farther green; ignored export leaves
green because geometric 0.75 does not.  Read back both color and D16 and repeat
with depth 0.75/geometric 0.25 to catch reversed or stale sideband behavior.

Run a color-only draw before and after the depth-writing draw to prove enable
and clear transitions, then test the Windows record order only as a separate
portability experiment.  Depth-only rendering and interpolated `v0.z` remain
later gates.  Compiler bytes alone do not prove depth-test timing, depth write,
clamp, precision, early/late ordering, or reset restoration; every NIR
`store_deref` to fragment depth remains fail-closed until all gates pass.
