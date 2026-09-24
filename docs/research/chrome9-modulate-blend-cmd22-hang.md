# Chrome9 cmd22 MODULATE+blend menu hang: vertex colour read as a command header

Status: static root-cause analysis, 2026-09-24. No target access, no kernel or
Mesa change. Evidence comes from the captured ring segment
`artifacts/2026-09-24/gl1-menu-timeout-segment.bin`, the kernel builder
`chrome9_build_fragment_program()` (`chrome9_engine.c:2378`), the Linux 5.75
ICD disassembly (`~/tmp/chrome9/dri575.dis`), the Windows UMD disassemblies
(`VTGUModeDX32A.dll.dis`, `VTGOGL32A.dll.dis`), the old 5.75 KMD sources
(`binary/5.75.32.87a-u910-55689/VIA_Chrome9_2.6.31/`), the manual text
(`~/tmp/chrome9/hd3d.txt`) and the ioquake3 source at
`83a776283bdb958f82db25554b5ed0966aaf6e49`.

## Summary

Every state word in the timed-out job matches vendor production output or a
state word that has already passed live. The one structural difference from
the vendor is how the vertices are framed. Our kernel sends
application-controlled vertex dwords as **uncounted H2 type-00 data**. The
command regulator (CR) has no length for that data, so it finds the end of the
packet by recognising the next header. In this job, every vertex's packed
diffuse colour is `feff0000`: alpha `0xFE`, top byte `FE`, which is the AGP
header prefix. It also sits on a 16-byte-aligned dword, the slot where headers
begin.

The colour comes from ioquake3's `UI_PULSE` glow pass. Its alpha is
`0.5 + 0.5*sin(realtime/75)`, so it sweeps through every byte value about
twice a second. That explains every observation:

* the hang is deterministic within seconds of reaching the menu;
* HUD text never hangs, because its alpha is a constant `0xFF`;
* the separate kind-7 fixed-MODULATE capture also shows `feff0000`;
* only the CR is busy, and it stops at the first 3D-wait packet after the
  list.

Vendor production never sends app-controlled vertex or index data through
uncounted H2. It always uses counted H3 (`FE03043C <dwords> 00000000
CC000000`). **Rank 1 hypothesis: the CR decodes an in-payload vertex
colour as a header, truncates or misaligns the A24 vertex list, and the
FE33 3D wait then never completes.** The experiments below confirm or
refute this in three runs.

## Decoded segment

The 8-word capture header is: magic `c9d00001`, segment ring offset `0x1080`,
PAUSE `+0x9f0` (`0x1a70`), STOP `+0xa70` (`0x1af0`), FE33 ID `0x448`, cookie
`00020002`, kind 22 and flags `0x2`.

| ring | content |
|---:|---|
| `1080` | H0 `006c=00800000` (3D wait) |
| `10a0` | H2 type-01: `50014cd0 51100000 52980064`, A38..A3F zero, `34000020 35000148 36000410 37000244`, `33007000` |
| `10f0` | H2 type-02 stage 0: `0000ced0 18001101 20100100 21000001 22008088 30990000` |
| `1120` | H2 type-02 sample 0: `30000000 31102490 32000049 cccccccc` |
| `1140` | H2 type-01: `860000ff 870000ff 53010c0f 00000011 01ce1540 88001ed0 89000800 1e000000 1f000000 20007fff` |
| `1180` | H2 type-FE: `00000011 01002521 08010101 04000000 05000000 0d810000 09000000 0a000000 02000000 03000000` |
| `11c0` | H2 type-04: `00000308 010000c2 020041d1 030052d1 04006342 1200000f 61800000 623081ff 20000000 23004f02 24000030 25f6c000 26000000 28ffc000 29000000`, viewport block, `52147fff` |
| `1240` | H3 VS float constants c256/c257, H2 VS integer constants, clip `80000258 81000320 82000258 83000320` |
| `12b0` | H2 VS program, 5 records; H2 type-04 `60800004` |
| `1330` | H2 PS controls `90018000 91000001 92010001 93000001 94..9b=0` |
| `1370` | H3 ALU `0140b6d0 30040053 00f20005 00000820`; H3 TAU `301001f0` |
| `13b0` | **H2 type-00** `fe020440 0000043c 00000000 cc000000` |
| `13c0..19bf` | 48 vertices x 8 dwords: X Y Z RHW **diffuse** specular S T |
| `19c0` | `fe330004 48200000 00000004 0000104a` (FE33, ID `0x448`, slot `0x0104a000`) |
| `19d0` | H0 `006c=00800000`, H2 A1C/A1D reset, H2 `00300000` dummy, H0 DST_POS cookie `00020002` |
| `1a70`/`1af0` | PID00 PAUSE / PID10 STOP |

