# Chrome9 repeated-texture production object and sampler namespace

This note records generic compiler and state facts.  It does not classify a
particular application shader.  The byte-exact data lives in
`chrome9-repeated-texture-production-corpus.json` and is checked by
`tools/re/chrome9_repeated_texture_corpus_check.py`.

## Authoritative eleven-fetch object

The Linux 5.75.32.87a production compiler was called through
`stmPPSCompile_invi`, then finalized with
`stmPPS_PostTranslationSetting_invi(object, 2)`.  The source repeats this
generic topology eleven times:

```text
coord_i = T0.xy + c[i].xy
sample_i = TEX(s0, coord_i)
accum_i = sample_i * c[i].z + accum_(i-1)
oC0.rgb = accum_10.rgb
oC0.a = c11.x
```

The finalized object has 35 ALU records, 11 TAU records, active-resource
tuple `(8,3,1,1)`, and controls:

```text
A90 90048001   A91 910000fe   A92 920b0023
A93 930007ff   A94 94000000
A95 95492492   A96 96000092   A97 97000000
A98 98000000   A99 99000000   A9a 9a000000
A9b 9b000000
```

The input token SHA-256 is
`becc27dd36715d7f56ca419a11c7d59ecd6c57f4784924013197bd9fcbeb303c`.
Packed little-endian native checksums are:

| stream | SHA-256 |
|---|---|
| 35 ALU records | `ae8f79211b18dd4677cb82d848f4b5b5205e0c4156a81a5bb06a48b8289be2ac` |
| 11 TAU records | `f131122d5e80accbf28245c5e9b85893b2b41bd95df737f8d348f034135ae2ce` |
| A90--A9b | `0289a33818924ab25000b9a35271abc6d11baa71d11df88202efe2bf16d36144` |
| ALU then TAU then controls | `b59f3de9031aad582edbe57772f29206372b4407420fc8e9553e48200af77b9e` |

This is production-finalizer evidence, not silicon proof.  The matching
Windows wrapper accepts 35 ALU and 11 TAU records, but leaves every TAU as
raw `201101b0`; its object is not an execution oracle.  The Linux finalized
stream is authoritative for dependency and lifetime bits.

## Transition bitmap packing

A93/A94 and A95--A9a do **not** form arrays of ordinary 32-bit bitmap
payloads.  The high byte is the register address, leaving 24 payload bits in
each word.  For a transition after logical record index `i`:

```text
word = i / 24
bit  = i % 24
payload[word] |= 1 << bit
```

A93/A94 contain TAU-to-ALU transitions.  A95--A9a contain ALU-to-TAU
transitions.  The eleven-fetch schedule switches after every TAU index
`0..10`, producing A93 payload `0007ff`.  Its ALU-to-TAU indices are
`1,4,7,10,13,16,19,22,25,28,31`.  Indices through 22 produce A95 payload
`492492`; indices 25, 28, and 31 become local A96 bits 1, 4, and 7, hence
A96 payload `000092`.  Treating A95/A96 as 32-bit chunks would generate the
wrong object.

A90 bit 18 selects an ALU-first schedule.  A92 is exactly
`(tau_count << 16) | alu_count`.  Execution ends from these counts and maps;
there is no native END record.

## A91 is not a sampler mask

A91 is the finalized TAU coordinate/source physical-slot mask.  The repeated
object allocates computed coordinates in slots 1 through 7, so A91 is `fe`
even though every fetch uses logical sampler zero.  An independent production
probe samples logical stages `0,1,2,4,5`, but gives every fetch T0; it emits
A91=`1`, not the logical sampler mask `0x37`.

That sparse probe has active tuple `(1,8,1,6)`.  The fourth value and compiled
object `+0x38=6` are the highest logical sampler index plus one, not the number
of distinct samplers.  Its finalized TAUs are:

```text
201001b0 211101b1 221201b2 241301b4 351401b5
```

