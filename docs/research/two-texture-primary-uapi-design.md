# Chrome9 projected two-texture-primary draw design

Status: implementation-grade static design plus executable Linux/Windows
compiler differentials, 2026-09-21. The existing two-texture,
two-texture+D16, and state-complete two-texture+D16+blend ioctls are merged;
the two-factor RGBA program is live. The full-RGBA three-factor and
primary-alpha-bypass programs in this note are compiler-exact but not yet
executed on VX900 silicon. No new program UAPI or capability described here
is currently advertised.

## Exact bounded operation

There are two distinct compatibility fixed-function outputs:

```text
full RGBA:    oC0 = texture0.rgba * texture1.rgba * primary.rgba
alpha bypass: oC0 = vec4(texture0.rgb * texture1.rgb * primary.rgb,
                         primary.a)
```

They are equivalent only when both sampled alpha values are one, or when
fragment alpha is provably unobservable. They are not interchangeable on a
BGRA destination, under alpha test, or under a blend equation which consumes
source alpha.

For the initial silicon gate, both texture resources are BGRX8888,
the target is BGRX8888, vertex D1 alpha is exactly `0xff`, blend and alpha test
are disabled, and both sample views are level zero. Thus either semantic has
output alpha one. This is the safest first raw test, not a kernel validation
restriction: the full-RGBA object has exact format-98/99 alpha semantics and
arbitrary normalized D1 is its reason to exist.

`RB_StageIteratorLightmappedMultitexture()` uses
`tess.constantColor255`, initialized entirely to `0xff` in `tr_init.c:1193`.
Consequently the actual optimized ioquake lightmap draw has a still safer
runtime specialization: when every post-TNL D1 is exactly `ffffffff`, the
full three-factor expression is byte-equivalent to the already-live
two-factor RGBA program for BGRX or BGRA sources. The alpha-bypass expression
can use the same specialization only when both views supply alpha one (the
initial BGRX/BGRX gate), or when alpha is otherwise unobservable. This
white-primary optimization needs no new kernel flag.

The linked fixed-function shader retains projectors `TEX0.w/TEX1.w`. The
native immediate record contains only S/T, so Mesa may select the first
implementation only after checking that both post-TNL Q lanes are bitwise
`0x3f800000` at all three vertices. Q is not added to the kernel ABI: the
kernel receives an already validated identity-projector operation. A raw
ioctl caller asks only for the ordinary-TEXLD three-factor program.

This is not an approximation: `glMultiTexCoord2f` supplies Z=0,Q=1, which is
the ioquake lightmap case. Any non-one Q remains rejected before ioctl until
one of the separately bounded projective paths below is implemented.

## UAPI without a new ioctl

