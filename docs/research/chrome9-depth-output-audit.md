# Chrome9 pixel depth-output compiler boundary

The D3DSPR_DEPTHOUT destination token `100f0800` is accepted by both
production compilers and sets their depth-output-present metadata.  A terminal
depth MOV uses native destination word `000d0920`.  Depth-only records
byte-match across Windows and Linux; a combined color-plus-depth program has
the same two records but the compilers choose opposite record order.  Packet
encoding is therefore closed, but context sideband ordering and silicon depth
export remain unproved, so depth output stays rejected.

## Token and records

`MOV oDepth,c0.wwww` compiles identically in both families:

```
01c00530 b01c0053 00f0806d 000d0920
```

`MOV oDepth,v0.zzzz` also byte-matches:

```
01c00530 201c0053 00f04049 000d0920
```

Both are phase 1 and set depth-output-present.  Windows reports color-output
count zero.  Both production compilers use A90=`90058000`, A92=`92000001`,
and all remaining transition controls zero.  The metadata acceptance and exact native destination distinguish the
real depth register from invalid `oC4`, which can alias the same native word
without setting depth-output-present.

For source-order token program `MOV oC0,v0; MOV oDepth,c0.wwww`, Windows emits
depth then color:

```
01c00530 b01c0053 00f0806d 000d0920
01c00530 301c0053 00f04005 000d0820
```

Linux configuration 2 emits color then depth:

```
01c00530 301c0053 00f04005 000d0820
01c00530 b01c0053 00f0806d 000d0920
```

Both set depth-present, stay in phase 1, and use A92=`92000002`; Windows also
reports one color output.  This order mismatch means the unordered record set
is compiler evidence, not a cross-driver executable byte sequence.  A new
backend must select and validate one target schedule deliberately.

The corrected production-RegMgr three-case differential JSON SHA-256 is
`79dc113c3167b516991bd17378fca209537f32cd3ff936e7ef4db77e9a48fdf8`.
The earlier JSON `d73508d0...` omitted the Windows register-manager builder;
its records, metadata, and record-order mismatch were accurate, but
A90=`90040000` was a harness artifact.
Exact tokens, controls and metadata are retained in
[`chrome9-depth-output-corpus.tsv`](chrome9-depth-output-corpus.tsv).

## Required A13 depth-source state

The compiler object depth flag is not descriptive metadata only.  The
recovered update path copies it into context-state byte `+0x4e.bit7` and
dirties group `0x00080000`.  The context image stores type-01 A13 at `+0x4c`,
so byte `+0x4e.bit7` is exactly A13 payload bit 23.  Its native name is
`HZSrcPS`: it selects pixel-shader Z instead of vertex/raster Z.  Therefore a
depth-writing shader must OR A13 with `0x00800000`:

```
LESS:   13010000 -> 13810000
ALWAYS: 13070000 -> 13870000
```

This bit is independent of the existing compare function in A13 bits 18:16
and the normalization factor in bits 7:0.  The VX900 A13 table labels bit 23
reserved, but the manual's fog constraint names `HZSrcPS` and states that Z is
calculated in the pixel shader when it is one.  The UMD dataflow plus that
manual cross-reference resolves the previously unknown emitted H2 word.
Uploading `000d0920` without A13.bit23 does not select shader depth; conversely
A13.bit23 must be cleared when returning to a color-only shader.  Fixed fog
from Z is forbidden by the same manual note, so the first key requires fog
disabled.

Keep depth output outside the structured UAPI until the kernel derives this
A13 delta from a bounded semantic key, validates one exact record ordering,
and restores/clears it transactionally on error and reset.  Do not expose raw
`oC4`/destination words or a user-controlled depth-source bit.

## Guarded live oracle