The vertices are 16 triangles (eight glyph quads) with Z=0 and RHW=1.0. X runs
from 258.25 to 444.50 and Y from 167.00 to 200.75. S runs from 0.023 to 0.891
and T from 0.016 to 0.238. All triangles have the same winding, with signed
areas of 168.8 to 379.7 px² (the 337..759 in the job description is twice
these values). A scan of every payload dword for top bytes `FE`, `CC`, `CD`,
`CE`, `CF` or `DD` finds **exactly 48 hits, all `feff0000`, all at dword 4
of a vertex, all 16-byte aligned** (ring `0x13d0 + 0x20*k`). No other word in
the payload has any of these top bytes.

The timeout gives `043c=0x1a00`. The read-ahead is `0x220`, so the consumer is
somewhere in `0x17e0..0x1a00`, and the FE33 record at `0x19c0` lies inside that
window. The FE33 slot still holds its poison value and `0494` holds the
previous ID, so the FE33 3D wait never completed. Only CR busy
(`0400=00000010`) and `0444=20000000` show that the reader was mid-command
with every 3D unit idle. The kind-7 capture has the same shape: fetch was
`0x20` past the H0 `006c` 3D wait, which was the first 3D-wait packet after
its list (`ioquake-gl1-kind7-cookie-timeout.md`).

## Source of the draw

In ioquake3 `code/q3_ui/ui_menu.c:313-321`, "SINGLE PLAYER" is at
`y=134` with `color_red` and `QMF_PULSEIFFOCUS`, and it holds the initial
menu focus. `ui_qmenu.c` `PText_Draw` adds `UI_PULSE` to the focused item.
`ui_atoms.c:552-564` then draws the string twice:

```c
UI_DrawProportionalString2(x, y, str, color, sizeScale, uis.charsetProp);
drawcolor[0..2] = color[0..2];                       /* 1, 0, 0 */
drawcolor[3] = 0.5 + 0.5 * sin(uis.realtime / PULSE_DIVISOR); /* 75 */
UI_DrawProportionalString2(x, y, str, drawcolor, sizeScale, uis.charsetPropGlow);
```

`charsetPropGlow` is `menu/art/font1_prop_glo.tga` (`ui_qmenu.c:1749`), a
256x256 BGRA image, which matches the texture state (`20100100`, `30990000`).
At 800x600 (`yscale=1.25`), `y=134` maps to 167.5, and `PROP_HEIGHT=27`
(`q_shared.h:300`) gives a 33.75-pixel glyph. The captured Y span is exactly
167.00..200.75. The job is therefore the **glow pass of the focused
menu item**. Its packed colour is `A=float_to_ubyte(pulse)`, `R=ff`, `G=B=00`,
which is `feff0000` at the captured moment.

Pulse arithmetic (period `2*pi*75 ms = 471 ms`): the byte `0xFE` is present
2.07% of the time. The dummy-prefix bytes `CC`, `CD`, `CE`, `CF` and `DD` are
each present about 0.3% of the time. At a few frames per second, a byte in
this set occurs within seconds, which matches "deterministic on first menu
run". If a state defect caused the hang, the captured alpha would be
effectively random. If the kind-7 capture is also this glow pass (its S/T
atlas pattern and colour say so), the chance that both would show `0xFE` is
about 0.04%.

Other ioquake3 text passes differ:

* The HUD and non-pulsed menu text use constant alpha 1.0 (`0xFF`), so they
  never produce an `FE` top byte.