The current allocation ledger reserves capability bits through 24, making 25
the next free bit. Reuse the existing command `0x0a`, `0x0d`, and `0x13`
structures:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_PRIMARY_MODULATE (1ULL << 25)
#define DRM_CHROME9_TWO_TEXTURE_PRIMARY_MODULATE     (1U << 0)
```

Capability bit 25 means the program flag is accepted consistently by
`DRAW_TWO_TEXTURED_TRIANGLE`, `DRAW_TWO_TEXTURED_TRIANGLE_D16`, and
`DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND`; userspace must still require
capability 14, 20, or 28 for the selected base ioctl. Structure sizes remain
240, 256, and 296 bytes. No reserved field changes meaning. In command 13 the
program bit shares the top-level `flags` word with depth compare/read-only;
the nested `blend.flags` namespace is unchanged.

Validation is exact:

* no-depth `flags` may be zero or the program bit, with every other bit zero;
* D16 `flags` additionally contain explicit LESS or LEQUAL in bits `[10:8]`
  and may contain live `DEPTH_READ_ONLY` bit 11 when cap 23 is present;
* the program bit does not narrow either source format or D1: existing source
  formats 98/99 and all four D1 bytes are valid; A30 supplies alpha one for
  logical XRGB and stored alpha for logical BGRA;
* target format remains implicit BGRX8888 in commands 0a/0d; command 13 keeps
  its validated destination, blend and alpha-test state; mip levels and
  borders remain unavailable;
* target, texture 0 and texture 1 are distinct for `0x0a`; depth is a fourth
  distinct object for `0x0d` and `0x13`; and
* all existing size, canonical pitch, guard, finite RHW/S/T, normalized S/T,
  scissor and nondegenerate-triangle checks remain unchanged.

An old kernel rejects the nonzero flag. A new kernel continues to interpret
the program bit clear as the byte-identical two-factor operation. The
capability must be entered in `chrome9-uapi-allocation.md` in the same commit
which reserves bit 25; if another feature claims 25 first, update this design
and all three UAPI copies atomically rather than silently reusing the number.

No new object lifetime logic is needed. The no-depth job keeps target WRITE
and both textures READ under one `drm_exec` transaction and fence. The D16 job
keeps target and depth WRITE and both textures READ. Existing actual-size
mapping, guard preparation/verification, cache ownership, retirement,
quarantine and four-object error unwind are reused without reordering.

## Exact native program

The Xorg-scheduled executable is:

```text
0140B6D0 30202053 00F20005 0000007F   # r1 = tex0 * tex1
0140B6D0 30040053 00F20105 0000007F   # r1 = r1 * v0
01C00000 301C0000 00F20105 000D083F   # oC0 = r1
CCCCCCCC DDDDDDDD
```

Set A92 to `92020003`. TAU remains
`301001B0 311211B1 CCCCCCCC DDDDDDDD`, A93 remains `93000002`, and all other
PS controls remain unchanged. The Windows compiler's explicit-temporary probe
independently produced three ALUs and the same two-TAU switch. Its finalized
ALU SHA-256 is
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`;
the TAU hash is
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
Compiler virtual register words are evidence for count/scheduling only; the
scheduled words above are the packet source.

### Exact primary-alpha-bypass object

The alpha-bypass expression is no longer merely a desired classifier. The
Linux 5.75 compiler was run with configuration-two post-translation on:

```text
texld r0,t0,s0; texld r1,t1,s1
mul r2,r0,r1
mul r3.rgb,r2,v0
mov r3.a,v0.a
mov oC0,r3
```

It produced this indivisible physical object:

```text
A90..A9a 90000000 91000001 92020004 93000002
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       a01001b0 311001b1
ALU       0140b6d0 3003f000 00f20005 000003e0
          0140b6d0 50040055 00703f00 000003e0
          01c00530 b01c0053 008040b6 200d03e0
          01c00530 001c0053 00f03f00 000d0820
```

The second ALU writes RGB only, the third writes alpha only, and the fourth
moves the completed temporary. Do not splice either partial instruction into
the Xorg full-RGBA allocation: the controls, TAUs, temporary allocation and
all four ALUs are one compiler-finalized object.

The dynamically executed Windows D3D backend independently returned four
ALUs and two TAUs for the same token stream:

```text
TAU 201001b0 211111b1
ALU 0140b6d0 00001000 00f00000 000000a0
    0140b6d0 50040055 00700200 000000e0
    01c00530 b01c0053 008040b6 000d00e0
    01c00530 001c0053 00f00300 000d0820
```

Its finalized ALU SHA-256 is
`71c23210cb6558df33c559314aab37102f515d9313d18d3936565ed98704a9cf`;
the ordinary two-TAU hash is
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
The Windows allocator is deliberately virtual/identity, so its words prove
the operation, masks, count and scheduling while the Linux physical object
above is the upload candidate. The OpenGL ICD has the byte-matched tokenizer,
dispatcher and finalizer at `0x244530`, `0x245280` and `0x256550`, providing
the second Windows-driver static cross-check.

Neither three-factor object reads a constant register. Keep the existing PS
constant payload at four zero dwords. The existing VS float constants remain
`1,1,1,1,0x49400000,0x49400000,0,0`, and the VS integer constants remain
zero. Primary color comes only from normalized packed D1.

