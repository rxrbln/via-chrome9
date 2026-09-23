# Chrome9 Fast-Z and coarse-Z metadata boundary

Status: exact register fields and the proprietary Linux Fast-Z state builder
are recovered.  The auxiliary-allocation and metadata lifetime are not.  Keep
both mechanisms disabled in the generic driver until those pieces and a
guarded clear/read/write sequence are closed.

This note prevents the control-panel name "Z compression" from being lowered
to an invented single register bit.  On Chrome9 HD, ordinary depth storage,
Fast-Z clear indexing, and coarse-Z rejection are three different mechanisms.
The already proven linear D16/D24 paths need neither optimization.

## Manual register split

The VX900 Part II manual defines these independent controls:

| word | field | function |
|---:|---:|---|
| A01 | bit 13 | enable coarse-Z testing for triangles |
| A01 | bit 9 | write/update the coarse-Z buffer |
| A27 | bit 0 | enable Fast-Z clear |
| A27 | `[11:8]` | Fast-Z scene-index length, 0 disables indexing |
| A27 | `[15:12]` | current scene index |
| A28 | `[23:0]` | separately stored stencil base, not a coarse-Z base |
| A29 | `[8:0]` | coarse-Z pitch in 32-byte units |
| A29 | `[21:20]` | coarse-Z memory location |
| A29 | bit 15 | one-shot coarse-Z tag-cache reset |
| A2A | `[23:0]` | coarse-Z base in 256-byte units |

A29 also carries conservative merge, inverse-depth, writeback, and forced
retest policy.  Consequently setting A01 bit 13 or 9 without a validated A29,
A2A, and separately allocated coarse-Z object is an out-of-bounds memory
request, not a harmless performance experiment.

## Exact proprietary Fast-Z builder

The executable oracle is `via_chrome9_dri.so` SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
function `STM_FastZClear_INV:0x195480..0x1955dc`.  Its hardware-state base is
the object loaded from context `+0x9430`; byte offsets `+0x9c/+0x9d` are the
low two bytes of A27.

The builder recognizes only these depth classes:

| private depth format | index-length capability source |
|---:|---:|
| D16 `101d` | device state `+0x380c` |
| D24X8/D24S8 `2032/2033` | device state `+0x3810` |
| fixed D32 / D32F `203f/2040` | device state `+0x3814` |

Every other format selects length zero.  If no depth surface is bound, the
builder clears A27 bit 0 and A27 `[15:8]`.  If the selected capability length
is zero, it does the same even when Fast-Z was requested.  Otherwise it:

1. builds the low-bit mask `(1 << length) - 1` and stores it in device state
   `+0x3818`;
2. sets A27 bit 0 from the requested enable;
3. writes `length & 0xf` to A27 `[11:8]`; and
4. writes `((depth_surface[+0x2f0] - 1) & mask) & 0xf` to A27 `[15:12]`.

The fourth step proves that Fast-Z is versioned metadata associated with a
depth surface and scene/clear generation.  A constant A27 scene index can
make stale physical depth appear current after a later clear.  The builder
also dirties the type-01 state transaction after changing A27; it does not
emit an ordinary depth clear or allocate metadata.

The only safe baseline state is therefore `A27=27000000`.  A first Fast-Z
implementation must recover the allocation represented by the depth-surface
metadata, its initialization, wrap behavior for one- through four-bit scene
indices, and the resolve/materialization rule before CPU mapping or texture
sampling.  A27 alone is insufficient.

## Exact coarse-Z reset transaction

`stmResetCoarseZBuffer_invi:0x176800..0x176941` emits a bounded two-write
type-01 transaction:

```text
FE020440 0000043c 00010000 cc000000
29008000
29xxxxxx                         # current A29 image
```

The first A29 write pulses bit 15, which the manual defines as an
auto-clearing coarse-Z tag-cache reset; the second restores the current A29
image.  The function then marks that state dirty in the vendor context.  It
does not clear the coarse-Z allocation and does not program A2A.  The separate
`sfmClearCoarseZBuffer_invi` path is much larger and owns the actual buffer
operation, reinforcing that the tag pulse is not a substitute for metadata
initialization.

The policy builder `STM_CoarseZ_INV:0x1938b0..0x19432f` modifies A01 and A29
through many render-state and primitive-class conditions.  It is not evidence
for an unconditional enable.  In particular the manual excludes lines, and
the executable selectively forces retest or disables rejection for state that
cannot safely use the optimized merge rules.

## Implementation and proof order

1. Keep A01 bits 13/9 clear and A27 zero for all ordinary depth, shadow, and
   depth-texture work.
2. Recover the auxiliary object sizing/alignment and A2A address producer for
   coarse-Z before adding either A01 enable.
3. Recover the Fast-Z per-surface metadata allocation and clear-generation
   lifetime before setting A27 bit 0.
4. Prove each optimization against a linear non-optimized reference with
   alternating full and partial clears, depth-write-disabled draws, CPU
   readback, and depth-as-texture transitions.
5. Only then treat the control-panel "Z compression" option as a capability;
   do not expose it as a public render feature or correctness prerequisite.

This is a performance ladder.  It must not block the full GL feature set:
ordinary uncompressed D16/D24 storage remains the conformance path.
