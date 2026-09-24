# Chrome9 PS miscellaneous-input relocation

Status: exact static producer lifecycle in Linux 5.75 and both Windows UMDs,
hardware-free production Linux compiler/finalizer differentials, and the
VX900 draw-time A90/A8A consumers.  This identifies the previously opaque
`compiled_object+0x70` / instance `+0x80` resource-bucket and MISC-enable
predicate.

## Result

Instance `+0x80` is a sticky flag set when a parsed instruction references a
source whose D3D register type is `D3DSPR_MISCTYPE` (17).  In PS 2.0 the two
token numbers zero and one are accepted by the vendor as vPos/misc0 and
vFace/misc1, but this is not a valid D3D profile contract: Microsoft defines
vPos and vFace as PS 3.0 registers, with declarations required.  The flag
therefore proves only that the vendor accepts and relocates an out-of-profile
MISC-class token.  It means that the compiled object needs a relocated
miscellaneous raster input; it is neither A91 nor a sampler mask, and it does
not distinguish face from position.

The shared instruction builder reconstructs the split D3D register type from
each of four possible source token words.  The Windows test is exactly:

```c
split_type = ((source >> 20) & 0x700) | (source & 0x1800);
if (split_type == 0x1100)       /* D3DSPR_MISCTYPE == 17 */
        instance->aux80 = 1;
```

All four source positions at normalized instruction offsets `+8`, `+0xc`,
`+0x10`, and `+0x14` are tested.  Destination tokens are not tested.  The
initializer clears the flag once; the builder only sets it, so one MISC
source anywhere in the shader makes it remain set for finalization.

## Cross-driver producer provenance

The Windows instruction-builder bodies are byte-identical for 212 bytes:

| binary | VA | RVA | file offset | four set sites |
|---|---:|---:|---:|---|
| D3D `VTGUModeDX32A.dll` | `0x101db2f0` | `0x001db2f0` | `0x001da6f0` | `0x101db34e`, `0x101db373`, `0x101db398`, `0x101db3bb` |
| OGL `VTGOGL32A.dll` | `0x10243af0` | `0x00243af0` | `0x00242ef0` | `0x10243b4e`, `0x10243b73`, `0x10243b98`, `0x10243bbb` |

Their common body SHA-256 is
`f72b41f4b04b624401585a03da82d3fc71680ffb8c0e298582a955e1ec830994`.
The D3D and OGL constructors clear instance `+0x80` at `0x101ddc50` and
`0x102463f0`.  Their A90 updaters read compiled-object `+0x70`, the same
instance field after the `instance+0x10` base adjustment, at `0x102024ed` and
`0x1025556d`.

Linux implements the same lifecycle with
`stmInitializePixelShaderInstance_inv:0x001b81de` clearing the field,
`stmSetMSInst_invi:0x001b5440` scanning four sources and setting it at
`0x001b54b8`, and `stmUpdateConfig:0x00198fee` reading it through the adjusted
object base.  Within the recovered pixel parser/compiler/finalizer path these
are the producer, initialization, and consumer sites.  Unrelated driver
structures also have members at offset `+0x80` and are not part of this
lifecycle.

## Smallest vendor trigger

The smallest vendor-accepted, out-of-profile PS 2.0 trigger is:

```text
ffff0200                       ps_2_0
03000001 000f0800 10e41000    mov oC0,vPos
0000ffff                       end
```

Before configuration two it has resource tuple `(0,0,0,0)`, aux80=1, and:

```text
A90 contribution 00040000
ALU              01c00530 301c0053 00f0cf05 000d0820
```

The MISC source occupies native slot A and uses reserved source id `0xcf`.
Configuration two increments active0 from zero to one, selects bucket
`0x18000`, returns relocation id 3, and rewrites the complete object to:

```text
A90..A93 90058008 91000000 92000001 93000000
ALU       01c00530 301c0053 00f2c305 000d0820
```

That A90 is the finalized compiler object, not yet the VX900 draw-time image.
For chip id 5, Linux `stmUpdatePS_inv:0x180119..0x180140` and Windows D3D
`0x1015b0dc..0x1015b0f0` independently copy this same predicate into A90 bit
19 before upload.  The executable value is therefore `900d8008`.  The
adjacent MISC-only raster update also owns A8A; native upper-left/no-inversion
uses explicit VX900 `A8A=8a020000`.  Exact register semantics, generation
dispatch, and live negative history are in
[`chrome9-vpos-cmp-frc-contract.md`](chrome9-vpos-cmp-frc-contract.md).