### Exact TEXLDP compiler differential, not yet a packet ABI

Changing both D3D sample opcodes from TEXLD to TEXLDP leaves every ALU and
control count unchanged and changes only the two texture-address words. The
configuration-two Linux physical pair is:

```text
ordinary TEXLD   a01001b0 311001b1
projected TEXLDP a07001b0 317001b1
```

For full RGBA, pair either TAU row with the Linux-finalized controls
`90000000 91000001 92020003 93000002` (A94--A9a zero) and ALUs:

```text
0140b6d0 3003f000 00f20005 000003e0
0140b6d0 00040053 00f03f00 200003e0
01c00530 001c0053 00f03f00 000d0820
```

For alpha bypass use the four-ALU physical object above and change only its
TAU row. This gives complete compiler-finalized fragment objects for both
ordinary and TEXLDP sampling; the missing native-projective evidence is in
the vertex/output and general-texture state, not the fragment compiler.

The Windows virtual pair changes identically:

```text
ordinary TEXLD   201001b0 211111b1
projected TEXLDP 207001b0 217111b1
```

The projected Windows TAU SHA-256 is
`01f8fd8e2e04c795e2f58f75514b384fecc6ce2e2dbe86499abfb958f9d70d14`.
The full-RGBA projected ALU hash remains the established
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`;
the alpha-bypass projected ALU hash remains the four-ALU hash above.

These compiler results close the fragment-program side of projective fetch,
but they are not sufficient to submit it. The current H6/VS path exports only
S/T for D3 and D4, and the immediate ABI carries only S/T. Native TEXLDP also
needs independently validated STRQ declarations/linkage, four coordinate
components per unit, and the image projection transition. The independently
recovered image delta is A18 `18001101 -> 18001901`; the one-stage
manual-derived A01/A08 tuple `01002543/08010301` remains unproved on silicon
and does not establish the two-stage tuple. Therefore no native-projective
flag may be exposed from the TAU differential alone.

There are only two safe pre-native Q cases:

1. Q0=Q1=1 bitwise: drop both projectors and use ordinary TEXLD, the required
   ioquake path;
2. a future software transform when Q0 and Q1 are finite, nonzero and
   bitwise equal at each vertex: set `rhw'=rhw*q` and divide both S/T pairs by
   that common q. One D0.w cannot represent two different projective
   denominators, so differing Q0/Q1 must remain rejected. Old UniChrome
   independently uses this transform only for one projected unit and falls
   back when a second unit is active, supporting the conservative gate.

Primary color requires two state deltas relative to the current two-factor
builder:

```text
no depth A01 = 01CE1540
D16     A01 = 01CE154C
A02          = 020041D1
```

The `CE` A01 form is already used by the one-texture MODULATE builder and
keeps diffuse interpolation perspective-correct. The current two-factor
builder's `01C01540/4C` is not sufficient merely because its D1 happens to be
unused. `A02=...D1` is already present and is retained; changing it to D0
feeds unnormalized 0..255 color and reproduces the measured saturation bug.

## Complete index deltas

Let `D=0` for command `0x0a` and `D=4` for command `0x0d`. The image, sampler,
VS and raster groups are byte-identical to the current builder. Key indices
in the new body are:

| state | no depth | D16 | required value |
|---|---:|---:|---|
| target A50 | 4 | 4 | translated target address |
| target A52 | 6 | 6 | `52980000 | pitch/32` |
| texture 0 H5 address | 20 | 20 | translated texture 0 address |
| texture 0 A22/A30 | 24/25 | 24/25 | size/format |
| sampler 0 A30/A31/A32 | 32/33/34 | 32/33/34 | current validated state |
| texture 1 H5 address | 40 | 40 | translated texture 1 address |
| texture 1 A22/A30 | 44/45 | 44/45 | size/format |
| sampler 1 A30/A31/A32 | 52/53/54 | 52/53/54 | current validated state |
| common A01 | 64 | 64 | `01CE1540` / `01CE154C` |
| D16 A10..A13 | absent | 67..70 | address, pitch, format, compare |
| Z clip A20 | absent | 71 | `20007FFF` |
| config A00/A08 | 72/74 | 76/78 | `21` / `08010102` |
| FVF A00/A02 | 84/86 | 88/90 | `0000040A` / `020041D1` |
| topology A20/A23/A24 | 93/94/95 | 97/98/99 | list / stride / 3 vertices |
| VS linkage D3/D4 | 172/175 | 176/179 | current two-coordinate mapping |
| VS control | 180 | 184 | `60800005` |
| PS A90/A92/A93 | 188/190/191 | 192/194/195 | current / `92020003` / `93000002` |
| TAU records | 208..211 | 212..215 | unchanged two-sampler TAU |
| first MUL | 216..219 | 220..223 | texture 0 times texture 1 |
| second MUL | 220..223 | 224..227 | prior result times D1 |
| output MOV | 224..227 | 228..231 | result to oC0 |
| program CC/DD | 228..229 | 232..233 | unchanged terminators |
| vertex H2 header | 232..235 | 236..239 | `FE020440 ...` |
| first vertex payload | 236 | 240 | X word, 30 payload dwords total |

The complete body is `266 + D` dwords: 266 without depth and 270 with D16.
The final H2 record contains its four-dword header plus 30 vertex dwords, so
the no-depth body ends at 265 and the D16 body at 269. Both fit the existing
one-page staging buffer with the unchanged prologue/epilogue.

That table describes the three-ALU full-RGBA object. The four-ALU
primary-alpha-bypass object keeps every index through the TAUs unchanged,
then adds one record before the output MOV:

| bypass state | no depth | D16 |
|---|---:|---:|
| A92 | 190 | 194 |
| TAUs | `208..211` | `212..215` |
| texture product MUL | `216..219` | `220..223` |
| RGB-primary MUL | `220..223` | `224..227` |
| primary-alpha MOV | `224..227` | `228..231` |
| output MOV | `228..231` | `232..235` |
| CC/DD | `232..233` | `236..237` |
| vertex H2 | `236..239` | `240..243` |
| complete body | 270 | 274 |

For mip-table count `H`, closed-destination delta `B`, and D16 delta `D`, the
full body is `266+12*H+B+D`; alpha bypass is `270+12*H+B+D`. A fully closed
single-level blend+alpha+D16 packet is consequently 274 dwords for full RGBA
or 278 for bypass. A33 remains body 19 and adds no length. In the full closed
packet A92/first-MUL/vertex-H2 are `198/224/240`; in bypass they are
`198/224/244`. Do not share a body-length assertion between the keys.

Implement this as separate constant arrays selected by the program flag, not
by overwriting indices after emission. Then assert the complete body length,
A01/A02, A92/A93, both MULs, MOV, CC/DD, vertex header and first/last vertex
payload positions. This prevents an instruction-count edit from shifting a
subsequent H2 header unnoticed.

The same rule is stronger for alpha bypass: assert all four ALUs, the
Linux-finalized A90/A91 tuple, both TAUs and both partial write masks. A body
which changes only A92 and appends one guessed MOV is not the recovered
program.

## XRGB/BGRA alpha contract

Source and destination formats have separate state words. Each texture image
A30 is `30980000` for BGRX/XRGB or `30990000` for BGRA/ARGB. The manual and
the live format path establish that format 98 supplies sampled alpha one;
format 99 supplies stored alpha. Destination A52 independently selects 98 or
99 and does not change the sampled values.

The exact lowering matrix is:

| NIR output | source views | alpha observer | admissible object |
|---|---|---|---|
| full RGBA product | any XRGB/BGRA pair | any bounded state | full RGBA |
| primary-alpha bypass | any pair | BGRA store, alpha test, or alpha blend | bypass only |
| primary-alpha bypass | XRGB + XRGB | any bounded state | either; prefer two-factor when primary is white |
| either RGB expression | any pair | XRGB target, blend/alpha disabled | matching object; alpha is store-unobservable |

