# Chrome9 projected two-texture-primary draw design

Status: implementation-grade static design. The existing two-texture and
two-texture+D16 ioctls are merged and the two-factor RGBA program is live.
The three-factor program in this note is cross-compiler proven but not yet
executed on VX900 silicon. No UAPI or capability described here is currently
advertised.

## Exact bounded operation

The successor represents one compatibility fixed-function operation:

```text
oC0 = texture0.rgba * texture1.rgba * primary.rgba
```

For the initial opaque ioquake3 subset, both texture resources are BGRX8888,
the target is BGRX8888, vertex D1 alpha is exactly `0xff`, blend and alpha test
are disabled, and both sample views are level zero. Thus output alpha is one
even though the native ALU performs a full four-lane product. This restriction
is intentional: it does not expose translucent three-factor alpha semantics
before a BGRA/alpha-test oracle exists.

The linked fixed-function shader is expected to retain projectors
`TEX0.w/TEX1.w`. The native immediate record contains only S/T, so Mesa may
select this operation only after checking that both post-TNL Q lanes are
bitwise `0x3f800000` at all three vertices. Q is not added to the kernel ABI:
the kernel receives an already validated identity-projector operation. A raw
ioctl caller asks only for the nonprojected three-factor program.

## UAPI without a new ioctl

The current allocation ledger reserves capability bits through 24, making 25
the next free bit. Reuse the existing command `0x0a` and `0x0d` structures:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_PRIMARY_MODULATE (1ULL << 25)
#define DRM_CHROME9_TWO_TEXTURE_PRIMARY_MODULATE     (1U << 0)
```

Capability bit 25 means the program flag is accepted by both
`DRAW_TWO_TEXTURED_TRIANGLE` and `DRAW_TWO_TEXTURED_TRIANGLE_D16`; userspace
must still require capability 14 or 20 for the selected base ioctl. Structure
sizes remain 240 and 256 bytes. No reserved field changes meaning.

Validation is exact:

* no-depth `flags` may be zero or the program bit, with every other bit zero;
* D16 `flags` may additionally contain the established depth function in bits
  `[10:8]`; zero/LESS and explicit LEQUAL are the only accepted compares;
* when the program bit is set, both texture formats must be
  `DRM_CHROME9_TEXTURE_FORMAT_BGRX8888` and every vertex color must have alpha
  byte `0xff`;
* target format remains the ioctl's implicit BGRX8888; blend, alpha test,
  destination reinterpretation, mip levels and borders remain unavailable;
* target, texture 0 and texture 1 are distinct for `0x0a`; depth is a fourth
  distinct object for `0x0d`; and
* all existing size, canonical pitch, guard, finite RHW/S/T, normalized S/T,
  scissor and nondegenerate-triangle checks remain unchanged.

An old kernel rejects the nonzero flag. A new kernel continues to interpret
flags zero as the byte-identical two-factor operation. The capability must be
entered in `chrome9-uapi-allocation.md` in the same commit which reserves bit
25; if another feature claims 25 first, update this design and all three UAPI
copies atomically rather than silently reusing the number.

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

Implement this as separate constant arrays selected by the program flag, not
by overwriting indices after emission. Then assert the complete body length,
A01/A02, A92/A93, both MULs, MOV, CC/DD, vertex header and first/last vertex
payload positions. This prevents an instruction-count edit from shifting a
subsequent H2 header unnoticed.

## Mesa path

Add a distinct `CHROME9_FS_TWO_TEXTURE_PRIMARY_MODULATE` enum. The NIR
classifier must flatten only identity four-lane multiply nodes and accept a
leaf multiset of exactly:

```text
projected texture 0 using TEX0.xy and TEX0.w
projected texture 1 using TEX1.xy and TEX1.w
full primary COL0
```

Accept operand permutation and reassociation only; reject repeated/missing
leaves, partial stores, swizzles, constants, discard, extra ALU and an
unobserved `fsat`. The shader scan must count exactly two texture instructions
and zero constants. A later live NIR dump may justify one exact `fsat`
placement, but static fixed-function semantics alone must not broaden the key.

The draw layout remains the existing ten-dword
`position4,D1,specular,T0.xy,T1.xy` kernel record. Gallium's draw module emits
four floats per coordinate, so the Mesa emitter must inspect T0.q and T1.q
before dropping them and require both raw bits equal `0x3f800000`. It must also
require both resolved sampler resources to be guarded BGRX8888 level-zero
2D resources, D1 alpha byte `0xff` on every post-TNL vertex, two distinct
texture handles, a guarded BGRX target, no blend/alpha successor, and either
no depth or guarded Z16 write-enabled LESS/LEQUAL. It sets the program flag
on the already selected no-depth or D16 ioctl. No screen GL capability changes.

## Staged smoke order

1. **Offline builder assertions.** Build both flag variants, check every index
   above, and reject unknown flags/formats/nonopaque D1 before BO lookup.
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
5. **Gallium positive path.** Construct the projected three-leaf NIR, provide
   Q0=Q1=1 and two XRGB views, and require submitted `+2`, rejected `+0` for a
   quad plus the same color/depth results.
6. **Gallium rejection matrix.** Independently use Q0=2, Q1=2, BGRA at either
   unit, D1 alpha `0x80`, missing COL0, duplicate texture leaf, blend enabled,
   alpha test enabled and an unadvertised cap. Every case must produce no
   ioctl; kernel raw tests repeat format/alpha/flag failures with `-EINVAL`.
7. **Retirement and closure.** At every live stage verify all target/depth/two
   texture guards, source bytes and padding, retirement cookie, CR PAUSE,
   work-mask clear, then run a legacy one-texture draw.

Only after the D16 stage passes should ioquake's diagnostic non-mip lightmap
path select this program. Mip addressing, RGB scale x2/x4, BGRA alpha,
alpha-test interaction, blend and batching remain separate successors.

## Provenance

The semantic source is ioquake3
`code/renderergl1/tr_shade.c:1181-1254`. Legacy UniChrome
`via_texcombine.c` independently confirms that stage-zero MODULATE consumes
texture and primary/previous while later stages consume texture and current;
RGB and alpha arguments are selected independently. The scheduled Chrome9
program, Windows Wine compiler hashes and result-scale derivatives are
recorded in `xorg-one-texture-oracle.md` under “Three-factor fixed-function
successor.” The current 262/266-dword baseline and exact H2 indices are taken
from the merged kernel `chrome9_build_two_textured_triangle()` assertions.
