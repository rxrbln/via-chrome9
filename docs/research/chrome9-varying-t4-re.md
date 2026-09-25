<!-- SPDX-License-Identifier: GPL-2.0-only
Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 fifth and later varyings: A61[22] wide VS output transfer

Status: root cause found and silicon-confirmed on VX900 (2026-09-25) with the
existing `tcl_a61_out` debug knob on the loaded module; no module was rebuilt
or swapped.  The proposed kernel diff is host-tested only.

Evidence labels: **static** (disassembly/manual), **dynamic** (offline
vendor-compiler or host-compiled kernel code), **silicon** (VX900 run),
**inference**.

## Result

Every VS output after the fourth texture-coordinate register (oT4..oT7, FE
coordinates E..H) reaches setup only when type-04 A61 bit 22 (field
A61[22:20] = 4) is set.  Without it the fragment program receives stale data
for T4..T7 while T0..T3 are correct, for both ALU and TAU consumers.  The
kernel sets A61[22:20] only for VS point size (`chrome9_tcl_a61_outputs()`)
and hard-codes `61800000` on the non-TCL path, so any program with more than
four direct T inputs fails.  This is the `glsl-max-varyings` failure.

The fragment encoding, FE linkage and A25/A26/A28/A29 linkage are all
byte-identical to the vendor for these programs and are not at fault.

## Silicon evidence

Target runs used `p68` Mesa, `pg1.sh` (GPU lock), `-auto -fbo`, each test
under 20 s.  `tcl_a61_out` was restored to `-1` after every sequence.

### Characterization (silicon)

Test generator `~/tmp/t4re/mkid.py N K [width]`: VS writes
`v_i = one.x * vec4((4i+1)/40, (4i+2)/40, (4i+3)/40, (4i+4)/40)` where `one`
is a per-vertex attribute of 1.0; FS sums `contribution[i] * v_i` with a
one-hot uniform `contribution[K]`.  The attribute factor matters: with plain
constants Mesa's varying linker propagates the values into the FS and every
case passes without exercising the hardware.  Scalar/vec2/vec3 varyings are
also packed by Mesa, so only vec4 cases map varying `i` to T`i`.

| case (vec4) | CHROME9_TCL=1 observed | CHROME9_TCL=0 observed |
|---|---|---|
| N=5, K=0..3 | pass | pass |
| N=5, K=4 | 0 0 0 0 | 0 0 0 0 |
| N=8, K=0..3 | pass | pass |
| N=8, K=4 | 0 0 0 0 | 0 0 0 0 |
| N=8, K=5 | 1 0 0 1 | 1 0 0 1 |
| N=8, K=6 | .996 1 0 0 | 0 1 0 0 |
| N=8, K=7 | 0 0 0 0 | .996 0 0 0 |

Stale values, not a lane shift or swizzle, and identical on both vertex paths.
vec3 x 8 (packed by Mesa into six vec4 slots) fails exactly from component 16
on: v5.x (in T3.w) is correct, v5.yz (T4.xy) are wrong.  vec2 x 8 (packed into
T0..T3) passes.  The boundary is the fifth T register, not a component count.

The original piglit-style repro showed `T6` passing only because its stale
data happened to equal the expected green.

### TAU consumer (silicon)

`~/tmp/t4re/mktex.py N 0.75 0.25`: the last varying is used only as the
`texture2D` coordinate of the piglit `rgbw` 8x8 texture (expected green),
the others only by ALU.  N=4 (coordinate T3) passes; N=5, 6, 8 (coordinate
T4, T5, T7) return red (1,0,0,1), the texel at a zero/stale coordinate, on
both vertex paths.  The fault is therefore upstream of the PS input read.

### A61[22:20] discriminator (silicon)

With `echo V > /sys/module/chrome9/parameters/tcl_a61_out` (TCL path only):

| A61[22:20] | N=5 K=4, N=8 K=3..7, tex N=5/8, mv84, mv87 |
|---:|---|
| auto (0) | K=4..7, tex, mv84/mv87 fail; K=3 pass |
| 1, 2, 3 | fail (same stale values) |
| 4 | all pass |
| 5 | all pass |