“Alpha observer” includes fixed A33 and every blend factor which consumes
source alpha; an XRGB target alone does not make fragment alpha irrelevant.
Conversely, a BGRA source does not force the full-RGBA object when the shader
explicitly bypasses its alpha: the recovered bypass program samples its RGB
and routes D1.a exactly.

The white-primary shortcut is exact for the full-RGBA key with any source
formats. For the bypass key it is exact through the existing two-factor
program only if both views provide alpha one, or alpha has no observer. Never
infer XRGB from the target format, and never confuse destination A52=98/99
with either sampler's A30=98/99.

### Immediate composition with the D16/blend successor

The first optimized ioquake lightmap draw does not need capability bit 25.
After the state-complete two-texture+D16+blend ioctl is available, Mesa may
classify the full three-leaf shader, prove Q0=Q1=1 and D1=`ffffffff` for all
three post-TNL vertices, then submit the existing two-factor program through
that ioctl. This preserves the shader result for XRGB or BGRA samplers and
does not alter any packet index or program word.

The exact live state tuple composes without an exception: source factor
`DST_COLOR` and destination factor `ZERO` require cap 29, LEQUAL is encoded in
flags `[10:8]`, and depth read-only is flag bit 11 gated by cap 23. The depth
object remains conservatively fenced WRITE. Fixed alpha test, if enabled,
still observes the ordinary two-factor product alpha; this equals the full
three-factor alpha because white D1 has alpha one. No primary-program flag is
set, and the blend successor's packet remains its documented 270 dwords.

Fail closed if any vertex D1 differs from `ffffffff`, either Q differs from
bitwise one, or the shader is the primary-alpha-bypass form with a sampler
whose alpha is not one and an alpha observer is active. Those cases require
the new three- or four-ALU object rather than a state-only promotion.

#### Patch-ready command-13 lowering

The white-primary fast path uses the 296-byte
`drm_chrome9_draw_two_textured_triangle_d16_blend` request at private command
`0x13` (x86 ioctl `0x41286453`), guarded by cap 28. It does not consume
`reserved0` or any member of `reserved[4]`; all five remain zero. The fixed
two-factor program is selected when top-level flag bit 0 is clear; capability
25 makes that bit select the full three-factor program without adding a
member. Textures start at byte 32, normalized blend state at 104, three
vertices at 144, scissor at 264, and destination format at 276.

For the live ioquake filter tuple, populate:

```text
flags                    = DEPTH_FUNC(LEQUAL) | DEPTH_READ_ONLY = 00000b00
blend.flags              = BLEND_ENABLE
blend.rgb_src_factor     = DST_COLOR       # normalized value 8, needs cap 29
blend.rgb_dst_factor     = ZERO            # normalized value 0
blend.alpha_src_factor   = DST_COLOR       # alpha lowering aliases DST_ALPHA
blend.alpha_dst_factor   = ZERO
blend.rgb_func           = ADD
blend.alpha_func         = ADD
blend.color_mask         = R|G|B|A = 0f
blend.alpha_test_func    = 0
blend.alpha_test_ref     = 0
```

Mesa must require caps 28, 29, 23 and 15 for that exact request, cap 10 if
alpha test is enabled, and cap 11 if a partial scissor is submitted. It also
requires the existing BGRA destination caps if destination format is BGRA.
The kernel independently accepts only the depth compare/read-only flag mask,
LESS/LEQUAL, known blend flags and factors, ADD equations, four-bit color
mask, canonical disabled alpha fields, zero reserved words, and the existing
four distinct guarded objects. Unknown flags fail before GEM lookup. D16 is
still reserved/fenced WRITE when read-only is selected.

The white-primary and identity-Q proofs are intentionally Mesa-side. The raw
request has only packed D1 plus S/T for each unit: the two-factor native
program does not consume D1 and the ABI cannot express Q. Do not add a kernel
`D1==ffffffff` rule to the unflagged two-factor command; that would reject
otherwise valid callers without strengthening packet safety. Mesa checks D1
and both Q lanes before discarding those semantic inputs, while the kernel
continues to validate the carried S/T coordinates.

