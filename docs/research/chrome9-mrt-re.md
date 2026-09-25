<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 HD multiple render targets: register, shader and vendor recovery

This note collects everything needed to program 2..4 colour render targets
(GL draw buffers, `gl_FragData[1..3]`, D3D9 MRT) on the VX900 Chrome9 HD.  It
extends [chrome9-mrt-output-audit.md](chrome9-mrt-output-audit.md) (compiler
records) and the RT rows of
[chrome9-local-fb-placement.md](chrome9-local-fb-placement.md).  Nothing here
has run on silicon.  MRT stays rejected until the ladder in section 5 passes.

Evidence labels: **M** VX900 Part II manual (`~/tmp/ogpm3d.txt`, page
numbers are the printed page footers); **S** static disassembly of vendor
binaries, **L** = `via_chrome9_dri.so` 5.75 (SHA-256 `fddb4dea85319c76...`),
**W** = `VTGUModeDX32A.dll` (SHA-256 `8ac6959fc3ab21b7...`); **C** existing
production-compiler corpus; **I** inference, not verified.

## 1. Summary

- RT0..RT3 each have four type-01 words: base, mode/location, format/pitch,
  and write mask/ROP/dither/sRGB.  RTn uses sub-addresses `0x50+8n..0x53+8n`
  (**M** pp.106--115; **S** L and W).
- The active RT count is A01 `[16:14]` = count-1 (**M** p.91).  Both vendor
  stacks set it only from the pixel shader's colour-output count (highest oCn
  index plus one), never from the number of bound surfaces (**S**).
- Blend and alpha-test enables, dither, fog and specular are per RT in A00
  (**M** p.90).  Blend equations/factors/constants (A34..A3F) and the
  alpha-test function/reference (A33) are shared by all RTs (**M** pp.100--106).
  Channel write masks are per RT in A53/A5B/A63/A6B (**M**, **S**).
- The pixel shader writes oCn with native destination word 3 =
  `000d0820 | n<<6` (**C**).  No PS control word (A90..A9b) carries the
  output count (**C**).
- The largest risk: no fragment program with more than one output record has
  ever completed on this silicon.  Partial oC0 stores and oC0+oDepth both
  timed out (section 4).

## 2. Render target registers (question 1)

### 2.1 Per-RT words

| RT | base | mode/location | format/pitch | mask/ROP/dither | manual |
|---|---|---|---|---|---|
| 0 | A50 | A51 | A52 | A53 | pp.106--110 |
| 1 | A58 | A59 | A5A | A5B | pp.111--112 |
| 2 | A60 | A61 | A62 | A63 | pp.112--114 |
| 3 | A68 | A69 | A6A | A6B | pp.114--115 |

Sub-addresses `54-57`, `5C-5F`, `64-67`, `6C-6F` are reserved (**M**).

Field layout.  The manual gives the same layout for every RT, except for the
location field, see below.

| word | bits | field | evidence |
|---|---|---|---|
| Setting 1 (A50/58/60/68) | `[23:0]` | base address in 256-byte units | **M** |
| Setting 2 (A51/59/61/69) | `23` | 0 linear, 1 tiled | **M** |
| | `22` | tile 16 pixels high (`HMRTnTileH16`) | **M** |
| | `[21:20]` | location: `00` SL, `01` SF (GART), `10`/`11` reserved | **M** for RT0 only; **S** for RT1..3 |
| Setting 3 (A52/5A/62/6A) | `[23:16]` | format, same table as RT0 (p.107--108) | **M** |
| | `15` | colour extension mode | **M** |
| | `12` | 1 = do not clamp oCn to the format range | **M** |
| | `[9:0]` | pitch: 32-byte units when linear, tiles when tiled | **M** |
| Setting 4 (A53/5B/63/6B) | `23`, `[22:21]`, `20`, `[19:18]`, `[16:15]` | dither Y/X invert/bias, dither mode | **M** |
| | `[11:8]` | ROP, `1100` = COPY_PEN | **M** |
| | `6`, `[5:4]` | read de-gamma, write sRGB | **M** |
| | `[3:0]` | channel write mask A/R/G/B (bit 3..0) | **M** |

