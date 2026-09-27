# Chrome9 HD hardware TCL: vertex shader, fetch and draw RE

Status: reverse-engineering ahead of implementation, 2026-09-24.  No row in
this note has executed on VX900 silicon in the new driver; nothing here was
run on the target.  The goal it serves is native vertex processing (the
programmable VS plus fixed-function transform/lighting lowered to the VS)
instead of Gallium `draw`, for glxgears above 100 fps and ioquake3 GL1/GL2.

It consolidates and extends [via-5.75-shader-re.md](via-5.75-shader-re.md),
[shader-isa.md](shader-isa.md), [shader-upload-oracles.md](shader-upload-oracles.md),
[chrome9-vs-varying-fvf.md](chrome9-vs-varying-fvf.md),
[general-shader-runtime-contract.md](general-shader-runtime-contract.md) and
[indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md).  Where this
note corrects one of them, the correction is called out in
[Corrections to earlier notes](#corrections-to-earlier-notes).

## Evidence classes and binaries

| label | meaning |
|---|---|
| **dynamic** | produced by calling the vendor code on this host (native 32-bit `dlopen`, or Wine for the Windows UMD); byte-exact, reproducible |
| **static** | read from disassembly/decompilation; addresses given |
| **cross** | the Linux 5.75 and Windows 2015 code agree independently |
| **inference** | a reasoned interpretation; never used as a byte recipe on its own |
| **silicon** | executed on VX900 (only the existing Xorg/post-TNL oracles) |

Addresses are file VMAs of
`binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (32-bit, symbols,
BuildID `f140f76c...`) unless prefixed `VTGUModeDX32A.dll:`.  Ghidra outputs
kept under `~/tmp/re-tcl/*/` are shifted by `+0x10000` relative to these
addresses.

Tools added for this note (all hardware-free, GPL-2):

| tool | purpose |
|---|---|
| [`tools/re/chrome9_tcl_vs_oracle_probe.c`](../../tools/re/chrome9_tcl_vs_oracle_probe.c) | Linux PVSCP parser, validator, dispatcher, postprocess on raw vs_2_0 tokens; prints native records, metadata and the input remap |
| [`tools/re/chrome9_tcl_vs_oracle_wine.c`](../../tools/re/chrome9_tcl_vs_oracle_wine.c) | same four stages of the Windows compiler under Wine, interchange directory from `CHROME9_VS_DIR` |
| [`tools/re/chrome9_tcl_vs_oracle.py`](../../tools/re/chrome9_tcl_vs_oracle.py) | vs_2_0 mini-assembler, 86-case corpus generator, `--check` (regression) and `--cross` (Linux/Windows) |
| [`chrome9-hw-tcl-vs-corpus.json`](chrome9-hw-tcl-vs-corpus.json) | the byte-exact Linux+Windows corpus (SHA-256 `e80a79f4...a7af`) |
| [`tools/re/chrome9_tcl_const_viewport_probe.c`](../../tools/re/chrome9_tcl_const_viewport_probe.c) | runs `STM_ViewPort_INV`; prints internal constant rows 256..258 and A40..A48 |
| [`tools/re/chrome9_tcl_stream_probe.c`](../../tools/re/chrome9_tcl_stream_probe.c) | runs `stmUpdateVertexInput_H5`; prints A00..A10, A12, A62 and the `3003` stream words |
| [`tools/re/chrome9_tcl_ffvs_gl_probe.c`](../../tools/re/chrome9_tcl_ffvs_gl_probe.c) | runs the real GL fixed-function state updaters and `__glFFVSGenerate`, captures the vs_2_0 tokens, compiles them and prints records plus the constant image |
| [`tools/re/chrome9_tcl_ffvs_corpus.py`](../../tools/re/chrome9_tcl_ffvs_corpus.py), [`chrome9_tcl_ffvs_disasm.py`](../../tools/re/chrome9_tcl_ffvs_disasm.py) | 19-key fixed-function golden replay (all pass) and token/record disassembly |
| [`tools/re/chrome9_tcl_win_model.py`](../../tools/re/chrome9_tcl_win_model.py) | Python model of the Windows declaration, stream, viewport and draw recipes (self-test reproduces Xorg A40..A42) |

Running the 32-bit DSO natively needs only `libexpat.so.1` and `libdrm.so.2`
from the i786 T2 sysroot on `LD_LIBRARY_PATH` (copied to `~/tmp/re-tcl/lib32`);
mixing the sysroot libc with the host loader fails.  Reproduce the corpus:

```sh
gcc -m32 -O1 -o ~/tmp/re-tcl/A/oracle tools/re/chrome9_tcl_vs_oracle_probe.c -ldl
clang --target=i686-w64-windows-gnu -fuse-ld=lld -nostdlib \
  -Wl,--no-insert-timestamp -Wl,-entry,mainCRTStartup -Wl,-subsystem,console \
  -L/usr/lib64/wine/i386-windows tools/re/chrome9_tcl_vs_oracle_wine.c \
  -lkernel32 -o ~/tmp/re-tcl/A/w/probe.exe
LD_LIBRARY_PATH=~/tmp/re-tcl/lib32 python3 tools/re/chrome9_tcl_vs_oracle.py \
  --probe ~/tmp/re-tcl/A/oracle --wine-exe ~/tmp/re-tcl/A/w/probe.exe \
  --wine-dir $HOME/tmp/re-tcl/A/w --out corpus.json
python3 tools/re/chrome9_tcl_vs_oracle.py --cross corpus.json
```

Probe SHA-256 in this run: Linux harness `f583dd81...0962`, Wine harness
`ec17989d...5fe3`.  `--check` against the stored corpus reports
`86 cases, 0 mismatches`.

## Summary

1. The vendor Linux GL driver does real hardware TCL: a programmable VS
   store, a VS-emulated fixed-function pipeline, and hardware vertex fetch
   from memory with one stream slot per element.  Indices are always inline
   in the command stream, including generated `0,1,2,...` for non-indexed
   draws (static).
2. Encodings are recovered for every vs_2_0 instruction the vendor
   compilers emit (both silently drop `ifc` and `breakc`); 86 compiler cases
   are byte-identical between Linux and Windows in their semantic prefix
   except two explained differences (cross, dynamic).  Execution semantics
   of op 14 and of flow control remain unmeasured.
3. A compiled VS always ends with a finalizer that converts clip-space
   position to the raster position using internal constant rows 256/257
   (unified sources 224/225), uploaded as 12 dwords at selector `32030400`.
   Their exact viewport formula, including the `786432` sub-pixel snap bias
   and the `-0.5` pixel-centre shift, is recovered and reproduced (static +
   dynamic).
4. User VS float constants start at selector `32030000`; the earlier
   `32030400` user-constant statement was a count/selector mix-up (static).
5. vs_3_0 has no hardware path; only VS 1.1/2.0 parsers exist, and the
   2.0 parser ignores the version token (dynamic).

## VS native instruction format

Each instruction is four little-endian dwords: three source words and one
destination/control word.  All field positions below are **cross** and
**dynamic** (corpus), extending the table in via-5.75-shader-re.md.

### Source word (dwords 0..2)

| bits | field |
|---:|---|
| 0 | negate |
| 13:11, 10:8, 7:5, 4:2 | X, Y, Z, W selector: 0..3 = x,y,z,w; 4 = literal 1.0; 5 = literal 0.0; 6/7 unassigned |
| 22:14 | unified source index (9 bits) |
| 31:23 | zero in every compiled record |

Unified source namespace (dynamic unless noted):

| unified | file | notes |
|---:|---|---|
| 0 | don't-care | only as op-26 third source in Linux output (see below) |
| 32..47 | inputs v0..v15 | dense **first-use** order after `PVS_ReMapInputReg_H5i` (0x18faf0); `mov oPos,v3; mov oD0,v1` gives native v0,v1 |
| 64..79 | temporaries r0..r15 | reallocated by lifetime (`VS_CalTempRegLifeCycle_H5i` 0x18e260, `VS_ReMapTempReg_H5i` 0x182980); source bits 18:14 hold the physical id |
| 96..111 | i0..i15 | loop/rep counters only |
| 112..127 | b0..b15 | A61[15:0] |
| 128 | internal scratch | written by destination field `0x80`; used by every multi-record lowering and by the position finalizer |
| 130 | literal-carrier | appears with all-literal selectors (`0020a490` = 1.0 vector) in branch records |
| 224..255 | internal constant rows 256..287 | 224/225 viewport scale/offset, 226 bias, 227..232 user clip planes, 233 `(2,4,0.5,1/pi)`, 234/235 immediate integers (static, fork C) |
| 256..511 | c0..c255 | row R at H3 selector `32030000 + 4*R` |

The literal selectors mean one and zero independently of the register id:
the compiler writes `00206490` (all ones) and `00206db4` (all zeros) with id
129, but `PVS_SWZ_ICD` proves the id is irrelevant for literal lanes.

### Destination/control word (dword 3)

| bits | field |
|---:|---|
| 3:0 | write mask X=1, Y=2, Z=4, W=8 |
| 11:4 | destination (8 bits, see table) |
| 14 | saturate (also the CALL "push" flag on op 29) |
| 21:17 | operation |
| 23 | return-after-this-instruction (last record of a subroutine; `0084010f`) |
| 27:24 | relative selector: 0 absolute, 4..7 = a0.x..a0.w, 8 = aL |

| destination field | meaning |
|---:|---|
| `0x01` | a0 (MOVA only; mask selects components) |
| `0x10..0x1f` | r0..r15 physical temporaries |
| `0x20` | oPos (32), clip position |
| `0x21` | raster position (33), written only by the finalizer |
| `0x22`, `0x23` | oD0, oD1 (compiler always sets saturate) |
| `0x26`, `0x27` | oFog (saturate except in context mode 8), oPts |
| `0x28..0x2f` | oT0..oT7 |
| `0x80` | internal scratch (read back as source 128) |
| `0xa0` | user oPos write: scratch plus oPos bits (`PVS_oPosHandling_H5` sets bits 9:8=2 and bit 11 on every oPos write, 0x181c50) |

Whether a `0xa0` write also reaches output 32 is **unknown**: the Linux
finalizer's `MOV dest32, src128` records are uploaded but lie outside A60's
active range, whereas in the CALL/RET variant the `MOV dest32,r1` is active.
Reproduce the vendor records exactly rather than rely on either reading.

### Operation selector (dword 3 bits 21:17)

All rows are **dynamic + cross** (corpus case names in brackets) unless noted.
Byte examples use `v0`/`c0` into `r0` (`...010f`).

| op | name | example record | lowering notes |
|---:|---|---|---|
| 2 | MAD | `0008014c 0040014c 0040414c 0004010f` [mad] | ADD = `a*1+b` (`00206490`); SUB = ADD with negated src2; MUL = `a*b+0` (`00206db4`); DST = one MAD with literal lanes (`00082150 0040218c 00206db4`) |
| 3 | DP4 | `0008014c 0040014c 00000000 0006010f` [dp4] | DP3 = DP4 with both sources' W selector literal 0 (`...0154`); DPH (GL) forces src0 W = 1 |
| 4 | MOV | `0008014c 00000000 00000000 0008010f` [mov] | |
| 5 | MOVA | `00080000 00000000 00000000 000a0011` [mova] | dest `0x01`, mask per component; D3D rounding semantics unmeasured |
| 6 | FLR | `000c010f` (GL `PVS_FLR_ICD`, static/dynamic earlier) | GL ARL = FLR into scratch then MOVA |
| 7 | FRC | `0008014c 00000000 00000000 000e010f` | |
| 8 | MAX | `0008014c 0040014c 00000000 0010010f` | ABS = `MAX(a,-a)` |
| 9 | MIN | `...0012010f` | |
| 10 | SGE | `...0014010f` | |
| 11 | SLT | `...0016010f` | |
| 12 | SGN | `0008014c 00000000 00000000 0018010f` | D3D temp operands dropped |
| 14 | finalizer only | `00200150 00101b6c 00201b6c 001c0107` | `op14(clip.xyz1, rcpw.wwww, clip.wwww)`; semantics unknown (guarded perspective multiply is an inference) |
| 16 | EXP | `00080000 00000000 00000000 0020010f` | vs_2_0 EXPP also compiles to 16 |
| 18 | LOG | `...0024010f` | vs_2_0 LOGP also compiles to 18 |
| 20 | RCP | `00081b6c 00000000 00000000 0028010f` | scalar source, broadcast result |
| 21 | RSQ | `00080000 00000000 00000000 002a010f` | |
| 22 | SINCOS | `00200000 00000000 00000000 002c0103` | preceded by `MAD scratch = x * c233.w (1/pi) + 0`; D3D coefficient constants ignored |
| 24 | LIT | `0008014c 00000000 00000000 0030010f` | |
| 26 | matrix final | `0008014c 0040c14c 0000014c 0034010f` | last row of M4x4/M4x3/M3x4/M3x3/M3x2: dot product into the last masked lane, earlier lanes merged from scratch rows |
| 28 | REP/LOOP | `00180154 00280400 00300400 00380000` | src0 = i#; dword2 bits 18:10 = body length (`end-start-1`) |
| 29 | branch/IF/ELSE/CALL | `001c014c 00280400 00300800 003a0000` | see flow control |

Multi-record lowerings (dynamic, byte-identical on both drivers):

```text
LRP r0,c0,v0,v1  0008014c 00206490 0008414d 0004080f   scratch = v0 - v1
                 0040014c 0020014c 0008414c 0004010f   r0 = c0*scratch + v1
POW r0,v0.x,c0.x 00080000 00080001 00000000 0010080f   scratch = |x|
                 00200000 00000000 00000000 00240801   scratch.x = log
                 00200000 00400000 00206db4 00040802   scratch.y = *c0.x
                 00200924 00000000 00000000 0020010f   r0 = exp(scratch.y)
NRM r0,v0        00080154 00080154 00000000 0006080f   scratch = dp3
                 00201b6c 00000000 00000000 002a080f   scratch = rsq(w)
                 0008014c 0020014c 00206db4 0004010f   r0 = v0*scratch
CRS r0.xyz,v0,c0 00081034 00400a14 00206db4 00040807
                 00080a14 00401034 00200155 00040107
M4x4 r0,v0,c0    0008014c 0040014c 00000000 00060801   scratch.x
                 0008014c 0040414c 00000000 00060802   scratch.y
                 0008014c 0040814c 00000000 00060804   scratch.z
                 0008014c 0040c14c 0000014c 0034010f   r0 (op 26)
SINCOS r0.xy,v0.x 00080000 003a5b6c 00206db4 0004080f  scratch = x/pi
                 00200000 00000000 00000000 002c0103
```

### Relative addressing, address register and flow control

Dynamic, cross:

```text
mova a0.x,v0.x ; mov oPos,c5[a0.x]
  00080000 00000000 00000000 000a0011
  0041414c 00000000 00000000 04080a0f     # relative nibble 4 = a0.x
mova a0,v0 ; mov oPos,c5[a0.y]      -> 000a001f, then nibble 5
mova a0.z / a0.w                     -> 000a0014 / 000a0018, nibble 6 / 7
loop aL,i0 ; add r0,r0,c1[aL]        -> body 0010014c 00206490 0040414c 0804010f
```

The relative selector is instruction-wide; a second relative source with a
different component is silently lost (via-5.75-shader-re.md).

Branches all use op 29 with a condition source and two 9-bit displacements in
bits 18:10 of dword 1 (taken when the condition is true) and dword 2 (taken
when false); the displacement words carry fixed upper bits (`0028xxxx`,
`0030xxxx`):

```text
if b0; add; endif          001c014c 00280400 00300800 003a0000   true +1, false +2
if b0; add; else; add; endif
                           001c014c 00280400 00300c00 003a0000   false -> else+1
                           0020a490 00280800 00000000 003a0000   ELSE: literal-true, +2
call l0 (target +8)        0020a490 00282000 00300400 003a4000   bit 14 = push
callnz l0,b0               001c014c 00282000 00300400 003a4000
subroutine last record     0010014c 00206490 0040014c 0084010f   bit 23 = return
rep i0 / loop aL,i0        00180154 00280400 00300400 00380000   dword2 = body length
```

The two-way reading of op 29 is an **inference** from these fixed forms;
silicon branch/stack behaviour and latency are unmeasured.  Both vendor
compilers silently drop `ifc` and `breakc` (the guarded body becomes
unconditional; cases `ifc_lt`, `breakc_in_rep`), so neither is a usable
oracle for those instructions.  A CALL program changes the finalizer: oPos
goes to a temporary (`0008011f`), `MOV dest32,r1` becomes active and the
finalizer works in the scratch register; object `+0` becomes the temp count
2.

### Limits

| resource | limit | evidence |
|---|---|---|
| active records | A60[8:0] last index, 512 encodable | static; vendor masks, does not reject (via-5.75-shader-re.md) |
| inputs | 16 (4-bit element VS-input field, 5-bit remap) | static |
| temporaries | 16 native (4-bit dest id); `temps_16_live` compiles, object `+0 = 0x10` | dynamic; VS 2.0 exposes 12 |
| float constants | 256 user rows + internal rows 256.. | static |
| address register | a0.xyzw + aL | dynamic |
| constants per instruction | three different c# in one MAD compile directly (`0040014c 0040414c 0040814c`) | dynamic; no port-limit splitting by the compiler, silicon unverified |

The Linux object keeps 256 normalized and 295 native records without bound
checks; Mesa must preflight (via-5.75-shader-re.md).

## Compiler object metadata and activation words

Dynamic (corpus `meta`), static sources from fork C:

| object | meaning | consumer |
|---:|---|---|
| `+0x00` | physical temporaries used (minimum 1: the finalizer uses r0) | A61 bit 23 = (`+0 <= 8`) |
| `+0x04` | output linkage descriptor (bit 2 oPos, 5 oPts, 6 oD0, 7 oD1, 13 oFog, 11:8 texcoord count, 2-bit widths at 16+2n) | A25/A26/A28[18:17] |
| `+0x08` | never nonzero for vs_2_0 in 86 cases | A61[22:20] |
| `+0x0c` | never nonzero for vs_2_0 | A61[19] |
| `+0x10` | register-pressure-like value; `temps_12_live`=11, `temps_16_live`=15, one output =0, oPos+oD0 =1, 4 partial oPos DP4 + oD0 =4; **reads uninitialized state when no v# is used** (varies run to run) | A62[8:0] |
| `+0x14` | active record count | A60[8:0] = `+0x14 - 1` |
| `+0x18` | `ffffffff` (oPos temp index when redirected) | finalizer |
| `+0x28` | `+0x14 - 1` | |
| `+0x424` | uploaded records = active + 2 inactive tails | H3 `31030000` count/4 |

Activation words (static, cross; `stmUpdateVS_inv` 0x17c4c0 and
`VTGUModeDX32A.dll:0x1015ac6d..0x1015ad13`), saved image base `state+0x20bc`:

```text
A60 = 60800000 | (active - 1)            # bits 20:12 cleared, bit 23 set
A61 = init | (temps <= 8) << 23 | (+8 & 7) << 20 | (+0xc != 0) << 19 | bool_mask
A62 = init | (inputs - 1) << 20 | (+0x10 & 0x1ff)
      # init: bit 15 set for 1106:7122 (bit 14 otherwise), inputs-1 = 0xf when none
emit order: <3203 constants> A61 A62 A60 <31030000 upload>
```

A62[23:20] is the input-register count minus one written by
`stmUpdateVertexInput_H5` (0x17d04a/0x17d2b0); this explains Xorg's
`622081ff` (v0..v2) and `623081ff` (v0..v3).  A61 bits 18 and 16 follow an
init-time manager flag (0x179313/0x178d15) whose meaning is open.

## Position finalizer, viewport and internal constants

Every compiled program appends (dynamic, Linux):

```text
00201b6c 00000000 00000000 00280108   RCP  r0.w   = 1 / src128.w
00200150 00101b6c 00201b6c 001c0107   op14 r0.xyz = f(src128.xyz1, r0.wwww, src128.wwww)
0010014c 00380150 00384154 0004021f   MAD  pos33  = r0 * src224.xyz1 + src225.xyz0
0020014c 00000000 00000000 0008020f   MOV  dest32 = src128   (inactive)
0020014c 00000000 00000000 0008020f   MOV  dest32 = src128   (inactive)
```

The Windows compiler prepends two records implementing the D3D-to-GL depth
remap `z' = (z + w) * c233.z`:

```text
00201248 003a6d54 003a6db4 00040804   scratch.z = scratch.z * 0.5
00201b6c 003a6d54 00201248 00040a04   scratch.z = scratch.w * 0.5 + scratch.z
```

The raster position is therefore `(x/w*sx+ox, y/w*sy+oy, z/w*sz+oz, 1/w)`,
the same tuple the live post-TNL path feeds directly (Xorg VS writes
destination 33 with `v0*src224.xyz1 + src225.xyz0`).  Feeding Gallium
`draw` output through a compiled program would transform it twice.

### Internal constant rows (static, fork C; cross with Windows)

| row | unified | upload | contents | producer |
|---:|---:|---|---|---|
| 256 | 224 | `32030400`, 12 dw (rows 256..258) | `(sx, sy, sz, 0)` | `STM_ViewPort_INV` 0x1bab80; upload `stmUpdateVS_inv` 0x17c6a5/0x17c9ad |
| 257 | 225 | same | `(ox, oy, oz, 0)` | same |
| 258 | 226 | same | `(786432, 786432, 0, 0)`, zeros for points and A23 type 7 | same |
| 259..264 | 227..232 | `3203040c`, 24 dw | user clip planes (DP4 per enabled plane into spare texcoord lanes) | `stmPVSUpdateDirtyConst_H5` 0x17be12/0x17c3d5 |
| 265 | 233 | `32030424`, 12 dw (rows 265..267) | `40000000 40800000 3f000000 3ea2f983` = `(2, 4, 0.5, 1/pi)` | `stmInitChipImage_inv` 0x178d1c; upload 0x17bf2a |
| 266..267 | 234..235 | `32030428`, 8 dw | the shader's `defi` values as floats | `PVSCP_VALIDATE_VS20_H5` 0x18fcb8 |

Windows uploads the same 12 dwords at `VTGUModeDX32A.dll:0x1015ada1` and
writes row 265 at `0x10155208`.

Viewport formula for a GL window drawable (static decompile of
`STM_ViewPort_INV`, `__glDpSTMUpdateViewport` 0x1ec240; dynamic via
`chrome9_tcl_const_viewport_probe`):

```text
Ytop = drawable_height - glY - H           # caller converts GL bottom-left origin
sx = W/2        ox = X    + W/2 - 0.5 + 786432
sy = -H/2       oy = Ytop + H/2 - 0.5 + 786432
sz = f - n      oz = n                      # f-n scaled by ctx+0x3838 when nonzero
bias 786432 = 0x49400000 = 1.5*2^19 (1/16-pixel snap) only when A23 low nibble is
not 0 (points) and not 7; rows 258.xy carry the same value.
```

Dynamic example, 640x480 window, triangles:
`43a00000 c3700000 3f800000 00000000 | 494013f8 49400ef8 00000000 00000000 |
49400000 49400000 00000000 00000000`.  For a renderbuffer/FBO the vendor
uses `sy=+H/2`, `oy=H/2-Y`, which for `Y != 0` looks like a vendor quirk and
is unverified.  GL's clip-space depth needs either the vendor `sz=f-n, oz=n`
plus the two Windows remap records, or equivalently `sz=(f-n)/2,
oz=(f+n)/2` with the Linux tail; the second form needs no extra records and
no row 265.

The same values also populate the fixed-function viewport words A40..A48
(low 24 bits per word, high bytes packed in A42/A45/A48, e.g.
`00a00000 004013f8 00004943 ...`); the programmable finalizer reads only
rows 256/257.

## VS constant uploads

Static, fork C (`stmPVSUpdateDirtyConst_H5` 0x17bc60):

```text
full:   fe03043c 00000400 32030000 cc000000  <1024 dwords, c0..c255>
dirty:  fe03043c <4*K>    32030000|(4*N)  cc000000  <cN..cN+K-1>
ints:   fe03043c 00000010 33030000 cc000000  <16 packed XYZW bytes>   (Windows form)
bools:  A61[15:0]
```

The earlier statement that the full user upload uses selector `32030400`
confused the count `0400` with the selector.  There is no overlap between
user constants and the internal rows.  Dirty flags are bytes at
`ctx+0x4638`, scanned 32 at a time.

## Vertex fetch: streams and declarations

The hardware-TCL state table on H5/H6 (`0x427660..0x427c00`) has an empty
`STM_SetStreamSource_H5` (0x1bb920); `STM_FVFSetting_H5` only marks state
dirty.  The real builder is `stmUpdateVertexInput_H5` (0x17cbb0), installed
by `stmCreate_inv` at 0x179e88.  `STM_SetStreamSource_INV`/`STM_FVFSetting_INV`
belong to the non-TCL table and use the older A21/A22 single-stream words
(static, fork B).  Windows `VTGUModeDX32A.dll:0x10159670` implements the same
recipe (static, fork E), so the following is **cross**; the element and
stream words were also produced **dynamically** by
`chrome9_tcl_stream_probe`.

```text
element i (i = 0..15):
  A(1+i)    = (1+i)<<24 | addr[9:2]<<12 | vs_input<<8 | format     # [23:20] = 0
  stream[i] = (addr & 0xfffffc00) | ((stride >> 2) & 0x1ff)        # slot i = element i
  addr      = buffer_gpu + stream_offset + element_offset + first_vertex*stride
A00 = (A00 & 0xfffff080) | (elements-1)<<8 | sum(element dwords)
      | [16:15]=01 for the H6/SF memory type (Linux: memory type 4 and ctx flag +0x69 bit 6)
A12[15:0]    = mask of VS input registers used
A62[23:20]   = input registers - 1
fe03043c <n> 30030000 cc000000 <stream[0..n-1]>
```

* There is exactly one stream slot per element; the vendor never shares a
  base between elements and never writes element bits 23:20.  Whether the
  hardware reads 23:20 is unproven, so copy the vendor form.
* Address bits 9:2 fold into the element word; bits 1:0 are dropped.  The
  only stride check is `< 0x801` (`__glS3InvConfigVertexStream`), so a stride
  of exactly 2048 encodes as pitch 0 (vendor bug); a constant (array-less)
  attribute is a float4 in a buffer with stride 0 (`stream = ...00300000`
  in the probe).
* `vs_input` for a programmable VS is the index in the shader's dense
  first-use input table (`parsed+0x15d8` count, 12-byte
  `{usage, index, reg}` entries); for fixed function it is the dense rank of
  the usage bit.  Windows assigns the index of the matching DCL and gives an
  unmatched element index = DCL count.
* First-vertex / base-vertex offsets are added to every element address,
  i.e. re-folded into stream bases and element bits 19:12.

Format byte: bits 7:6 = components-1, bits 5:0 = type (cross; Windows
tables 0x103334b8/0x10333504, Linux `stmMapDeclFormat_invi_H5` 0x1760e0):

| type | code | D3D bytes |
|---|---:|---|
| float32 | `02` | FLOAT1..4 `02 42 82 c2` |
| half float | `04` | FLOAT16_2/4 `44 c4` |
| UBYTE | `06` | UBYTE4 `c6` |
| UBYTE normalized | `07` | UBYTE4N `c7` (GL UNSIGNED_BYTE[4] normalized) |
| USHORT | `08` | not used by D3D |
| USHORT normalized | `09` | USHORT2N/4N `49 c9` |
| SHORT | `0a` | SHORT2/4 `4a ca` |
| SHORT normalized | `0b` | SHORT2N/4N `4b cb` |
| D3DCOLOR (BGRA, normalized) | `11` | `d1` |
| packed 10:10:10 | `0c`, `0f` | UDEC3 `8c`, DEC3N `8f` |

GL BYTE, INT, UINT and DOUBLE arrays are converted on the CPU
(`conversion_table_inv` 0x3f8f60); the direct table is `stmtype_table_inv`
0x3f8fc0.  GL attribute to usage: 0 POSITION, 1 BLENDWEIGHT, 2 NORMAL, 3/4
COLOR0/1, 5 FOG, 8..15 TEXCOORD0..7, 16..31 generic (usage 2, index 0..15)
(`stmdecl_usage_table` 0x429220).

Dynamic example (float3 position + D3DCOLOR, one 16-byte interleaved buffer
at GPU `0x00100000`, SF/H6 domain):

```text
A00=00008104 A01=01000082 A02=020031d1 A12=12000003 A62[23:20]=1
fe03043c 00000002 30030000 cc000000 00100004 00100004
first=3:  A01=0100c082 A02=0200f1d1 (streams unchanged)
```

## Draw packets and command order

Every hardware-fetch draw is an **indexed** draw with inline indices (cross:
Linux `__glS3H5DrawPrimitiveTriangleList` 0x1e7490 /
`__glS3H5DrawIndexedPrimitiveTriangleList` 0x1e3240; Windows DrawPrimitive
0x101b1b60, DrawIndexedPrimitive 0x101b15e0).  There is no index-buffer
address in the H6 command set.

```text
A20[1:0] = 01 (16-bit) | 11 (32-bit); 00 = inline vertices
A2D = 2d000000 | (min & 00ffffff)
A2C = 2c000000 | (bound & 00ffffff)     # Linux: max index (inclusive)
                                        # Windows: MinIndex + NumVertices (exclusive)
A2E = 2e000000 | (max >> 24) << 8 | (min >> 24)
per chunk:
  fe020440 0000043c 00040000 cc000000  24<count> cc000000 ...
  fe03043c <index dwords> 00000000 cc000000  <inline indices>
```

* Non-indexed draws generate indices `0..n-1` on the CPU, packed
  `((i+1)<<16)|i`, 16-bit below 65536 vertices, else 32-bit.  The unused lane
  of an odd count holds the next index, not zero; a new driver should write
  zero (it lies outside A24).
* Chunk size is bounded by command-buffer space, rounded to whole primitives:
  lists to multiples of 3, strips overlap by 2, fans repeat the first index.
* The Linux "`_VM`" variants (e.g. 0x1dfde0, 0x1da7a0) gather vertices on the
  CPU into selector-0 inline packets; they are used for 8-bit indices, for
  memory types 1/8, and for mixed memory types, because the A00 domain is
  global.  `__glDpHwDrawTNLedPrim_Begin` (0x1fe1b0) is the software-VS route
  with one pre-transformed stream.
* The inclusive/exclusive A2C difference is unresolved; the inclusive Linux
  value is the tighter one and matches the documented `{0,1,2}` oracle.

Complete Windows per-draw emission order (static, fork E; validator
0x100ff570, function table 0x103309c8 index 4):

1. dirty handlers update the image only (viewport 0x101f7570, declaration
   0x10159670, A20/A2C 0x101f5e20);
2. `33030000` integer constants and A61 Boolean bits (0x101593b0);
3. VS (0x1015abb0): `32030400` 12 internal dwords, user constants
   (`32030000` full or dirty runs, then `3203040c` clip planes, `32030424`
   or `32030428`), A61, A62, A60, then the `31030000` program;
4. PS (0x1015b000): A90..A9b, `20030000`, `21030000`;
5. state flush (0x10156b20, gathered by 0x10150be0): type-01 blocks, FE02,
   `14030000`, `00030000`, `11030000`, type-04 A00..A1F (declaration),
   A20..A3F (A25/A26/A28 linkage), A40..A5F (viewport), A60..A7F, FE00,
   `3003` streams, per-unit texture state, `1003` mip tables;
6. primitive packets (A24 + inline indices).

The Linux flush (fork B) has the same relative order: type-01 groups, H3
`1403/0003/1103`, dirty type-04 words, FE, `3003` streams, texture state.
Windows also ORs `0x540000` into A00 on 1106:7122 (`0xa80000` on other
chips) and initializes A28=`28f9ffff`, A52=`52147fff`; the A00 bits are not
present in the live Xorg `00000206` word and are unexplained.

## Fixed-function TCL emulated by the VS

The vendor GL driver lowers fixed-function vertex state to a **vs_2_0 token
stream** and compiles it with the same PVS compiler as user shaders (static
+ dynamic, fork D):

* `__glDpUpdateVertexShaders` 0x1ef0c0 -> `__glFFVSUpdateVS` 0x24a5b0 ->
  `__glFFVSNeedRegenerate` (256-bucket cache keyed by 24 state bytes) ->
  `__glFFVSGenerate` 0x247570 -> `stmCreateVertexShaderFunc` 0x24939e ->
  `PVScpCreateVSFunc` (parse/validate/dispatch/postprocess 0x181715..0x181747).
* FF handles are negative; `hwctx+0x4fcd = handle>>31` selects
  `stmPVSCompile_H5` in `stmUpdateVS_inv` 0x17c52a.  The older
  `stmFFVS*_H5i` builders are reached only via `stmFFVSCompile_H5` for
  non-negative D3D FVF handles and are not the GL lighting path (static,
  medium confidence).
* `__glFFVSLightCompute` points to `_Looped` for both chip groups
  (0x1d8669, 0x1d878d): up to four lights of one type are unrolled; five or
  more emit `defi i0,(N,0,1,0)` + `loop aL,i0` with aL-relative constants
  (dynamic).  Vendor GL therefore relies on VS LOOP/aL, which is unproven
  on silicon; a Mesa lowering should unroll.

State key: 24 bytes at `hwobj+0x1c4+4`; lighting S+7 bit 7, fog S+7 bits
6:4, local viewer S+7 bit 2, two-side S+7 bits 1:0, single-color S+7 bit 3,
normalize S+0xf bit 7, per-type light counts in S+8/S+0xc, texgen 11 bits per
unit from S+0xa, texture-matrix flags S+0xb/S+0xf.

Constant map (index = vec4; static producers, dynamic values):

| cN | contents |
|---|---|
| c4..c7 | modelview rows (`__glFFVSUpdateModelviewConst` 0x241d70) |
| c8..c11 | MVP rows (`__glFFVSUpdateDelayConst` 0x2466c0) |
| c12..c14 | normal matrix rows (scaled by 1/row length with RESCALE_NORMAL) |
| c16, c17 | point size/min/max/clamped, distance attenuation |
| c18 | `(0, 0.5, 1, -1)` (`__glFFVSInitConstant`) |
| c19..c23.x | front diffuse, specular, emissive, ambient, shininess |
| c24, c25 | front ambient x model ambient; c24 + emissive (scene color, w=0) |
| c26..c32 | back-face equivalents of c19..c25 |
| c33 | light-model ambient |
| c34+k, c42+k, c50+k, c58+k | eye-space light position; attenuation (k0,k1,k2,spot exp); spot dir + cos(cutoff); half vector |
| c66/74/82+k | front ambient/diffuse/specular light x material products |
| c90/98/106+k | back products |
| c114/122/130+k | raw light colors (color-material path) |
| c204+4u..c207+4u | texture matrix of unit u |
| c232 | fog `(density*log2e, end/(end-start), density, 1/(end-start))` |
| c236+u | rectangle-texture scale `(1/w, 1/h, 0, 1)` |

Vendor defects to avoid: every light's half vector reads c58 (light 0);
unit 7's texture matrix (c232..c235) overlaps the fog constant c232.

glxgears key (LIGHT0 directional, light specular default 1, normalize,
cull, single color), 31 uploads, 29 active, `+0=5 +4=44 +10=6`, record
SHA-256 prefix `2d27101e51e14dc9`; generated tokens and native records
(dynamic, reproduced independently from fork D's probe):

```text
m4x4 r2,v0,c8 ; m3x3 r0.xyz,v1,c12 ; mov oPos,r2
dp3 r0.w,r0,r0 ; rsq r0.w,r0.w ; mul r0.xyz,r0,r0.w
mov r3,c25 ; mov r4,c18.x ; mov r2.xyz,c34 ; mov r5.xyz,c58
dp3 r2.x,r2,r0 ; dp3 r2.y,r5,r0 ; mov r2.w,c23.x ; lit r2,r2
mul r5,r2.y,c74 ; add r5,r5,c66 ; add r3,r5,r3
mul r6,c82,r2.z ; add r4.xyz,r6,r4 ; mov r3.w,c19.w ; add_sat oD0,r3,r4

0008014c 0042014c 00000000 00060801   0008014c 0042414c 00000000 00060802
0008014c 0042814c 00000000 00060804   0008014c 0042c14c 0000014c 0034010f
00084154 00430154 00000000 00060801   00084154 00434154 00000000 00060802
00084154 00438154 0000014c 00340117   0010014c 00000000 00000000 00080a0f
00104154 00104154 00000000 00060118   00105b6c 00000000 00000000 002a0118
0010414c 00105b6c 00206db4 00040117   0046414c 00000000 00000000 0008012f
00448000 00000000 00000000 0008013f   0048814c 00000000 00000000 00080107
004e814c 00000000 00000000 00080147   00100154 00104154 00000000 00060101
00110154 00104154 00000000 00060102   0045c000 00000000 00000000 00080108
0010014c 00000000 00000000 0030010f   00100924 0052814c 00206db4 0004014f
0011014c 00206490 0050814c 0004014f   0011014c 00206490 0010814c 0004012f
0054814c 00101248 00206db4 0004011f   0010414c 00206490 0010c14c 00040137
0044db6c 00000000 00000000 00080128   0010814c 00206490 0010c14c 0004422f
+ the five-record finalizer/tail
```

This production shader writes oPos (`0xa0`) before the M3x3 scratch rows
are dead and before other scratch-free work; `PVS_oPosHandling_H5` redirects
oPos to a temporary whenever a later record would write the scratch again.
Because the shipped GL driver draws correctly with this code, the `0xa0`
destination must deposit the clip position where the finalizer's source 128
reads it (strong inference).  Its LIT and the diffuse/specular
accumulation need numerical silicon checks.

Other keys (all in the replay corpus): unlit position+color (11 uploads:
`m4x4 r2,v0,c8; mov oPos,r2; mov_sat oD0,v1`), separate specular (oD1,
32 uploads), no light specular (25), T0 passthrough (13), T0+T1 lightmap
(15), linear fog (14; `MAD_sat oFog.x = -c232.w*|eye z| + c232.y`), the
ioquake3 GL1 combination T0+T1+linear fog (18 uploads, `+4=2244`), point,
spot, local viewer, color material, rescale and two-sided lighting.  Two-sided
lighting writes the back color to ATTROUT 2 (native destination 36,
`...0004424f`) and sets object `+0xc = 0x40`; A23 bit 7/back-color linkage
remains unproven, so keep two-sided lighting in software.  EXP/EXP2 fog
emits no oFog (the eye-z DP4 is dead code) and fog is computed elsewhere.

For Mesa this is a template, not a dependency: Mesa already lowers
fixed-function to NIR (`ffvertex_prog`), so the native backend only needs
the NIR operations the vendor output uses (DP3/DP4, M4x4 via DP4, RSQ, MAD,
MAX, LIT or its expansion, MOV with SAT to oD0).

## Windows cross-check summary

* The Windows D3D UMD implements the same stream, declaration, index and
  constant recipes (section above) and the same activation order
  A61/A62/A60 + `31030000` (static, fork E; `0x1015abb0`, `0x1015ac6d`).
* Its compiler output is byte-identical to Linux for 75 of 86 corpus cases.
  Nine differ only in the don't-care third source of the matrix-final op 26
  (Linux `0000014c`, Windows `0020014c`); both drivers ship working, which
  makes it very likely that op 26 ignores that register field (inference).
  The two CALL cases differ only by the Windows depth-remap records inside
  the relocated finalizer and the resulting call displacement (`00282c00`
  versus `00282000`).  All metadata agree except `+0x14`, which is two larger
  on Windows because of the two depth records (dynamic, `--cross`).
* Windows viewport (0x101f7570): same scale/offset/bias/-0.5 rules; D3D
  depth maps `sz = MaxZ-MinZ`, `oz = MinZ` after the `(z+w)/2` remap.
  Pre-transformed default `s=(1,1,1)`, `o=(786432,786432,0)` is exactly the
  Xorg c256/c257 pair.
* Pixel Z clip A1E/A1F uses the same low-24/high-byte split; default
  min=`ff000000`, max=`7f000000`, consistent with the kernel's `20007fff`.

## Corrections to earlier notes

1. **User VS float constants begin at `32030000`**, not `32030400`
   (shader-isa.md "full vertex-float-constant path", general-shader-runtime-
   contract.md).  `0x400` was the payload count.  Internal rows begin at
   `32030400`; there is no overlap.
2. shader-isa.md states that Linux reproduces the Windows one-light
   lighting records byte-for-byte.  Two of them differ (op-26 third source
   `0000014c` versus `0020014c`), reproducibly; see the Windows summary.
3. The VS destination field is 8 bits (`dword3[11:4]`), not the 6-bit
   `9:4` "unified index" of via-5.75-shader-re.md; `0x80` is the internal
   scratch register, not a0.  `tools/chrome9_vs_decode.py` still decodes the
   6-bit form and labels scratch writes as `a0` (not modified here).
4. indexed-vbo-topology-plan.md: streams are one slot per element with
   element bits 23:20 left zero, not a stream-ID palette; non-indexed
   hardware draws also use generated inline indices; stride 2048 is not
   encodable; pitch 0 is used for constant attributes.
5. The full vs_2_0 path compiles EXPP/LOGP to the full-precision selectors
   16/18; selectors 17/19 were seen only from direct emitter calls.
6. general-shader-runtime-contract.md "r0..r11" is the VS 2.0 API budget;
   the native file has 16 temporaries (`temps_16_live` compiles with object
   `+0 = 0x10`, destinations `0x10..0x1f`).
7. via-5.75-shader-re.md reports an end-of-body marker (dword3 bit 2) for
   IF/REP bodies.  With full-mask bodies (`if_b0`, `rep_i0`) the body record
   is identical to the unconditioned encoding; loop extent is carried only by
   the op-28/29 displacement fields.
8. A62[8:0] = object `+0x10` is not an output count; it behaves like a
   register-pressure value and is nondeterministic for programs without any
   `v#` read.

## Open unknowns

* op 14 semantics (only in the finalizer and `stmFFVS_Transform_H5i`
  0x1a1b38); whether the `0xa0` destination also writes output 32; whether
  the inactive `MOV dest32,src128` tails matter to the hardware.
* Numerical behaviour of every VS op on silicon (LIT, EXP/LOG precision,
  RCP/RSQ of 0/inf, SGE/SLT, FRC, MOVA rounding, SINCOS input in units of
  pi).
* Branch/loop/call execution (op 28/29, bit 14 push, bit 23 return), stack
  depth and loop-counter semantics of i# `(count, start, step)`.
* Whether three distinct constants (or inputs) per instruction execute
  without port conflicts.
* Object `+0x08`/`+0x0c` (A61[22:20]/[19]); `+0xc` is set to `0x40` by
  two-sided FF lighting, `+0x08` never observed nonzero.  A61 bits 18/16.
* Exact meaning of A62[8:0]; the vendor value is reproducible only for
  programs that read at least one input.
* Element bits 23:20 (unused by both vendors), A2C inclusive versus
  exclusive, A00 `0x540000` (Windows, VX900 only), A23 low nibble 7 (no
  bias), row 258 consumers, rows 266/267 in the Windows image.
* FBO viewport `oy = H/2 - Y` for `Y != 0`.
* Hardware behaviour for unaligned attributes (vendor drops address bits
  1:0 silently) and stride not a multiple of four.

## Silicon results (2026-09-24)

Kernel oracles `DRM_CHROME9_FIXED_QUAD_VS_*` (Chrome9 kernel commits
`e7bcaf9`, `54912ca`, `b50ab09` and the near-plane fix after them) replace
only the VS state of the proven 32x32 quad; runner
`tools/re/chrome9_vs_oracle.c`.  All ran under the GPU lock with FE33
enabled and produced no timeout, fault, canary or quarantine report.

| oracle | variable | result |
|---|---|---|
| base | corpus `out_d0_d1` program + finalizer, rows 256..258 (`41800000 c1800000 3f800000`, `494000f8 494000f8`, bias row), A60 `60800005`, A62 `62208002`, clip-space W=1 | pass, 1024/1024 |
| w2 | same with x, y, w doubled | pass (RCP/op 14 divide) |
| half | first triangle only (A24 = 3) | pass, 528 pixels, TL/TR/BR set, BL clear: `sy = -16` orientation confirmed |
| m4x4 | corpus M4x4 + `mov oPos,r0`, pixel-to-clip matrix c0..c3 at `32030000`, pixel vertices | pass (user constants, DP4, op 26) |
| clipwide | A01 bit 11 + A40..A48 from rows 256/257 (low 24 bits, exponent bytes in A42/A45/A48; reproduces the Xorg words), +-4 clip-space quad | pass, exact coverage |
| clipnear | same, triangle with one vertex behind the eye, near plane at w = 0.1 | first **fail** (107 unclipped pixels): the clip enable had been written to FE02 word 1 (texture attribute) instead of type-01 A01; with A01 `\|= 0x00100800` it **passes**, 568 pixels, 0 mismatches |
| clipneartail | clipnear with the first `MOV dest32, src128` tail active | same as clipnear (tail not needed) |
| fetch | W=1 quad from an SF page: A00 bit 15, three `30030000` stream slots `base | 6`, A20 bit 0, A2C/A2D/A2E, A24 = 4, inline indices `00010000 00030002` | pass, 1024/1024 |

So the programmable VS store, finalizer, viewport rows, user constants,
hardware homogeneous clipping (including the near plane) and indexed memory
vertex fetch are qualified; see
[chrome9-near-plane-clipping-re.md](chrome9-near-plane-clipping-re.md).  The
implementation plan is [chrome9-hw-tcl-implementation-plan.md](chrome9-hw-tcl-implementation-plan.md).

### Kernel-compiled programs through DRAW_VERTEX_PROGRAM

Kernel commits `4e7e5e0` (compiler), `4e69730` (stage A ioctl), `5fcfcd9`
(port rules, cull); runner `tools/re/chrome9_tcl_oracle.c` (64x64 target,
one triangle, compared pixel for pixel with the same triangle through
cmd22 at the -0.5 pixel-center bias).

| rung | program | result |
|---|---|---|
| L3 | `MOV oPos,v0; MOV oD0,v1` (BGRA8 `d1`), oD1 literal fill | pass, 1128 px, 0 mismatches |
| L4/L5 | `MOV oT0,v1` (FLOAT4), oD0 and oD1 literal fills | pass |
| matrix | four DP4 lanes into r0 with c0..c3 at `32030000`, pixel-space input | pass |
| L6 | MOV ADD SUB MUL MAD DP3 DP4 DPH MIN MAX ABS SGE SLT SGN FRC FLR into oT0 | exact (at most 1 LSB) |
| L6 | MAD v1,c0,c1 and MAD c0,v1,c1 | **wrong**: both constant slots read c1 (`v*c1+c1`); c0 twice is exact |
| L6 | MAD v1,v2,c1 | **wrong**: `v2*v2+c1`; two distinct temps (MAD t1,t2,c / MAD t1,t2,v) are exact |
| L6 | RCP/RSQ/EXP2/LOG2 of a constant, to an output or a temp | **wrong**: RCP(x) for x in (1,2) is 1/1.5, EXP2 similar, LOG2 always 0 |
| L6 | the same ops reading an input or a temp holding the constant | exact |
| L6 | swizzled constant reads (`.yyyy`, `.wwww`) in vector ops | exact |
| L7 | A01 bit 7, A23 `23004e02` (cull CW) / `23004f02` (cull CCW) | the screen-clockwise triangle disappears with CW only; next uncull draw exact |
| L11 | chains of 100..246 dependent ADDs plus fills and finalizer, up to 256 records | exact (1 LSB): the store holds at least 256 records |
| L13 | A23[14:12] = 0 (solid) | one color latched for the whole draw (the third vertex of the first triangle) |
| L13b | A23[14:12] = 3 (FlatC), two triangles, distinct vertex colors | each triangle fills with its own third vertex color: FlatC = GL provoking-last |
| A61 | bit 16 set | no visible effect |
| A61 | bit 18 set | **engine timeout**, needs a cold boot; masked in the RE knob |

Consequences, enforced by `chrome9_vertex_validate()`: at most one distinct
constant row and one distinct input per record, and scalar ops never read a
constant directly.  Userspace legalizes with a MOV into a temp.  The vendor
compiler emits `mad r0,v0,c0,c1` and `mad r0,v0,v1,v2` directly, so it
probably relies on state this driver does not set (open; see
[chrome9-vs-scalar-precision.md](chrome9-vs-scalar-precision.md) once written).

### Mesa integration (CHROME9_TCL=1)

`gallium/chrome9` commits `1fa63e1`, `b7a4fef` lower the Gallium VS NIR to
the kernel IR (`chrome9_tcl.c`): CB0 values feeding branches are folded
into per-draw variants (at most 32 per shader), loads are hoisted and the
remaining ifs flattened, multiply-adds fused, integers and booleans
lowered to float, VAR/TEX outputs mapped to fragment varyings n, and the
read-port rules legalized with temp copies.  Attributes are unpacked to
FLOAT4 on the CPU and submitted in chunks of up to 128 triangles.  Guarded
fragment domains are accepted when every guarded lane is proven from
literals, saturation or default-filled attribute lanes.

| workload | result |
|---|---|
| ioquake3 q3dm1 GL1 | 100 % of triangles on hardware TCL, image unchanged |
| ioquake3 q3dm1 GL2 | 99.5 % on hardware TCL (two guarded draws fall back), image unchanged |
| glxgears | 100 % on hardware TCL (flat faces via FlatC), 35 fps versus 58 fps post-TNL |
| glmark2 default set | validation unchanged; conditionals 2 -> 12 fps, jellyfish 2 -> 7, build 2 -> 5 |

The per-draw ioctl, full state rebuild and CPU attribute unpack dominate
small draws (glxgears); stage B GEM vertex buffers and multi-draw batching
are the performance follow-ups.

### Native vertex formats and stage B vertex buffers

Kernel `7f00ab5` adds the native element formats (FLOAT1..4 `02/42/82/c2`,
BGRA8 `d1`, RGBA8 `c7`/`c6`, RG16/RGBA16 UNORM `49/c9`, SNORM `4b/cb`,
SSCALED `4a/ca`, half `44/c4`); `tools/re/chrome9_tcl_oracle fmt` passes all
14 within 1 LSB.  Inline (stage A) vertices must be packed: A00[6:0] is the
sum of element fetch dwords and the command parser consumes exactly that
many dwords per vertex, so a padded stride stops the engine (observed once,
cold boot).  Mesa `12f631e` copies native attributes as-is.

Kernel `d538ba0` / Mesa `a155dfa` implement stage B (`buffer_count != 0`,
Mesa opt-in `CHROME9_TCL_BUFFERS=1`): one `3003` stream slot per element
(`address & ~0x3ff | stride >> 2`), element bits 19:12 = address bits 9:2,
A00 bit 15 (SF memory), A20 `20000001` (| 2 for 32-bit indices), A24 =
index count, A2C/A2E = inclusive rebased maximum, A2D = 0, then the counted
type-00 index payload.  The kernel rebases indices to their minimum, folds
`(min + base_vertex) * stride` into each element address, narrows to 16-bit
when the range fits, bounds-checks every fetched element against its GEM
object, checks float elements for Inf/NaN, and flushes only the fetched
byte range.  `tools/re/chrome9_tcl_oracle idx <step>` compares a Gouraud
quad against the identical stage A draw:

| step | variable | result |
|---|---|---|
| b2a | one interleaved BO, FLOAT4 + BGRA8, stride 20, 16-bit indices | pixel-identical |
| b2 | positions and colors in two BOs (element n uses slot n, bits 23:20 = 0) | pixel-identical |
| b3 | color element offset 0x404 (folded across the 1-KiB window) | pixel-identical |
| b4 | stride 28 with padding, A00[6:0] = 5 = fetch dwords | pixel-identical: A00 is the dword sum, not the stride |
| b5 | index range 90000, 32-bit indices | pixel-identical |
| b6 | base_vertex 3 with shifted data (kernel rebase) | pixel-identical |
| b9 | FLOAT3 position, stride 16 | pixel-identical (w = 1 fill) |
| b10 | RGBA8 `c7` color | pixel-identical to BGRA8 `d1` |

| b12 | stride-0 color buffer (kernel `f80fdfd`) | pixel-identical to the constant sent inline |
| b15 | last element ends at the last byte of the BO, three runs | pixel-identical, no fault (any read-ahead lands on mapped or scratch GART pages) |
| b16 | stride 2044 (pitch field 0x1ff) | pixel-identical |

B7 (several A24 chunks per job) runs in every DRAW_VERTEX_PROGRAMS batch,
B14 (`0x540000` NaN kill) is on by default (`fd0f932`), B8 (out-of-range
index ends the list) and B13 (clear A12 bit reads (0,0,0,1)) passed on
2026-09-26 (chrome9-tcl-vertex-formats-stage-b.md section 7).

Mesa (`a155dfa`, `b58338c`, `b309b8e`, default on with `CHROME9_TCL=1`)
rebases indices to the draw's biased minimum, lets elements of one BO and
stride share a buffer entry (rend2 binds one VBO five times with
per-attribute offsets; mapping it five times exceeded the 64 MiB mapped
budget, `-E2BIG`, before the kernel also deduplicated handles) and copies
only the referenced span of client arrays into the stream uploader.
ioquake3 GL1 and GL2 and glxgears now send 100 % of their TCL triangles
through stage B with no kernel rejection; images unchanged.

glmark2 (`artifacts/2026-09-24/tcl/stb/glmark2-stageA-vs-stageB.tsv`,
3 s per scene) keeps every validation result and gains: build (vbo)
5 -> 32 fps, shading 3 -> 21..23, bump high-poly 1 -> 12, bump normals/height
17 -> 30, buffer 4 -> 20..22, jellyfish 7 -> 18, shadow 1 -> 10, conditionals
11..13 -> 26..36, function 11..12 -> 26..32; refract now completes (1 fps).

## Recommended first silicon test

Goal: execute the first compiler-generated VS on VX900 with the smallest
possible diff from a silicon-proven body, so a failure isolates the VS
store/finalizer/internal-constant path.  Vertex fetch from memory, user
constants and texture are deliberately excluded.

Base: the proven 188-dword Oracle A in [vp-stall-analysis.md](vp-stall-analysis.md)
(32x32 target, 4 immediate six-dword vertices, declaration
`00000206 010000c2 020041d0 030052d0`, A23 low nibble 2, A25/A26/A28
unchanged).  Change only:

1. Internal constants (dwords 76..87 grow by 4): viewport rows 256..258 for a
   32x32 window-oriented target, exactly what `STM_ViewPort_INV` produces
   (dynamic, `chrome9_tcl_const_viewport_probe 0 0 32 32 0 1 4 1 1`):

   ```text
   fe03043c 0000000c 32030400 cc000000
   41800000 c1800000 3f800000 00000000     # sx=16  sy=-16 sz=1
   494000f8 494000f8 00000000 00000000     # ox=oy=15.5+786432, oz=0
   49400000 49400000 00000000 00000000     # row 258 bias
   ```

2. VS program: the compiled `mov oPos,v0; mov oD0,v1; mov oD1,v2`
   (corpus `out_d0_d1`, identical on Linux and Windows apart from the tail),
   eight uploaded records, keeping the proven H2 `31030000` framing:

   ```text
   fe020440 0000043c 31030000 cc000000
   0008014c 00000000 00000000 00080a0f
   0008414c 00000000 00000000 0008422f
   0008814c 00000000 00000000 0008423f
   00201b6c 00000000 00000000 00280108
   00200150 00101b6c 00201b6c 001c0107
   0010014c 00380150 00384154 0004021f
   0020014c 00000000 00000000 0008020f
   0020014c 00000000 00000000 0008020f
   ```

   It writes the same oD0/oD1 set as the Xorg program, so A25 `25f6cc00`,
   A26 and A28 stay exactly as proven.

3. Activation: `A60 = 60800005` (six active records), `A62 = 62208002`
   (proven upper bits, compiler `+0x10 = 2`), `A61 = 61800000` unchanged
   (one temporary, bit 23 set).
4. Vertices become clip-space with W=1, the rest of each record unchanged:
   TL `bf800000 3f800000 00000000 3f800000`, TR `3f800000 3f800000 ...`,
   BR `3f800000 bf800000 ...`, BL `bf800000 bf800000 ...`, colors
   `ffff0000 ffff0000`.

Expected result: pixel-identical to the proven Oracle A (all 1024 pixels
filled, same color, guards intact).  The arithmetic is exact in float:
`-1*16 + 786447.5 = 786431.5`, the same raster X/Y the Xorg program
computes from `-0.5*1 + 786432`, and raster W = 1/1.  Retirement, `040c`,
CR/VP busy bits and D16/color guards use the established health checks.

Follow-ups, one variable at a time:

* **1b** W=2 (`x,y` doubled, `w=40000000`): same image; exercises RCP/op 14.
* **1c** `A62` low nine `1ff` (Xorg value) versus `002`: identifies whether
  the compiler value matters.
* **1d** only the upper-left triangle half (distinct TL color) to prove the
  `sy=-16` orientation.
* **2** `m4x4 oPos,v0,c0` with a pixel-to-NDC matrix uploaded as
  `fe03043c 00000010 32030000 cc000000 <c0..c3>` and pixel-space vertices:
  first user constant upload and DP4/op 26.
* **3** memory fetch: the same vertices in a guarded 1-KiB-aligned BO,
  `A00=00008206`, `fe03043c 00000001 30030000 cc000000 <base|6>` extended to
  one slot per element, `A20=20000001`, `A2C=2c000003`, `A2D/A2E` zero,
  `A24=24000004` and inline 16-bit indices, per
  [indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md) but with
  per-element stream slots.
* **4** the glxgears fixed-function program above with its constants, as
  the first lighting/LIT numerical gate.

Do not combine steps; stop on the first retirement timeout, nonzero `040c`,
persistent busy bit or guard change.

## A60[20:12] as a vertex program start (silicon, 2026-09-27)

Question: can several vertex programs stay resident in the 512-record
store, selected by a start index, so that a program switch writes A60
instead of re-uploading records?  The vendor clears A60[20:12], a 9-bit
field as wide as the record index.  Rung (debug knobs `vs_base`,
`vs_base_re`, not merged): the program uploaded at record B (`31030000 +
4 * B`), A60[8:0] = B + active - 1 or active - 1, A60[20:12] = 0, B, or a
free value; checked with `re/chrome9_gl_vertex_default_check.c`.

Valid results (before the first lockup, program at B = 64):

| A60[20:12] | A60[8:0] | result |
|---|---|---|
| 0 | B + active - 1 | correct |
| 0 | active - 1 | wrong (the relocated program does not run) |
| 1..32 | B + active - 1 | correct |
| 64 (= B) | B + active - 1 | wrong output (magenta) |
| 128 (past the end) | B + active - 1 | nothing drawn, then the engine stopped (fence timeout, cold boot) |

This fits A60[20:12] = first record - 1 (like A60[8:0] = last record):
64 would skip the program's first record, 1..32 run stale records before
it, 128 starts past the end.  It is not proven: the confirming runs
(start 63, and `vs_instructions` per vertex from the PPC counter) came
after the lockup and are invalid, and a repeat after a cold boot stopped
the engine again on its first draw with the knobs at their defaults and
PPC profiling (mask 7) on, so the cause of that second stop is unknown.

Status: open.  A residency cache would need a clean start-63 rung with
poisoned records before the program and the executed-record count, and
must never let A60[20:12] point past A60[8:0].  Not merged; the per-draw
upload stays (ideas: about 360 dwords emitted per draw).

Follow-up (2026-09-27, kernel x172/k173, oracle `twoside t9`): with the
poison program at record 0 and program B at record 256 (A60[8:0] = 256 +
active - 1), A60[20:12] = 0 made the engine execute the uninitialized
records between and stop (fence timeout, cold boot); 256 ran without a
stop and B's draw rendered.  So execution starts at A60[20:12].  A full
residency cache (programs above record 64, a switch writes only A60;
`~/tmp/vs-residency-attempt.diff`) then rendered
`re/chrome9_gl_vertex_default_check.c` wrong: generic varyings read
magenta, with or without A62[8:0] counting from the program start.  A
relocated program loses its varying outputs; the missing piece (the
finalizer tail records, A62 or another linkage word) is not known.
Residency stays off; the rungs only run with the start field inside the
uploaded program.

## VS store start selection: vendor and manual audit (2026-09-27)

Host-only follow-up to the two sections above (no target access).  The
question is which register field selects the first executed record of the
512-record VS store, and whether several programs can stay resident with a
switch done by register writes only.

### Manual

The Chrome9 HD DX9 Part II manual (`~/tmp/ogpm3d.txt`) names HParaType 03h
sub-type 31h "Vertex Shader Instruction" (32h constants, 33h integers) in
the command-regulator overview (p. 72), but the register chapter lists
"HParaType = 03h (HParaSubType = 31h-33h): Reserved" (p. 153) and
"HParaType = 04h, Sub-Address = 53-62h: Reserved" (p. 173).  A60, A61 and
A62 are therefore undocumented, and nothing in the HCM, HC3 or UniChrome
Pro II manuals describes a VS program counter, entry point or instruction
RAM (manual).

The header bytes are documented: byte 3Ch is HParaAdr, "The Beginning of
Internal Address for Parameter Programming", 3Dh HParaOS, "Offset Setting
for Some Special Parameter Types", 3Eh HParaType, 3Fh HParaSubType
(manual, p. 70..72).  `31030000 + 4 * B` is thus sub-type 31h, type 03h
and a 16-bit internal start address `HParaOS:HParaAdr = 4 * B`, one address
per dword.  The address is absolute, not "continue after the last upload":
the vendor places constant rows with the same field (`32030400` row 256,
`3203040c`, `32030424`, `32030428`; static, both drivers), rows 256..258 at
`32030400` and c0 at `32030000` coexist on silicon (m4x4 rung), and a
program uploaded at `31030000 + 0x400` left records 1..255 uninitialized
(the x172 fence timeout above) (manual + static + silicon).

### Vendor code

Every A60 producer in the vendor binaries clears bits 20:12 and every VS
record upload starts at internal address 0 (static):

| binary | site | A60 | upload |
|---|---|---|---|
| via_chrome9_dri.so | `stmUpdateVS_inv` 0x17c586..0x17c5b2 (only writer of the A60 image `state+0x223c`) | `and 0xffe00fff`, bit 23, `[8:0] = +0x14 - 1` | 0x17c85e `31030000`, `+0x424` records |
| via_chrome9_dri.so | `via3DBlit` 0x1f304c, `__glDpSyncFrontBufferToFakeFront` 0x237a94 | fixed `60800004` | 0x1f2efe / 0x237946 `31030000` |
| VTGUModeDX32A.dll | 0x1015ac6d..0x1015ac9a (image `+0x2260`), emitted 0x1015af21 | same as Linux | 0x1015af74 `31030000` |
| VTGOGL32A.dll | 0x102052fd | same | 0x10205694 `31030000` |
| via_drv.so (Xorg) | `via3DEmitVetexShader_H6` 0x31136 | `60800000 | (n + 3) & 0x1ff` | 0x30b60 `31030000` |

No immediate `3103xxxx` with nonzero low bits, no `or reg,31030000` form and
no pre-built `fe03043c ... xxxx0331` header table exists in any of these
binaries, `VTGKModeDX32A.sys` or the vendor kernel library.  The other
`and 0xffe00fff` hits (DX 0x10205a5f, OGL 0x10258a7f) patch a 6-bit source
field of an instruction record, not A60.  The only VS "cache" is
single-slot: `stmUpdateVS_inv` compares the program id with `ctx+0x3844`
(0x17c55e) and re-uploads the whole program at record 0 when it changed or
is dirty (0x17c779..0x17c786); Windows keeps the id at `+0x3060`
(0x1015ac1f).  There is no multi-program placement, no instruction-RAM
allocator and no symbol for one (`PVScp*`, `PVS_*`, `stm*VS*` are compiler
and state code) (static).  The vendor emits A61, A62, A60 and *then* the
record upload (0x17c7e4..0x17c85e, 0x1015aeff..0x1015af74), while this
driver uploads first; both work at record 0 (static + silicon).

The vendor kernel verifier (`VIA_Chrome9_2.6.31/via_chrome9_verifier.c`)
lists type-04 sub-addresses 60, 61, 62, 9b and aa after the viewport
group and nothing between 53 and 5f or above 62 except those; unknown
sub-addresses are not rejected, so this only shows that the vendor knew of
no further VS register (static).  Flow control in the records is relative
(op 29 displacements, op 28 body length), so the records themselves are
position independent (dynamic, section "Relative addressing").

### What the silicon results imply

No vendor code exercises A60[20:12], so its semantics come only from the
rungs above and the 2026-09-27 residency WIP (`~/tmp/vs-residency-wip.diff`,
`vs_order` knob).  One model fits every recorded observation
(**inference**, not proven):

> A draw executes record 0 first and then records A60[20:12] + 1 through
> A60[8:0].  A60[20:12] = 0 degenerates to the vendor case 0..last.

| observation | model |
|---|---|
| [20:12] = 0, program at B, [8:0] = B + active - 1: correct, slower, garbage in 1..B-1 locks up | 0, 1 .. B-1, B .. last |
| [20:12] = 1..32, program at 64: correct | 0, stale S+1..63, then the program |
| [20:12] = B, no record-0 copy: wrong (magenta varyings) | record 0 of another program runs, the program's first record is skipped |
| [20:12] = B, program's first record also written to record 0: correct | 0 (= first), B+1 .. last |
| [20:12] = 128 past [8:0] = 64 + n: nothing drawn, engine stopped | start beyond last |
| [20:12] = 0, record-0 copy, poison `0020014c 0 0 0008020f` in 1..63: correct | first, poison (oPos = scratch, overwritten), full program again |

The earlier reading "A60[20:12] = first record - 1" is the same model
without the record-0 fetch.  A62[8:0] (object `+0x10`) did not matter
either way in the WIP rung, the two inactive finalizer tails lie outside
[8:0] and are never executed under this model, and A61 carries no address
bits in any vendor producer (static); no additional start register is
known.  So the only field that moves execution is A60[20:12], and record 0
is always part of the executed sequence.

Not explained: after a resident draw with the record-0 copy, the next
draw of a program uploaded fully at record 0 with [20:12] = 0 renders wrong
once and then correct.  Under the model it should be correct, because the
full upload rewrites record 0 before the draw.  Candidates (all inference):
a first-record latch or prefetch that the store write does not refresh
until a later draw, or an ordering hazard between the store write and
vertices of the preceding draw still in flight.  The vendor never changes
[20:12] and always rewrites record 0, so it never exposes either.

### Consequences and next rungs

A register-only switch is possible under the model if record 0 holds a
record that is harmless for every program (for example the poison MOV
above) and each resident program at record B >= 1 is selected with
A60[20:12] = B - 1, A60[8:0] = B + active - 1; nothing else changes.  A
switch that also rewrites record 0 (8 dwords plus A60) is the fallback.
Both need, in order, one variable at a time:

1. **Model check**: program at S = 256 whose first record is `MOV oD0, c0`;
   record 0 = `MOV oD0, c1` with c0 != c1.  [20:12] = S must show c1 (first
   record skipped), [20:12] = S - 1 with the poison record at S - 1 must
   show c0.  If S shows c0, execution instead runs 0, S .. last.
2. The executed-record count from the PPC `vs_instructions` counter for
   [20:12] = 0 and S - 1, in a run separate from rung 1 (PPC profiling was
   active in the unexplained stop of the first attempt).
3. The wrong-once case with and without a pipeline idle between the two
   draws, and with record 0 written again after the A60 write.

Never let A60[20:12] exceed A60[8:0], and keep every record between
them initialized: both mistakes stopped the engine (cold boot).

Silicon, 2026-09-27 afternoon (kernels x174..x190, k191; diffs in
`~/tmp/vs-residency-wip.diff` and `~/tmp/vs-residency-record0.diff`):

* Program at record B >= 64, A60[20:12] = B, the program's first record
  copied to record 0: `re/chrome9_gl_vertex_default_check.c` and
  `re/chrome9_gl_vs_nonfinite_check.c` pass, also across processes.
  Records 1..63 filled with a poison record did not change that.
* The depth-only check then fails deterministically in 13 of 19 cases
  (every case whose depth clear is 0.5): the first fixed-builder draw
  after a relocated draw renders wrong once (a clear-only run shows the
  clear itself wrong; the following one is right).  Not changed by:
  vendor A60-before-upload order on either path, A62[8:0] + B, tail
  records after the fixed program, placing the fixed program resident
  too, rewriting record 0 as a single-record upload, max_inflight=1.
* A60[20:12] = 0 with the program at B and the record-0 copy passes the
  checks but runs every record from 0 to the program's end: glmark2 bump
  9.9 -> 12.8 ms per frame, and ideas stopped the engine (cold boot).
* The layout the host analysis proposed (record 0 permanently the fixed
  builders' first record, programs at B with A60[20:12] = B - 1) leaves
  the depth-only failures unchanged and stopped the engine in the
  non-finite check (cold boot).

The start field does move execution, but its exact semantics and the
state that corrupts the next record-0 program are not known, and wrong
guesses stop the engine.  Superseded by the working model below.

## VS residency: working model and implementation (2026-09-27, k202)

Silicon results (kernels x198..x201, k202) that explain the failures above:

* The store holds 256 records.  An upload at record 256 or above (the
  earlier B = 256 and B = 337 rungs) is where the engine stops; the
  "512-record" assumption above is wrong.
* A draw first executes one record at the previous draw's A60[20:12]
  (the latch), then continues at its own A60[20:12] + 1 up to A60[8:0].
  The latch changes only per draw; a second A60 write before the same
  draw does not reset it.  This is the "wrong once" after a relocated
  draw: the next record-0 program ran the relocated program's first
  record in place of its own.
* The upload header `31030000 + 4 * record` is an absolute record address.
* Every fixed-builder program starts with the same record
  `00380150 00000000 00000000 0008010f`, and every compiled program
  tolerates executing it once in front of its own first record.
* REP/IF flow programs are only reliable at record 0.

Kernel `7ba5d2e` (module parameter `vs_resident`, default on):

* Resident programs live in records 16..255.  Each occupies count + 1
  records: the common first record at base, the program at base + 1;
  A60[20:12] = base, A60[8:0] = base + count.  Whatever the latch is,
  the first executed record is harmless.  Placement: content compare for
  hits, first-fit, LRU eviction; a switch to a resident program is one
  A60 write.
* Flow programs and programs longer than 239 records stay at record 0.
  If the latch lies outside their range, record 0 is lent to the latch
  (the program's first record copied there) and restored after the draw.
  If the latch lies inside, a zero-area fixed-function draw first moves
  the latch to 0.
* An upload that overlaps the range the previous draw executes is
  preceded by the H0 3D wait (`006c` = `00800000`).
* Store state is staged per command segment and restored when a draw
  does not fit and moves to the next segment; ring priming resets it.

Results: `re/chrome9_gl_depth_only_check.c` 19/19 (repeated),
`re/chrome9_gl_vertex_default_check.c`, `re/chrome9_gl_vs_nonfinite_check.c`
pass; full piglit unchanged against `vs_resident=0` (the five failures of
the earlier attempt pass); glmark2 ideas 8.73 -> 8.26 ms per frame, other
scenes and ioquake3 GL1/GL2 unchanged (fill-bound), no job timeouts.