The `0xcf -> 3` rewrite and source-A final-use flag are both visible in d2.
`mov oC0,vFace` with source token `10e41001` produces the exact same raw
record, finalized record, metadata, and controls.  Thus the compiler proves
the MISC-class relocation but does **not** prove distinct vPos/vFace hardware
semantics.  Both inputs remain runtime-rejected until raster linkage and a
guarded silicon oracle distinguish them.

MISC also relocates correctly in the other parallel source slots.  With no
TAUs, hence relocation id 3:

```text
ADD c0,vPos   B raw 00cf0530 -> final 02c30530
MIN c0,vPos   C raw 300cf053 -> final 302c3053
MOV vPos      A raw 00f0cf05 -> final 00f2c305
```

The added d0.bit25, d1.bit21, and d2.bit17 fields show that the configuration
pass gives the relocated MISC value the same terminal-use treatment as other
temporary-like resources.  It is not a constant/input source that can bypass
the final-use pass.

Repeated MISC operands use the last occurrence in the finalizer's native
A-to-C-to-B scan on the last instruction: B wins over C, which wins over A.
`MUL(vPos,vPos)` therefore releases C and `MAD(vPos,vPos,vPos)` releases B.
The decisive noncommutative row is `SUB(vPos,vPos)`: semantic source zero is B
and semantic source one is A, yet B receives the marker.  A two-record MOV
then ADD leaves the first record live and releases final B.  This is the
opposite of TEMP alias precedence, which prefers A, then C, then B.

ABS follows the same MISC-native order even though its repetitions are
synthetic.  One `ABS oC0,vPos.xyxy` token expands to A=vPos, C=vPos,
B=-vPos, and configuration two emits
`02c30418 100c3041 00f0c304 00090820`: only synthesized B has the terminal
marker.  Ordinary TEMP ABS instead follows TEMP alias precedence and releases
A.  A semantic compiler must choose the lowest-valued native slot in this
driver's B=0/C=1/A=2 enumeration for repeated POSITION on its last
instruction, including ABS's expanded C/B occurrences.  It cannot use UAPI
source order or mark A merely because ABS has one semantic source.

## Real relocation-code ladder

To exercise every configuration-two return without injecting private state,
the probe prefixed the terminal vPos MOV with N independent texture samples
that all overwrite r0.  These WAW forms are compiler discriminators only,
not acceptable Mesa schedules:

| N | pre-final tuple | adjusted0/active1 | bucket | relocation | final A source id |
|---:|---|---|---:|---:|---:|
| 0 | `(0,0,0,0)` | `1/0` | `0x18000` | 3 | 3 |
| 4 | `(4,1,4,4)` | `5/1` | `0x10000` | 4 | 4 |
| 5 | `(5,1,5,5)` | `6/1` | `0x08000` | 7 | 7 |
| 8 | `(8,1,8,8)` | `9/1` | `0` | 15 | 15 |

The raw terminal ALU is `...00f0cf05...` in every row.  Final d2 is
`00f2c305`, `00f2c405`, `00f2c705`, or `00f2cf05`, respectively.  This is a
direct dynamic confirmation that the updater return feeds
`stmReplaceLocationTexIndexAINST`, rather than being unused status.

## Coexistence with ordinary T inputs

One ALU can read MISC and an ordinary T input simultaneously, but the active
texture-resource result is deliberately schedule- and slot-sensitive.  The
production configuration-two results for commuted XYXY MULs are:

```text
MUL oC0,vPos.xyxy,T0.xyxy
resources 0 0 0 0
ALU 0140b6d0 102c0041 00f2c304 00000820
A90..A93 90058009 91000000 92000001 93000000

MUL oC0,T0.xyxy,vPos.xyxy
resources 1 0 1 0
ALU 0140b6d0 102c3041 00f2c004 00000820
A90..A93 90058009 91000000 92000001 93000000
```

The programs are algebraically equivalent, and both A90 images retain bit
zero for direct T0 plus bit three for relocated MISC.  They differ in A00's
third resource output: the native resource walker considers at most one
class-3 source per ALU, in source-slot priority A, C, then B.  In the first
record MISC occupies A, wins that selection, and contributes no external
texture component; T0 in C is not revisited.  In the commuted record T0 is A,
so it contributes B[0] and yields resource count one.  This exactly matches
the `stmGetActiveTexTmpNum` control flow at Linux
`0x19a550/0x19a562/0x19a572` and Windows
`0x1020200d/0x1020229b/0x10202348`.

