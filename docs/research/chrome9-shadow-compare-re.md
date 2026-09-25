<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 depth-compare (shadow) texture sampling

Question: does the VX900 texture unit implement depth comparison (ARB_shadow,
`shadow2D`, D3D9 "hardware shadow maps", PCF), or does the vendor emulate it?

Binaries (static analysis only, no hardware access):

| binary | path |
|---|---|
| Linux 5.75 DRI | `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (i386, symbols) |
| Windows OGL ICD | `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGOGL32A.dll` |
| Windows D3D UMD | `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll` |

## Summary

* **The vendor OpenGL drivers emulate the compare in the pixel shader.**
  Compare mode and function go into the fixed-function pixel-shader key. The
  generator adds D3D-token ALU code after `TEXLD` that compares the raw
  sampled depth with the texture-coordinate `r`/`q`. static-high
* **No sampler compare field was found.** The Linux sampler path always
  selects Z mode 4 (raw depth copied to all four channels) for D24X8/D24S8.
  Nothing in the texture-register builders reads compare mode or function.
  static-high for Linux. The Windows Z mode 1 is still unexplained (see
  below). static-medium
* **No PCF.** Both proprietary texture builders force point sampling for
  D24X8. The Windows D3D format table marks D24S8/D24X8 textures as
  `D3DFORMAT_OP_NOFILTER`. static-high
* Exposed APIs: `GL_ARB_shadow`, `GL_ARB_shadow_ambient`, `GL_EXT_shadow_funcs`
  (fixed function only). `GL_ARB_fragment_program_shadow` and all GLSL
  extensions exist as strings but are not enabled (`gl-feature-matrix.md`).
  static-high

## 1. Linux GL: state key

`__glDpFFPSUpdateTexShadowMapStateKey` (`via_chrome9_dri.so:0x204550`) is
called from `__glS3InvValidateAttribute` (`0x2359ab`). The key has one
12-byte record per unit at `ctx+0x18d0+12*unit`:

* The texture object's compare mode is at `+0x68`. If it equals
  `GL_COMPARE_R_TO_TEXTURE` (`0x884e`, `0x2045d0`), the code sets record byte
  `+0x12` bit 7 (`0x2045db`). Otherwise it clears that bit (`0x204587`).
* The compare function is at `+0x6c`. The code subtracts `0x200` and uses an
  8-entry jump table (`0x3f8730`, dispatch at `0x2045c0`). It writes the result
  to record byte `+0x13` bits `[2:0]`, which equal `func - GL_NEVER`:
  NEVER=0, LESS=1, EQUAL=2, LEQUAL=3, GREATER=4, NOTEQUAL=5, GEQUAL=6,
  ALWAYS=7 (`0x2046a8`, `0x204688`, …, `0x2046b8`).
* The code sets `ctx+0x1e88 = 1`, which marks the key dirty. The whole key
  (`0x68` bytes) is hashed and compared with `memcmp` in
  `__glFFPSSearchInstance` (`0x2049b0`). Each compare function therefore
  gets its own cached pixel shader.

The key does not use `TEXTURE_COMPARE_FAIL_VALUE_ARB` (`0x80bf`). That value
is only validated in the TexParameter entry points (`0x2bb66d` etc.).

The Windows ICD contains the same function at `VTGOGL32A.dll:0x1004f400`
(`cmp ecx,0x884e` at `0x1004f428`). Its record layout differs (`+0x18ec`,
bit `0x800000`).

## 2. Linux GL: generated compare code

`__glDpGenerateTexStageBlendMSToken` (`0x206460`) first emits `dcl t_n`,
`dcl_2d s_n` and `texld r_n, t_n, s_n` (`0x3000042`). If key field
`0x18d8` selects projection, it emits `texldp` (`0x3010042`) instead
(`0x206628`). For each unit whose key byte `+0x12` bit 7 is set (`0x20675a`),
it then dispatches through an 8-entry table at `0x3f8768` (`0x20676b`).

Decoded D3D9 tokens (MUL=5, SUB=3, MOV=1, CMP=0x58). `r_n.x` is the raw depth
sample, `t_n.z` is the reference `r` and `t_n.w` is `q`:

| func | address | sequence (`CMP d,a,b,c` = `a>=0 ? b : c`) |
|---|---|---|
| NEVER | `0x207dfe` | `mov r_n, c0.z` |
| LESS | `0x207e1f` | `r9 = t.z - r_n.x*t.w`; `cmp r_n, r9, c0.z, c0.y` |
| EQUAL | `0x207e6a` | `r9 = -(r_n.x*t.w - t.z)^2`; `cmp r_n, r9, c0.y, c0.z` |
| LEQUAL | `0x207d07` | `r9 = r_n.x*t.w - t.z`; `cmp r_n, r9, c0.y, c0.z` |
| GREATER | `0x207d52` | `r9 = r_n.x*t.w - t.z`; `cmp r_n, r9, c0.z, c0.y` |
| NOTEQUAL | `0x207c48` | `r9 = -(r_n.x*t.w - t.z)^2`; `cmp r_n, r9, c0.z, c0.y` |
| GEQUAL | `0x206776` | `r9 = t.z - r_n.x*t.w`; `cmp r_n, r9, c0.y, c0.z` |
| ALWAYS | `0x207ddd` | `mov r_n, c0.y` |

`__glDpFFPSUpdateConstants` (`0x204d19..0x204d2d`) sets
`c0 = (0.5, 1.0, 0.0, 4.0)`, so pass is 1.0 and fail is 0.0. The ambient
(fail) value is always 0.0. Every case writes all four channels (`.xyzw`).

Notes:

* The comparison is `D*q` against `r`. This avoids a divide and is correct
  for both `texld` and `texldp`, because the sampler's projection affects
  only `s`/`t`. static-high
* The ALU operands are marked `_pp` (dst modifier `0x2`). Whether the Chrome9
  back end honors that marking is unknown.
* Only the fixed-function pixel-shader generator does this. The ARB fragment
  program sampling update (`__glDpSTMPPSUpdateSamplingMode`, `0x1ed2c0`)
  does not look at compare state, which matches the missing
  `ARB_fragment_program_shadow`. static-high

## 3. Sampler hardware: A30 Z mode

* Linux `INV_D3DFmt_DDPFToDepthTX` (`0x1a7030`) returns Z mode 4 for
  `2032/2033` with no condition (`fixed-function-texture-sync.md`). The
  generated shader relies on this mode, because it reads the raw depth from
  `r_n.x`. static-high
* The Windows helpers `VTGUModeDX32A:0x1020a960` and `VTGOGL32A:0x1025d890`
  (byte-identical) return 4 if `(*(res+0x2f4))+0x1c == 8`, and 1 otherwise
  (`sub 8; neg; sbb; and ~2; add 4`). The shared caller places the result in
  A30 `[13:11]` (`0x101d7f57..0x101d7f7d` and `0x10240a87..0x10240aad`).
  No compare function or reference value is involved. The object at `+0x2f4`
  could not be identified within the time budget: nearby `+0x2f4` stores in
  both DLLs belong to a different device structure. static-medium
* Mode 1 (`001`) is not one of the manual's routing encodings
  (`000/010/100/110`). One possibility is an undocumented fixed LEQUAL
  hardware compare against texcoord z, as D3D9 shadow maps require. The
  opposite is equally possible. If mode 1 did compare in hardware, the
  Windows OGL shader emulation would compare an already-compared value. This
  hypothesis is therefore **unproven**. static-low
* A search of all Linux sampler/filter code for compare state found only the
  key function above.
* Filtering: Linux `STM_FilterMode_INV` (`0x1b3418..0x1b3448`) clears all
  A31 min/mag/mip filter selectors for `0x2032` (D24X8; also `0x2036` and
  `0x203c..0x2040`). Windows does the same at `VTGUModeDX32A:0x101d9709..
  0x101d9747`. Linear filtering followed by a compare, or PCF, is never
  submitted. On Linux, `0x2033` (D24S8) is not in that list. Whether this
  omission is significant is unknown.

## 4. D3D9 format capabilities

The `DDPIXELFORMAT` format-operation tables (`dwFlags=DDPF_D3DFORMAT`,
operations in the dword at `+0x10`) are chosen at `0x100c7227..0x100c7307`
and `0x100e87f3..0x100e8822`. The chip branch at `0x100c7231` compares the
PCI device ID with `0x7122` (VX900). When the caps call's third argument is
non-zero, it selects the tables at `0x10311770`, `0x10311d10` and
`0x10314b28`. Otherwise it selects the tables at `0x10310cb0`, `0x10311210`
and `0x103145c8`. The meaning of that argument is unresolved.

| format | ops (non-zero argument) | ops (zero argument) |
|---|---|---|
| D24S8 (75), D24X8 (77) | `0x2440c1` = TEXTURE, ZSTENCIL, ZSTENCIL_ANY_COLOR, OFFSCREENPLAIN, NOFILTER, NOALPHABLEND | `0x2440c0` (no TEXTURE) |
| D16 (80), D16_LOCKABLE (70), D32 (71), D32F_LOCKABLE (82) | `0x2440c0` (never a texture) | `0x2440c0` |

Example entries: D24S8 at file offset `0x310d50` (VA `0x10311b50`) and
`0x314108` (VA `0x10314f08`). So D24 depth textures are exposed but not
filterable. static-high

The D3D UMD has no `TEXLD`+`CMP` compare expansion (no `0xb0aa0000` or
`0x4000058` immediates). In `VTGOGL32A.dll` the expansion is at
`0x1004fe59..0x10050211`. On D3D, a sampled D24 texture therefore returns
either raw depth (mode 4) or whatever mode 1 does. NVIDIA-style
compare-and-PCF behavior is not established. static-medium

## 5. Recommended Mesa strategy

1. Emulate the compare in the shader and keep the sampler in raw Z mode 4
   (`A30=30fe2000`) with NEAREST. Use `nir_lower_tex_shadow()`
   (`src/compiler/nir/nir_lower_tex_shadow.c`, used by etnaviv, r300 and
   d3d12). Pass per-sampler `compare_func` and swizzle arrays from a
   fragment-shader variant key; the vendor also keys on these. Lower `txp`
   first, or use the vendor's divide-free form `D*q` compared with `r`.
   `is_fixed_point_format=true` clamps the reference as the specification
   requires for D24.
2. Then allow `compare_mode != NONE` in `chrome9_create_sampler_state`
   (`chrome9_context.c:481`), record the function in the key, and set
   `caps->texture_shadow_map = true` (`chrome9_screen.c:456`). This also
   unlocks GL 1.4 (`mesa-capability-audit.md`).
3. With LINEAR filtering requested, fall back to one nearest sample, as the
   vendor does. Optional later: build 2x2 PCF in the shader (four `txf`/
   offset fetches, four compares, a `lerp` with `fract(uv*size-0.5)`). This
   costs four texture instructions per lookup and is a performance question,
   not a correctness one.
4. Do not submit A30 Z modes other than 4 until the probe below is done.

## 6. Hardware probe for Z mode 1 (optional)

Use the existing bounded D24 oracle (`fixed-function-texture-sync.md`): a
32x32 D24X8 surface with quadrant depths `000000/555555/aaaaaa/ffffff`,
`A30=30fe0800` (Z mode 1), NEAREST, and a pixel shader that outputs the
`texld` result. Provide texcoord `r`/z as a per-vertex ramp or a constant
of 0.5, and w=1.

| observation | interpretation |
|---|---|
| output equals the mode-4 result `(D,D,D,D)` for all r | mode 1 is a routing variant and does not compare |
| output is 0/1 (0x00/0xff) and switches where r crosses the quadrant depth | hardware compare. Find the function by the `r==D` edge (LEQUAL expected for D3D) |
| r=0.5 gives `0,0,1,1` over the quadrants (D = 0, 1/3, 2/3, 1) | consistent with `r <= D` |
| hang or garbage | reject mode 1 permanently |

Run the mode-4 control first in the same submission. If the probe shows a
compare, repeat it with LINEAR in A31 to test for hardware PCF (fractional
outputs at the quadrant edges). Both vendor stacks clear that filter for D24,
so fractional output is not expected.

## 7. Mesa implementation and silicon results (2026-09-25)

Implemented as recommended (Mesa chrome9 `0c2c83d`, `d00e5a2`):

* `chrome9_create_sampler_state` accepts `PIPE_TEX_COMPARE_R_TO_TEXTURE`;
  `caps.texture_shadow_map` is on, which also exposes
  ARB_fragment_program_shadow.
* Every FS with a shadow TEX (`chrome9_shader.shadow_sampler_mask`) is
  compiled per variant; the key holds, per shadow sampler, the compare
  function and the view swizzle (`chrome9_shadow_key`).  The variant runs
  `chrome9_nir_lower_shadow`: project the lookup, clamp the reference,
  scale it, then `nir_lower_tex_shadow` (which applies the swizzle, so the
  draw skips its depth-view swizzle rewrite for those units).
* **Sampled D24 normalization (measured):** the sampler returns a D24 texel
  as `raw / 2^24`, not GL's `raw / (2^24 - 1)`.  Evidence: with
  `chrome9_gl_depth_value_smoke`, `0x400000` samples below
  `0x400000 / 0xffffff`, and in `glsl-fs-shadow2D-clamp-z` the texel
  `0xffffff` compares GEQUAL/GREATER but not EQUAL/LEQUAL against a
  reference of 1.0 (the shader reads `d * 31 - 30` as just below 1).  The
  lowering therefore compares against `fsat(ref) * 16777215 / 16777216`,
  which puts the reference on the sampler grid; `0xffffff` then equals a
  clamped reference of 1.0 exactly.
* LINEAR compare: the texture state forces NEAREST for X8Z24, the vendor
  behaviour (single sample, binary result; GL leaves the LINEAR result
  implementation-dependent).  In-shader 2x2 PCF is not implemented.
  Non-compare X8Z24 with LINEAR goes to llvmpipe.
* X8Z24 is also allowed for rectangle textures (st lowers RECT to
  normalized coordinates); D16/D32 remain unsampled.
* The Z mode 1 probe of section 6 was not run.

piglit on VX900 (kernel `3b52a780`, Mesa tag `tcl-20260925/s4`), before
(`p13`) -> after:

| group | before | after |
|---|---|---|
| glsl-1.10 `glsl-fs-shadow1d*` (13) | 2 pass | 13 pass |
| glsl-1.10 `glsl-fs-shadow2d*` (16) | 0 pass | 16 pass |
| arb_texture_rectangle `glsl-fs-shadow2drect*` (11) | 2 pass | 11 pass |
| arb_fragment_program_shadow (8) | skip | 8 pass |
| `depth-tex-compare`, `glsl-fs-loop-shadow-variables`, `depth-tex-modes-glsl` | fail/skip | pass |
| `arb_depth_texture@depth-tex-modes` | fail | fail (every draw of the test gets EINVAL from the kernel before any texture state; not sampler related) |
| `tex-miplevel-selection *shadow` | skip | skip (needs ARB_framebuffer_object) |