For RT1..3 the manual labels Setting 2 `[21:0]` reserved, but both vendor
builders write `[21:20]` for all four slots with the same helper.  **W**
`0x101fa28c..0x101fa2a0` (RT1), `0x101fa32c..` (RT2), `0x101fa3cc..` (RT3),
helper `0x101c9bb0`, VX900 branch `device+68 & 0x4000`.  **L**
`STM_RenderTarget_INV` RT1 `0x1969e6..0x1969f8`, RT2 `0x196952..0x196964`,
RT3 `0x196899..0x1968ab`: memory type 4 (GART) sets bit 20.  This matches
[render-target-format-oracles.md](render-target-format-oracles.md).
**I** A GART RTn with `[21:20]=00` would be read as an SL offset and would
write into the SL carve-out.  The kernel must set `01` for SF buffers exactly
as for RT0 (`51100000`).

Vendor construction per slot, identical for n = 0..3 (**S**):

- base: `surface >> 8` into `[23:0]`, top byte preserved (W `0x101fa22a..`,
  L `0x196970..0x19698a` for RT1);
- tiled bit 23 from surface flag bit 1 (W `+2c`, L `+20`);
- pitch `(pitch & 0x3fff) >> 5`, masked to 10 bits;
- format byte from the same RT format lookup for every slot (W `0x1020a4c0`,
  L `stmLookupRenderTargetFormat_invi` `0x1a6c20`);
- bit 22 (H16) is not written by the builder.

The builders dirty exactly three words per slot, `0x50+8n..0x52+8n` (**S** L
`0x196088..0x196141`, W `0x101fa403..0x101fa453`).  The slot index is a
call argument; the context slot array is L `ctx+0x3aac+4n`
(`stmSetRenderTarget` `0xee970`) and W `ctx+0x3da0+4n` (`0x100ff7f0`).

Proposed kernel words for a linear SF BGRA8888 RTn (**I**, derived from the
proven RT0 words `51100000`, `52990000|pitch>>5`, `53010c0f`):

```
A58 = 58000000 | bo_offset >> 8
A59 = 59100000
A5A = 5a990000 | pitch >> 5
A5B = 5b010c0f            # RGBA write, COPY_PEN, same dither mode as A53
```

The same pattern applies to `60..63` and `68..6b`.

### 2.2 Active-target count

A01 `[16:14]` "Number of Render Target": `000` RT0 only, `001` RT0+RT1, `010`
RT0..RT2, `011` RT0..RT3, `1xx` reserved (**M** p.91).  This is the only
count or enable field found.  There is no per-RT enable bit in the RT words
themselves (**M**).

Both vendor stacks derive it from the pixel shader, not from the bound
surfaces:

- **L** `stmUpdatePS_inv` `0x17f864..0x17f908`: `(ps+0x30 - 1) & 7` into image
  A01 (`image+4`) under mask `fffe3fff`; the fixed-function path writes 0
  (`0x17f8d7`).
- **W** `0x1015b1b2..0x1015b22f`: `ps+0x30 - 1`, shifted by 14 and merged into
  A01; the no-shader path uses 0 (`0x1015b202`).  W `0x10158d1c` clears the
  field on another path.

Per the corpus, `ps+0x30` is the highest written oCn index plus one: an oC2-only
store gives 3 (**C**,
[chrome9-mrt-output-corpus.tsv](chrome9-mrt-output-corpus.tsv)).  **S** Neither
vendor clears or re-points a slot when its surface is unbound: L
`STM_RenderTarget_INV` returns early for a NULL slot (`0x195ed8`), and W
`0x101f9fcb` does the same.  **I** The hardware will therefore write every
slot `< count` using whatever A58.. values are latched, stale or not.  The
kernel must emit and validate all slots below the count in every job and must
reset A01 `[16:14]` to 0 for single-target draws.  The current kernel emits
`A01=01ce1540`, which has count 0.

