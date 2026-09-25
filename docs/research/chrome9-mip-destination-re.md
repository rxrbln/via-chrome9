<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 render targets at mip levels and GPU mipmap generation

## Result

The pixel engine renders into any 256-byte aligned window of a guarded
object.  Mesa draws into non-zero mip levels directly (no shadow copy) and
generates mipmaps on the GPU: level L is drawn from level L-1 of the same
object with one bilinear sample per 2x2 texel group.  Kernel CAP2 bit 22
`DESTINATION_OFFSET` (module parameter `destination_offset`, default on).

Silicon, `tools/re/chrome9_mipdst_rungs.c`, module k89-mipdst
(Build ID `fcb59d04`), kernel `77ca263`:

| rung | result |
|---|---|
| `dstoff` | 16 windows, every dword of the object checked: bases at page + 0x100 .. 0xf00 and at the end of a 64-page object, widths 1..2048, heights 1..150, pitches 32, 64, 128, 224, 256, 416, 800, 6400, 8192 (windows crossing pages included): every window pixel written, no byte outside the window (row padding included) changed |
| `reject` | offset 128-aligned, offset in the first page, offset without EXTENDED, window into the last page, window past the object: all `EINVAL` |
| `lodmag` | min_lod = max_lod = 2 reads level 2 when magnifying 8x and at LOD 0, lod 1 reads level 1 when minifying 8x: A30 whole-level limits clamp magnification too (GL would use the base level) |
| `mipgen` | chains 64x32, 100x75, 1x64, 2048x16, 1600x1200 generated level by level in their own object (4 NEAREST taps): every level within 1 LSB of the rounded box filter, nothing outside the level written; POT tail levels at +0x80/+0xc0/+0xe0 and 2048x16 L7..L11 drawn from a rounded base with 2..7 shifted rows |
| `bilinear` | one LINEAR sample at the 2x2 corner: within 1 LSB up to 800-texel sources, 1600x1200 L1 76719 of 480000 pixels off by 3..15 LSB |

Sub-256-byte bases are not expressible (A50 is base >> 8), so no
alignment below 256 bytes was tried.  The bilinear error is the sampler's
coordinate precision at a texel corner (about 2^-16 relative, 0.024 texel
at 1600, beyond the weight step); the texel centres keep half a texel of
margin.

## Register contract (static)

VX900 Part II manual p. 106 and `render-target-format-oracles.md`:
`A50[23:0]` is the RT0 base in 256-byte units, `A52[9:0]` the linear pitch
in 32-byte units.  No width, height or sub-256-byte offset is carried by
A50..A52; A80/A81 clip and A82/A83 scissor bound the written pixels.  The
vendor UMD builds mip levels with a 3D blit per level
(`sfmGenerateMipLevels_inv`, `chrome9-refract-cpu-mipmap-semantics.md`),
so level surfaces as render targets are a vendor path.

## Mesa layouts against that contract

Mip levels start at `level_offset[L]`.  NPOT levels are 256-aligned.  POT
levels are packed at their exact size, `offset(L+1) = offset(L) +
ALIGN(w*cpp,32)*h`; small tail levels fall below 256-byte alignment (a
64x32 BGRA chain: L4 at +0x80, L5 at +0xc0, L6 at +0xe0 from a 256-byte
boundary).  Every earlier level's size is a multiple of this level's pitch
(POT pitches, each at least the next), so such a level starts whole rows
after `ALIGN_DOWN(offset, 256)`: it is drawn from that base with
`first_row = (offset & 255) / pitch`, the vertices shifted by `first_row`
and a scissor on rows `[first_row, first_row + h)`.  Cube faces
(`ALIGN(h, 8)` rows, 256-aligned face stride) are 256-aligned.

## UAPI and kernel

* `drm_chrome9_draw_fragment_program.destination_offset` (former
  `reserved0`, also inside the vertex program draw): with
  `FRAGMENT_DESTINATION_EXTENDED`, byte offset of pixel (0,0), a multiple of
  256, at least `PAGE_SIZE`; zero means `PAGE_SIZE`.  The pitch stays
  `ALIGN(width * cpp, 32)`, width and height at most 2048.