The safest first shape is full color `v0` plus terminal depth from finite
job-local `c0.w` constrained to [0,1], using the Linux order above and
A13=`13810000`.  Use command 19's mandatory fresh D16, depth clear 1,
LESS/write, and guarded color/depth
destinations.  First draw red at geometric z=0.75 with shader depth 0.25.  Then
draw green at z=0.5 with the already proven color-only shader.  Correct depth
export leaves red because 0.25 blocks the farther green; ignored export leaves
green because geometric 0.75 does not.  Read back both color and D16 and repeat
with depth 0.75/geometric 0.25 to catch reversed or stale sideband behavior.

Run a color-only draw before and after the depth-writing draw to prove enable
and clear transitions, then test the Windows record order only as a separate
portability experiment.  Depth-only rendering and interpolated `v0.z` remain
later gates.  Compiler bytes alone do not prove depth-test timing, depth write,
clamp, precision, early/late ordering, or reset restoration; every NIR
`store_deref` to fragment depth remains fail-closed until all gates pass.

## Lock-up analysis, 2026-09-25

Kernel `42677af` with `depth_output=1` ran the `fragdepth` mode of
`tools/re/chrome9_coarsez_rung.c` (`d9e0575`).  That mode first does an
ALWAYS colour-only clear draw, then the shader-depth draw
`MOV oC0,c0; MOV oDepth,c1.xxxx` (D16, LESS/write, geometric z 0.75,
c1 = 0.25).  The shader-depth draw timed out in the FE33 phase with
`0400=00003711`, and the target needed a cold boot.  The analysis below is
static only.  Vendor addresses refer to `via_chrome9_dri.so` (SHA-256
`fddb4dea...`, **L**) and `VTGUModeDX32A.dll` (SHA-256 `8ac6959f...`, **W**).
**F** marks a fact from code or the manual.  **I** marks an inference.

### What the engine was doing

**F** Decoding `00003711` with the `0400` table in `via-5.75-kmd.md`: overall 3D
(0), CR (4), raster (8), read-colour (9), pixel shader (10), burst engine (12)
and GEMI (13) are busy.  Vertex processor (5), clipping (6), setup (7) and the
pixel engine (11) are idle.  **I** Setup finished its work.  The circular wait
is between raster, PS, RC and BE, which is the path that carries Z when it
comes from the pixel shader.  The VX900 manual (A11 bit 9, p.92) describes the
full Z data path as "PERZ through PS and RC, then BE".  The kernel keeps A11
bit 9 at 0, the same full-path value the vendor uses.

### The encoder output matches the vendor bytes

**F** The kernel encoder (`chrome9_fragment_alu_template` MOV template,
`chrome9_fragment_encode_source` slot A, `chrome9_fragment_encode_destination`
with DEST_DEPTH -> file 2, id 4) produces this for the program that ran:

```
01c00530 301c0053 00f08005 000d0820   # oC0 = c0
01c00530 001c0053 00f08100 000d0920   # oDepth = c1.xxxx
A90..A9b = 90058000 91000000 92000002 93000000 .. 9a000000 9b000000
```

The record layout is the vendor layout.  With c1.wwww the second record is
`01c00530 b01c0053 00f0816d 000d0920`, the exact oDepth record in
`chrome9-texkill-depth-combo-audit.md`.  The only difference is the source
swizzle.  **F** The ALU records have no terminal or end-of-program bit.  Dword 3
bit 5 (`...20`) is set in every record in every position, both for colour then
depth and for depth then colour.  Program length comes only from A92
(`92000002`).  Neither record releases a source, because constants never
release.  **F** A90 is `90058000`: bit 18 (starts with ALU) plus bucket
`18000`.  This matches the Linux configuration-2 value above.  A91 and A93..A9a
are zero, as in the vendor output.  Phase 1 (ALU last) holds.  The colour/depth
order is the Linux vendor order.  **Conclusion:** the lock-up is not caused by
the record bytes or the control words.

### Every vendor consumer of the depth-output flag

A search of object `+0x78` readers and of context byte `+0x4e` (A13 bits 23:16)
accesses finds these uses:

