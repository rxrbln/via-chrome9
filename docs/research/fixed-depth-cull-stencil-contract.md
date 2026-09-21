# Chrome9 fixed depth, cull, stencil, and alpha-test contract

Status: packet fields are cross-checked against the VX900 manual, the
proprietary Linux builder, both Windows UMDs, and the old UniChrome Mesa
policy.  Only the D16 LESS/write path has been measured on VX900 unless a
linked live note says otherwise.  The other combinations below are bounded
implementation oracles, not yet advertised capabilities.

This note narrows the state which a structured draw ABI should expose.  The
kernel must receive logical, validated fields and synthesize all native words;
userspace must never submit A01, A13, A23--A26, or the type-04 A23 word.

## Independent executable evidence

The register descriptions are in the *VX900 Series Chrome9 HD Open Graphics
Programming Manual*, preliminary revision 1.0, 2011-01-19, PDF pages 91,
93, 96--98, and 154.  They establish:

- type-01 A01 bits 7/4/3/2 as cull, stencil, depth-test, and depth-write
  enables;
- A13 `[18:16]` as the eight ordered depth functions;
- A23/A24 as the physical clockwise stencil face and A25/A26 as the physical
  counter-clockwise face;
- type-04 A23 bit 8 as the culled physical winding and bit 7 as the VS
  back-face-color selector, not another cull enable.

Three independently compiled driver families reproduce those fields:

- Linux `via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`:
  `STM_ZFunc_INV:0x192cf0`, `STM_Stencil_INV:0x1955e0`,
  `STM_ZWBEnable_INV:0x195b90`, and `STM_UpdateCull_INV:0x196a70`;
- Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`:
  depth enable `0x101fa910`, depth function `0x101faa10`, cull
  `0x101fabd0`, and stencil `0x101fc060`;
- Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`:
  cull `0x1024e890`, depth function `0x1024e6d0`, and stencil
  `0x1024fa10`.

The D3D and OpenGL cull routines are instruction-identical modulo the absolute
address of one orientation lookup table.  Their 0x118-byte bodies differ only
at byte offsets 112--114, the relocated table VA.  At
`0x101fac64-0x101facbd` and `0x1024e924-0x1024e97d`, public modes
`NONE/CW/CCW = 1/2/3` respectively clear A01 bit 7, set A01 bit 7 while
clearing type-04 A23 bit 8, and set both bits.  At
`0x101fac44-0x101fac5e` and `0x1024e904-0x1024e91e` they separately replace
type-04 A23 bit 7 from the front-face state.  Linux
`STM_UpdateCull_INV:0x196ae6-0x196bac` performs the same two independent
updates.

The old Mesa 7.11.2 UniChrome path is not Chrome9 encoding evidence, but it is
a useful API-policy check.  `via_state.c:1274-1295` clears depth-write when
depth test is disabled, and `via_state.c:1429-1444` resolves GL front/back
culling to a physical winding before emitting hardware state.

## Depth state

For a linear 32-pixel-wide D16 surface, the closed block is:

```text
A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11004001
A12 = 12380000
A13 = 13000000 | (compare << 16)
A1E = 1e000000
A1F = 1f000000
A20 = 20007fff
```

`compare` is exactly `NEVER..ALWAYS = 0..7`, identical to Gallium
`PIPE_FUNC_*`.  Linux `STM_ZFunc_INV:0x192d0a-0x192d25` subtracts one from
the GL/D3D-style `1..8` state and replaces the low three bits of image byte
`+0x4e`, which is A13 `[18:16]`.  Windows
`0x101faa25-0x101faa3f` performs the same subtract, shift by 16, and mask
`0x00070000`.  Thus the complete words are:

```text
NEVER    13000000        GREATER   13040000
LESS     13010000        NOTEQUAL  13050000
EQUAL    13020000        GEQUAL    13060000
LEQUAL   13030000        ALWAYS    13070000
```

