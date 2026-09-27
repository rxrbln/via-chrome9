<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 anisotropic texture filtering: vendor register contract

Status: host-only reverse engineering, 2026-09-27.  No word in this note has
run on VX900 silicon.  It closes the open anisotropy item of
[sampler-next-implementation-audit.md](sampler-next-implementation-audit.md)
("Why anisotropy is not an A31-only extension") and
[gl-feature-matrix.md](gl-feature-matrix.md) (candidate, cap no greater than 16).

Evidence labels:

| label | source |
|---|---|
| manual | `OGPM_Chrome9 HD DX9 R100 Part II 3D/Video`, printed page 130 (A31/A32), page for HParaType 03h/11h (4x4 coefficient table) |
| static | `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (i386, symbols); Windows `VTGUModeDX32A.dll` SHA-256 `8ac6959f…cee49b45`, `VTGOGL32A.dll` SHA-256 `a54e38ca…4d2c` |
| dynamic | `tools/re/chrome9_aniso_state_probe.c`: runs the real Linux `stmInitChipImage_inv`, `__glDpSTMUpdateTextureParameter` and `STM_FilterMode_INV` on a synthetic context (no DRM, no GPU) |
| inference | not observed; must be proven on silicon |

## 1. Summary

* Anisotropy is **A31 filter code 2 ("Linear Anisotropy") on all four S/T
  selectors plus the A31[20:16] ratio**.  Nothing else: no A30, A32 wrap,
  A33/A34, A35/A36, global packet, or coefficient table (dynamic, manual).
* The vendor GL word for maximum anisotropy *N* = 2..16 with any mipmapped
  minification filter is

  ```text
  A31 = 31004921 | N << 16      # S/T enlarge = S/T shrink = 2, D filter = 1
  A32 R filter fields [21:16] = 0
  ```

  i.e. `31024921`, `31044921`, `31084921`, `31104921` for 2, 4, 8, 16.  It is
  identical for `LINEAR_MIPMAP_LINEAR`, `LINEAR_MIPMAP_NEAREST`,
  `NEAREST_MIPMAP_*` and for mag `NEAREST` or `LINEAR` (dynamic).
* The D (mip) filter is **forced to 1 (nearest)** whenever code 2 is used with
  mipmaps, even for `*_MIPMAP_LINEAR`.  All three vendor builders do this
  (Linux `0x1b36cb..0x1b36d6`/`0x1b3892..0x1b389e`, Windows D3D
  `0x101d9489..0x101d949c`, Windows OGL `0x10241cb6`) (static, dynamic).
* With a **non-mipmapped** min filter (`LINEAR`/`NEAREST`), the vendor GL
  ignores anisotropy: filters stay ordinary and A31[20:16] becomes 0
  (dynamic).
* Private modes 4/5 are the **4x4 filter** (A31 code 3), not anisotropy.
  They are D3D-only quad filters; GL never produces them, and the
  `11030000` coefficient packet is emitted only when they are active
  (`ctx2+0x3c54`, static, dynamic).  **No coefficient table is needed.**
* The vendor GL advertises `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 16`
  (SI-core default constant, not overridden by the Chrome9 device
  constants) (static).
* Volume textures (depth > 1) never get code 2: the builder rewrites them to
  plain linear (static, dynamic).  The format denylist of the audit is gated by
  an application "game patch" bit and is **off** by default (static).

## 2. Register fields (manual, page 130)

```text
A31 (HParaType 02h, subtype 20h-2Fh, sub-address 31h)
  [20:16] maximum anisotropy ratio: 0 and 1 = no anisotropy, 2..16, others reserved
  [15:13] S enlarge   [12:10] S shrink   [9:7] T enlarge   [6:4] T shrink
          000 nearest, 001 linear, 010 linear anisotropy, 011 4x4 filter
          "S and T enlarge (resp. shrink) must be the same when linear
          anisotropy is set"
  [3:0]   D filter: 0 level 0 only, 1 nearest, 2 linear, 4 dither
A32 (sub-address 32h)
  [21:19] R enlarge, [18:16] R shrink: 000 nearest, 001 linear (3D volume only)