| stack | address | effect |
|---|---|---|
| L | `stmPPS_GetDstSetting_invi:0x162733` | the compiler sets object `+0x78 = 1` for the oDepth destination (id 4) |
| L | `stmUpdatePS_inv:0x17f779..0x17f7bd` | A13 bit 23 = flag; dirty group `0x80000` |
| L | `STM_RenderTarget_INV:0x196359..0x196360, 0x1967c6..0x1967d1` | clears A13 bit 23 on depth bind, then sets it again from the bound PS |
| L | `STM_CoarseZ_INV:0x193c2a..0x193c31` | depth output -> coarse-Z buffer invalid, A01 bits 13/9 off |
| W | `0x1015b051..0x1015b09f` | the same A13 bit 23 update and dirty group `0x80000` |
| W | `0x101fa6d8..0x101fa6e7` | the same A13 bit 23 set on the render-target path |
| W | `0x101fcac8` | the same coarse-Z exclusion |
| W | `0x10204ec8` | compiler rewrite that clears bit 26 of intermediate word `+0xc` (compiler-internal; final records already byte-match, and our records have that bit clear) |

**F** Neither vendor stack writes any other register for a depth-writing
shader.  There is no separate early-Z or late-Z control, no change to A01
(besides coarse-Z), A11, A12, A27 (Fast-Z), A29/A2A, the FE configuration or
A90..A9b.  **I** HZSrcPS alone moves the Z test after the PS.  The kernel's
state for the failed draw already matches every item in this table:

```
A01=01ce154c A10=10xxxxxx A11=11008001 A12=12380000 A13=13810000 A27=27000000
(no A29/A2A, A01 bits 13/9 clear, fog disabled: A00=00000011)
```

### Remaining differences, ranked by likelihood of causing the lock-up

1. **A8A (Miscellaneous Setting 11) is not programmed.  High.**
   **F** The VX900 manual (p.120) names A8A bit 17 "TS ZTAG Deadlock ECO
   Enable Signal" (default 0).  W's chip-image init sets it on PCI ids
   `7122`/`a122` (`0x10154cbf..0x10154cd2`).  W also sets bit 23
   (`0x10154c98`; `ebx=0x800000` from `0x10154a62`; cleared only by config bit
   `+0x374.27`).  The Windows VX900 image is therefore **`8a820000`**.  L's
   image sets only bit 23 (`stmInitChipImage_inv:0x17897f`), giving
   `8a800000`.  The HCM manual names bit 23 "Enhance TX's precision", so it is
   texture-related.  The kernel emits A8A only for MISC-position programs
   (`misc_location = 8a020000`, when A90 bit 19 is set).  For every other draw,
   including this one, A8A keeps whatever value an earlier MISC draw left, or
   its unknown reset value.  **I** A Z-tag deadlock is a hazard that only
   appears when the Z value reaches the Z unit late, after the PS.  That
   matches the busy pattern above.  Ordinary colour-only depth draws have run
   without A8A for months, which fits an ECO that only matters for the HZSrcPS
   path.  The WHQL D3D driver, which must pass DX9 oDepth tests, always enables
   the ECO.  L's Linux GL, which leaves it off, has no silicon proof for
   gl_FragDepth.  Unknown: whether any MISC-position draw ran before the failed
   draw in the same boot.  If one did, A8A was already `8a020000` (ECO on) and
   this candidate drops.
2. **Order of program upload and A13 in one segment.  Low for this lock-up.**
   **F** L emits A90..A9b and the ALU/TAU H3 directly into the command buffer
   inside `stmUpdatePS_inv` (`0x17fc92..0x17fdfc`).  A13 is only marked dirty
   there (group `0x80000`).  **I** A13 is therefore flushed with the type-01
   state after the program.  The kernel emits A13 (common group) before the
   controls and ALU.  In the failed run the two draws were separate jobs.  Every
   segment starts with `006c=00800000` (3D wait, `chrome9-multi-inflight.md`),
   so the earlier colour-only draw had drained before the new A13 arrived.  This
   only matters for batched draws in one segment that switch between a
   colour-only and a depth-writing program.