### 2.3 Blend, alpha test, write masks: shared or per RT

| state | scope | words | evidence |
|---|---|---|---|
| dither enable | per RT | A00 `[3:0]` (bit n = RTn) | **M** p.90 |
| alpha blend enable | per RT | A00 `[7:4]` | **M** p.90 |
| fog / specular enable | per RT | A00 `[11:8]` / `[15:12]` | **M** p.90 |
| alpha test enable | per RT | A00 `[19:16]`, bit 20 = use RT0's result for all | **M** p.90 |
| alpha test function/reference | shared | A33 | **M** p.100 |
| blend equation, factors, operands | shared | A34..A37 | **M** pp.101--104 |
| blend constants | shared | A38..A3F | **M** pp.104--106 |
| channel write mask, ROP, dither mode, sRGB | per RT | A53/A5B/A63/A6B | **M**, **S** |
| clamp of oCn | per RT | A52/5A/62/6A bit 12 | **M** |

- **S** Per-RT write masks: L `STM_ColorWriteMode_INV` `0x194df0` takes a
  slot index (rejects > 3 at `0x194e1c`), reads the mask from `ctx+0x1dec+4n`,
  writes image `0x14c/0x16c/0x18c/0x1ac` (A53/A5B/A63/A6B), and dirties
  `0x53+8n` (`0x194eef..0x194f58`).  W `0x101f9cd0` does the same with jump
  table `0x101f9f78` and dirty index `0x53+8n` at `0x101f9eec`.  This matches
  D3D `INDEPENDENTWRITEMASKS`.
- **S** The vendor blend builders set or clear all four A00 blend-enable bits
  together (`|= 0xf0` / `&= ~0xf0` on the A00 image word, W `0x101feccc`, `0x101fecd8`,
  `0x101fb27d`; see [blend-alpha-packet-recipes.md](blend-alpha-packet-recipes.md)).
- **I** The hardware has one blend equation.  D3D9 MRT blending
  (`MRTPOSTPIXELSHADERBLENDING`) and GL `EXT_draw_buffers2` (per-buffer enable
  and mask) are expressible.  `ARB_draw_buffers_blend` (per-buffer equations)
  is not.
- **I** A8A (p.120) has no MRT-related field.  Its bit 17 is the Z-tag
  deadlock ECO discussed in
  [chrome9-depth-output-audit.md](chrome9-depth-output-audit.md).  No other
  MRT-specific word was found in `01h` sub-addresses `00..AA`.

## 3. Pixel shader outputs (question 2)

Native destination encoding, from the production-compiler corpus (**C**)
and the kernel encoder layout (`chrome9_fragment_encode_destination`,
`chrome9_fragment.c:660`): dword 3 `[9:6]` = register id, `[11:10]` = file (2 =
output), bit 12 = saturate.  Dword 2 `[23:20]` is the write mask.

| output | dword 3 | note |
|---|---|---|
| oC0 | `000d0820` | proven on silicon |
| oC1 | `000d0860` | corpus only |
| oC2 | `000d08a0` | corpus only |
| oC3 | `000d08e0` | corpus only |
| oDepth | `000d0920` | id 4; the invalid `oC4` token aliases it without depth metadata |

The combined vendor program `oC0=c0; oC1=c1` byte-matches in both compilers
and keeps token order (**C**):

```
01c00530 301c0053 00f08005 000d0820
01c00530 301c0053 00f08105 000d0860
A90=90058000 A92=92000002
```

Shader header or linkage: A90 is `90058000` for oC0-only through oC3-only,
and A92 only counts records (**C**).  No A28/A29/A90..A9b field declares the
colour-output count.  The count is conveyed only through A01 `[16:14]`
(section 2.2).  Unlike oDepth (A13 bit 23 `HZSrcPS`), no other sideband bit
for secondary colour outputs was found in L `stmUpdatePS_inv` or W
`0x1015b0xx..0x1015b23x` (**S**, within those functions).

