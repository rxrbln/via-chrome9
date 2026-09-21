# ioquake3 GL1 keys after the first projected blit

Status: static source/packet audit, 2026-09-21. The first live ioquake3
fragment program and the missing Gallium resource-copy callback now have
bounded Mesa implementations, but no later ioquake3 NIR has yet been captured
on VX900. Consequently the NIR spellings below are implementation targets,
not claims about a live post-fix trace. The source-level render order and the
native programs are exact.

## What should fail next

The first admitted UI program does not imply that the next world draw reaches
the kernel. Ordinary ioquake3 world images are created with `IMGFLAG_MIPMAP`
and the live configuration selected `GL_LINEAR_MIPMAP_NEAREST`. Chrome9 still
admits only level-zero views and `PIPE_TEX_MIPFILTER_NONE`. The most likely
next rejection is therefore sampler/view state, before a new shader key. A
run with `r_textureMode GL_LINEAR` is useful only to capture the next state;
it is not a substitute for the missing mip table and LOD selection.

Once that diagnostic gate is removed, renderer source fixes the likely order:

1. a projected one-texture full-RGBA product for vertex-lit entities;
2. a projected two-texture product with primary color for opaque lightmapped
   world surfaces;
3. the same one- or two-texture color program plus fixed A33 alpha test for
   masked materials; and
4. read-only EQUAL depth with additive/destination-color blend for dynamic
   lights, followed by source-over fog.

The optimized paths are selected in
`code/renderergl1/tr_shade.c:1104` and `:1181` of ioquake3
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`. The lightmap path sets
`GLS_DEFAULT`, binds base coordinates on unit 0 and lightmap coordinates on
unit 1, leaves the color array enabled, and sources it from
`tess.constantColor255`. `tr_init.c:1193` initializes that array to `0xff`,
but it remains a runtime vertex input and Mesa is not entitled to delete it
from an unmatched shader.

## Key A: projected texture times primary RGBA

The existing captured UI key has output alpha `COL0.a`, so it is safely
lowered to RGBA MODULATE only when texture alpha is known to be one. The
adjacent fixed-function key expected for a texture with meaningful alpha is:

```text
tc = smooth vec4 TEX0
t0 = tex(unit=0, coord=tc.xy, projector=tc.w)
c0 = smooth vec4 COL0
out = t0 * c0
```

An exact classifier should accept only:

* one non-array, non-shadow 2D `tex` from texture/sampler zero;
* exactly two texture sources: coordinate `TEX0.xy` (or the linked VAR slot)
  and projector `TEX0.w` from the same varying;
* one identity four-lane `fmul` with the other leaf equal to full `COL0`;
* one complete color store and no constants, discard, derivatives, alternate
  output or extra instructions; and
* post-TNL `TEX0.q` bitwise `0x3f800000` at every vertex.

Under the Q=1 condition the projector is an identity and the already-live
one-texture MODULATE program is exact. Unlike the captured RGB/primary-alpha
key, this full-RGBA form does not need an XRGB resource restriction: BGRA
alpha is intentionally multiplied. It may use the existing one-texture+D16
LESS/LEQUAL ioctl when depth write is enabled. Any other Q remains rejected;
native projective A18 state and software `s/q,t/q,rhw*q` are separate gates.

A closed smoke vector uses a BGRA texel `804080ff`, primary D1 `80ff8040`, and
a BGRA target. The expected product is approximately `40404040`, within one
normalized-multiply LSB. Repeat with Z16 cleared to `0x8000`: LEQUAL at
`0x8000` passes, then LESS at the same value preserves color/depth, and LESS
at `0x4000` passes. Changing only one Q to `2.0` must add rejected triangles
without an ioctl. This distinguishes the full-alpha key from the already
implemented primary-alpha bypass, which would store alpha near `0x80`.

## Key B: projected base times lightmap times primary

`RB_StageIteratorLightmappedMultitexture()` establishes the semantic tree:

```text
p0  = tex0(TEX0.xy / TEX0.w) * COL0
out = tex1(TEX1.xy / TEX1.w) * p0
```

Linked NIR may reassociate the two multiplies. Before a live dump exists, do
not accept every arbitrary `fmul` tree. A bounded recognizer can flatten only
identity four-lane `fmul` nodes (optionally identity MOVs) and require the
multiset of leaves to be exactly `{projected_tex0, projected_tex1, COL0}`.
Each sample must use its matching unit and matching coordinate/projector
varying. There must be two texture instructions, no constants, and one full
RGBA output store. If the live NIR contains an intermediate `fsat`, add only
the observed placement; do not silently treat general saturation as an
identity operation.

Both post-TNL Q values must be bitwise one. Initial admission should require
XRGB for both sources, full color write, blend/alpha disabled, guarded native
BOs, XRGB target and write-enabled D16 LESS or LEQUAL. That is the exact
opaque-lightmap subset and avoids claiming independent RGB/alpha scale or
translucent semantics. Texture 0's mip state remains a separate prerequisite.

The Chrome9 scheduled fragment program is already cross-checked against both
proprietary compiler families:

```text
0140B6D0 30202053 00F20005 0000007F   # r1 = tex0 * tex1
0140B6D0 30040053 00F20105 0000007F   # r1 = r1 * v0
01C00000 301C0000 00F20105 000D083F   # oC0 = r1
CCCCCCCC DDDDDDDD
```

Use `A92=92020003`; the two TAU words and `A93=93000002` do not change. The
Windows compiler's explicit-temporary probe emitted three ALUs with SHA-256
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`,
and the TAU image hash remained
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
The executable scheduled words above, rather than the compiler's virtual
register words, are the implementation candidate.