* `UI_DrawString` with `UI_PULSE` (`ui_atoms.c:732-739`) lerps alpha between
  1.0 and 0.8, which covers `0xCC..0xFF`. It is a second, independent
  generator for the whole suspect byte set.

## Vendor comparison, word by word

| word(s) | value | vendor / live reference | result |
|---|---|---|---|
| H0 6c | `00800000` | cross-driver 3D wait; old Xorg and Linux streams (`ioquake-gl1-kind7-cookie-timeout.md`) | match |
| A50..A52 | `50014cd0 51100000 52980064` | format 98 XRGB, pitch 3200; XRGB+source-over passed (`ioquake3-alpha-depth-blend-cmd22-qualification.md` step 4) | match |
| A38..A3F | zero | closed constant block (`blend-alpha-packet-recipes.md`) | match |
| A34..A37 | `34000020 35000148 36000410 37000244` | GL1 same-factor source-over; Windows `VTGUModeDX32A:0x101fd9e0`, OGL `VTGOGL32A:0x10251220`; passed live | match |
| A33 | `33007000` | canonical alpha test disabled | match |
| type-01 A00 | `00000011` | `chrome9_lower_blend()` bit 4 blend, bit 0 base; `STM_AlphaBlend_INV:0x196e30` dirties the same slot | match |
| A53 | `53010c0f` | full RGBA mask, copy ROP | match |
| A01 | `01ce1540` | vendor GL default byte OR `ce/15/40` at `stmInitChipImage_inv:0x1787c4..0x1787da`; bits 17/18/19 correct diffuse, specular and fog perspective | vendor-exact |
| A86/A87 | `860000ff 870000ff` | same as fixed builders (`textured-d16-format-contract.md`) | match |
| A88/A89 | `88001ed0 89000800` | X guard band, `0x178866..0x1788af` (`chrome9-near-plane-clipping-re.md`) | match |
| A1E/A1F/A20 | `1e000000 1f000000 20007fff` | `VTGUModeDX32A:0x101f8bb4..0x101f8c0e`, `via_chrome9_dri.so:0x1bb1e0..0x1bb292` | match |
| stage A00..A30 | `0000ced0 18001101 20100100 21000001 22008088 30990000` | same group as `__glDpSyncFrontBufferToFakeFront:0x2375d9..0x2376a9` | match |
| sampler A30..A32 | `30000000 31102490 32000049 cccccccc` | linear min/mag, no mip, clamp S/T; same field layout as the live `31102492/32000049` (`chrome9-generic-rectangular-mip-audit.md`) | consistent |
| FE A00 | `00000011` | one texture plus texture-cache clear, `via3DBlit:0x1f26b1` | vendor-exact, pulsed every job |
| FE A01/A08 | `01002521 08010101` | T0 perspective, 2D; one float2 output (`fixed-function-state-transitions.md`) | match |
| FE A02..A0D | zero, `0d810000` | builder baseline, `chrome9_fragment_fe_config()` | match |
| raster | `00000308 ... 623081ff ... 25f6c000 28ffc000` | identical words in `ioquake-fog-dlight-successors.md:127` | match |
| A20/A23/A24 | `20000000 23004f02 24000030` | list, Gouraud, 48 vertices; vendor A24 is an exact count (`windows-draw-path.md`) | match |
| ALU | `0140b6d0 30040053 00f20005 00000820` | Windows and Linux production `TEX; MUL oC0,r0,v0` (`hardware-lab.md`, `chrome9_fragment.c:1979`) | vendor-exact |
| TAU | `301001f0` | production `301001b0`; differs only in swizzle lane z (`.xyww` instead of `.xyzw`) from the Q=1 lowering of float2 T0 | minor; same lowering class as the passed cmd22 oracle and the HUD |
| **immediate** | **H2 type-00** `fe020440 0000043c 00000000 cc000000` + 384 dwords | vendor app draws use **counted H3** type-00: ICD `__glS3H5DrawPrimitiveTriangleList_VM:0x1dffef..0x1e008f` (`fe03043c, count, 0, cc000000`, then `memcpy` of app vertices); D3D helper call at `0x101b1364`, `memcpy` at `0x101b1393`; ICD `_H6` indices through H3 at `0x21557a` | **mismatch** |
| FE33 | `fe330004 48200000 00000004 0000104a` | Windows KMD `0x000b755f..0x000b85f4` grammar (`fe33-persistent-ring-production.md`) | match |
| A1C/A1D | reset | `hwmUpdateStatus_h5`; after FE33, so not reached first | match |