Ordering: the only vendor evidence is the token-order oC0-then-oC1 sequence
above.  There is no cross-driver order mismatch like the one for
colour+depth.  **I** Use ascending oCn order, full XYZW masks, one store per
target, all output records at the end of the program.  This is exactly the
vendor byte sequence.

## 4. Vendor D3D programming and limits (question 3)

A D3D9 `SetRenderTarget(n>0)` path, reconstructed (**S**):

1. The DDI stores the surface in slot n (W `0x100ff7f0`, `ctx+0x3da0+4n`; L
   `stmSetRenderTarget` `0xee970`).  Index and dimensions are not validated
   there.
2. The RT builder (W `0x101f9f90`, jump table `0x101fa8ec`; L
   `STM_RenderTarget_INV` `0x195e90`) fills base, mode, location, format and
   pitch for slot n and dirties its three words.
3. Before building, both stacks scan **all four** slots (W
   `0x101f9fde..0x101fa0a0` into `ctx+0x3490`; L `0x195ef9..0x195f30` into
   `ctx+0x3c74`) for internal formats `0x1028`, `0x102c`, `0x203c`, `0x203d`,
   `0x203e`.  The L lookup at `0x1a6c20` maps these to RT format bytes `16`
   (L16), `f0` (R16F), `bb` (G16R16), `f4` (G16FR16F) and `f5` (R32F).  These
   are exactly the formats for which the manual says "post-rendering must be
   disabled" (pp.107--108).  If any slot holds one, W `0x1015908c..0x101590a3`
   clears A00 `[11:0]` and bit 20 and A01 bit 5 (anti-aliasing).  L
   `stmGlobalHardwareAdjust_inv` `0x17d4cd` → `0x17d5f0..0x17d648` clears A00
   byte 0 and bit 20 and A01 bit 5, and forces all four masks A53/A5B/A63/A6B
   to `0xf`.  The rule applies to every RT at once.
4. The PS update writes A01 `[16:14]` from the shader output count (section
   2.2).  Write masks come per slot from `COLORWRITEENABLE[n]` (section 2.3).

Limits:

- At most 4 targets: **M** A01 encoding; **S** W caps `NumSimultaneousRTs=4`.
- Same bit depth: **S** W `PrimitiveMiscCaps=0x002b6cfb` lacks
  `MRTINDEPENDENTBITDEPTHS` (`0x00040000`, decoded by
  `tools/re/chrome9_umd_caps_dump.py`).  Under D3D9 rules this means all
  simultaneous RTs must share one bit depth.  The hardware reason is not
  known.
- Shared blending: `MRTPOSTPIXELSHADERBLENDING`, `INDEPENDENTWRITEMASKS`,
  `SEPARATEALPHABLEND` are advertised (**S**).  Formats with `NOALPHABLEND`
  (G16R16, L16, R16F, G16R16F, R32F) can be render targets (FORMATOP table),
  but they disable post-rendering on all targets (item 3).
- No per-RT width, height or clip exists in any RT word (**M**).  **I** Every
  slot is written over the single shared scissor/viewport/clip rectangle.  The
  kernel must check, for each slot below the count, that
  `offset + (y_max * pitch) + x_max * cpp` lies inside that RT's BO.  This is
  the MRT memory-safety condition.
- Both vendor GL ICDs expose one draw buffer (see
  [gl-feature-matrix.md](gl-feature-matrix.md)).  D3D9 is the only vendor MRT
  user, so there is no GL reference packet.

## 5. Silicon risk and guarded test ladder (question 4)

### 5.1 Risks

- **Multiple output records.**  Fragment programs with several COLOR stores
  (disjoint partial oC0) timed out, and oC0+oDepth locked up the engine
  ([chrome9-glmark-shadow-depth-proxy.md](chrome9-glmark-shadow-depth-proxy.md)
  "partial oC0",
  [chrome9-depth-output-audit.md](chrome9-depth-output-audit.md)).  MRT
  necessarily has two or more output records.  **I** One possible explanation
  is a PS→BE hand-off that expects exactly the A01-declared number of
  output records per pixel.  It is not verified.  If that were true, count=1
  plus exactly `oC0,oC1` would be consistent, and a mismatch would lock up
  the engine.