Sampler identity is duplicated in TAU bits 27:24 and 3:0.  Separate one-fetch
probes of s0, s1, s7, s8, and s15 emitted respectively
`301001b0`, `311001b1`, `371001b7`, `381001b8`, and `3f1001bf`, while A91
remained `1` in every case.  The native field is four bits, but the shipping
OpenGL limit is eight units and only stages 0/1 have live hardware proof.
Validate the logical ID before encoding and expose no more than s0--s7 until a
distinguishable eight-stage guarded test passes.

A second sparse probe pairs those samplers with logical coordinates
T0,T1,T2,T4,T5.  The production allocator compacts the coordinate namespace
to physical slots 0..4 and emits:

```text
TAU = 301001b0 311111b1 321221b2 341331b4 351441b5
A91 = 9100001f
active tuple = (5,8,5,6)
object +40 = 00000037   # logical texture-coordinate use
object +70 = 0000001f   # allocated TAU coordinate-slot use
```

This directly proves that sparse logical sampler IDs remain sparse in the TAU
sampler nibbles while sparse logical coordinate IDs are compacted separately.
Neither `+40` nor A91 may be substituted for the logical sampler set.

## Per-stage state namespace

The shader sampler number and all state selectors must agree.  For validated
logical stage `n` in `0..15`, vendor initialization and builders give:

```text
image H2 selector       = (n << 24) | 00020000
sampler H2 selector     = ((0x20 + n) << 24) | 00020000
mip L1--L8 H3 selector = 10030000 | (n << 3)
```

The image packet owns base A00, A18, dimensions A20, pitch/range A21, log2/POT
A22, and image-format A30.  The sampler packet owns LOD A30, filter A31, wrap
A32, and optional border A35/A36.  Image A30 and sampler A30 are different
register banks despite sharing the subaddress.

Reusable ordinary sampler triples are:

| intent | sampler A30 / A31 / A32 |
|---|---|
| nearest L0 repeat | `30000000 31100000 32000052` |
| bilinear L0 repeat | `30000000 31102490 32000052` |
| bilinear + nearest mip L0--L1 repeat | `30000040 31102491 32000052` |
| trilinear L0--L1 repeat | `30000040 31102492 32000052` |
| nearest L0 clamp | `30000000 31100000 32000049` |
| bilinear L0 clamp | `30000000 31102490 32000049` |

For each active stage, emit a complete image and sampler state image; insert
its mip table when a nonzero level is reachable.  Do not infer stage state
from A91.  Bind and reserve the texture BO named by every TAU logical sampler,
and reject a program if its sampler set and submitted state/BO set differ.

The generic three-source refract path uses stages 0, 1, and 2.  Terrain's
fragment shader uses sparse stages 0, 1, 2, 4, and 5; stage 3 is consumed only
by its vertex shader.  The sparse production object proves that the fragment
TAU namespace can express exactly this non-contiguous set.  It does not prove
D24 render-to-texture, multi-stage cache closure, mip tails, or vertex texture
fetch, all of which remain separate capability gates.

For fixed-function general texture state, `via3DEmitState_H5` constructs the
count from the enabled-stage high-water mark:

```text
general A00 = ((high_water << 4) & 000000fe) | 1
general A08 = 08010100 | (high_water & 00fefeff)
```

Consequently a sparse stage set ending at s5 must retain high-water six and
use A00=`00000061`, not popcount-five A00=`00000051`.  Its final A01--A03 and
A08--A0a coordinate linkage must describe the compiler-allocated physical
coordinates; the TAU sampler nibble alone does not create that linkage.  This
six-stage sparse combination is exact static/compiler evidence and still
requires a distinct-color live state-closure test.

### Sparse-stage ABI audit

The high-water rule applies to the semantic draw ABI as well as to A00/A08.
`texture_count` cannot mean the population count of active samplers when TAU
records retain their logical sampler ids.  The production sparse object above
uses sampler nibbles `0,1,2,4,5`; its required count is therefore six.  A
five-element compact descriptor array would either make sampler five fail the
range check or silently bind it to the wrong resource.

The current clean-room paths expose that mismatch exactly:

* Mesa `chrome9_draw.c:241..243` assigns the generic draw's
  `texture_count = popcount(semantic_sampler_mask)`, and
  `chrome9_fragment_sampler_unit()` compacts set bits into dense descriptor
  indices.