Vendor evidence on wait and flush around blending or texture changes:

* No vendor path emits a wait or flush between a texture upload or bind and
  the first sample. The only extra action is the FE A00 bit-0 pulse, which
  this job already sends.
* `stmFlushPipe_inv:0x176260` emits nothing.
* `stmUpdateTextureUsageEventTag_inv:0x1767c0` is empty.
* `STM_TextureMap_INV:0x1b2df0` has no wait.
* `__glDpFlushCache` is CPU ownership only (`resource-layout-clear-cache.md`).
* Blend state is built independently of the texture program: `STM_AlphaBlend_INV`
  and the Windows builders (`chrome9-glmark-blend-mip-closure.md`).

No vendor wait packet is missing.

## Vendor framing evidence

This section is from a sub-agent audit, with the addresses spot-checked.

* **H2 type-00 in the ICD:** only `via3DBlit` (`0x1f2931`) and
  `__glDpSyncFrontBufferToFakeFront` (`0x2372b1`) use it, and Xorg
  `via3DEmitQuad_H5` (`0x2e7b8`) likewise. Each sends four driver-built
  vertices whose diffuse and specular are the constant `ffff0000`.
* **Application draws in the ICD:** the roughly 80 `__glS3*Draw*` emitters all
  use counted H3.
* **Windows D3D and OpenGL:** no H2 type-00 site was found. Inline vertices
  go through counted H3, and the vertex data is copied with `memcpy`.
* **Escaping:** no vendor code escapes or sanitises payload dwords (no
  alpha-`FE` or header-code tests outside the command parsers).
* **Vendor software stream parsers:** `dispatchDMAToCModel_inv` (loop
  `0xb0620..0xb0670`), `cmKickoffBCI_inv` (`0xb17e8..0xb1880`),
  `USER_SPACE_KickoffBCI_inv` and the KMD `is_agp_header()`
  (`via_chrome9_3d_reg.h:356`) all end H2 data at the first dword whose top
  16 bits are one of `FE00..FE07` (the KMD list also has `FE09` and `FE0A`).
  They test every dword, not only aligned ones, and do not scan H3 payloads.
* **The real header set is wider:** these lists omit headers that work on
  silicon (`FE33`) and ones defined in `via_chrome9_3d_reg.h:69-81`
  (`FE20`, `FE25`, `FE4x`, `FE8x`, `FEE2`, `FEE3`). The hardware decoder
  therefore accepts more than `FE00..FE0A`, and whether it treats `FEFF` as a
  header is not recorded anywhere.
* **Manual:** it only says that type-00 ends on the A24 count "or receive a
  'Stop Command'" (`hd3d.txt:4121-4122`). It does not describe header
  detection.

The framing mismatch is certain. That `feff0000` aliases a header in the
hardware is strongly correlated with the hang but not proven. The first
experiments below settle it.

## Ranked hypotheses

1. **In-payload header aliasing (high).** A vertex colour with top byte
   `FE` (possibly also `CC..CF`/`DD`) at a CR header-eligible slot ends or
   corrupts the uncounted H2 vertex stream. The 3D front end receives fewer
   than `A24*8` well-formed dwords, never raises PLEND, and the FE33
   `00200000` 3D wait blocks forever. All 3D units stay idle because they
   are starved of input, so only CR is busy. This explains the menu/HUD split,
   first-run determinism, both captures, and why every state word is correct.
   The earlier "one-triangle and 16-triangle MODULATE+blend" failures and the
   11,158-draw run that ended at shutdown also came from menu glow passes. The
   earlier Mesa/DRM MODULATE+blend gate hid the problem only because the
   pulsing glow pass is the blended draw.
