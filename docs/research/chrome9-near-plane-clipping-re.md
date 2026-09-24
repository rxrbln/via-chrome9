# Chrome9 near-plane clipping in the hardware-TCL path

Scope: why today's `DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR` oracle draws the
unclipped projection, how the vendor drivers enable clipping for hardware TCL
draws, and the next silicon oracle.  Static analysis only; no target access.

Binaries (hashes in [native-clip-cull-list-fastpath.md](native-clip-cull-list-fastpath.md)):
Linux `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, Windows
`VTGUModeDX32A.dll` and `VTGOGL32A.dll` from `VT3410_Windows7_241601j_VIAwSetup_LOGO`.
Manual: `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf` (text lines cited
from `~/tmp/chrome9/hd3d.txt`).

## Result

1. **Today's oracle never enabled the clipping engine.**  In
   `chrome9_emit_3d_quad_h6_internal()` (`drivers/gpu/drm/chrome9/chrome9_drv.c:893`)
   the bit is ORed into `clip_config[1]`, which is emitted under paratype
   `0xfe020000` (line 863).  That is HParaType 02h / HParaSubType FEh,
   sub-address 01h, "Attribute of Texture 0/1/2" (manual line 7712): bit 11
   is inside the texture-1 source field `[12:10]`, so `01002521 -> 01002d21`
   only re-selects the source of an unused texture.  The real clipping-engine
   enable is HParaType 01h sub-address 01h (manual line 4973, bit 11 at line
   5015), which the oracle sends unchanged as `state[15] = 01c01540`
   (`chrome9_drv.c:856`, paratype `0x00010000`).
   Consequences:
   * the +-4 "clip wide" quad does not prove clipping: unclipped, it
     projects to +-64 pixels and the target bounds produce the same full
     32x32 fill;
   * the clip-near result (unclipped projection, 107 pixels, identical with
     and without the `MOV dest32,src128` tail) is exactly what a disabled
     clipping engine produces.
2. **Vendor hardware-TCL draws clip in hardware.**  Both vendor families set
   type-01 A01 bits 11 **and 20** together whenever the vertex input is not
   pre-transformed, and clear both for XYZRHW input.  Bit 20 is "Reserved" in
   the manual (line 4996) but is never written separately.
3. The CPU clippers in the Linux GL driver belong to the software-VS and
   immediate-mode paths only.  They are not applied before hardware-TCL
   submission.
4. The clip-space position reaching the clipping engine is VS output 32
   (oPos).  The vendor compiler writes it with destination `0xa0`
   (scratch 128 plus oPos 32); the finalizer's output 33 is the post-divide
   raster position.  The remaining uncertainty is whether `0xa0` alone
   reaches output 32.  Today's tail experiment could not show this because
   the engine was off.

## Vendor A01 clip decision

### Linux `STM_ViewPort_INV` (0x1bab80..0x1bb91f)

The hardware image pointer is `ctx+0x9430`.  Type-01 register N is at image
offset `4*N` (A01 at `+0x4`, confirmed by `STM_EdgeAntialias_INV`
0x19441c/0x194440 using `0x4(%edx)` for A01 bit 5).  Type-04 starts at
`+0x20bc` (A23 at `+0x2148`, A40 at `+0x21bc`).

```text
0x1bb029  mode = *(*(*(ctx+0x9464)+0x240)+0xc)
          fvf  = *(ctx+0x436c)->0x204
          if mode == 5 || mode == 8:  tcl = (fvf & 0x400e) != 4     # not XYZRHW
          else:                       tcl = (fvf & 0x8000) != 0
tcl == 0 (0x1bb0e9 -> 0x1bb11e):
          andb $0xef,0x6(%esi)   # A01 bit 20 = 0
          andb $0xf7,0x5(%esi)   # A01 bit 11 = 0
          scale (1, +-1, 1), offset 0 (+786432 bias), A1E/A1F = ff000000/7f000000 class
tcl != 0 (0x1bb06d/0x1bb4b0 -> 0x1bb4f0 -> 0x1bb522):
          sx=W/2, sy=-H/2 (window) ... sz=f-n, bias for A23 low nibble not 0/7
          orb $0x10,0x6(%esi)    # A01 bit 20 = 1
          orb $0x8,0x5(%esi)     # A01 bit 11 = 1
          A1E/A1F = [min(n,f), max(n,f)] split as low 24 bits + high byte
common:   A40..A48 at +0x21bc..+0x21dd (low 24 bits, high bytes in A42/A45/A48),
          dirty type-04 0x40..0x48; if chip byte (0xa0a & 0xf0) == 0x30 also
          rows 256/257/258 at image +0x23ec/+0x23fc/+0x240c