```

The 4x4-filter selector code 3 is what consumes the 32-entry coefficient
table (HParaType 03h, subtype 11h, `HTX4X4Flt*`), which is why modes 4/5 copy
it.  Code 2 has no table in the manual.

## 3. Vendor data flow (Linux GL)

Private per-stage state lives at `hw + stage*0xf4`:

| offset | meaning |
|---|---|
| `+0x2100` | mag mode: 0/1 point, 2 linear, 3 anisotropic, 4 Gaussian 4x4, 5 B-spline 4x4 |
| `+0x2104` | min mode, same enumeration |
| `+0x2108` | mip mode: 0 none, 1 point, 2 linear |
| `+0x2118` | maximum anisotropy (integer) |
| `+0x219c` | bound texture object (`+0x2c` private format, `+0x180` depth) |

The image stage record is `img + 0xf60 + stage*0xdc`, where `+0x0c..+0x24`
hold A30..A36 (dynamic).

### 3.1 `glTexParameterf(GL_TEXTURE_MAX_ANISOTROPY_EXT)`

`__glTexParameterfv:0x2bb498..0x2bb4fa` stores the float unchanged into the
texture parameters (`+0x5c`) and sets texture dirty bit `0x20000` only.  A
value whose 32-bit truncation is negative is re-converted to 64 bits and
compared unsigned against `ctx+0x1f4`, so negative input fails with
`GL_INVALID_ENUM`; any value from 0.0 up to the int32 range is stored without
the spec's `< 1.0` error and without clamping to 16 (static).  Min filter sets only bit `0x8`, mag filter
only `0x10` (`0x2bb2be`, `0x2bb38b`, static).

### 3.2 `__glDpSTMUpdateTextureParameter:0x1ef940`

Order inside one validation: wraps, min (`0x8`), mag (`0x10`), anisotropy
(`0x20000`).  The anisotropy block (`0x1efc6d..0x1efda1`, static):

1. `+0x2118 = trunc(max_anisotropy)` (x87 RC=truncate).
2. If mip mode is 0: if the float is > 1.0 then `+0x2118 = 0`; filters are
   untouched.
3. Otherwise the registry override (`osGetRegistryOptions` `+0xe4`, default
   `0x80000000` = application controlled) decides.  With the default: float
   > 1.0 sets min = mag = 3; float <= 1.0 re-derives min/mip from the GL min
   enum (`__glDpTexParameterMinFilter`) and mag from the GL mag enum.
   Override 0 forces the ordinary filters; override `1 << k` forces min = mag
   = 3 and ratio k+2.

### 3.3 `STM_FilterMode_INV:0x1b32d0`

1. A31[20:16] = `+0x2118 & 0x1f` (no clamp), A31[3:0] = mip mode, codes > 2
   folded to 2 (`0x1b3321..0x1b3385`).  `ctx2+0x3c54` (4x4 table dirty) = 0.
2. Mag switch (`0x3f2778`: 0,1 → point; 2 → `0x1b3800`; 3 → `0x1b3848`;
   4 → `0x1b38a8`; 5 → `0x1b37a0`) and min switch (`0x3f2760`: 0,1 → point;
   2 → `0x1b3630`; 3 → `0x1b3678`; 4 → `0x1b36e0`; 5 → `0x1b3740`).
   * mode 2: S/T code 1, R code 1.
   * mode 3: S/T code 2, R code 0, and if mip mode != 0 then D = 1.
   * mode 4: S/T code 3, R code 0, copy `INV_4X4FLTCOEF_GUASS`, set
     `0x3c54`.  Mode 5: same with `INV_4X4FLTCOEF_BSPLINE`.
3. If game-patch bit `mgr+0x278 & 4` is set and the private format is one of
   `101d 1028 102c 2032 2036 203c..2040`, all S/T/R codes become 0 (point).
   `mgr+0x278` comes from registry `+0xc8` (default 0) or the per-application
   table in `ctmInitGamePatch` (static).
4. If the texture's depth `+0x180 > 1` and mag or min is mode 3, S/T/R codes
   become 1 (linear); D and the ratio stay.  `+0x178/+0x17c/+0x180` are
   width/height/depth (`sfmUpdateResourceStructures:0x0d05c5..0x0d05dd`,
   static).
5. `0x3c54 != 0` marks the global table dirty (`+0x67c = ffffffff`);
   `stmFullUpdateHardware_invi:0x1781c4..0x17822f` emits
   `FE03043C 00000020 11030000 CC000000 <32 words>` only in that case.

## 4. Dynamic results

`chrome9_aniso_state_probe` stage words A30..A36 after a full validation of
min + mag + anisotropy (A32 wraps untouched because the wrap bits were not
dirtied).  `x` = any ratio input.

| GL min / mag | aniso | mag min mip ratio | A31 | A32 |
|---|---|---|---|---|
| any `*_MIPMAP_*` / LINEAR or NEAREST | 2 | 3 3 m 2 | `31024921` | `32000000` |
| same | 4 | 3 3 m 4 | `31044921` | `32000000` |
| same | 8 | 3 3 m 8 | `31084921` | `32000000` |
| same | 16 | 3 3 m 16 | `31104921` | `32000000` |
| same | 1.5 | 3 3 m 1 | `31014921` | `32000000` |
| same | 17 / 31 / 32 | ratio 17 / 31 / 32 | `31114921` / `311f4921` / `31004921` | `32000000` |
| `LINEAR_MIPMAP_LINEAR` / LINEAR | 1.0 | 2 2 2 1 | `31012492` | `32090000` |
| `LINEAR_MIPMAP_LINEAR` / LINEAR | 0.5 | 2 2 2 0 | `31002492` | `32090000` |
| `LINEAR` / LINEAR | 1.0 | 2 2 0 1 | `31012490` | `32090000` |
| `LINEAR` / LINEAR | 2..32 | 2 2 0 0 | `31002490` | `32090000` |
| `NEAREST` / NEAREST | 2..32 | 1 1 0 0 | `31000000` | `32000000` |
| 2D texture, `L_M_L`/LINEAR | 16 | 3 3 2 16 | `31104921` | `32000000` |
| volume depth 4, `L_M_L`/LINEAR | 16 | 3 3 2 16 | `31102491` | `32090000` |
| game patch bit 2, format `101d`/`2032`, `L_M_L` | 16 | 3 3 2 16 | `31100001` | `32000000` |
| registry override 0, `L_M_L`/LINEAR | 16 | 2 2 2 16 | `31102492` | `32090000` |
| registry override `1<<2`, `L_M_L`/LINEAR | 1 | 3 3 2 4 | `31044921` | `32000000` |
| private mode 4 / 5 (D3D only) | 16 | 4/5 4/5 2 16 | `31106db2` | `32000000` |

`m` is the GL mip mode (1 or 2); the result is the same.  Private modes
4 and 5 copy exactly the Gaussian and B-spline columns of
`sampler-4x4-coefficients.tsv` into `img+0x1fbc` and set the table-dirty
word; mode 3 never does (dynamic).

Vendor dirty-order quirk (dynamic, not to be copied): after an anisotropic
validation, a later min-filter-only change leaves mag = 3 and produces mixed
words such as `31084511` (S/T enlarge 2, shrink 1) until anisotropy is
dirtied again; re-dirtying anisotropy with a non-mip min filter even keeps
mag = 3 (`31004510`).  A Gallium driver must derive the whole A31 from one
sampler state.

The initial chip image has A31 = `31000000` (dynamic), and the GL default
anisotropy 1.0 yields ratio 1, so ordinary vendor GL words carry `3101xxxx`.
The current kernel's `3110xxxx` (ratio 16 without code 2) matches the vendor
with the registry override set to 0; the manual says the ratio is inert
without code 2 (inference until rung R1).

## 5. Maximum anisotropy and restrictions

* `__glDoGet(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT)` returns integer
  `ctx+0x1f4` (`0x2965a6`).  `__glCreateContext:0x281f98..0x281fb7` passes
  `ctx+0x1c0` to `__glInitConstantDefault`, which writes `+0x34 = 0x10`, then
  to the device hook `__glS3InvGetDeviceConstants`, which does not write
  `+0x34`.  Result: **16** (static).  The device constant `+0x94 = 16` of
  gl-feature-matrix.md is therefore not the anisotropy limit.  The D3D caps
  also report `MaxAnisotropy = 16` without volume anisotropy
  (ioquake3-blocker-roadmap-re.md).
* Ratios 17..31 are manual-reserved but emitted unclamped by the vendor for
  application values above 16; 32 wraps to 0.  Clamp to 16.
* Mipmaps required: anisotropy is dropped for non-mipmapped min filters.
* 3D volume textures: code 2 is replaced by linear.
* Formats: no format restriction in the default configuration.  The
  denylist (D16, L16, R16F, D24X8, V16U16, G16R16, G16R16F, R32F, D32, D32F)
  is only an application workaround, and it forces point sampling for those
  formats regardless of anisotropy.  Our kernel already restricts R32F to
  NEAREST; X8Z24 is a single-level depth format and has no mip chain.
* Cube maps and DXT textures: nothing in the builder excludes them (static);
  hardware behaviour is untested (inference).

## 6. Implementation contract

### 6.1 uAPI (Mesa `include/drm-uapi/chrome9_drm.h`)

No new struct member is needed.  `drm_chrome9_fragment_texture_state.flags`
carries the ratio:

```c
/* A31 linear anisotropy for mipmapped 2D/cube textures. */
#define DRM_CHROME9_CAP2_TEXTURE_ANISOTROPY	(1ULL << 39)
/* Maximum anisotropy 2..16 in flags[12:8]; 0 disables it. */
#define DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_ANISO_SHIFT	8
#define DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_ANISO_MASK	(0x1fU << 8)
```

### 6.2 Kernel (`chrome9_engine.c`)

`chrome9_validate_fragment_texture`: split the flag check so the TILED
requirements apply only to `FLAG_TILED`, and for a nonzero ratio require
`CAP2_TEXTURE_ANISOTROPY`, `2 <= ratio <= 16`, target 2D or CUBE,
`mip_filter != NONE`, and a filterable format (not R32F or X8Z24):

```c
u32 aniso = (texture->flags & DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_ANISO_MASK) >>
	    DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_ANISO_SHIFT;