- **Stale or wrong slot state** writes anywhere in SL or GART (section 2.2),
  including a wrong location field (section 2.1).
- **Post-rendering on unsupported formats** (item 3 of section 4).
- **RC cache and BE packing** (A02, A00 bits 21/22) interaction with several
  targets is unknown.

Mitigations for every rung: a kernel module parameter `mrt=0` by default;
the established target procedure (hash-gated module handoff, `flock -w 5 -x`
GPU lock, `timeout -k 2s 7s`, one draw per job, 3D-wait segment prefix,
reset-on-timeout; after a timeout do not unload, reboot, and power-cycle via
NanoKVM if the target does not return).  Use 64x64 linear SF BGRA8888
targets, each BO with guard pages and a CPU-filled canary, fresh D16 with the
command 19 depth setup (clear 1, LESS/write, z=0.5), A8A unchanged, fog and
AA off.  Record `0400` status and dmesg after each run.  Stop at the first
timeout and analyse statically before retrying.

### 5.2 Ladder

0. **Host only.**  Kernel packet self-test: count=2 emits A01 `[16:14]=001`,
   A58..A5B with location `01`, and the exact vendor records
   `...000d0820`, `...000d0860`, `A92=92000002`.  Validators reject count 0
   with oC1, oC4, non-ascending or partial stores, and any slot below the
   count without a bound, bounds-checked BO.
1. **Inert state.**  The proven single-RT draw (`oC0=c0`, A01 count 0), plus
   A58..A5B pointing at a second canary BO.  Pass: RT0 correct and RT1
   canary untouched.  This shows that the secondary words alone are inert
   and accepted by the command parser.
2. **Count without a second store.**  Only if rung 1 passes: A01 count 1 and
   the oC0-only program.  This measures what RT1 receives when the count is
   larger than the stores (undefined, stale or zero).  Pass: no timeout and
   RT1 writes stay inside its BO.  If this rung times out, it supports the
   output-count hypothesis; do not continue until it is analysed.
3. **First MRT.**  Count 1, `oC0=c0 (red)`, `oC1=c1 (green)` using the exact
   corpus bytes; A00 = `00000033` (dither and the proven identity blend
   enabled for RT0 and RT1, mirroring the RT0 configuration); A5B=`5b010c0f`.
   Pass: RT0 all red, RT1 all green, both guards and a third canary BO in the
   A60 slot untouched.
4. **Swap.**  Exchange the RT0/RT1 BOs and the c0/c1 values, to rule out
   aliasing and stale latches.  Then run a single-RT draw in the same job
   after the MRT draw to prove that count 0 stops RT1 writes.
5. **Per-RT state.**  A5B=`5b010c00` (RT1 masked, RT0 written); then A00 blend
   bit 5 only with a non-identity equation, to prove per-RT enable over the
   shared equation.
6. **Three and four targets.**  Counts 2 and 3 with ascending `oC0..oC3`
   distinct constants.
7. **Placement and format.**  Tiled RT1 (A59 bit 23), then SL RT1 (location
   `00`), then an RGB565 pair (same bit depth).  Mixed bit depths and the
   post-render-disabled formats come last, with A00 `[11:0]` cleared as the
   vendor does.

Admit a GL `PIPE_CAP_MAX_RENDER_TARGETS > 1` only after rungs 1--6 pass and
the kernel validator enforces the slot bounds, count and location rules above.
Keep partial per-target stores and oC holes rejected until they have their own
oracle.

## 6. Open items

- Silicon behaviour for rungs 2 and 3 (the output-record question).
- A01 count with an oC hole (for example oC0 and oC2 written, count 3).  The
  vendor shows only that the count is a high-water mark.
- Use of A00 bits 21/22 (BE packing), RC cache and H16 tiling with several
  targets.
- Whether any hardware reason lies behind the missing
  `MRTINDEPENDENTBITDEPTHS` capability.
