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