if ((texture->flags & ~(DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_TILED |
			DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_ANISO_MASK)) ||
    ((texture->flags & DRM_CHROME9_FRAGMENT_TEXTURE_FLAG_TILED) &&
     (!(caps2 & DRM_CHROME9_CAP2_TILED_COLOR) || block_bytes ||
      texture->target != DRM_CHROME9_FRAGMENT_TARGET_2D)) ||
    (aniso && (!(caps2 & DRM_CHROME9_CAP2_TEXTURE_ANISOTROPY) ||
	       aniso < 2 || aniso > 16 ||
	       texture->target == DRM_CHROME9_FRAGMENT_TARGET_3D ||
	       texture->mip_filter == DRM_CHROME9_TEXTURE_MIP_FILTER_NONE ||
	       texture->format == DRM_CHROME9_FRAGMENT_TEXTURE_R32F ||
	       texture->format == DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24)))
	return CHROME9_EINVAL;
```

`chrome9_fragment_sampler_state`: replace A31 when the ratio is set; A30,
A32 (2D R clamp `0x40`, R filters 0) and A35/A36 stay as today:

```c
if (aniso)
	/* S/T enlarge/shrink code 2, D nearest (vendor, all builders). */
	sampler[1] = 0x31000000 | aniso << 16 | 2 << 13 | 2 << 10 |
		     2 << 7 | 2 << 4 | DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST;
