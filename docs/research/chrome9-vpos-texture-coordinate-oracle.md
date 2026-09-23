# Chrome9 vPos-derived texture-coordinate oracle

Status: production Linux 5.75 pixel-compiler/finalizer object, checked against
Linux and Windows disassembly; no silicon execution.  This is the coordinate
prefix needed by glmark2 `ideas-logo-shadow.frag`, not proof of native
`gl_FragCoord`, repeat sampling, or TEXKILL.

## Source program and exact compiler result

The installed glmark2 2023.01 shader computes
`texture2D(tex, gl_FragCoord.xy / 32.0)` before testing sampled alpha against
one half.  The logo uploads a 32x32 GL_ALPHA texture with NEAREST/REPEAT.
The independent coordinate/sampling discriminator below substitutes a 2x2
BGRA texture; its coordinate graph is the same finite `vPos.xy * (1/32)`.

The accepted D3D PS 2.0 token stream is:

```text
ffff0200
04000005 00030000 10e41000 20e40000  # MUL r0.xy, vPos, c0
04000042 000f0001 00e40000 20e40800  # TEXLD r1, r0, s0
03000001 000f0800 00e40001           # MOV oC0, r1
0000ffff
```

With c0.xy set to `0x3d000000` (1/32), the production configuration-two
probe command is:

```sh
CHROME9_POSTPROCESS=2 CHROME9_RESOURCE_COUNTS=1 \
CHROME9_DUMP_CONTROLS=1 \
qemu-i386 -E LD_LIBRARY_PATH=/tmp/chrome9-probe-libs \
  /tmp/chrome9_pps_probe ffff0200 \
  04000005 00030000 10e41000 20e40000 \
  04000042 000f0001 00e40000 20e40800 \
  03000001 000f0800 00e40001 0000ffff
```

The accepted, finalised result is:

```text
active-resource-counts: 1 2 0 1
ALU count=3
0140b6d0 d008006d 0032c306 00000020
01c00530 c01c0053 00f20006 000d0420
01c00530 301c0053 00f20105 000d0820
TAU count=1
301101b0
instance +2c=00058000 +6c=00000008 +70=00000001
controls A90-A9a:
90058008 91000001 92010003 93000001 94000000 95000002
96000000 97000000 98000000 99000000 9a000000
```

The first ALU uses native class 3, physical MISC id 3 (`vPos`), with XY
selected by the instruction.  The second ALU bridges the computed TEMP into
coordinate slot zero; the TAU samples slot zero and writes TEMP one; the
third ALU copies the sample to oC0.  Configuration two owns the `0xcf` to 3
relocation and final-use bit in the first ALU, and the bridge/TAU/consumer
phase ordering.  The exact uploader mapping produces A90's phase-and-bucket
`0x58000` (`0x40000 | 0x18000`) plus MISC-usage bit 3 (`0x8`).  A91 bit zero
names internally computed coordinate slot zero; A92 says three ALUs and one
TAU; A95 bit one changes from ALU to TAU after the bridge; and A93 bit zero
returns from TAU to the terminal color MOV.  The complete A90--A9a line above
is therefore runtime-exact control construction from the finalized object,
not an inferred phase sketch.  It is still not a captured full draw packet:
the probe prints compiled metadata, records, and uploader-derived controls.

The semantic test defines only r0.xy, so its TEX operand uses the legal
`r0.xyxy` swizzle rather than naming undefined r0.zw.  Replacing the TEX
coordinate token `00e40000` above with `00440000` yields the same resource
counts, first/third ALUs and TAU; only the bridge changes to
`01c00530 101c0053 00f20004 000d0420`.  The raw smoke uses this
production-compiled XYXY variant.  Both token forms sample the same 2-D
coordinate lanes; the latter also satisfies the clean-room validator's
explicit initialized-lane rule.

The XYXY variant reports the same resource tuple `(1,2,0,1)`.  The third
output is the field later stored at public instance `+0x34` and copied to FE
A00 bits 7:4, so this computed-coordinate program requires A00 resource count
**zero**, despite having one TAU.  The bridge defines coordinate slot zero
internally before the TAU reads it.  Only external texture-coordinate
components contribute to that third output.  Consequently a clean compiler
must not rescan the finalized MISC source as ordinary class-3/id-3 T3: doing
so would incorrectly emit A00 resource count four and declare nonexistent VS
outputs.  Resource analysis must either run before MISC relocation or retain
the source's semantic MISC identity.

`via_chrome9_dri.so` disassembly corroborates the provenance:

- `stmSetMSInst_invi:0x001b547d..0x001b54c2` tests all four source tokens
  for D3D register type 17 and sets instance `+0x80`.