* The kernel `chrome9_fragment.c:197` requires every native sampler id to be
  below `texture_count`, and `:269` independently requires
  `texture_count == fls(sampler_mask)`.

The implementation invariant is consequently:

```text
texture_count = sampler_mask ? fls(sampler_mask) : 0
descriptor index = logical sampler id
```

The present UAPI cannot express that rule safely: it has neither an active
mask nor a logical index in each descriptor, and the kernel validates, maps,
and emits every element through `texture_count`.  Do not fill holes by
renumbering active descriptors, and do not invent a hidden dummy BO contract.
Until the ABI grows either an active-slot mask over a high-water-sized array or
explicit logical indices, reject sparse programs.  In the successor ABI every
set bit must have one complete image/sampler state pair and retained BO, while
holes emit closed state and own no BO.  The TAU sampler nibble must remain the
logical id.  This rule is required by terrain's fragment units `0,1,2,4,5`; it
is not a terrain-specific special case.

## Multi-source resource-state tables

The installed glmark2 2023.01 sources provide two useful generic state
envelopes.  These are not shader keys; they are ordinary arrays of validated
image and sampler descriptors.  Source SHA-256 values are:

```text
scene-refract.cpp                  a45f827710dcb6796107d8a96dbc2a83bd2353c459461fa3a305214fd741f141
light-refract.frag                 5d60dd85933ef77a7bc89eb5d567fb9791fd9b14170ce82b06985da718a5e001
scene-terrain/terrain-renderer.cpp 1eb240ab4c5db674463bc9864058b3e86de68e8728869796d437dfabe2a6432d
scene-terrain.cpp                  48a39330414b0a86c4b5464588f7b61c35a2ee53f4b5b9b652c52d0591b2f49b
terrain.frag                       efc2c1cd2ab3ad01e9d42430f427e6ba441f2e3311fb9afe5aaafe25b503a8d2
terrain.vert                       f07ac9490a748428ce9e1df9c5896f61659f927e99c1aa5334f926f4b013c76a
```

The three-stage fragment envelope is:

| stage | resource | image A30 | sampler A30 / A31 / A32 |
|---:|---|---:|---|
| 0 | raw D24 depth, one level | `30fe2000` | `30000000 31100000 32000049` |
| 1 | dynamic BGRA color, L0--L9 | `30990000` | `30000240 31102492 32000049` |
| 2 | BGRA image, one level | `30990000` | `30000000 31102490 32000049` |

At 800x600 the chain reaches L9, giving exact A30=`30000240`, and therefore
depends on the separately unresolved L9 implicit-tail rule; the
ordinary L1--L8 H3 table is not enough.  General state high-water is three,
so A00/A08 are `00000031/08010103`.

The sparse six-stage envelope uses fragment stages 0,1,2,4,5 and vertex
stages 3,5:

| stage | resource | use | image A30 | sampler A30 / A31 / A32 |
|---:|---|---|---:|---|
| 0 | 512 BGRX asset | fragment | `30980000` | `30000240 31102492 32000052` |
| 1 | 512 BGRX asset | fragment | `30980000` | `30000240 31102492 32000052` |
| 2 | 512 BGRX asset | fragment | `30980000` | `30000240 31102492 32000052` |
| 3 | 256 BGRA generated normal | vertex | `30990000` | `30000000 31102490 32000049` |
| 4 | 512 BGRA generated specular | fragment | `30990000` | `30000240 31102492 32000052` |
| 5 | 256 BGRA generated height | fragment and vertex | `30990000` | `30000000 31102490 32000049` |

`30000240` is zero bias, max L9, min L0.  Each mipmapped 512 resource needs
its own stage-specific H3 selector and validated L1--L8 table plus the L9 tail
contract.  The generated BGRA resources additionally need render-to-texture
cache closure before sampling.  Vertex sampling is a software/draw-module
problem in the planned driver and must not be represented as a fragment TAU.

The six-stage fixed-function general-state image derived from the vendor
stage packing is:

```text
A00 00000061   A01 01292521   A02 0235312d   A03 03000000
A04 04000000   A05 05000000   A08 08010106   A09 09010101
A0a 0a000001   A0b 0b000000   A0c 0c000000   A0d 0d810000
```

This is the high-water-six extension of the independently recovered five- and
eight-stage images.  It is an implementation oracle pending silicon proof,
not permission to expose six units.  A safe live discriminator binds distinct
constant colors to stages 0..5, executes one sample from each in separate
bounded programs, and checks old-six-one transitions with all BO guards and
engine status clean.  The same test must verify that leaving stage 3 unused in
the fragment program does not make stage 4 or 5 inherit stale image/sampler
state.

## Patch-ready TAU scheduler and allocator

Lower the fragment program to an ordered dependency graph whose native nodes
are four-dword ALU records and one-dword TAU records.  A direct interpolated
texture coordinate can feed TAU after coordinate-slot compaction.  An
ALU-generated coordinate cannot feed TAU directly from an ordinary temporary:
the production compiler inserts a MOV into texture-coordinate register class
one before every such fetch.  One iteration of the eleven-tap object is:

```text
ALU: coordinate arithmetic into ordinary TEMP
ALU: MOV texture-coordinate slot j, TEMP
TAU: TEX using coordinate slot j
ALU: consume sampled-result TEMP
```

For a full-vector MOV from ordinary TEMP zero, the bridge records differ only
in destination slot:

```text
01c00530 c01c0053 00f00006 (000d0420 | (j << 6))
```

Slots j=1..7 produce dword three `000d0460, 000d04a0, ...,
000d05e0`.  General source selection and final-use bits still come from the
normal MOV encoder.  The eleven-tap total follows exactly: 11 coordinate ALU,
11 bridge MOV, 11 weight/accumulate ALU, and two output MOV = 35 ALU.

A smaller production discriminator has no direct T input:

```text
r0.xy = c0.xy + c1.xy
r1 = TEX(s0, r0)
oC0 = r1

ALU 008106d0 d0140924 00308006 00000020
     01c00530 c01c0053 00f20006 000d0420
     01c00530 301c0053 00f20105 000d0820
TAU 301101b0
A90..A96 90058000 91000001 92010003 93000001
          94000000 95000002 96000000
```

With no direct T coordinate occupying slot zero, the bridge uses coordinate
slot zero.  A95 bit1 switches after the bridge ALU, and A93 bit0 switches back
after the TAU.  This is the smallest whole-object oracle for computed
coordinates; its packed native-object SHA-256 is
`2abd956c99e275c765ab105b3fe5d7a58a7d29d8fc50cc7406059aca984ccb9e`.

Construct a semantic TAU word only after physical allocation:

```text
tau = 20000000
    | (coordinate_last_use ? 10000000 : 0)
    | ((logical_sampler & 0xf) << 24)
    | ((operation & 0xf) << 20)
    | ((destination_temp & 0xf) << 16)
    | ((coordinate_slot & 0x3f) << 12)
    | swizzle
    | (logical_sampler & 0xf)
```

TEXLD/TEXLDB/TEXKILL/TEXLDP operation nibbles are 1/3/4/7 and identity
swizzle is `1b0`.  Destination and coordinate contributions overlap in bits
17:16 because the vendor emitter ORs them.  Keep the two semantic ids
separate until this final OR; never decode, patch, or validate one by masking
the already packed word.  The H5 register manager has ten coordinate slots
and twelve ordinary temporary slots.  The exact static initialization at
`stmPPSSetRegManager_invi:0x1b5b6e..0x1b5b9c` writes normal TEMP limit 12 and
coordinate limit 10.  All current live oracles stay within coordinate slots
0..7; the initial clean-room allocator therefore deliberately caps
coordinates at eight and ordinary temporaries at twelve.  Slots 8 and 9
remain a separately gated extension.  These limits also keep the coordinate
contribution's high overlapping bits clear.