The current two-texture body is 262 dwords without depth and 266 with D16.
The extra four-dword ALU record makes the new bodies 266 and 270 dwords. In
the D16 form the fixed indices are:

```text
A02 normalized D1 declaration  body[90]
A92 instruction count          body[194] = 92020003
first MUL                      body[220..223]
second MUL                     body[224..227]
output MOV                     body[228..231]
CC/DD                          body[232..233]
immediate vertex header        body[236]
```

Keep `A02=020041D1`; `D0` reproduces the already measured unnormalized-color
saturation bug. The common state keeps perspective-correct diffuse
interpolation (`A01=01c0154c` in the current D16 builder).

This should be a new structured program selector and capability, not an
implicit reinterpretation of the existing two-texture ioctl. A minimal ABI
can reserve one validated flag bit for `TEXTURE0*TEXTURE1*PRIMARY`, accept it
only with the new capability, and continue requiring every unrelated flag and
reserved field to be zero. BO lookup, mapping, fences and quarantine are
unchanged: target/depth are WRITE and both textures are READ.

The discriminator reuses the proven texture quadrants whose two-texture
product is red/black/blue/yellow and supplies D1=`ff808080`. Expected XRGB is
approximately `00800000/00000000/00000080/00808000`. Ignoring D1 returns
full intensity; using unnormalized D1 saturates. Clear D16 to `0x8000`, draw
at `0x4000` with LEQUAL, verify four colors and depth, then submit the old
two-factor no-depth program to prove transition closure. Check target, depth,
both texture guards, retirement cookie and CR/work status. Negative tests
independently set Q0 or Q1 to `2.0`, replace either XRGB source with BGRA,
enable blend/alpha test, omit COL0, duplicate a texture leaf, and leave the
new program flag set while clearing its capability; every case must fail
before packet emission.

## Alpha-test and later state combinations

Legacy UniChrome's `via_texcombine.c` confirms that compatibility MODULATE is
stage-local: stage 0 selects texture and primary/previous, while later stages
select texture and `HC_XTC_Cur`; RGB and alpha argument selection are
independent. It does not justify deleting the primary leaf or multiplying
alpha differently. Chrome9's programmable three-factor schedule is the
corresponding exact staged operation.

Masked surfaces do not require a TEXKILL shader. Both Windows UMDs and the
Linux proprietary driver emit the fixed render state:

```text
A00 bit 20 = alpha test enable
A33[14:12] = GL NEVER..ALWAYS mapped directly to 0..7
A33[10:0] = ref == ff ? 0x400 : (ref << 2) | (ref >> 6)
```

ioquake uses GREATER/0, LESS/0.5 and GEQUAL/0.5. The safe promotion order is
one-texture+D16+alpha first, then three-factor+D16+alpha after Key B passes.
Alpha failure must suppress both color and depth. The three-factor
state-complete packet can place A33 in the existing aligned destination slot,
so it adds no dword, but it still needs an explicit ABI/capability tuple.

Dynamic light and fog come later. Source inspection shows D16 EQUAL with
depth writes disabled plus `ONE,ONE` or `DST_COLOR,ONE` for dynamic light,
and source-over (sometimes also EQUAL/read-only) for fog. These are not
covered by the current write-enabled LESS/LEQUAL contract. They must remain
rejected until read-only EQUAL and the additional blend factors have their
own color/depth/guard and state-transition oracles.

## Capture sequence

After deploying the three Mesa checkpoints, capture in this order:

1. default texture mode, recording the exact sampler/view rejection;
2. diagnostic `r_textureMode GL_LINEAR`, recording the first newly reached
   linked NIR and complete DSA/blend/format tuple;
3. if Key A appears, capture Q and source format before admitting it;
4. if the lightmap key appears, compare its multiply tree and any `fsat`
   placement against Key B before adding the program selector; and
5. for every admitted draw, archive submitted/rejected deltas, all four BO
   guards, cookie retirement and MMIO error/work state.

No static inference in this note is grounds to expose mipmaps, arbitrary
projectors, general fragment ALU, or a higher GL version.