The resulting 270-dword body is byte-identical to the state-complete
two-factor design. These are the patch assertions which matter for the fast
path:

| body | exact state |
|---:|---|
| `4/6` | target A50 / `A52=5298xxxx` or `5299xxxx` |
| `7..14` | closed zero A38--A3F |
| `15..18` | `34000010 35000114 36000420 37000224` |
| `19` | disabled `A33=33007000` |
| `24/29` | texture-0 base / image A30 |
| `44/49` | texture-1 base / image A30 |
| `64..70` | A86,A87,`53010c0f`,`00000011`,`01c01548`,A88,A89 |
| `71..75` | D16 A10,A11,`12380000`,`13030000`,`20007fff` |
| `152..155` | A80 height, A81 width, A82 Y-scissor, A83 X-scissor |
| `196..208` | PS controls, including `A92=92020002` at 198 |
| `212..219` | two-TAU H2 and `301001b0 311211b1 cccccccc dddddddd` |
| `220..223` | ALU H2 header |
| `224..227` | `0140b6d0 30202053 00f20005 0000007f` |
| `228..231` | `01c00000 301c0000 00f20105 000d083f` |
| `232..233` | `cccccccc dddddddd` terminators |
| `236..239` | immediate H2 header |
| `240..269` | three ten-dword vertices |

The packed blend words follow directly from the live factor-code table:
RGB `DST_COLOR` is code `01`, RGB ZERO is `05`, alpha `DST_COLOR` is code
`02`, and alpha ZERO is `09`. Do not substitute the source-over shortcut or
change A92 to the three-factor count. Read-only changes only A01 bit 2
(`...154c -> ...1548`); the object mapping, packet length, depth state and
fence lifetime are unchanged.

#### Alpha test and scissor interaction

Alpha test adds no dword. Enable it by adding
`BLEND_ALPHA_TEST_ENABLE` to `blend.flags`, setting normalized function/ref,
putting `A33=33000000|(func<<12)|encode_ref(ref)` at body 19, and setting A00
bit 20 at body 67. `encode_ref(ff)=400`; every other byte is
`(ref<<2)|(ref>>6)`. A failed alpha test is already live-proven to suppress
both the color store and a write-enabled D16 update. With read-only depth it
still must not perturb color, blend destination reads, or any guard.

Scissor also adds no dword. The UAPI is the half-open rectangle
`[min_x,max_x) x [min_y,max_y)`. With scissor disabled all four coordinates
must be zero and the builder emits:

```text
body[152] A80 = 80000000 | height
body[153] A81 = 81000000 | width
body[154] A82 = 82000000 | height
body[155] A83 = 83000000 | width
```

When enabled, only A82/A83 change to
`82000000|min_y<<12|max_y` and `83000000|min_x<<12|max_x`. The kernel checks
unknown flags, ordering and target bounds. Mesa should turn an empty
intersection into a no-op rather than relying on unmeasured zero-area raster
behavior. Alpha test and scissor are orthogonal: A33/A00 control fragment
alpha, while A82/A83 prevent raster work outside the rectangle. Always emit
the complete destination and clip blocks so `alpha on -> off` and
`partial scissor -> full` cannot inherit stale state.

Use two live discriminators in order:

1. Prefill XRGB with `004080c0` and D16 with `8000`; use a two-texture product
   `00804020`, white D1, LEQUAL/read-only, DST_COLOR/ZERO, and alpha disabled.
   The inside result is approximately `00202018`, depth remains `8000`, and
   all four object guards remain intact.
2. Prefill a 32-square XRGB target with `00ffffff` and D16 with `ffff`.
   Enable scissor `[8,24)x[8,24)`, alpha LESS/ref 80, and write-enabled
   LEQUAL. Use a BGRA first sampler and opaque-white second sampler: first
   draw product `c0ff0000` at Z=`4000` (killed), then `4000ff00` at Z=`8000`
   (passes). Only the rectangle intersection becomes `0000ff00/8000`;
   outside remains `00ffffff/ffff`. Follow with alpha-disabled/full-scissor
   state and then repeat to prove both closure directions.

