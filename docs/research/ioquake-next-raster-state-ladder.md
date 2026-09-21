# Chrome9 ioquake next raster-state ladder

Status: packet-ready static design, 2026-09-21.  The encodings below agree
across the VX900 manual and the proprietary Linux, Windows D3D, and Windows
OpenGL state builders.  D16 LESS/write, D16 LEQUAL/write, blend, color masks,
fixed A33 alpha test, and software cull/offset are already established paths.
D24S8 and stencil have not yet run on the live VX900 and remain behind the
new versioned capability proposed here.

## Scope and immediate ordering

The first live ioquake draw after the fixed-function shader work was an
untextured D16 `ALWAYS`/write draw with color mask zero.  Its exact command-08
promotion is specified in
[ioquake-depth-always-mask-zero.md](ioquake-depth-always-mask-zero.md).  The
states likely to follow do not all need native ABI at once:

| state | current safe owner | next action |
|---|---|---|
| color mask, including zero | command-08 A53 | no new field or capability |
| front/back cull | Gallium draw pipeline | retain software cull |
| triangle polygon offset | Gallium draw offset stage | retain software offset |
| alpha test | fixed A00/A33 in blend state | reuse capability 10 |
| D24S8 and stencil | unsupported | add one versioned structured successor |

This ordering matters.  The current vbuf path always requests Gallium draw,
which clips, resolves physical winding, culls, applies triangle depth offset,
and decomposes primitives before Chrome9 emits independent triangles.  Native
cull or bias would be a performance path, not an ioquake correctness blocker.
Enabling either in hardware while leaving the software stage active would
double-apply the state.

The first genuinely new contract is therefore a retained guarded D24S8 BO and
complete stencil state.  Reserve private command `0x17` and capability bit 36
as `DRAW_TRIANGLE_D24S8_STENCIL`; the canonical allocation is recorded in
[chrome9-uapi-allocation.md](chrome9-uapi-allocation.md).  The capability is
not advertised until the bounded readback suite in this note passes.

## Independent binary evidence

The principal proprietary sources are:

* Linux `via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`:
  `STM_AlphaTest_INV:0x194c40`, `STM_AlphaFunc_INV:0x194ad0`,
  `STM_Stencil_INV:0x1955e0`, `STM_DepthBias_INV:0x1959d0`, and
  `STM_UpdateCull_INV:0x196a70`;
* Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`:
  color mask `0x101f9cd0`, cull `0x101fabd0`, alpha enable/function
  `0x101fab20`/`0x101fb090`, depth bias `0x101fbde0`, and stencil
  `0x101fc060`; and
* Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`:
  color mask `0x1024d990`, cull `0x1024e890`, alpha test `0x1024edc0`,
  depth bias `0x1024f790`, and stencil `0x1024fa10`.

All three stencil builders require the D24S8 resource format before setting
A01 bit 4.  They pack an eight-bit reference, compare mask, and write mask in
A23/A25, and compare plus three operations in A24/A26.  The Windows D3D
builder's format check is at `0x101fc08b..0x101fc0b0`; its disabled path at
`0x101fc343` clears A01 bit 4.  Its `0x101fc0b6..0x101fc1e6` operation packing
is independently byte-equivalent to the Linux builder.

The Windows cull builder at `0x101fabd0..0x101face4` proves that A01 bit 7 is
the enable and type-04 A23 bit 8 selects the physical culled winding.  Type-04
A23 bit 7 is a separate vertex-output selector and must remain fixed.  The
depth-bias builder at `0x101fbde0..0x101fc058` independently proves the split
IEEE words and the 16-bpp versus 32-bpp units scale described below.

## Version-one structured request

Do not reinterpret command 08: it promises an always-present linear D16
surface and always-enabled depth test.  The D24S8 successor must represent
depth disabled with stencil enabled, depth enabled read-only, and stencil
disabled while a D24S8 attachment remains bound.  Explicit booleans make
those cases unambiguous, including native compare code zero (`NEVER`).