2. **Completion or list-end handshake specific to MODULATE+blend 16-triangle
   lists (low).** In-game HUD text uses the same program class, blend
   factors, stride and batch accumulator without hanging. Isolated cmd22
   TEX x PRIMARY + source-over passed on silicon.
3. **Newly uploaded or rebound texture, cache state (low).** The FE A00 pulse
   is present and the vendor adds nothing more. A texture fetch fault would
   more likely leave TAU/PS or PE busy, or set `040c`.
4. **Blending that reads a newly created XRGB render target (low).** XRGB
   source-over passed. A stuck destination read would leave the PE busy bit
   set.
5. **Sampler `31102490`, linear minification of a 256x256 image with no mip
   levels (low).** The same field class has run live. A TAU stall is not
   CR-only.
6. **Perspective bits with RHW=1 (very low).** A01 is the vendor default.
   With RHW=1 the correction is arithmetically neutral. FE A01 is also the
   vendor word.
7. **TAU swizzle `.xyww` (low).** It is the same lowering that passed
   elsewhere. It is still a real difference from the vendor TAU, so it is
   listed for completeness.
8. **A1C/A1D reset (excluded as the first cause).** The reset comes after
   FE33, and FE33 never completed.

## Security consequence

If hypothesis 1 holds, any client with render-node access can place
arbitrary CR headers in the kernel-built ring through vertex colours. For
example, it could inject `FE33` with a chosen DMA address, an `FE8x` branch, or
H0/H2 register writes. The earlier accept-and-validate model assumed that the
semantic ioctl kept userspace data out of command position; uncounted H2
breaks that assumption. Even under the vendor software rule alone, a colour
`FE02xxxx` (alpha `FE`, red `02`) is a well-formed H2 header. **Do not run
experiments with `FE3x`, `FE4x` or `FE8x` values.** They can cause
system-memory writes or branches.

## Discrimination ladder (userspace cmd22 replays)

Run the ladder with `fe33_fence=1`. Before each run, check that a
known-good submission in the same boot retires. Stop at the first timeout,
retain its diagnostics, and cold power cycle.

For each run record: FE33 ID, `0494`, cookie, `0444.bit31`, `0400`, `040c`
and `043c`, and target pixels at glyph centres. Keep the guard pages.

**Replay tool.** The replay must be rebuilt from this segment, because a
timeout produces no Mesa `CHROME9_DUMP_FAILED_CMD22` dump.

* Recreate the semantic request: `TEX r0,T0.xy,s0` and
  `MUL oC0,r0,PRIMARY`, one float2 varying, BGRA 256x256 texture with linear
  min/mag, no mip levels and clamp S/T, and an 800x600 XRGB target with pitch
  3200 and no depth. Use `rgb_src=alpha_src=SRC_ALPHA`,
  `rgb_dst=alpha_dst=ONE_MINUS_SRC_ALPHA`, mask RGBA and no alpha test.
* Take the 48 vertices from ring `0x13c0..0x19bf`. X/Y go to 16.16 fixed
  point, `rhw_float=3f800000`, `color` is dword 4, `specular` is dword 5 and
  S/T are dwords 6/7.
* The replay then needs two overrides, which `tools/re/chrome9_cmd22_replay.c`
  does not have yet (it has only `triangles=` and `noblend`): an all-vertex
  and per-vertex `color=`, and `specular=`.