Bit 22 is the operative bit.  With 4 forced, the four-or-fewer cases
(N=5 K=0/3, `tex_4`, `mv63`, `mv64`, vec2 x 8) still pass.  Unmodified piglit
`bin/glsl-max-varyings -auto -fbo` with `CHROME9_TCL=1`: **pass** at 4,
**fail** at automatic (0) in back-to-back runs.

## Vendor evidence (static)

A61[22:20] is written from the VS runtime object `+0x08` by
`stmUpdateVS_inv` (chrome9-queries-msaa-3d-flow-re.md section 12.1).

* Windows `VTGUModeDX32A.dll:0x10206cd5..0x10206d34`, the tail of the D3D
  output-linkage updater:

  ```text
  if (ctx->mode(+0x1c) == 8)          out+8 = 4
  n = texcoord_count + (ucp_count + 2) / 3
  if (fvf_out & 0x2020)  /* oPts or oFog */
          out+8 = n > 3 ? 4 : 1       # 0x10206d0a..0x10206d19
  else
          out+8 = n > 4 ? 4 : 0       # 0x10206d20..0x10206d2e
  ```

  The kernel implemented only the first (point-size) branch.

* Linux `via_chrome9_dri.so` 5.75 `stmUpdateFVFOut_H5i`
  (`0x1ba8a1..0x1ba8ab`) stores 4 into `+0x08` unconditionally: the GL driver
  always runs wide transfer.

The field name in the kernel ("vendor output transfer field") and its
behavior both fit a VS output-to-setup transfer width: narrow mode carries
oPos, oD0, oD1 and four oT registers; oT4..oT7 need the wide mode
(**inference** for the internal meaning; the register effect is silicon).
The VX900 manual does not document type-04 A60..A62.

## What was ruled out

All of the following were compared for the failing programs (N=4, 5, 8
float4, MUL/MAD chain `c_i.x * T_i` as Mesa emits it):

* **Fragment ALU source encoding** (dynamic): the Linux production compiler
  (`tools/re/chrome9_pps_probe.c`, configuration two) emits T4..T7 as class 3
  id 4..7 in the C slot, `002c4053`..`002c7053`, identical to the kernel.
* **A90..A9a** (dynamic): vendor `9005800f`/`9005001f`/`900480ff` for 4/5/8
  inputs (resource buckets `18000/10000/08000`), identical to the kernel.
* **FE A01..A03, A08..A0a** (static + manual): per-field layout of
  `stmUpdateTCI_invi` image bytes `+0x1c70..0x1c79` (stage fields, 3/3/2 per
  word) and `+0x1c8d..0x1c96` (producer widths: A08 bytes 1-2 = A,B; A09
  bytes 0-2 = C,D,E; A0a bytes 0-2 = F,G,H) match `chrome9_fragment_fe_config`
  and the manual (type 02/FE, sub-addresses 08h..0Ah).  Kernel output for
  eight float4: `012b2723 0237332f 03003f3b 08030308 09030303 0a030303`.
* **A25/A26/A28/A29** (static + manual): `stmUpdateFVFOut_H5i` builds A26
  nibbles for T4..T7 as `0xf000 >> 4*(i-4)`, and `stmUpdatePS_inv`
  (`0x17fa3d..0x17fb5b`) puts consumer ordinals 4..7 into A29[15:0]; both
  match the kernel (`25f6ffff 2600ffff 28ffffff 2900ffff`) and the manual
  (type 04 25h/26h/28h/29h).
* **H6 declarations, A12, A62[23:20], pass-through VS records**: correct per
  chrome9-vs-varying-fvf.md; the TCL path does not use them yet fails
  identically.

## Proposed fix (kernel)

Host-tested: `git apply --check` against the current tree, and the extracted
`chrome9_fragment_raster_selftest()` (with the new A61 check) returns 0.
Raster output for 4/5/8 float4 varyings becomes `61800000`/`61c00000`/`61c00000`.
Mesa needs no change.