* The object must hold `ALIGN(offset + pitch * height, PAGE) + PAGE`.
  An extended destination is guarded by its first and last page only.
  Before, the kernel treated the page after level 0 as a guard and rewrote
  it with guard words before a draw after CPU writes: in the packed mip
  layout that page holds level 1, which only CPU mip generation afterwards
  repaired.
* A 2D texture of the draw may name the destination handle.  Its base level
  and the levels of its whole-level LOD range must not overlap the rows the
  draw can write (scissor, else the window); the sampler object then is the
  destination object (write fence, destination guard and cache handling).
  A different handle of the same object stays refused.
* Cache handling is unchanged and range based: before a job only guard
  pages and ranges announced by CPU writes are flushed, after a job only the
  guard pages; a later CPU access invalidates the range it announces.  GPU
  mip generation writes no CPU range, so no clflush of the object occurs.
* Jobs run in order and every segment starts with a 3D wait: a level
  written by one ioctl is in memory before the next ioctl samples it.
  Mesa therefore submits one draw per level.

## Mesa

* `chrome9_resource_image_view()` makes a single-level resource sharing the
  parent's BO for a level/layer at a 256-aligned offset; the framebuffer
  binds it instead of a shadow (`chrome9_shadow.direct`).  Draws set
  `DESTINATION_EXTENDED` and `destination_offset`; legacy fixed ioctls keep
  exact single-image targets, so such draws with a non-generic fragment
  program render in llvmpipe.
* `chrome9_generate_mipmap()` first tries the GPU: 2D/1D BGRA/BGRX guarded
  chains, TEX of varying 0 through a LINEAR sampler clamped to level L-1
  (min_lod = max_lod), texture coordinates `2w/sw` (`1` for a source of one
  texel) so each pixel centre falls on the corner of texels `2x, 2x+1`.  A
  source row or column of one texel is used alone, the last of an odd
  count is dropped, as in the CPU path.  `CHROME9_GPU_MIPMAP=0` or any
  rejected level returns to the CPU path for the remaining levels.

## Measurements (k89-mipdst, VX900)

| | before (CPU mipgen, lib mip0) | after (lib mip2) |
|---|---|---|
| glmark2 refract | 7 fps, 147 ms | 9 fps, 124 ms (CPU mipgen via `CHROME9_GPU_MIPMAP=0`: 142 ms) |
| refract perf | `chrome9_generate_mipmap` 5.7 %, `clflush_cache_range` 6.2 % | generator gone, clflush 3.1 % (vertex ranges), kernel profile: 0 full flushes |
| texture:texture-filter=mipmap | Success, 206 fps | Success, 208 fps |
| shadow | 33 fps | 33 fps |
| ioquake3 timedemo GL1 / GL2 | 61..65 / 43 | 61.9 / 42.5 |

One bilinear tap and four NEAREST taps cost the same (124 ms).  piglit
fbo-generatemipmap, -filtering, -formats, -noimage, -nonsquare, -npot,
-scissor, -versus-READ_FRAMEBUFFER, -viewport, gen-teximage,
gen-texsubimage, gen-nonzero-unit, mipmap-setup, fbo-clearmipmap,
ext_framebuffer_object-mipmap, fbo-copyteximage pass as before (the
remaining list entries skip at GL 2.1 without arguments); the GPU path is
taken (fbo-generatemipmap-npot: 8 `DRAW_FRAGMENT_PROGRAM` ioctls, 15 fewer
CPU-access ioctls).  `chrome9_gl_rtt_smoke` 0 mismatches (2D level 1 now
through a view).  Screenshots: `artifacts/2026-09-25/refract-mipgen-gpu1.png`
(GPU) and `-gpu0.png` (CPU).

The remaining refract frame time is outside mip generation; the kernel
draw mapping cache misses (137 per 200 jobs, 15 entries) are a candidate.