```

Advertise `CAP2_TEXTURE_ANISOTROPY` only after rungs R1..R3 pass.

### 6.3 Mesa (`src/gallium/drivers/chrome9`)

* `chrome9_create_sampler_state`: remove `max_anisotropy > 1` from the
  unsupported list; store `MIN2(max_anisotropy, 16)` when it is >= 2 and
  `min_mip_filter != PIPE_TEX_MIPFILTER_NONE`, else 0 (the vendor ignores it
  in that case; GL leaves the anisotropic footprint implementation-defined).
  Gallium already truncates (`max_anisotropy:5`, 1.5 -> 1 = off).
* At emit time (view known) drop the ratio for 3D, R32F and depth-sampler
  views, and put it into `flags[12:8]`.
* Screen caps with the kernel bit: `anisotropic_filter = true`,
  `max_texture_anisotropy = 16.0f`, which exposes
  `GL_EXT_texture_filter_anisotropic` and `GL_ARB_texture_filter_anisotropic`.
* Mesa's `set_sampler_max_anisotropy` already rejects `< 1.0` and clamps to
  the cap, unlike the vendor.

## 7. Silicon rungs (single variable each)

Scene: one 256x256 mip-chain texture with a distinct solid colour per level
plus a fine checker in L0, on a floor quad receding at a grazing angle, at
1280x720, 1 draw, readback with `xset s off -dpms`.  Baseline B = current
`31102492` trilinear.  Each rung changes only the listed word.

| rung | change vs. previous accepted state | expected | hang risk |
|---|---|---|---|
| R1 | A31 ratio 16 -> 1 (`31012492`), no code 2 | bit-identical to B | very low (vendor default word) |
| R2 | A31 = `31024921` (ratio 2, vendor word) | far region sharper along the compressed axis, level colours shift toward finer levels vs. `31102491`, no mip blending | low-medium (new filter path; vendor emits exactly this) |
| R2a | control `31102491` (bilinear, D nearest) | reference for R2 with the same D mode | very low |
| R3 | ratio sweep 4, 8, 16 (`31044921`, `31084921`, `31104921`) | monotonic sharpening, finer levels chosen; time the draw | low-medium |
| R4 | two stages: stage 0 trilinear, stage 1 `31104921` | stage-independent result | low |
| R5 | DXT1 texture with `31084921` | same behaviour as ARGB | low-medium |
| R6 | cube map with `31084921` | seam-free, sharper at grazing faces | medium (untested on vendor path) |
| R7 | D filter 1 -> 2 under code 2 (`31104922`) | vendor never emits it; may be trilinear aniso or reserved | medium |
| R8 | ratio 1 and 0 with code 2 (`31014921` is the vendor word for 1.5, `31004921`) | expected equal to R2a if ratio <= 1 disables | low (1) / low-medium (0) |
| R9 | code 2 without mips (`31102490`, one level) | vendor never emits it | medium |
| R10 | fill-rate bench at ratio 16 vs. B | performance cost for the default cap | low |

Do not test on silicon: ratios 17..31 (reserved), code 2 on 3D volume
textures (the vendor deliberately avoids it), and asymmetric S/T codes
(manual forbids).  Promote Mesa exposure after R1..R3 (and R5 for ioquake3
DXT content); R7..R9 only refine the contract.

## 8. Reproduction (host only)

```sh
L=~/tmp/re-tcl/lib32            # i386 libexpat.so.1 + libdrm.so.2
gcc -m32 -O0 -g -o aniso tools/re/chrome9_aniso_state_probe.c -ldl
LD_LIBRARY_PATH=$L ./aniso 0x7122
```

Run from the repository root; the probe dlopens the vendor DSO and never
opens a DRM device.

## 9. Silicon results and implementation (2026-09-27)

Implemented as proposed in section 6 (kernel `cad21d4`, uAPI Mesa
`cde5120`, Mesa `53eda24`), with one change after rung R7: the kernel keeps
the draw's mip filter under code 2 instead of the vendor's nearest level
(module parameter `aniso_re` bit 0 restores the vendor word).
`tools/re/chrome9_gl_aniso_check.c` draws a floor receding to z = -60 over
a 256x256 chain with one colour per level at 512x256, ten times per ratio,
and counts the lower half's pixels per level colour
(`artifacts/2026-09-27/aniso-lml-*`, `aniso-r7-*`):

| ratio | D nearest (vendor, R2/R3): ms, level 1/2/3 pixels | D linear (R7): ms, level 1/2/3 pixels |
|---|---|---|
| 1 (plain trilinear `31102492`) | 0.90, 0 / 2824 / 11744 | same |
| 2 | 0.18, 4772 / 17852 / 12624 | 0.30, 3436 / 14976 / 8216 |
| 4 | 0.30, 21960 / 13288 / 7163 | 0.63, 20412 / 10944 / 4485 |
| 8 | 0.43, 25798 / 16193 / 3885 | 0.80, 23756 / 12491 / 1971 |
| 16 | 0.46, 25798 / 16646 / 4850 | 0.88, 23756 / 12972 / 3500 |

* Every ratio renders on the hardware (no llvmpipe draw), no timeout.
* Each higher ratio moves the bands to finer levels, as anisotropic
  filtering must: the ratio field works.
* With D nearest the level bands are hard edges; with D linear (R7, never
  written by the vendor) adjacent anisotropic levels blend smoothly, at
  about twice the cost at ratio 16.  R7 is the default: it is what GL
  expects for LINEAR_MIPMAP_LINEAR.
* The ratio-1 timing includes the first draw of the program.

Not run: R5 (DXT), R6 (cube), R9 (code 2 without mips; Mesa never sends
it: the descriptor drops the ratio for single-level views).  The piglit
build has no anisotropy tests.