```c
struct drm_chrome9_stencil_face {
	__u32 func;          /* NEVER..ALWAYS = 0..7 */
	__u32 fail_op;       /* normalized Gallium order, 0..7 */
	__u32 zfail_op;
	__u32 zpass_op;
	__u32 compare_mask;  /* 0..255 */
	__u32 write_mask;    /* 0..255 */
	__u32 reference;     /* 0..255 */
}; /* 28 bytes */

struct drm_chrome9_draw_triangle_d24s8_stencil {
	__u32 destination_handle;             /*   0 */
	__u32 depth_stencil_handle;           /*   4 */
	__u32 flags;                          /*   8: depth func/read-only */
	__u32 depth_enable;                   /*  12: exactly 0 or 1 */
	__u32 stencil_enable;                 /*  16: exactly 0 or 1 */
	__u32 width;                          /*  20 */
	__u32 height;                         /*  24 */
	__u32 destination_pitch;              /*  28 */
	__u32 depth_stencil_pitch;            /*  32 */
	__u32 destination_format;             /*  36 */
	struct drm_chrome9_blend_state blend; /*  40..79 */
	struct drm_chrome9_stencil_face cw;   /*  80..107 */
	struct drm_chrome9_stencil_face ccw;  /* 108..135 */
	struct drm_chrome9_vertex_d16 vertices[3]; /* 136..183 */
	struct drm_chrome9_scissor_state scissor; /* 184..195 */
	__u32 reserved[3];                    /* 196..207 */
}; /* exactly 208 bytes */
```

`flags` reuses only `DRM_CHROME9_DEPTH_FUNC_MASK` and
`DRM_CHROME9_DEPTH_READ_ONLY`.  If `depth_enable==0`, both fields must be zero;
the kernel emits a neutral A13 and clears both A01 depth bits.  If depth is
enabled, all advertised compare values are meaningful, including zero, so the
new path must not apply the legacy zero-to-LESS default.  Read-only clears only
the Z write bit.  It never suppresses stencil writes.

`stencil_enable==0` requires both input face structures to be all zero and
lowers the canonical disabled block.  With stencil enabled, version one may
require `cw==ccw` and let Mesa mirror one-sided state.  Lifting that equality
requires the opposite-winding oracle below; the UAPI fields already name
physical screen-space winding, not API front/back, so no ABI change is then
needed.

The public operation values deliberately follow Gallium:

```text
KEEP ZERO REPLACE INCRSAT DECRSAT INCR_WRAP DECR_WRAP INVERT
  0    1      2       3       4         5         6       7
```

Hardware order is `{0,1,2,3,4,6,7,5}`.  The kernel must translate through
that exact table.  Copying the three-bit public value would exchange INVERT
with both wrap operations.

## Exact 204-dword body

Start from the current command-08 blend body, which is 192 dwords for three
vertices.  Replace its six-word A10/A11/A12/A13/A20/A33-or-pad region with
the following 18-word closed region, a net growth of 12 dwords:

```text
body[26] A10 = 10000000 | (depth_stencil_sf_va >> 8)
body[27] A11 = 11000001 | ((depth_stencil_pitch >> 5) << 13)
body[28] A12 = 123e0000                         # linear D24S8
body[29] A13 = 13000000 | (depth_func << 16)   # neutral 13070000 if off
body[30] A16 = 16000000                         # zero native bias
body[31] A17 = 17000000
body[32] A18 = 18000000
body[33] A19 = 19800000                         # -1 clamp low payload
body[34] A1A = 1a800000                         # +1 clamp high payload
body[35] A1B = 1b803fbf                         # precision + clamp high bytes
body[36] A1E = 1e000000
body[37] A1F = 1f000000
body[38] A20 = 20007fff                         # wide pixel-Z interval
body[39] A23 = 23000000 | cw.ref<<16 | cw.cmask<<8 | cw.wmask
body[40] A24 = 24000000 | cw.func<<16 | cw.fail<<6 |
                         cw.zfail<<3 | cw.zpass
body[41] A25 = 25000000 | ccw.ref<<16 | ccw.cmask<<8 | ccw.wmask
body[42] A26 = 26000000 | ccw.func<<16 | ccw.fail<<6 |
                         ccw.zfail<<3 | ccw.zpass
body[43] A33 = alpha-test word, or 33007000 when disabled
```