This is a safety constraint for the clean compiler.  It must compute A00 from
the scheduled native source slots, not from a semantic union of POSITION and
varyings, and it must not reorder commutative operands after resource
analysis.  MISC still participates independently in the relocation bucket
and final direct-route mask.

A two-record boundary proves that the MISC relocation bucket also cannot use
the number of compact varying coordinates:

```text
MAD r0,T0,T1,T2
MAD oC0,r0,vPos,T3

resources 1 1 1 0
ALU 02c20530 302c1053 00f2c005 00000020
    02c30530 300c3053 00f20005 00000820
A90..A93 9005800f 91000000 92000002 93000000
```

All four T inputs are compacted and appear in A90 bits zero through three,
but active0 is one: the first record selects A-slot T0 and the second selects
C-slot MISC, which hides B-slot T3 from this resource walk.  MISC adjustment
therefore changes active0 from one to two, retains bucket `0x18000`, and
relocates to id 3.  Selecting the bucket from `coordinate_count=4` would
instead use adjusted count five, incorrectly choose id 4 and produce
`9005001f`.  MISC relocation must happen after the scheduled resource result
is known, or through an equivalent two-pass encoder; choosing it during
semantic varying allocation is not exact.

The exact first two resource outputs are high-water values over the scheduled
maps, not semantic counts:

```text
active0 = max(1 + highest nonzero B, 1 + highest nonzero C)
active1 = 1 + highest nonzero A
```

Empty maps contribute zero.  Linux computes the B/C maximum at
`0x19a6d5..0x19a71b` and writes it through output zero at
`0x19aa2e..0x19aa31`; it scans A at `0x19a6b8..0x19ab79` and writes output
one at `0x19aa33..0x19aa3c`.  Therefore the B-only third output used by FE
A00 is not generally a valid substitute for A90's active0: internally
defined coordinate slots in C contribute to active0 but not A00.

The boundary object also exposes a collision: the proprietary finalizer gives
both vPos and direct T3 native class 3/id 3 in the second record, and their
usage shares A90 bit three.  That result is byte-exact but has no silicon
semantic proof; it may be an unsupported compiler corner case.  A safe first
implementation should reject a program when the selected MISC location is
already occupied by a direct T coordinate.  It must not silently choose a
different bucket, because that would no longer be the vendor algorithm.

The same coexistence survives every relocation bucket.  Prefixing N WAW
texture samples before `MUL oC0,vPos.xyxy,T0.xyxy` gives:

| N / tuple | MISC id | final ALU dword 2 | A90 | A91 | A92 | A93 |
|---|---:|---:|---:|---:|---:|---:|
| 4 / `(4,1,4,4)` | 4 | `00f2c404` | `90010011` | `9100000f` | `92040001` | `93000008` |
| 5 / `(5,1,5,5)` | 7 | `00f2c704` | `90008081` | `9100001f` | `92050001` | `93000010` |
| 8 / `(8,1,8,8)` | 15 | `00f2cf04` | `90000001` | `918000ff` | `92080001` | `93000080` |

At ids 4 and 7, the MISC location bit and ordinary T0 bit coexist in A90.
At id 15, MISC usage spills to A91 bit 23 while T0 remains A90 bit zero.
The WAW prefixes are offline relocation discriminators and remain unsuitable
for runtime admission.

The complete final control/record rows are in
[`chrome9-ps-misc-input-relocation-corpus.tsv`](chrome9-ps-misc-input-relocation-corpus.tsv).
At the eight-texture overflow boundary final A91 is `0x008000ff`, not merely
the sampler mask `0xff`; this exact compiler output is retained but the
meaning of A91.bit23 is not inferred here.

## Backend boundary

A clean backend may model the silicon-proven vPos route as a special raster
input, reserve the resource id chosen by the A90 bucket, rewrite every native
A/B/C occurrence, and include it in terminal-use analysis.  It must not
allocate physical id
`0xcf`, upload a raw sentinel, confuse aux80 with A91, or choose the bucket
from the final sampler mask.  Because vPos and vFace compile byte-identically
and no valid-profile vFace producer is recovered, FACE must remain rejected;
the byte-identical invalid-profile token is not a second runtime input.

Evidence uses `tools/re/chrome9_pps_probe.c`, SHA-256
`f73e238a22ec86ab342c1b27ce339cd2dcc1796ba5c7e09681d8cab601f103f3`.
Binary SHA-256 values are
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`
(Linux),
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
(D3D), and
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`
(OGL).  No row is silicon proof.
