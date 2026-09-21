# Chrome9 sampler implementation audit and guarded probe ladder

This audit reconciles the sampler state emitted by the Windows D3D/OpenGL
drivers, the proprietary Linux ICD, and old open UniChrome Mesa.  It is aimed
at the next Mesa implementation tranche: real mip selection first, then the
small legacy formats.  It deliberately does not promote anisotropy, implicit
L9--L11 tails, or packed/video formats merely because their selectors exist.

The offline arithmetic verifier is
`tools/chrome9_sampler_oracles.py`.  It builds A30/A31/A32 words, canonical
and current guarded level layouts, and the complete L1--L8 H3 table without
opening DRM or emitting commands.

## Executable evidence revisited

The Windows images are:

```text
VTGUModeDX32A.dll  sha256 8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll      sha256 a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
```

The Windows D3D address builder at `0x101d9250` indexes the dword table at
`0x10335f24`.  Entries for public D3D address modes 1 through 5 are exactly:

```text
D3DTADDRESS_WRAP        1 -> 2
D3DTADDRESS_MIRROR      2 -> 3
D3DTADDRESS_CLAMP       3 -> 1
D3DTADDRESS_BORDER      4 -> 0
D3DTADDRESS_MIRRORONCE  5 -> 7
```

The proprietary Linux `STM_TexAddressMode_INV:0x1b17b0` indexes the
independent table at `0x3f67a0`, whose same five entries are `2,3,1,0,7`.
Both builders update S, T, and R independently.  The resulting ordinary 2-D
word remains:

```text
A32 = 32000000 | (1 << 6) | (wrap_t << 3) | wrap_s
```

where unused R is clamped.  Thus clamp, repeat, mirror, and mirror-once on S/T
are `32000049`, `32000052`, `3200005b`, and `3200007f` respectively.  Border
requires its independently emitted A35/A36 color and cannot be enabled by an
A32 value alone.

The Windows D3D filter builder is `0x101d9380..0x101d9826`; OpenGL carries
the paired copy at `0x10241ba0`.  The Linux implementation is exported as
`STM_FilterMode_INV:0x1b32d0`.  Its state offsets close the ordinary fields:

```text
A31[20:16]  maximum anisotropy
A31[15:13]  S enlargement
A31[12:10]  S shrinking
A31[9:7]    T enlargement
A31[6:4]    T shrinking
A31[3:0]    D/mipmap filtering (0 none, 1 nearest, 2 linear)
```

For public point and linear modes, the Windows switch tables at
`0x101d982c/0x101d9840` and Linux jump tables at `0x3f2778/0x3f2760` agree
with the already documented six GL combinations.  A normal implementation
therefore needs only these A31 values:

| minification | mag nearest | mag linear |
|---|---:|---:|
| nearest | `31100000` | `31102080` |
| linear | `31100410` | `31102490` |
| nearest-mip-nearest | `31100001` | `31102081` |
| linear-mip-nearest | `31100411` | `31102491` |
| nearest-mip-linear | `31100002` | `31102082` |
| linear-mip-linear | `31100412` | `31102492` |

The shipping builders also maintain A32 high bits while changing filter
modes.  The manual resolves them: A32[21:19] and A32[18:16] are R enlargement
and R shrinking, respectively.  Thus their ordinary linear/linear path sets
A32[19] and A32[16], yielding `00090000`, because the generic builder applies
the same filter to S, T, and R.  Current 2-D VX900 tests correctly leave the
unused R filters nearest/zero while A31=`31102490` produces the exact
bilinear midpoint.  Preserve that minimal A32 encoding for 2-D; a future
3-D/STR sampler must program and validate the R fields rather than inheriting
the 2-D word.

### Why anisotropy is not an A31-only extension

`STM_FilterMode_INV` copies the requested maximum anisotropy into A31[20:16],
but that field is only a limit.  Mode 3 also changes the S/T filter selectors
and A32-side controls.  Private quality modes 4 and 5 additionally copy 32
dwords (128 bytes) from one of two coefficient tables into the general
texture state at `state+0x1fbc` and mark the global table dirty.  The Windows
paths do the same at `0x101d94a7..0x101d9527` and
`0x101d95cd..0x101d964f`.

The builders further disable or rewrite filtering for selected integer,
floating, depth, and multi-level resources (`Windows
0x101d96ff..0x101d97b1`, Linux `0x1b33f0..0x1b35cf`).  The exact private
format denylist is `101d` D16, `1028` L16, `102c` R16F, `2032` D24X8,
`2036` V16U16, `203c` G16R16, `203d` G16R16F, `203e` R32F, `203f` D32, and
`2040` D32F.  RGB565, ARGB1555, ARGB4444, L8, A8, and L8A8 are not in that
rewrite list, reinforcing their ordinary nearest/linear promotion path.
Consequently:

* do not expose `max_texture_anisotropy` from the literal A31 value 16;
* do not lower Gallium anisotropy by setting only A31[20:16]; and
* do not accept private modes 4/5 until the 32-dword coefficient packet,
  its selector, and its transition lifetime have their own bounded oracle.

This is the main filtering hazard for the next Mesa implementation.  Point,
bilinear, nearest-mip, and trilinear stay closed; anisotropy does not.

The coefficient upload transaction itself is now exact, although its visual
behavior is still untested.  Manual type-03/subtype-11 defines 32 entries
indexed by the five-bit S/T fraction.  Both Linux update paths emit:

```text
FE03043C 00000020 11030000 CC000000
<32 coefficient dwords>
```

`stmFullUpdateHardware_invi:0x1781f1..0x17822f` copies the 32 words from
`state+0x1fbc`; incremental paths use the same count and selector at
`0x17b2f2..0x17b306` and `0x17b38a..0x17b39e`.  This proves again that
`11030000` is the global 4x4-filter table selector, not a stage-1 mip table.
The two Linux source objects are `INV_4X4FLTCOEF_BSPLINE:0x3f27a0` and the
misspelled `INV_4X4FLTCOEF_GUASS:0x3f47a0`.  SHA-256 of their first 128 bytes
is respectively `ea5cf02b1da49962e77466a322fb499b82b55faeba3d7348c3bdc62af83145ee`
and `b60cb2fd7c2a9fd89cabd5ffa9131b9a0857b14c6db4a068718c03ac37dcf140`.
The exact host-order words are in `sampler-4x4-coefficients.tsv`.

This packet is 36 dwords including its header.  It is context-global rather
than per texture stage, so a future implementation must serialize table
ownership, dirty it when switching B-spline/Gaussian state, and either restore
a known table after context loss or include it in the complete state image.
None of that is needed for ordinary point/linear/mip sampling.

## LOD conversion and fail-closed range

The Windows D3D LOD builder at `0x101d9a40` and Linux
`STM_MipMapLOD_INV:0x1b1a50` both call a floating-to-fixed helper with five
fractional bits.  Windows helper `0x1020a9a0` shifts the IEEE mantissa and
applies two's-complement sign after the shift.  It therefore truncates toward
zero rather than rounding to nearest.  Both callers then mask the result to
ten bits before inserting it at A30[21:12]:

```text
A30 = 30000000
    | ((trunc(lod_bias * 32) & 03ff) << 12)
    | ((maximum_level & 3f) << 6)
    |  (minimum_level & 3f)
```

There is no saturation in these final builder blocks.  A value already
outside signed s5.5 can wrap after `&03ff`, even if an API front end normally
constrains it.  A safe Gallium lowering must reject non-finite values, clamp
or reject outside `[-16.0, 15.96875]`, truncate toward zero, and only then
pack.  The initial capability should be narrower still: zero bias plus the
exact `+0.5/+1.0/-0.5/-1.0` diagnostic values.

The level count is separately capped at twelve by both proprietary drivers.
A30 can select L0--L11, but H3 exposes addresses only for L1--L8.  Do not use
the twelve-level selector range as evidence for the inferred L9--L11 packing.

## Layout contract and two address spaces

The proprietary allocation helper uses this canonical linear formula:

```text
pitch[l] = max(32, align(max(width >> l, 1) * cpp, 32))
size[l]  = pitch[l] * max(8, align(max(height >> l, 1), 8))
offset[l + 1] = offset[l] + size[l]
```

Every uncompressed vendor contribution is therefore 256-byte aligned.  The
current Mesa oracle layout intentionally differs in two ways: it places a
leading page before L0 and the historical guard page immediately after the
page-rounded L0 range; after that, it stores only each level's logical
`pitch*height` bytes and aligns the start of the next level to 256 bytes.
It does not round every level height to eight.  A final page-aligned guard
terminates the BO.  For a 32x32 XRGB texture both size rules coincide and the
layout is:

```text
leading guard   0000..0fff
L0              1000..1fff
internal guard  2000..2fff
L1              3000..33ff
final padding   3400..3fff
final guard     4000..4fff
```

Thus the vendor-contiguous L1 entry is `40000010`, while the current guarded
resource requires `40000020`, both relative to L0.  Never derive H3 entries
from the vendor formula if the actual Mesa object uses the internal guard.
Derive each accepted offset from the resource layout, check 256-byte
alignment and the complete mapped range, then emit:

```text
FE03043C 00000008 10030000 CC000000
40000020 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

Stage 1 uses selector `10030008`, not `11030000`.  Always write all eight
entries, zero unused slots, and rewrite A30/A31/A32 in the same draw.  A stale
entry is unreachable only if the new A30 range proves it unreachable.

For asymmetric NPOT chains the two layouts diverge.  At 30x18 XRGB through
L2, vendor-contiguous `(offset,size)` pairs are
`(0000,0c00),(0c00,0400),(1000,0100)`.  The current guarded Mesa pairs are
`(1000,0900),(3000,0240),(3300,0080)`, so its exact SF-relative H3 entries
are `40000020,40000023`.  The manual exposes explicit L1--L8 addresses and
does not require a fixed inter-level stride, but that makes the Mesa layout a
separate silicon question rather than proof by vendor allocation policy.
Keep the first mip oracle POT, where the rules coincide; only then run this
fully mapped 30x18 NPOT discriminator.  Do not use the compact Mesa rule for
the implicit L9--L11 tail, whose only evidence is the vendor-canonical layout.

Compressed DXT levels use block rows and a different size formula.  YUY2 has
special A30 low bits and paired-pixel semantics.  Neither belongs in the
first uncompressed mip ABI.

## Next implementation order

The highest-value and lowest-risk sequence is:

1. **XRGB L0--L1 address gate.**  Use the current guarded `0x5000` object,
   red L0, green L1, A31=`31100000`, and force L1 with
   A30=`30020040`.  This proves H3 address and bias before derivatives.
2. **Exact min=max selection.**  Use A30=`30000000` for L0 and
   `30000041` for L1 with A31=`31100001`.  This independently proves both
   A30 level fields.
3. **Derivative nearest-mip.**  Minify 32x32 to 16x16 with
   `30000040/31102491`; safely interior pixels must be green.
4. **Trilinear midpoint.**  At 1:1, use bias +0.5 and
   `30010040/31102492`; accept only red/green `{7f,80}` and blue zero.
5. **Transition closure.**  Run mip -> L0-only -> mip without a context
   reset, rewriting the table and all three sampler words each time.
6. **RGB565 level zero.**  Only after the common XRGB mechanism passes, add
   sampler-only selector `30890000`, cpp 2, canonical pitch, and exact
   red/green/blue/white endpoint readback.
7. **L8, A8, L8A8, ARGB1555, ARGB4444.**  Promote one decoder at a time;
   alpha-bearing rows need BGRA destination readback.  Keep render, scanout,
   import, and blendability independent.
8. **Per-format mips.**  Reuse the same guarded two-level discriminator only
   after that format's nearest and linear level-zero paths pass.

The complete format selectors and endpoint vectors remain in
`legacy-texture-format-oracles.md`; the DXT/YUV/float catalog remains in
`fixed-function-texture-sync.md`.  This order avoids changing decoder,
layout, address selection, and filtering in one first hardware experiment.

## Later wrap probes

Signed REPEAT through magnitude 32 is live-proven.  Signed MIRROR remains a
separate gate.  MIRRORONCE has stronger cross-driver encoding evidence than
previously recorded, but no silicon result.  Its D3D semantics are absolute
mirror around zero followed by edge clamp.  A safe first nearest oracle uses
an 8x1 unique-column texture and these constant coordinates:

| S | expected column |
|---:|---:|
| `+0.28125` | 2 |
| `-0.28125` | 2 |
| `+1.28125` | 7 |
| `-1.28125` | 7 |

Use A32=`3200007f`, keep T at 0.5, and run
`clamp -> mirror-once -> repeat -> mirror-once -> clamp` without reset.
Check both source and destination guards, the complete source allocation,
retirement cookie, CR PAUSE, and work bits after every draw.  A wrong color
with clean retirement is a semantic failure; a timeout quarantines every
mapping.  Do not alias this result to signed MIRROR or BORDER.

## Hazard checklist

Before any sampler capability is exposed, require all of the following:

* finite coordinates and mode-specific bounded coordinate ranges;
* finite, representable signed s5.5 LOD bias with truncation defined;
* `min_level <= max_level <= resource.last_level` and no incomplete chain;
* every reachable level offset derived from the actual guarded layout;
* eight initialized H3 entries and the correct per-stage selector;
* exact format/cpp/pitch/range agreement, with block formats rejected by the
  uncompressed path;
* all texture BOs reserved and retained through the completion fence;
* producer ordering plus FE A00 texture-cache clear for CPU/render writes;
* no anisotropic claim from A31[20:16] alone; and
* timeout quarantine rather than a closure submission after an uncertain
  fetch.

These gates are sufficient for a bounded L0--L8, uncompressed, linear 2-D
sampler path.  They are not evidence for tiled mips, cube/3-D textures,
implicit L9--L11, anisotropy, arbitrary border modes, or external layouts.