## Mesa path

Use two distinct internal keys:

```text
CHROME9_FS_TWO_TEXTURE_PRIMARY_RGBA
CHROME9_FS_TWO_TEXTURE_PRIMARY_ALPHA_BYPASS
```

For both, require exactly two ordinary, non-array, non-shadow 2D texture
instructions, sampler/texture pairs 0 and 1, no bias/LOD/offset, and projectors
from the matching coordinate varying's `.w`. Coordinates are that same
varying's `.xy`. Reject a projector borrowed from the other unit.

The full key accepts a four-lane multiply tree whose semantic leaves are
exactly:

```text
projected texture 0 using TEX0.xy and TEX0.w
projected texture 1 using TEX1.xy and TEX1.w
full primary COL0
```

The bypass key requires the same three RGB leaves, while output alpha is a
direct identity read of `COL0.a`; texture alpha must not feed the alpha lane.
Match a full vec4 store assembled from those lanes as well as an equivalent
RGB/alpha pair of stores only when masks are disjoint and cover all four
lanes. Do not classify a partial color result as bypass merely because the
current target is XRGB.

Accept operand permutation. Accept reassociation only when NIR marks the
multiply fast/reassociable; otherwise require the tree order represented by
the selected native schedule. Reject repeated/missing leaves, source
swizzles other than the bypass alpha selection, constants, discard,
derivatives, extra ALU and an unobserved `fsat`. The shader scan must count
exactly two texture instructions and zero constants. A later live NIR dump
may justify one exact final `fsat`, but static fixed-function semantics alone
must not broaden the key.

The draw layout remains the existing ten-dword
`position4,D1,specular,T0.xy,T1.xy` kernel record. Gallium's draw module emits
four floats per coordinate, so the Mesa emitter must inspect T0.q and T1.q
before dropping them and require both raw bits equal `0x3f800000`. Validate
only the consumed S/T as finite after the identity-Q check; R is unused.

The first live gate remains the opaque subset: two guarded BGRX level-zero 2D
resources, D1 alpha `ff`, distinct texture handles, guarded BGRX target, no
blend/alpha successor, and either no depth or guarded Z16 LESS/LEQUAL. That is
a test-order rule, not the advertised ABI. Before capability 25 is exposed,
the same implementation must accept the recovered BGRX/BGRA matrix and
arbitrary packed D1 through all three two-texture ioctls, with command 13
retaining its independently validated blend/alpha/destination restrictions.
Read-only depth composes when cap 23 is present. No screen GL capability
changes.

Before using that flag, test the post-TNL white-primary specialization. A full
key with all D1=`ffffffff` selects the existing two-factor program for any
source formats. A bypass key may do so only for XRGB/XRGB (or the stricter
no-alpha-observer case). General D1 selects the three-ALU full object. The
four-ALU bypass object and general BGRA/blend/alpha combinations remain behind
their own not-yet-allocated program capability until their live gates pass.

## Staged smoke order

1. **Offline builder assertions.** Build no-depth/D16 full and bypass bodies,
   check every index above, and reject unknown flags/formats before BO lookup.
   Verify that the bypass body is exactly four dwords longer.
2. **No-depth raw ioctl.** Use three guarded BOs and the proven texture
   quadrants whose two-factor result is red/black/blue/yellow. Set D1 to
   `ff808080`; expected XRGB is approximately
   `00800000/00000000/00000080/00808000`. This isolates the new ALU from D16.
3. **Program closure.** Submit `three-factor -> legacy two-factor ->
   three-factor` in one context. The legacy middle draw must return full-scale
   red/black/blue/yellow and the final draw half intensity.
4. **D16 raw ioctl.** Add a fourth guarded BO, clear depth to `0x8000`, submit
   LEQUAL at `0x8000`, LESS at the same value, then LESS at `0x4000` with
   distinguishable primary colors. Verify pass/fail color and exact depth.