For 32 pixels, the canonical D24S8 pitch is 128 bytes and A11 is
`11008001`.  D24S8 memory words are `depth24<<8 | stencil8`.  Keep reading-Z
cache, direct-write-through, tiled mode, and fast-Z clear disabled for the
first test.

The A01 matrix is exact:

| depth | Z write | stencil | A01 |
|---|---|---|---:|
| off | off | off | `01c01540` |
| off | off | on | `01c01550` |
| on | on | off | `01c0154c` |
| on | read-only | off | `01c01548` |
| on | on | on | `01c0155c` |
| on | read-only | on | `01c01558` |

Color remains enabled even for mask zero; A53 is the only RT0 channel gate.
Hardware cull bit 7 remains clear.  With stencil disabled, lower the complete
neutral block regardless of the zeroed request faces:

```text
2300ffff 24070000 2500ffff 26070000
```

The 12-word insertion moves every later command-08 index by 12:

| state | command 08 | D24S8 successor |
|---|---:|---:|
| next H2 header | 32 | 44 |
| type-04 A23/A24 | 57/58 | 69/70 |
| scissor A80--A83 | 108..111 | 120..123 |
| immediate H2 header | 168..171 | 180..183 |
| three six-dword vertices | 172..189 | 184..201 |
| closing pad | 190..191 | 202..203 |

Thus the body is exactly 204 dwords.  The existing command-08 blend words,
destination A50/A52, A53 color mask, A00 blend/alpha enable, shader constants,
FVF, immediate vertex format, termination, ring cookie, and retirement path
are otherwise byte-identical.

Alpha test remains the established fixed function:

```text
A00 bit 20 = enable
A33 = 33000000 | (func << 12) | ((ref << 2) | (ref >> 6))
```

with the existing special encoding `ref=255 -> 0x400`.  It consumes final
fragment alpha.  Do not replace it with the partially recovered TEXKILL
sequence.  The new combined oracle must prove that an A33-killed fragment
changes neither color, depth, nor stencil.

## Kernel validation and lifetime contract

Validation is deliberately narrower than the physical encoding:

1. Require capability 36, command 17, exactly zero reserved words, booleans
   in `{0,1}`, and no unknown flag bits.
2. Require distinct native shmem target and D24S8 objects.  Both use the
   established guard-page layout.  The target pitch is exactly
   `ALIGN(width*4,32)`; D24S8 pitch is exactly `ALIGN(width*4,32)`; dimensions
   remain `1..2048`; overflow-check `pitch*height` before adding both guards.
3. Accept only BGRX/BGRA target formats already advertised.  D24S8 is
   implicit in command 17; there is no caller-controlled native A12 value.
4. Validate blend, color mask, alpha test, scissor, finite vertices, triangle
   area, target bounds, and destination format through the existing strict
   helpers.  Capability 10 remains required when alpha test is enabled, cap 9
   for BGRA, and the relevant depth-function cap remains required for EQUAL or
   ALWAYS.
5. If depth is disabled, require zero depth-function/read-only flags.  If
   stencil is disabled, require zero face structures.  If stencil is enabled,
   require functions and operations `<=7`, byte fields `<=255`, and, for the
   first gate, equal CW/CCW blocks.
6. Retain both objects in one `drm_exec` transaction.  Fence the target WRITE
   even for color mask zero and fence D24S8 WRITE even for depth read-only,
   because stencil may update.  Do not alias the objects.  Preserve timeout
   quarantine and all guard mappings until retirement or quarantine release.

CPU clear and readback are mandatory for the first implementation.  Do not
enable fast Z/stencil clear, compression, tiled layout, or narrower
reservation modes in the same change.  Mesa currently discards
`set_stencil_ref`; it must store both reference bytes and combine them with
the DSA face state at draw time.