Ordinary TEX destinations enter the same physical TEMP allocation used by
ALU.  The later ALU consumer carries the TEMP final-read marker: source A is
dword2 bit17, source C dword1 bit21, and source B dword0 bit25.  When one
record reads the same physical TEMP more than once, precedence is A, then C,
then B.  TAU bit28 is the corresponding final-read marker for the physical
coordinate slot.

These markers describe the last occurrence of a **physical register in the
complete program**, not the end of each SSA value interval.  The eleven-tap
program reuses ordinary TEMP zero for every coordinate, but only its last
bridge read has dword2 bit17.  It reuses coordinate slots 1..4, so only their
second TAU reads set bit28; one-use slots 5..7 set it immediately.  Therefore:

1. lower and DCE;
2. allocate/reuse physical TEMP and coordinate slots;
3. create the final interleaved ALU/TAU schedule;
4. scan that schedule backwards and mark only the last read of each physical
   TEMP and coordinate slot;
5. derive A91 from the set of allocated coordinate slots;
6. derive the transition maps from the finalized order.

The production coordinate allocator is circular, not an ordinary immediately
reusing linear scan.  Direct logical T inputs are globally compacted by logical
id: a deliberately permuted use order `T5,T0,T4,T1,T2` produces physical
slots `4,0,3,1,2`.  With no direct T input, twelve computed fetches use
`0,1,2,3,4,5,6,7,0,1,2,3`; with persistent T0 used by coordinate ALU they use
`1,2,3,4,5,6,7,1,...`.  This is why the eleven-tap object cycles through
1..7 rather than immediately reusing one slot.

The static ten-slot limit is real but belongs to the highest-pressure resource
bucket.  A production program with eight direct TAU coordinates followed by
three computed coordinates emits physical slots
`0,1,2,3,4,5,6,7,8,9,8`, A91=`0003ff`, and A90=`90000000`.  Its exact TAUs
and controls are retained in the JSON corpus.  This object is compiler proof,
not silicon proof for slots 8/9.  The first generic backend should therefore:

1. compact direct coordinates into 0..D-1 by logical id;
2. reject D greater than eight;
3. assign every computed-coordinate bridge round-robin over D..7;
4. reject computed coordinates when D equals eight;
5. enable slots 8/9 only behind a later ten-slot capability after a guarded
   numerical test and exact A90 bucket closure.

This policy byte-matches the current computed-coordinate corpora and preserves
the vendor's reuse distance.  It is intentionally more conservative than the
full static manager.

Upload ALU and TAU stores separately, retaining their ordinal positions in
the interleaved schedule.  Set A90 bit18 when the first scheduled node is
ALU.  At every TAU-to-ALU boundary after TAU ordinal `k`, set T2A bit `k`; at
every ALU-to-TAU boundary after ALU ordinal `k`, set A2T bit `k`.  Pack both
maps in the 24-bit chunks described above.  A92 carries the two final counts.
No native END record is emitted.

A simple safe list scheduler keeps TEXKILL and all texture operations in
source order, selects only dependency-ready nodes, and prefers another node
from the current engine when legal to reduce transitions.  Ordinary unread
TEX results should be removed by DCE; the vendor uses TAU bit 31 to retire an
unconsumed or overwritten TAU destination, but that path is unnecessary for
the initial backend and remains rejected.  TAU bit 30 is unresolved and
remains zero.  This optimization
cannot move a bridge after its TAU or a sampled-result consumer before its
TAU.  Reject the program if allocation, record counts, or transition-map
capacity fails; do not spill or silently truncate.

## Fail-closed compiler rules

1. Count lowered native records and reject more than 96 ALU or 32 TAU.
2. Build the ordered ALU/TAU schedule first; derive 24-bit transition chunks
   from that final schedule.
3. Run allocation/lifetime finalization before emitting A91 or TAU high bits.
4. Require duplicated TAU sampler nibbles to match the validated logical ID.
5. Require one complete image/sampler state pair and one reserved BO per
   logical sampler used by the shader.  Sparse holes stay disabled/reset.
6. Keep stage exposure at eight or below despite the 16-value native field.
7. Treat the objects in the corpus as static/offline proof pending bounded
   silicon tests with distinct constant textures and clean guard/status checks.