5. **BGRA alpha discriminator.** Through command 13 on a BGRA target use
   texture0=`80ff8040`, texture1=`40c0ffff`, and D1=`80ff40c0`. Full RGBA
   must store approximately
   `10c02030`; bypass must store `80c02030`. Repeat with both sampler views
   XRGB: both keys must produce alpha `80`. This proves A30 source alpha and
   A52 destination alpha independently.
6. **White-primary specialization.** With D1=`ffffffff`, compare the full key
   against the existing two-factor program byte for byte for XRGB/XRGB,
   XRGB/BGRA and BGRA/BGRA. For bypass, admit only the XRGB/XRGB comparison.
7. **Gallium positive path.** Construct both projected NIR keys, provide
   Q0=Q1=1, and require submitted `+2`, rejected `+0` for each quad plus the
   same color/depth results.
8. **Gallium rejection matrix.** Independently use Q0=2, Q1=2, unequal Q,
   missing COL0, duplicate texture leaf, an alpha-bypass shader routed to the
   full object on BGRA, extra `fsat`, blend/alpha without the combined cap,
   and an unadvertised program cap. Every case must produce no ioctl.
9. **Native-projective diagnostic last.** Only after a four-coordinate
   VS/FVF ABI exists, compare ordinary TAUs with `a07001b0/317001b1`. Draw the
   triangle `(0,0),(16,0),(0,16)` with equal position RHW, homogeneous S
   `0,1,0`, and common Q `1,4,1` for both units. At pixel center `(8.5,4.5)`,
   correct projective S is about `.205` (texel 1 of an 8x1 texture), while
   dropping Q gives about `.531` (texel 4). Put gray `80/40` at texture-0
   texels `1/4` and `c0/20` at texture-1 texels `1/4`, with white elsewhere
   and D1 white. Correctly projecting both units gives approximately
   `00606060`; projecting only unit 0 gives `00101010`, only unit 1 gives
   `00303030`, and neither gives `00080808`. Run `ordinary -> projected ->
   ordinary`; quarantine every object on timeout. Do not test this by editing
   TAUs alone in the current S/T packet.
10. **Retirement and closure.** At every live stage verify all target/depth/two
   texture guards, source bytes and padding, retirement cookie, CR PAUSE,
   work-mask clear, then run a legacy one-texture draw.

The white-primary path can unblock ioquake's diagnostic non-mip lightmap draw
without the new program flag. Only after the general D1 and D16 stages pass
should Mesa select the three-ALU program. Mip addressing, RGB scale x2/x4,
combined alpha-test/blend and batching remain separate successors.

## Provenance

The semantic source is ioquake3
`code/renderergl1/tr_shade.c:1181-1254`; `tr_init.c:1193` initializes the
iterator's `tess.constantColor255` to four `ff` bytes. Legacy UniChrome
`via_texcombine.c` independently confirms that stage-zero MODULATE consumes
texture and primary/previous while later stages consume texture and current;
RGB and alpha arguments are selected independently, and its software
projective fallback admits only one projected unit.

The scheduled Chrome9 full-RGBA program and result-scale derivatives are
recorded in `xorg-one-texture-oracle.md` under “Three-factor fixed-function
successor.” The Linux physical objects above were produced by executing the
5.75 configuration-two translator/finalizer. The Windows virtual objects and
TEXLD/TEXLDP differentials were produced by executing the supplied Windows 7
D3D UMD under headless Wine; the supplied OpenGL ICD contains byte-matched
tokenizer, dispatcher and finalizer implementations. The two Windows files
are the independently tracked D3D and OpenGL artifacts with SHA-256 values
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
and `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`.

The current 262/266-dword baseline and exact H2 indices are taken from the
merged kernel `chrome9_build_two_textured_triangle()` assertions. The cap-23
depth-read-only and cap-29 extended-factor rules come from the live merged
structured ABI; they change validation/state selection, not this fragment
program's instruction records.