## Bounded live promotion

Every oracle uses a 32x32 interior triangle, canonical pitches, poisoned guard
pages, bounded retirement, exact word readback, and post-test `040c==0`.

1. **Stencil write mask with color and Z inert.** Fill target with `44332211`
   and D24S8 with `6543213c`.  Use depth disabled, stencil ALWAYS, ref `a5`,
   compare mask `ff`, write mask `0f`, KEEP/KEEP/REPLACE, color mask zero.
   Covered color remains `44332211`, covered D24S8 becomes `65432135`.
2. **Compare mask.** Fill adjacent regions with stencil `a1` and `b5`; use
   EQUAL, ref `a5`, compare mask `f0`, full write mask, REPLACE on pass.  Only
   `a1` passes.  This distinguishes mask direction without depth semantics.
3. **Operation matrix.** Verify `INCRSAT(ff)->ff`, `INCR_WRAP(ff)->00`,
   `DECRSAT(00)->00`, `DECR_WRAP(00)->ff`, and `INVERT(55)->aa`, changing only
   the pass field.
4. **Z-fail routing.** Enable depth read-only with a comparison guaranteed to
   fail.  Use stencil ALWAYS, sfail KEEP, zfail REPLACE, zpass KEEP.  Only the
   stencil byte becomes the reference; color and the upper 24 depth bits stay
   unchanged.
5. **Depth write plus mask zero.** Repeat the live D16 ALWAYS discriminator
   using D24S8: incoming far depth must replace the upper 24 bits, stencil
   KEEP preserves the low byte, and target remains byte-identical.
6. **A33 kill ordering.** Draw two regions whose final alpha is `40` and `c0`
   with alpha LESS/ref `80`, stencil REPLACE-pass, and enabled depth writes.
   The killed region must preserve color, depth, and stencil; the passing
   region must update all enabled planes.
7. **State closure.** Submit D24S8 stencil -> command-08 D16 -> D24S8 stencil,
   then alpha on -> off -> on.  A01 bit 4, A33 enable, all A23--A26 words, and
   A12 format must not leak across requests.
8. **Two-sided gate.** Only after the one-sided suite passes, use distinct CW
   and CCW references and opposite-winding triangles to prove the physical
   face mapping before lifting the equality validator.

A pixel mismatch fails the capability but does not by itself justify reset.
Any timeout, VM fault, guard mutation, or non-idle work mask follows the
existing quarantine policy.

## Native cull and polygon offset remain later optimizations

If native cull is eventually promoted, its exact delta from the current
Gouraud state is:

```text
none:      A01 bit7=0, type-04 A23=23004f02
cull CW:   A01 bit7=1, type-04 A23=23004e02
cull CCW:  A01 bit7=1, type-04 A23=23004f02
```

The apparently unchanged last word in the CCW row is meaningful only with
A01 bit 7 set.  Bit 7 of type-04 A23 stays unchanged (clear in the established
`23004f02/23004e02` pair) in every row.  Front/back and
viewport-Y inversions must be resolved to physical winding before submission;
`FRONT_AND_BACK` suppresses the draw rather than selecting a native mode.

Native polygon offset uses:

```text
A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000) | (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

For scale zero and offset `-0.25`, the exact words are
`16800000 170000be 18000000`; disable is
`16000000 17000000 18000000`.  Public units are multiplied by
`0x38000080` for D16 or `0x34000001` for 32-bpp D24S8.  A future native-bias
command-08 extension would replace the current six depth words with 14 words,
growing the blend body from 192 to 200 dwords: next H2 at 40, immediate H2 at
176, vertices 180..197, pad 198..199.  It must disable the Gallium draw offset
stage for that draw.  No capability is reserved now because the software
stage already supplies the required ioquake semantics.

The D24S8 successor intentionally emits neutral A16--A18 and retains software
offset.  Native cull and bias should each receive their own isolated live
oracle rather than being bundled into the first stencil experiment.