- `stmUpdateConfig:0x00198fee..0x001990cc` reads compiled object `+0x70`,
  adds one to the first active-resource count, chooses buckets and returns
  MISC relocation id 3/4/7/15.
- `stmReplaceLocationTexIndexAINST:0x001990e0..0x001991b7` replaces the
  reserved `0xcf` native source index in ALU source slots.

The Windows D3D UMD independently has all four type-17 set sites at
`VTGUModeDX32A.dll:0x101db34e,0x101db373,0x101db398,0x101db3bb`; its
A90 updater reads the same compiled-object `+0x70` at `0x102024ed`.
The finalizer passes active outputs zero and one to the updater at Linux
`0x0019b099..0x0019b0b4`; the exact configuration-two thresholds are
`adjusted0<=4/active1<=3 -> id3`, then `<=5/<=4 -> id4`, then
`<=8/<=6 -> id7`, otherwise id15, where
`adjusted0=active0+(MISC-present)`.  This vPos program's `(1,2,0,1)` tuple
therefore becomes adjusted pair `(2,2)`, bucket `0x18000`, relocation id 3.
The class-3 MISC source is not a T0 vertex varying.  No FVF mutation was
found in the pixel compiler/finalizer, so fabricating a VS output for vPos
would change the resource and raster contract.  vPos and vFace compile
byte-identically; this oracle authorizes neither until live linkage is known.

## Clean-room seams and safety boundary

The current semantic UAPI has only TEMP/VARYING/CONSTANT source files in
`include/uapi/drm/chrome9_drm.h`.  `chrome9_fragment_validate_source()`,
`chrome9_fragment_source_id()` and `chrome9_fragment_encode()` in
`drivers/gpu/drm/chrome9/chrome9_fragment.c` need a distinct position source,
XY-only validation, the 3/4/7/15 MISC relocation, final-use accounting, and
the A90 usage bit.  Its MISC register must not be counted as an ordinary
external T coordinate: a naive class-3/id-3 resource scan would incorrectly
raise the active texture count to four.  The existing TEMP-to-TAU bridge
already describes the computed coordinate.  `chrome9_fragment_fe_config()`
and `chrome9_fragment_raster_state()` in `chrome9_engine.c` should continue
to count only real VS T outputs in A08/A25/A28; a future position source may
alter the PS resource controls, not the raster varying map.

Mesa needs a separate semantic source, support for NIR
`load_frag_coord`/`load_frag_coord_xy`, and `fs_position_is_sysval` only after
the native pixel-center/origin oracle.  The existing
`STATE_FB_WPOS_Y_TRANSFORM` must remain a draw-time constant: window and FBO
orientation differ, and an arbitrary render-target height cannot be baked
into a shader without a variant key.  Reject `gl_FragCoord.zw` until separately
proved.  Neither this object nor a successful vendor compile proves GPU
execution.

## Guarded silicon sequence

`tools/re/chrome9_vpos_texture_guard_smoke.c` is an opt-in raw cmd22 test
scaffold.  It refuses GPU submission until a future kernel explicitly
advertises `DRM_CHROME9_CAP_FRAGMENT_POSITION`.  The first live step should
still be the smaller MISC-identity/pixel-center oracle in
`chrome9-vpos-cmp-frc-contract.md`, with a known-good draw before and after.
Only then run the computed-coordinate smoke once on a fresh healthy boot:

1. One 64x64 BGRA target and D16 surface, each with full guard pages, plus
   one guarded 2x2 BGRA texture with distinct texels; preserve a full source
   snapshot.  Use the token-equivalent semantic `MUL(position.xy,c0.xy)`,
   TEX(s0, TEMP.xy), MOV sample to color; c0.xy is exactly 1/32.
2. NEAREST/REPEAT yields two repeats per axis across the 64x64 target.
   Interior samples at x/y = 12, 28, 44, 52 must form the exact repeated
   four-color grid.  D16 and BGRA must change on the same triangle pixels;
   exterior color/depth, all guards and the texture snapshot remain intact.
3. Check timeout/quarantine/reset counters and an ordered completion cookie;
   stop on the first timeout and cold-boot before any further GPU work.
   Follow with a known-good draw.  Then separately test `vPos` origin at
   nonzero viewport/scissor and the window/FBO Y-transform before Mesa
   exposes `gl_FragCoord`.

This deliberately excludes KILL and A8: first isolate vPos and the computed
coordinate bridge.  The later `ideas` shadow test adds A8 NEAREST/REPEAT,
literal-half SUB, KILL, and D16 ordering only after each primitive passes.