```

These are the only A01 bit 11/20 writers in the library.  A grep of the full
disassembly finds only 0x1bb122/0x1bb526 for bit 11 and 0x1bb11e/0x1bb522
for bit 20.

### Windows

The D3D builder uses one dword operation on the same image layout:

```text
VTGUModeDX32A.dll 0x101f8a73  orl  $0x100800,0x4(%esi)    # TCL
VTGUModeDX32A.dll 0x101f8af2  andl $0xffeff7ff,0x4(%esi)  # pre-transformed
VTGOGL32A.dll     0x1024d4a6  orl  $0x100800,0x4(%esi)
VTGOGL32A.dll     0x1024d525  andl $0xffeff7ff,0x4(%esi)
```

The D3D selector (0x101f88d9..0x101f8996) tests FVF bit 15 (`and $0x8000`)
in one mode and the D3D clipping/device state (`+0x6154`, `+0x1db4`) in the
other.  This matches the Linux structure.

### Exact words

The vendor TCL A01 is therefore `base | 0x00100800`.  For the proven raster
base used by the kernel oracles:

```text
pre-transformed (current oracles):  A01 = 01c01540
vendor hardware TCL:                A01 = 01d01d40
```

The table in [native-clip-cull-list-fastpath.md](native-clip-cull-list-fastpath.md)
(`01ce1d40` and similar) omits bit 20.  The vendor-exact forms are
`01de1d40`, `01de1dc0`, `01de1d4c` and `01de1dcc`.

### Other clip state in vendor TCL draws

| state | vendor value | source |
|---|---|---|
| A23 bits 23:16 | never written (0): near-plane triangle/point clip and W=0 clip enabled, ECOs enabled | no store to image `+0x214a` anywhere; A23 writers touch only bytes `+0x2148/+0x2149` |
| A40..A48 | same scale/offset floats as rows 256/257, split low 24 / high byte | `STM_ViewPort_INV` 0x1bb297..0x1bb37b |
| A48[23:16] area exponent | not written by the viewport (0 from image init) | stores only to `+0x21dc/+0x21dd` |
| A50/A51/A52 | `50000000 51000000 52147fff` (clamps +-0x7f000000, HC2SXYEmax4CL=20) | `stmInitChipImage_inv` 0x1789b6..0x178a14 |
| A88/A89 X guard band | `88001ed0 89000800` (-304, 2048) unless the registry values `+0x228->+0x3c0/+0x3c4` override them; the viewport widens them by `(right-left)*factor` when `+0x228->+0x47c != 0` | 0x178866..0x1788af, 0x178f25..0x178f51, 0x1bb7c8..0x1bb86b |

The kernel oracle already matches A23, A40..A48 (as computed at
`chrome9_drv.c` lines 870-890), A50..A52 and A88/A89.  **The only missing
vendor TCL difference is A01.**

## Software clipping in the vendor GL driver

The Linux library contains a complete CPU clipper: `ClipFront`/`ClipBack`,
guard-band variants, `DrawClipOneTriangle`, `SWEMU_NewClipDraw`,
`__glDpClip*Primitive*` and `__glSwp*_SWRasterise_Clip`.  Callers:

| clipper entry | callers |
|---|---|
| `UpdateClipper`, `CheckClipCodeBuffer` | `SWEmu_SWTL` 0x1225d0, `stmEmulateVertexShader` 0x144130, `stmSwEmuVB` 0x145860 |
| `__glDpUpdateSWClipContext` | `__glS3InvTnLAndAccum` 0x2318c0, `__glS3InvBegin` 0x2334c0 (immediate mode) |
| `__glS3SwvsUpdateClipInfo` | `__GLDpUpdateAccumState`, `__glS3EmuRasterPos`, `__glS3EmulateVertexShader` |

These paths produce transformed XYZRHW vertices.  For XYZRHW input,
`STM_ViewPort_INV` clears A01 bits 11/20.  The hardware-fetch TCL draws
(`__glS3H5DrawPrimitiveTriangleList` 0x1e7490 and related functions) do not
use a CPU clip test.  They rely on the clipping engine being enabled by the
viewport state.  `STM_Clipping_H5`, `STM_ClipPlaneEnable_H5` and
`STM_SetClipPlane_H5` are empty on H5/H6 (0x1bab30..0x1bab50).  User clip
planes are DP4s into texcoord lanes (rows 259..264), not a clipping-engine
feature.

## Position data seen by the clipping engine

* Manual type-04 A00 bits 23:18 ("Test the VS's oPos", lines 9003-9018) and
  the A40..A48 note (line 10163: HC2S coefficients "used in Clipping
  Engine", c256/c257 "used in VS") show two position consumers.  The VS
  finalizer consumes rows 256/257 and produces raster output 33.  The
  clipping engine consumes clip-space oPos, and A40..A48 map the new
  vertices it creates to screen space.
* A52 HC2SXYEmax4CL (line 10286) talks about the screen coordinate of a
  "clipped new vertex", i.e. the engine computes screen coordinates only for
  generated vertices.  Unclipped vertices keep the finalizer's output 33.
* op 14 is part of the finalizer, which writes output 33 only.  It does not
  feed the clipping engine and does not need to change.
* Destination `0xa0` is `0x80` (scratch 128) plus `0x20` (oPos 32).  This is
  a strong inference that the user oPos write reaches output 32 directly, and
  that the Linux `MOV dest32,src128` tails are unused padding in the normal
  case.  In the CALL form, oPos lives in a temporary and `MOV dest32,r1`
  becomes active, which is the case where no `0xa0` write exists.
  (hw-TCL RE doc; still unmeasured.)

## Recommended silicon oracles

Base: the existing `DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR` build (passthrough
program, rows 256..258, A40..A48 from rows 256/257, A23=`23000f02`, 3-vertex
triangle), with **FE02 A01 restored to `01002521`**.  This removes the
incorrect `clip_config[1] |= 0x800`, so FE02 is back to the value of the
proven W=1/W=2 runs.

**Oracle N1 (one variable: type-01 A01 word)**
`state[15] = 01c01540 -> 01d01d40` (vendor-exact bits 11+20), emitted only
when `vs->clip`.

Expected result, 32x32 target:
* clipping works: the near-clipped quadrilateral
  `(7.5,23.5) (23.5,23.5) (37.9,-19.7) (-6.9,-19.7)` in screen space fills
  about 584 pixels in rows 0..23 and nothing in rows 24..31;
* clipping still off: the known 107-pixel apex-down projection in rows
  24..31.

The coverage model is in `~/tmp/chrome9/clipnear/cov.py`.  It gives 110
pixels for the unclipped case against the measured 107, a fill-rule
difference.  The same health checks apply: retirement, `040c`, busy bits and
guards.  Run `CLIP_WIDE` again with the same A01 change as a no-regression
check; it must stay a full fill.

Follow-ups, one variable each, only after N1:

* **N2** the same run with `CLIP_NEAR_TAIL` (A60 +1).  An identical image
  proves that `0xa0` alone feeds output 32 and the tails can stay inactive.
  If only N2 clips, emit the vendor CALL form (`MOV dest32` active) for
  clipped programs.
* **N3** `01c01d40` (bit 11 only), to learn whether reserved bit 20 is
  required.  Mesa should emit the vendor pair regardless.
* **N4** `01d01540` (bit 20 only), to identify bit 20's own effect.
* **N5** near-plane convention: replace the apex with `(0,.5,-.25,.5)`
  (`00000000 3f000000 be800000 3f000000`).  D3D `0<=z` clipping gives about
  170 pixels (cut at screen y=8.07).  GL `-w<=z` or no clipping gives about
  200 pixels (full triangle to y=-0.5).  This tells whether GL depth needs
  the Windows `z'=(z+w)/2` remap before the clipping engine sees oPos.
  If the engine clips at `z=0`, the remap must be applied to oPos itself,
  not only in the finalizer.
* **N6** only after N1..N5: A23 bit 22 set (`23400f02`) with a
  mixed-sign-W triangle whose Z values are all positive (manual line 9515),
  to confirm the W=0 plane.

Required code correction regardless of results: delete `clip_config`
(`chrome9_drv.c:893`).  Derive A01 from the base for clip-enabled keys as
`01c01540 | 0x00100800`, rebuilt per segment rather than ORed into inherited
state.

## Silicon result (2026-09-24)

With type-01 A01 `|= 0x00100800` (kernel Chrome9 `2541417`), the clip-near
oracle draws 568 pixels with zero mismatches against the clipped reference
and clip-wide fills the target; both reproduce after an X restart once the
oracles also program the pixel Z clip `1e000000 1f000000 20007fff`
(`e7bcaf9`..`54912ca` series, fix in the commit after `07dff3d`).  The FE02
bit-11 mistake that preceded this set texture 1's source and is the likely
cause of the GART translation-flush wedge recorded in
`/root/tmp/chrome9-live/wedge-gti-20260924/`.