The test and write gates are independent native bits, but API semantics impose
one additional rule:

```text
depth disabled:          A01 = 01c01540
depth read-only:         A01 = 01c01548
depth test and write:    A01 = 01c0154c
```

Windows `0x101fa946-0x101fa96c` sets bit 3 for a valid enabled depth binding,
then conditionally sets or clears bit 2.  Linux
`STM_ZWBEnable_INV:0x195e40-0x195e7f` does the same.  Both clear the pair when
depth is unavailable or disabled.  Therefore effective depth write is
`depth_test_enable && api_depth_writemask`; never leave A01 bit 2 set while
bit 3 is clear.

### Backward-compatible structured extension

The current D16 ABI reserves depth-function field zero to mean its historical
implicit LESS.  Hardware and Gallium both use zero for NEVER.  Consequently
the existing field cannot truthfully expose all eight functions without a
presence bit.  A compatible extension should retain:

```text
depth_func field == 0 and EXPLICIT_DEPTH_FUNC clear -> legacy LESS
EXPLICIT_DEPTH_FUNC set -> validate depth_func <= 7, including NEVER == 0
DEPTH_READ_ONLY set -> clear A01 bit 2
DEPTH_READ_ONLY clear -> preserve legacy write-enabled behavior
```

Both additions require capability bits before userspace sets them.  A future
state-complete successor can instead carry explicit booleans
`depth_test_enable`, `depth_write_enable`, and `depth_func`, avoiding the
legacy alias.  The kernel must require a retained depth BO whenever either
test or write is effective and must emit A10--A13 plus the wide A1E--A20 clip
tuple on every transition.  A disabled block should still close A13 to a
canonical value; LESS preserves the current packet and is the least novel
choice.

## Culling and front-face resolution

There are only three directly representable hardware states:

```text
none:       A01[7]=0                       A23[8] inert
cull CW:    A01[7]=1, type-04 A23[8]=0
cull CCW:   A01[7]=1, type-04 A23[8]=1
```

From the current baseline they are packet-exact:

```text
none:       A01=01c01540  type-04 A23=23000f02
cull CW:    A01=01c015c0  type-04 A23=23000e02
cull CCW:   A01=01c015c0  type-04 A23=23000f02
```

The kernel-facing field should therefore be a normalized physical
`NONE/CW/CCW` enum.  Mesa must resolve `pipe_rasterizer_state::{cull_face,
front_ccw}` after all framebuffer, viewport-Y, and primitive-orientation
inversions.  `PIPE_FACE_FRONT_AND_BACK` has no single native encoding and
should suppress the draw before submission.  Passing the API's raw
`front_ccw` bit would be wrong whenever orientation is inverted.

Always preserve type-04 A23 bit 7 from the established vertex-program linkage.
The cull ABI must not expose it.  Close both A01 bit 7 and type-04 A23 bit 8 on
every cull transition, including `enabled -> disabled -> opposite winding`.

## Stencil state

Stencil is valid only with retained D24S8 state (`A12=123e0000` for the
linear bounded oracle).  A01 bit 4 is enabled only when such a binding exists.
The physical face words are:

```text
A23 = 23000000 | (cw.ref << 16) | (cw.valuemask << 8) | cw.writemask
A24 = 24000000 | (cw.func << 16) |
                  (cw.fail << 6) | (cw.zfail << 3) | cw.zpass
A25 = 25000000 | (ccw.ref << 16) | (ccw.valuemask << 8) | ccw.writemask
A26 = 26000000 | (ccw.func << 16) |
                  (ccw.fail << 6) | (ccw.zfail << 3) | ccw.zpass
```

Functions are the same `NEVER..ALWAYS = 0..7` ordering as depth.  Native
operation values are:

```text
KEEP=0 ZERO=1 REPLACE=2 INCRSAT=3 DECRSAT=4 INVERT=5 INCR_WRAP=6 DECR_WRAP=7
```

