# ioquake3 GL1 live texture and sampler census

## Result

The 2026-09-24 VX900 q3dm1 run allocates only the two ordinary 32-bit color
formats already implemented by the Chrome9 structured fragment path:
`PIPE_FORMAT_B8G8R8X8_UNORM` and `PIPE_FORMAT_B8G8R8A8_UNORM`.  Its private
sampler resources include rectangular chains through L9.  No A8, L8, L8A8,
RGB565, ARGB1555, or ARGB4444 resource is part of this captured default GL1
workload.

The retained target log is:

```text
/root/tmp/chrome9-live/ioquake-projector-20260924/
  gl1-mip-primary-limit2048-resources.log
SHA-256 d8ba767afbe3407b9db2ea714a4d41ba2d82aee5ee522a8a76f18c7448ef3186
46,855 lines
```

The application reported:

```text
texturemode: GL_LINEAR_MIPMAP_LINEAR
picmip: 0
texture bits: 32
```

It entered q3dm1 map loading and reached the deliberate
`CHROME9_DRAW_LIMIT=2048` stop.  The log contains no sampler-view rejection,
bounded-draw-state rejection, cmd22 failure, timeout, quarantine, or GPU
fault.  This proves allocation and accepted draw-state coverage through that
cap.  It does not by itself prove that every allocated level was selected by
a visible fragment.

## Exact private sampler-resource census

These are the actual `resource target=2 ... bind=0xa` creation records, not
the state tracker's `can-create` queries.  Format 281 is BGRX8 and format 105
is BGRA8.

| format | last level | creations |
|---|---:|---:|
| BGRX8 | 0 | 260 |
| BGRX8 | 3 | 1 |
| BGRX8 | 4 | 3 |
| BGRX8 | 5 | 3 |
| BGRX8 | 6 | 68 |
| BGRX8 | 7 | 28 |
| BGRX8 | 8 | 69 |
| BGRX8 | 9 | 2 |
| BGRA8 | 0 | 130 |
| BGRA8 | 4 | 2 |
| BGRA8 | 5 | 43 |
| BGRA8 | 6 | 26 |
| BGRA8 | 7 | 14 |
| BGRA8 | 8 | 15 |

The two L9 BGRX objects are 512x512 and 64x512.  The latter matters: the
current game envelope is not square-only.  The run also creates two Z16
depth resources and BGRX render/display resources, but those are not sampler
format evidence.

The apparently larger `can-create` population with `samples=1/1, bind=0`
is format-capability probing.  It must not be counted as allocated texture
objects.

## Source-exact state requirement

ioquake3 GL1 `Upload32()` scans each RGBA upload for stored alpha.  With the
captured `r_texturebits=32`, opaque images select `GL_RGB8` and alpha-bearing
images select `GL_RGBA8`.  The state tracker consequently chooses the BGRX8
and BGRA8 rows seen above.  `Upload32()` uploads the complete chain through
1x1 and installs the selected global minification/magnification filter.

For this run `GL_LINEAR_MIPMAP_LINEAR` requires:

```text
min image = linear
mag image = linear
mip       = linear
```

Ordinary stage images use repeat; q3 lightmaps are created with
`IMGFLAG_CLAMPTOEDGE`, no mipmap flag, and clamp-to-edge.  These translate to
the already-established Chrome9 sampler words:

```text
A31 = 31102492   linear min/mag plus trilinear mip
A32 = 32000052   repeat S/T
A32 = 32000049   clamp-to-edge S/T
```

The current implementation keeps these dimensions orthogonal:

* `chrome9_format.c` maps BGRX8/BGRA8 to image selectors 98/99;
* `chrome9_context.c:181..220` maps spatial, mip, and wrap modes separately;
* `chrome9_draw.c:1320..1372` serializes the full cmd22 level table and
  sampler state; and
* `chrome9_engine.c:1572..1621,1992..2023,7135..7225` validates canonical
  pitches/offsets and emits image, H3, and A30--A32 state.

No q3-specific format promotion or sampler workaround is justified by this
census.  The packed 16-/8-bit rows remain useful independent API coverage,
not a blocker for this default workload.

## Remaining bounded distinctions

1. Allocation of L9 plus a clean capped run is not a forced-L9 color oracle.
   Keep the explicit implicit-tail test as the correctness proof for L9--L11.
2. Nonzero first-level views, imported mip chains, anisotropy, border modes,
   and mirror-once remain outside this captured state.
3. The UAPI A30 min/max selectors are whole levels.  Mesa's sampler object
   currently accepts an arbitrary finite `max_lod`; if a value below the
   view maximum has fractional s5.5 bits, cmd22 serializes them but the
   kernel correctly rejects the descriptor because bits 4:0 must be zero.
   ioquake3's default large maximum clamps to the integral view last level,
   so this mismatch is not active here.  A generic Mesa fix should reject or
   exactly lower that fractional clamp before submission; it must not round
   silently and change API semantics.