3. **A9b.  Low.**  **F** L writes A9b = current PS handle index
   (`ctx+0x4f2c & 0xffffff`, `0x17f858`, `0x17fd7f..0x17fd8f`).  The kernel
   writes `9b000000`.  Every proven kernel program uses `9b000000`.
4. **Coarse-Z / Fast-Z policy.  Not active in this run, but required.**
   **F** Both vendors force coarse-Z off for depth-output programs (table
   above).  The rung passed neither `COARSE_Z` nor `FAST_Z`, so A01 bits 13/9
   were 0 and A27 was `27000000`.  The kernel currently leaves both to the
   userspace flags.  It must reject both, or strip them and invalidate the
   coarse-Z buffer, when `program->depth_output` is set.
5. **A01[16:14] render-target count.  None for colour+depth.**  **F** L
   programs it from object `+0x30` (colour-output count) as `(count-1)&7`
   (`0x17f864..0x17f908`).  With one colour output that is 0, which is what the
   kernel emits.  For a depth-only shader (Windows count 0) the result is `111`,
   a reserved value, so depth-only stays rejected.
6. **Source lane.  None expected.**  The failed program used `.xxxx`.  Every
   vendor oDepth corpus row uses a replicated `.wwww` or `.zzzz`.  Use
   `c1.wwww` so that the next attempt is byte-identical to the corpus.

### Next single silicon attempt

This changes one state difference (A8A), removes the preceding clear draw, and
avoids the depth read.  Setup: `depth_output=1`, one draw, 64x64 BGRA8888
colour (pitch 256) and D16 depth (pitch 128), both BOs with guard pages.  The
CPU prefills D16 with `0xffff` and colour with `0xff000000`.  No earlier 3D
draw runs in that job.  Use `flock -x` and `timeout -k 2s 7s`.  Required kernel
change for the attempt only: emit A8A in the common group when
`program->depth_output` is set.

```
common H2 00010000:
  860000ff 870000ff 53010c0f 00000011 01ce154c 88001ed0 89000800
  8a820000                                  # new: ZTAG ECO + bit 23 (Windows VX900)
  10<depth>>8> 11008001 12380000 13870000   # ALWAYS + HZSrcPS, Z write on
  27000000 16000000 17000000 18000000 19800000 1a800000 1b803fbf
  1e000000 1f000000 20007fff 2300ffff 24070000 2500ffff 26070000 33007000
PS constants H3 22030000 (8 dwords):
  3f800000 00000000 00000000 3f800000       # c0 = red
  3e800000 3e800000 3e800000 3e800000       # c1 = 0.25
controls H2 00010000:
  90058000 91000000 92000002 93000000 94000000 95000000 96000000
  97000000 98000000 99000000 9a000000 9b000000
ALU H3 20030000 (8 dwords):
  01c00530 301c0053 00f08005 000d0820       # oC0 = c0
  01c00530 b01c0053 00f0816d 000d0920       # oDepth = c1.wwww
geometry: two triangles covering 64x64 at z = 0.75
```

Pass: every pixel is `ffff0000`, every D16 word is `3fff` or `4000`, and the
guards are intact.  If shader depth is ignored, D16 holds `bfff` (geometric
0.75).  ALWAYS removes the Z read and compare from the first attempt.  **I**
This does not avoid a write-side Z-tag interlock.

After a pass, run in order:

1. The same draw with LESS (`13810000`) against the prefilled `0xffff`.
2. The two-draw oracle above, which tests colour-only to depth-writing to
   colour-only transitions.  The next colour-only draw must emit
   `13010000`/`13070000`.  The kernel already does this, because it rebuilds
   A13 for every draw.
3. A8A kept at `8a020000` (ECO without bit 23), to find out which bit matters.

If the first attempt still locks up, the ECO is not sufficient.  The next
suspect would be the PS-to-Z hand-off, and a vendor stream capture of a real
oDepth draw would be needed.  Such a capture does not exist yet: the corpus
above is compiler output only.