Windows `0x101fc0b6-0x101fc1e6` subtracts one from each D3D `1..8` function
or operation and packs those exact fields.  Linux
`STM_Stencil_INV:0x195750-0x19583e` independently does the same.  Windows
also proves the resource gate: `0x101fc08b-0x101fc0b0` requires private
format `0x2033` plus stencil enable, otherwise
`0x101fc343` clears A01 bit 4.

Gallium compare functions can be copied after range validation, but Gallium
stencil-operation values cannot.  The mandatory translation is:

| Gallium operation | Gallium | hardware |
|---|---:|---:|
| KEEP | 0 | 0 |
| ZERO | 1 | 1 |
| REPLACE | 2 | 2 |
| INCR, saturating | 3 | 3 |
| DECR, saturating | 4 | 4 |
| INCR_WRAP | 5 | 6 |
| DECR_WRAP | 6 | 7 |
| INVERT | 7 | 5 |

Use the explicit lookup `{0,1,2,3,4,6,7,5}` and reject every out-of-range
value.  A direct three-bit copy would silently exchange INVERT with the wrap
operations.

Gallium DSA stores API front/back functions and masks, while
`pipe_stencil_ref` supplies two dynamic reference bytes.  Mesa must combine
them at draw time and resolve API front/back to physical screen-space CW/CCW,
using the same orientation decision as culling.  The structured kernel ABI
should carry already resolved `cw` and `ccw` blocks; it should not guess GL
front face.  With two-sided stencil disabled, mirror the resolved active
block into both pairs.  The proprietary builders do exactly this before their
orientation-specific exchange paths.

When stencil is disabled, clear A01 bit 4 and still close all four words to:

```text
A23=2300ffff  A24=24070000  A25=2500ffff  A26=26070000
```

This is reference zero, full compare/write masks, ALWAYS, and
KEEP/KEEP/KEEP.  Reject stencil with D16; do not reinterpret low depth bits as
stencil.

## Alpha test remains fixed A33

Compatibility alpha test is not part of the fragment shader key.  Keep it in
the existing fixed-function block:

```text
A00 bit 20 = enable
A33 = 33000000 | (func << 12) | encode_ref(ref8)
encode_ref(255) = 0400
encode_ref(v) = (v << 2) | (v >> 6), v < 255
```

`func` is again `NEVER..ALWAYS = 0..7`.  Windows D3D
`0x101facf0` and OpenGL `0x1024edc0`, plus Linux
`STM_AlphaTest_INV:0x194c40`/`STM_AlphaFunc_INV:0x194ad0`, independently
produce this state.  It consumes final fragment alpha and has already passed
solid, sampled-alpha, and D16 ordering tests.  Do not lower compatibility
alpha test to TEXKILL: the recovered kill sequence implements only a
non-strict signed threshold and has not proved suppression of depth/stencil
writes.  Every transition must close both A00 bit 20 and A33; stale A33 is
inert only while the enable is definitely clear.

## Bounded promotion order

1. Extend the existing D16 oracle with LEQUAL `13030000`, then the complete
   compare matrix, resetting color and depth before every row.
2. Prove read-only depth with LESS/read-only followed by EQUAL/read-only and
   verify the depth BO remains byte-identical.
3. Test cull CW and CCW with reversed vertex order, then the closure sequence
   `CW -> none -> CCW`, keeping type-04 A23 bit 7 fixed.
4. Add D24S8 with stencil ALWAYS/REPLACE and distinct physical-winding refs;
   only then enable the remaining compare and operation matrix.
5. Run `depth+stencil -> disabled neutral block -> depth+stencil` and
   `alpha A33 -> disabled -> alpha A33` closure tests before exposing the
   corresponding Gallium capabilities.

All tests retain guard pages, bounded retirement, timeout quarantine, and
post-retirement color/depth/stencil readback.  None should be combined with a
new render-target format, sampler mode, or shader object.
