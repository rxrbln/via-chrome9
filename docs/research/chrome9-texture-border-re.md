<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 texture border, GL_CLAMP, mirror once, 1D textures and LOD limits

Status of the sampler wrap features beyond clamp/repeat/mirror, with the
register evidence and the silicon results (VX900, 2026-09-25).

## Registers

Type-02 sampler state of stage `n` (`((0x20 + n) << 24) | 00020000`):

| word | bits | meaning |
|---|---|---|
| A32 | `[2:0]` S, `[5:3]` T, `[8:6]` R | 000 border, 001 clamp, 010 repeat, 011 mirror, 111 mirror once |
| A35 | `[23:12]` R, `[11:0]` G | border colour, sign-magnitude s1.10 |
| A36 | `[23:12]` B, `[11:0]` A | border colour, sign-magnitude s1.10 |

Sources: Windows address table `VTGUModeDX32A.dll:0x101d9250` (BORDER -> 0,
MIRRORONCE -> 7) and border builder `0x101d9860..0x101d9a2a`, Linux
`STM_BorderColor_INV:0x1b1c40`, converter `0x1020aaa0`
([xorg-one-texture-oracle.md](xorg-one-texture-oracle.md) "Exact border
color").  The vendor quantizes an ARGB8 colour: `code = c * 0x400 / 255`
truncated, 255 -> `0x400`.  With a border the vendor sampler group is
`30 31 32 35 36 cc cc cc` (16-byte shape kept).

## Kernel ABI (drm/chrome9 `b9d6f7d`)

* `DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER` (4) and
  `DRM_CHROME9_TEXTURE_WRAP_MIRROR_CLAMP_TO_EDGE` (5) for fragment textures,
  gated by `DRM_CHROME9_CAP2_TEXTURE_BORDER` (bit 16) and
  `DRM_CHROME9_CAP2_TEXTURE_MIRROR_ONCE` (bit 17).  Module parameters
  `texture_border` (default on) and `texture_mirror_once` (default off),
  both writable at runtime (0600): Mesa reads the capability at screen
  creation.
* `border_color` of `drm_chrome9_fragment_texture_state` is ARGB8888; the
  kernel converts with the vendor rule and emits A35/A36 only when an axis
  wraps to the border.  Legacy textured-triangle ioctls keep codes 1..3.
* `chrome9_fragment_texture_border_selftest()` checks the complete vendor
  opaque-green packet (`32000040 35000400 36000400`), half grey
  (`35202202 36202202`) and the mirror-once A32 word `3200004f`.

## Mesa (chrome9 driver)

* `PIPE_TEX_WRAP_CLAMP_TO_BORDER` -> WRAP 4 with the sampler border colour
  rounded to ARGB8 (`chrome9_border_argb`).  Integer border colours are
  unsupported (llvmpipe).  The state tracker already translates the colour
  for the base format (`st_translate_color`), and the sampler returns the
  border unswizzled, so no border quirk is needed for the native formats.
* GL_CLAMP: Chrome9 keeps `caps.gl_clamp = false`.  Mesa lowers GL_CLAMP to
  CLAMP_TO_EDGE for NEAREST and to CLAMP_TO_BORDER plus a saturated
  coordinate for LINEAR, sampling with `txd(fsat(c), ddx(c), ddy(c))`.  The
  sampler has no TEXLDD, so `chrome9_nir_lower_hw_textures` turns that TXD
  back into TEX when both derivatives are those of the unsaturated
  coordinate.  The hardware then derives the LOD from `fsat(c)`; this only
  differs where `c` is clamped, i.e. where the sample lies on the edge
  texel/border blend anyway.  With a single level or equal min/mag filters
  the result is identical.  The llvmpipe fallback keeps the original TXD
  (`chrome9_shader.sw_nir`).
* Mirror once: `PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE` -> WRAP 5 and
  `caps.texture_mirror_clamp_to_edge` only with CAP2_TEXTURE_MIRROR_ONCE.
  Not enabled by default: the Windows table proves only the code, not the
  GL edge convention (see open probe below).
* 1D textures are height-1 2D images (`chrome9_is_surface_target`); the
  fragment shader lowers 1D to 2D with T = 0.5 (`nir_lower_tex.lower_1d`)
  and the draw forces T to clamp-to-edge, since GL ignores the T mode of a
  1D texture.
* LOD limits: A30 `[11:6]` max and `[5:0]` min are whole levels (vendor
  builders pass MAXMIPLEVEL-style integers).  `chrome9_lod_level` passes
  whole GL limits through, turns a fractional limit into the level NEAREST
  mip selection picks at that LOD (`ceil(lod + 0.5) - 1`), and leaves a
  fractional limit with LINEAR mip filtering to llvmpipe
  (`chrome9_draw_needs_sw_textures` now routes every unsupported sampler
  state there).

## Silicon results

`tools/re/chrome9_gl_border_smoke.c` (4x4 RGBA texture over a 64x64 FBO,
texcoords -1..3, CPU GL reference, border (0.2, 0.4, 0.6, 0.8)), kernel
`8cc40884`, Mesa tag `tcl-20260925/s1`:

| case | 2D nearest | 2D linear | 1D nearest | 1D linear |
|---|---|---|---|---|
| CLAMP_TO_BORDER | 0 mismatches, exact border 51,102,153,204 | worst 1 LSB | exact | worst 1 LSB |
| GL_CLAMP | exact (= clamp to edge) | worst 1 LSB | exact | worst 1 LSB |
| REPEAT / MIRRORED_REPEAT / CLAMP_TO_EDGE | exact | worst 1 LSB | exact | worst 1 LSB |

So the hardware border mode matches GL CLAMP_TO_BORDER, including the
linear blend between the edge texels and the border, and GL_CLAMP with
LINEAR through the saturated coordinate.  piglit `lodclamp`,
`lodclamp-between` (llvmpipe) and `lodclamp-between-max` pass
(`tcl-20260925/s3`).

## Mirror once probe

With `texture_mirror_once=1` (kernel `3b52a780`), `chrome9_gl_border_smoke
MIRROR_CLAMP_TO_EDGE` matches GL MIRROR_CLAMP_TO_EDGE exactly (nearest) and
within 1 LSB (linear), 2D and 1D, in 9 of 10 runs: the sample map shows
the mirrored image in [-1, 0] and the edge texel beyond both ends.  The
first run after a module reload returned 0,0,0,0 for samples at negative
coordinates (816 of 4096 pixels for 2D nearest); it did not recur in the
following nine runs, including runs with and without hardware TCL.  Until
that first-use behaviour is explained, `texture_mirror_once` stays off by
default and Mesa does not expose ARB_texture_mirror_clamp_to_edge.

## Open

* Mirror once: done 2026-09-25.  Two fresh module loads (TEXLDB module
  `dbadc6e` and the mip-destination module), each followed first thing by
  three `chrome9_gl_border_smoke MIRROR_CLAMP_TO_EDGE` runs and one full
  smoke, were exact (nearest) or within 1 LSB (linear), 2D and 1D
  (`artifacts/2026-09-25/mirror-once-*.txt`).  The earlier zero samples
  predate `018a504`, which fixed SWIOTLB lost jobs with partially zero
  readbacks.  `texture_mirror_once` is on by default (kernel `cb20c55`).
* piglit `texwrap`: every draw of the test currently fails with EINVAL
  before any texture state is involved (a constant-colour clear-like quad
  whose vertices start at x = y = -0.5); this is outside the sampler path.