| step | single change | expected if hypothesis 1 holds | if not |
|---:|---|---|---|
| 1 | Exact job, but every diffuse `feff0000` becomes `ffff0000` (alpha byte only) | exact FE33 ID written, cookie, PAUSE and non-CR idle all pass; glow pixels differ by 1/255 in blend weight | a hang means state or list causes; go to hypotheses 2 to 5 |
| 2 | Exact job, unmodified (`feff0000`) | timeout with FE33 absent, CR-only busy, fetch just past FE33 | a pass means the menu hang depends on history (preceding job); replay the previous job first |
| 3 | Step 2 with `triangles=1` and `noblend` (A00 `00000001`, destination tail without A38..A3F closure) | still a timeout: one aliasing dword, no blend and no batch | a pass means blend or count interacts; add back one at a time |
| 4 | Step 1, but vertex 0's **specular** (dword 5, not 16-byte aligned) becomes `feff0000`. Specular is not read by the program | pass means only 16-byte-aligned slots decode headers; timeout means every dword does | — |
| 5 | Step 3, vertex 0 diffuse alpha set in turn to `fd`, `cc`, `cd`, `ce`, `cf`, `dd` (RGB `ff0000`), one byte per boot | `fd` passes. A `CC..CF`/`DD` timeout means dummy codes are also dropped inside H2 data | maps the forbidden set |
| 6 | Diagnostic kernel build only: the same job as step 2, with the vertices framed as counted H3 `fe03043c 00000180 00000000 cc000000` + 384 dwords | pass, with glow pixels as in step 1 | a timeout would refute the framing fix; go back to state hypotheses |

Steps 1 and 2 together prove that the single byte causes the hang. Step 3
removes blend and batching as necessary conditions. Step 4 fixes the
alignment rule. Step 6 is the proposed repair, framed exactly as the vendor
frames it.

## Repair direction

These are not implemented here; each needs its own silicon gate.

1. Send every vertex or index payload that userspace controls through counted
   H3 type-00. The affected sites are:
   * `chrome9_build_fragment_program()` (immediate header near the end of the
     function);
   * the textured and two-textured builders (`chrome9_engine.c:1478`, `1810`);
   * the triangle, batch and depth builders that embed vertices in the quad
     body;
   * the `chrome9_drv.c` fixed-quad paths when `vs->vertices` comes from a
     request.

   `chrome9_emit_header3()` (`chrome9_engine.c:489`) already produces the
   exact vendor framing. Keep A24 as the vertex count. Recompute the
   page-staging bounds, because the H3 payload length is explicit.
2. Until step 1 is qualified, the kernel should refuse or disable any job
   whose H2 type-00 payload contains a dword with top byte `FE`. It should
   also refuse `CC..CF`/`DD` until step 5 clears them. This is a containment
   for both the hang and the injection risk. It is not a rendering fix.
3. After the framing fix passes, revisit the Mesa and DRM "MODULATE plus
   blend unproven" gates and the kind-7 containment. Their recorded evidence
   is fully explained by the menu pulse alpha. The earlier
   completion/tail-visibility theory in `hardware-lab.md` and
   `chrome9-glmark-blend-mip-closure.md` should be marked as superseded once
   steps 1 and 2 confirm this cause.

## Open points

* The hardware header decoder rule is unknown: whether it checks every dword
  or only aligned ones, and which second bytes it accepts. Steps 4 and 5
  measure it, using only `FEFF` and dummy codes.
* The in-game HUD colours are inferred from source (constant alpha 1.0), not
  from a capture. A Mesa-side log of any vertex dword with an aliasing top
  byte, per submitted job, would confirm that passing jobs never contained one.
* The kind-7 capture retained only 16 of its 48 vertices. Its colour
  `feff0000` and its atlas-like S/T fit the same glow pass, but its X/Y
  were not compared with the menu layout.

## Silicon result (2026-09-24)

Confirmed and fixed.  Three captured timeouts (menu runs, dumps
`artifacts/2026-09-24/gl1-menu-timeout-segment.bin`,
`seg-after-base-oracle.bin`, `cmd22-609-0.bin`) all carried diffuse
`feff0000` in 16-byte aligned vertex slots.  Kernel Chrome9 commit `26fdb1d`
frames every client vertex list (triangle, textured, two-textured, depth/RHW
and generic fragment-program builders) as counted H3
`fe03043c <dwords> 00000000 cc000000`.  With that module four consecutive
ioquake3 GL1 main-menu runs, the VS oracles (base, clip-near, fetch) and
glmark2 `texture` validation passed with no timeout.  An earlier suspicion
that the VS oracles' row-258 upload leaked into later jobs was a
coincidence of the pulsing alpha and was not committed.  The legacy
MODULATE+blend ABI gate and Mesa's `blend_successor` containment are now
candidates for removal after their own replay.
