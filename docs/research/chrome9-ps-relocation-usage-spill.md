# Chrome9 PS relocated-resource usage spill

Status: exact static agreement among Linux 5.75 and both Windows UMD
finalizers, plus hardware-free production Linux compiler/finalizer boundary
differentials.  This is compiler-object evidence, not silicon proof.

## Result

Configuration two builds a 16-bit usage bitmap from the AINST-class physical
native-location references discovered in the finalized ALU stream.  Locations
0 through 14 are stored in instance `+0x6c` and become the low 15 payload
bits of A90.
Location 15 cannot remain there because A90 bits 16:15 contain the resource
bucket.  The finalizer therefore moves usage bit 15 to A91.bit23:

```c
uint32_t a90_usage = usage16 & 0x00007fff;
uint32_t a91_usage = (usage16 & 0x00008000) << 8;

instance->a90_aux_6c =
        (instance->a90_aux_6c & 0x00040000) | a90_usage;
instance->a91 = a91_other | a91_usage;
```

The exact location map is:

| location id | instance field | uploaded usage field |
|---:|---|---:|
| 0 | `+0x6c` | A90 `0x00000001` |
| 1 | `+0x6c` | A90 `0x00000002` |
| 2 | `+0x6c` | A90 `0x00000004` |
| 3 | `+0x6c` | A90 `0x00000008` |
| 4 | `+0x6c` | A90 `0x00000010` |
| 5 | `+0x6c` | A90 `0x00000020` |
| 6 | `+0x6c` | A90 `0x00000040` |
| 7 | `+0x6c` | A90 `0x00000080` |
| 8 | `+0x6c` | A90 `0x00000100` |
| 9 | `+0x6c` | A90 `0x00000200` |
| 10 | `+0x6c` | A90 `0x00000400` |
| 11 | `+0x6c` | A90 `0x00000800` |
| 12 | `+0x6c` | A90 `0x00001000` |
| 13 | `+0x6c` | A90 `0x00002000` |
| 14 | `+0x6c` | A90 `0x00004000` |
| 15 | `+0x70` | A91 `0x00800000`; A90.bit15 is clear |

This map is about finalized native-location occupancy.  It is not a
temporary-register declaration mask or sampler mask.  Ordinary temp-only
programs leave it zero.  The currently proved token-level producer is a
`D3DSPR_MISCTYPE` source, whose `0xcf` sentinel is relocated by
`stmUpdateConfig`/`stmReplaceLocationTexIndexAINST` to location 3, 4, 7, or
15 according to the resource bucket.

## Three-driver static provenance

Linux `stmPPS_PostTranslationSetting_invi` accumulates the location bitmap at
`0x0019b513..0x0019b5a4`.  For configuration two, the branch at
`0x0019bc40..0x0019bc5b` tests bit 15, ORs `0x00800000` into the A91 local,
and clears bit 15 before storing A91 at `0x0019b625` and the A90 auxiliary
word at `0x0019b62b..0x0019b639`.

Both Windows finalizers implement the identical operation:

| binary | spill branch VA | RVA | file offset | final A91 store | final A90-aux store |
|---|---:|---:|---:|---:|---:|
| D3D `VTGUModeDX32A.dll` | `0x102031aa..0x102031c3` | `0x002031aa` | `0x002025aa` | `0x102031d6`, object `+0x60` | `0x102031d9..0x102031ea`, object `+0x5c` |
| OGL `VTGOGL32A.dll` | `0x102561ca..0x102561e3` | `0x002561ca` | `0x002555ca` | `0x102561f6`, object `+0x60` | `0x102561f9..0x1025620a`, object `+0x5c` |

The two 26-byte spill branches are byte-identical, SHA-256
`5da5ab3aee18087a8f9a5017105374dde3b0584a6f34e3ceaf28ff16aba0e406`.
They require configuration `2`, test the usage local with `0x8000`, OR
`0x800000` into the A91 local, and AND the usage local with `0xffff7fff`.
The object passed to the finalizer is instance `+0x10`, so object `+0x5c`
and `+0x60` are instance `+0x6c` and `+0x70` respectively.

Binary SHA-256 values are
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`
(Linux),
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
(D3D), and
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`
(OGL).

## Genuine MISC boundary differentials

The compact exact rows are in
[`chrome9-ps-relocation-usage-spill-corpus.tsv`](chrome9-ps-relocation-usage-spill-corpus.tsv).
Two independent production families reach every updater result and confirm
that the selected relocation id becomes the usage bit.

An ALU-only family allocates distinct r0..rN-1 with independent MOVs and
then emits `MOV oC0,vPos`.  It reports active tuple `(0,N,0,0)`:

| N | updater result | A90 | A91 | terminal source id |
|---:|---:|---:|---:|---:|
| 3 | 3 | `90058008` | `91000000` | 3 |
| 4 | 4 | `90050010` | `91000000` | 4 |
| 5 | 7 | `90048080` | `91000000` | 7 |
| 6 | 7 | `90048080` | `91000000` | 7 |
| 7 | 15 | `90040000` | `91800000` | 15 |

A TAU-first family emits N distinct texture samples and terminates with
`ADD oC0,r[N-1],vPos`.  It reports `(N,N,N,N)` and exercises the MISC
sentinel in source slot B:

| N | updater result | A90 | A91 | final B word |
|---:|---:|---:|---:|---:|
| 3 | 3 | `90018008` | `91000007` | `02c30530` |
| 4 | 4 | `90010010` | `9100000f` | `02c40530` |
| 5 | 7 | `90008080` | `9100001f` | `02c70530` |
| 6 | 7 | `90008080` | `9100003f` | `02c70530` |
| 7 | 15 | `90000000` | `9180007f` | `02cf0530` |

The id-15 rows are the decisive discriminator.  A clean implementation that
simply ORs `1 << 15` into A90 would corrupt the bucket field and would fail
to reproduce all three proprietary finalizers.  A91.bit23 must instead be
kept independent of the sampler mask in A91 bits 7:0.  In the displayed
texture rows `a91_other` is exactly that sampler mask; the static formula
does not assume it is the only possible unrelated A91 field.

The stress shaders deliberately retain dead independent allocations to hit
exact resource boundaries.  They prove compiler/finalizer accounting only
and are not candidates for runtime admission.  vPos/vFace linkage and
silicon meaning remain unproved, so MISC inputs remain fail-closed.