```diff
--- a/drivers/gpu/drm/chrome9/chrome9_engine.c
+++ b/drivers/gpu/drm/chrome9/chrome9_engine.c
@@ -3400,7 +3400,8 @@ static u32 chrome9_fragment_raster_state
 	}
 	i = 4 + varying_count;
 	raster[i++] = 0x12000000 | GENMASK(2 + varying_count, 0);
-	raster[i++] = 0x61800000;
+	/* A61[22]: VS outputs beyond oT3 reach setup only in wide transfer. */
+	raster[i++] = 0x61800000 | (varying_count > 4 ? 4 << 20 : 0);
 	/* H6 resource high-water is D2 plus the dense T inputs. */
 	raster[i++] = 0x620081ff | (2 + varying_count) << 20;
 	raster[i++] = 0x20000000;
@@ -4028,6 +4029,8 @@ static int chrome9_fragment_raster_selft
 				0, NULL, 3, raster);
 			if (count != 27 + varying_count ||
 			    raster[0] != ((2 + varying_count) << 8 | stride) ||
+			    raster[5 + varying_count] != (0x61800000 |
+					(varying_count > 4 ? 0x400000 : 0)) ||
 			    raster[6 + varying_count] !=
 					(0x620081ff | (2 + varying_count) << 20) ||
 			    raster[10 + varying_count] != expected_a25 ||
@@ -12651,7 +12654,8 @@ static int chrome9_tcl_program_get(struc
 
 /*
  * A61[22:20] as the vendor derives it from the output linkage
- * (VTGUModeDX32A.dll 0x10206cd5): the VS point size is lost without it.
+ * (VTGUModeDX32A.dll 0x10206cd5..0x10206d34): the VS point size and every
+ * oT beyond the fourth coordinate output are lost without it.
  */
 static u32 chrome9_tcl_a61_outputs(const struct chrome9_tcl_draw *tcl,
 				   const struct chrome9_fragment_program *fp)
@@ -12662,9 +12666,9 @@ static u32 chrome9_tcl_a61_outputs(const
 
 	if (knob >= 0)
 		return knob & 7;
-	if (!tcl->point_size_vertex)
-		return 0;
-	return extra > 3 ? 4 : 1;
+	if (tcl->point_size_vertex)
+		return extra > 3 ? 4 : 1;
+	return extra > 4 ? 4 : 0;
 }
 
 /* Validate, copy and compile one fragment draw into d->request. */
```

The TCL `extra` count already includes UCP coordinate groups, matching the
Windows `(ucp + 2) / 3` term, so five or more varyings-plus-UCP-groups also
switch to wide transfer.  The non-TCL path has no VS point size or UCP, so
`varying_count > 4` is the full Windows rule there.  Keeping 0 for four or
fewer outputs preserves every previously qualified packet; the Linux DRI
"always 4" alternative was only silicon-checked on the TCL cases listed above.

## Remaining qualification

1. Non-TCL (`CHROME9_TCL=0`) path with the patched module: rerun
   `~/tmp/t4re/id/id4_5?.shader_test`, `id4_8?`, `tex_5/6/8`, `my/mv8?` and
   `glsl-max-varyings`.  Silicon evidence for this path is currently the
   identical failure signature plus the shared A61 field (**inference**).
2. TCL with UCP: 4 varyings + 1..3 clip planes (5 coordinate outputs) and
   8 varyings + clip planes, which is where `extra` exceeds the varyings alone.
3. Point size with four or more varyings is already covered by the existing
   `psizen` rung (1..8 varyings, values 1/4/5).

## Reproduction

```sh
# host: vendor PS compile for the Mesa MUL/MAD chain, N inputs
W=~/tmp/t4re; L=$W/lib   # stub libexpat/libdrm as in chrome9-ps-read-ports.md
LD_LIBRARY_PATH=$L CHROME9_POSTPROCESS=2 CHROME9_DUMP_CONTROLS=1 \
  CHROME9_RESOURCE_COUNTS=1 $W/pps $(python3 $W/mkps.py 8)
# host: kernel compile + FE/raster words (harness built from chrome9_engine.c)
$W/t 8
# target: A/B of the operative bit (TCL path), then restore
echo 4 > /sys/module/chrome9/parameters/tcl_a61_out
sh pg1.sh <p68> bin/glsl-max-varyings -auto -fbo
echo -1 > /sys/module/chrome9/parameters/tcl_a61_out
```

Scratch files: `~/tmp/t4re/` (`mkid.py`, `mktex.py`, `mkps.py`, `gen.py` +
`main.inc` host harness, `chrome9-a61-wide-output.diff`); target copies in
`/root/tmp/piglit/my/t4re/`.
