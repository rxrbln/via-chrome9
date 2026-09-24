<!--
SPDX-License-Identifier: GPL-2.0-only
Copyright 2026 René Rebe <rene@exactco.de>
-->

# Chrome9 VS scalar precision, SFU init and constant read ports

Status: offline reverse engineering, 2026-09-24.  Nothing in this note was
run on the VX900 target.  It answers why the hardware vertex shader returns
wrong RCP/RSQ/EXP/LOG results and why two distinct constants in one record
read the same row, from the vendor binaries already in the repository.

Evidence labels follow [chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md):
**static** (disassembly, address given), **dynamic** (vendor code executed on
the host, byte-exact), **cross** (Linux 5.75 and Windows 2015 agree),
**silicon** (measured on VX900 by our kernel), **inference** (reasoned, not a
byte recipe on its own).

Binaries (unchanged from the earlier notes):

| short name | file |
|---|---|
| DRI | `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (32-bit, symbols; addresses are file VMAs) |
| UMD | `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll` (image base `0x10000000`) |
| KMD | same directory, `VTGKModeDX32A.sys` |
| OGL | same directory, `VTGOGL32A.dll` (Windows GL ICD) |
| INF | same directory, `dc07W7i.inf` |
| manual | `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf` |

## Summary

1. **The vendor never lets a scalar op read a constant.**  Both vendor VS
   compilers lower `rcp/rsq/exp/expp/log/logp` with a `c#` source into
   `MOV scratch.xyzw = c#.ssss` followed by the scalar op reading
   `scratch.wwww` (dynamic + cross, static at DRI `PVS_RCP` 0x18b082..0x18b0a6
   and five twins).  Inputs and temporaries are read directly.  This matches
   the silicon result that scalar ops are exact from temps/inputs and wrong
   only from constants.  It is a hardware rule, not a missing table.
2. **There is no special-function lookup table to upload.**  Every H3
   sub-type written by the DRI, UMD, KMD and OGL is accounted for (VS uses
   only `30/31/32/33`), the manual's HParaType 03 list has no VS table, and
   the KMD reset replay contains none (static, exhaustive immediate scan).
3. **A61 bits 16 and 18 are the debug option `Halcyon1ALU1CACHE`.**  Both
   drivers set them together only when that registry value is nonzero; the
   shipped INF does not set it and the Linux driver has no writer, so the
   production value is both bits clear (static, cross).  Bit 17 is never
   written by any vendor code path.  Keep all three clear.
4. **One distinct constant row and one distinct input row per record.**  The
   vendor GL token regenerator configures read-port limits `input = 1`,
   `constant = 1` for `vs_2_0` and splits MAD/LRP instructions that exceed
   them (static, DRI 0x1cf340..0x1cf377, 0x1d03e0, 0x1d02b0).  The D3D
   compiler does not split, because D3D shader validation already enforces
   the same limit (inference from the matching limit values).  There is no
   enable bit that lifts the limit.
5. **op 14 is never emitted outside the position finalizer**, and neither
   compiler ever adds refinement records after RCP/RSQ/EXP/LOG.  Its exact
   arithmetic remains unmeasured; a discriminating test is given below.

## 1. Init-time state that touches the VS

### A61 bits 16/17/18

DRI `stmInitChipImage_inv` (0x1786a0), image base `state+0x20bc`, so A61 is
the dword at `+0x2240` and byte `+0x2242` holds bits 23:16 (static):

```text
178cff  mov  -0x20(%ebp),%esi          # hw context
178d02  mov  0x228(%esi),%ecx          # registry-option block
178d08  cmpl $0x0,0x2e8(%ecx)
178d0f  jne  179313
178d15  andb $0xfa,0x2242(%eax)        # A61 &= ~(bit 18 | bit 16)
...
179313  orb  $0x5,0x2242(%eax)         # A61 |=  bit 18 | bit 16
```

The block pointer is installed by `hwmInitContainerRegistryOptions`
(0xaac81) and filled by `osGetRegistryOptions` (0xa3ea0).  That function
assigns defaults to dozens of fields but never to `+0x2e8`, and the only
reader of `+0x2e8` in the whole DSO is 0x178d08 (static, exhaustive grep of
`0x2e8(` operands after a `0x228(` load).  The Linux driver therefore has no
path that sets the bits.

The Windows UMD has the same code and names the option (static):

```text
100ed56b  lea  0x30c(%edi),%eax         # registry block field +0x30c
100ed572  push $0x100174e4              # L"Halcyon1ALU1CACHE"
100ed579  movl $0x0,(%eax)              # default 0
100ed57f  call 0x100e9520               # registry DWORD read
...
101551e6  cmpl $0x0,0x30c(%edx)         # %edx = device+0x230
101551ef  orl  $0x50000,0x2264(%eax)    # A61 |= bits 18,16
101551fb  andl $0xfffaffff,0x2264(%eax) # A61 &= ~bits 18,16
```

The registry reader is 0x100e9650, called once from 0x100f0741 with
`device+0x230` as its output block, which is the same block tested at
0x101551e6.  The neighbouring fields are `+0x308 HalcyonSVSEnable` and
`+0x310 HalcyonSPSEnable`.  `dc07W7i.inf` lines 146..220 set many `Halcyon*`
values but not `Halcyon1ALU1CACHE`; the OGL ICD contains the same string.

Conclusions:

* Bits 16 and 18 form one debug mode ("one ALU, one cache" by name,
  **inference**); the vendor always writes them together and ships them
  clear.  The silicon hang with bit 18 alone is consistent with a partial
  debug configuration.  Bit 16 alone having no visible effect proves nothing
  about precision.
* Bit 17 has no writer in DRI or UMD (static: the A61 image writers are
  the two sites above, the Boolean-mask stores at DRI 0x17bc49 / UMD
  0x101594fa (bits 15:0) and the activation update at DRI
  0x17c5de..0x17c620 / UMD 0x1015acc8..0x1015ad13 (bits 23, 22:20, 19)).
* No A61 bit is related to scalar precision.  The kernel debug knob
  `tcl_a61_or` (`chrome9_engine.c:116`) has no vendor-backed use.

### Init tables

The complete set of H3 sub-types with immediate selectors (static scan of
`$0x..030...` operands):

| binary | sub-types |
|---|---|
| DRI | 10, 11, 12, 14, 15, 17, 20, 21, 22, 30, 31, 32, 33, 80 |
| UMD | 10, 11, 12, 14, 15, 20, 21, 22, 30, 31, 32, 33, 80 |
| OGL | 10, 11, 12, 14, 15, 20, 21, 22, 30, 31, 32, 33, 80 |
| KMD | 10, 11, 12, 14, 15, 17, 20, 21, 22, 31 |

Each one is identified:

| sub-type | content | evidence |
|---|---|---|
| `12` | `INV_HWCoarseZMergeTable` (DRI 0x3f1bc0), 20 dwords; in `hwmHWEnable_inv` (0xab2fe) and, when `ctx+0x3824` is set, in `stmFullUpdateHardware_invi` | DRI 0x177c69..0x177c97 |
| `15` | texture de-gamma table (5 dwords) | manual HParaSubType list; [via-5.75-kmd.md](via-5.75-kmd.md) |
| `17` | gamma/de-gamma table for destination writes (31 dwords) | manual; DRI `hwmHWEnable_inv` 0xab3d5 |
| `10`, `11`, `14`, `00` | mip base offsets, 4x4 filter coefficients, stipple, palette | manual |
| `20..22` | PS ALU, TAU, constants | manual |
| `30..33` | VS streams, program, float constants, integer constants | manual |
| `80` | `stmPSRectSamplingPatch` only | DRI 0x1b6c07 |

The manual's HParaType 03 sub-type list (text page 71) has no other VS entry,
and the KMD program uploads at `VTGKModeDX32A.sys` 0xb0e1e and 0xb0fe1 are
two small MOV-only blit programs (`0008014c 0 0 0008021f`, ...), not tables.
`33030000` is written only by `stmUpdateIntBoolConst_inv` (0x17bba1) for
`defi` loop constants.  Our `chrome9_init_3d_h5s2vp1()` already replays the
15/17 tables; the missing sub-type 12 is coarse-Z only and unrelated.

**Answer to Q1/Q2:** the vendor programs nothing at init that affects VS
scalar precision, and there is no SFU table address or content to upload.

## 2. The vendor scalar-from-constant lowering

The `vs_2_0` dispatcher routes `rcp/rsq/exp/log` (and `expp/logp`, see
below) to `PVS_RCP` 0x18ae20, `PVS_RSQ` 0x18b280, `PVS_EXP` 0x184130,
`PVS_LOG` 0x185270; `vs_1_1` additionally reaches `PVS_EXPP` 0x184590 and
`PVS_LOGP` 0x1856d0 (callers in `PVSCP_DISPATCHER_VS20_H5` 0x18f1d7..0x18f4df
and `PVSCP_DISPATCHER_VS11_H5` 0x18f84a..0x18fa4a).  The six bodies are
byte-identical except the opcode byte (`or $0x20/0x22/0x24/0x26/0x28/0x2a`,
i.e. ops 16/17/18/19/20/21).  In each, a register-class test sends constant
sources to a separate block that first builds a MOV record (static):

| emitter | class test | MOV-to-scratch block |
|---|---|---|
| `PVS_EXP` | 0x184240, 0x184398 | 0x1843ab |
| `PVS_EXPP` | 0x1846a0, 0x1847f8 | 0x18480b |
| `PVS_LOG` | 0x185380, 0x1854d8 | 0x1854eb |
| `PVS_LOGP` | 0x1857e0, 0x185938 | 0x18594b |
| `PVS_RCP` | 0x18af30, 0x18b088 | 0x18b09b |
| `PVS_RSQ` | 0x18b390, 0x18b4e8 | 0x18b4fb |

`PVS_RCP` 0x18b090..0x18b0b5 sets mask `0xf`, destination `0x80` and op 4
(MOV); the scalar record that follows reads unified 128.  `PVS_LIT`,
`PVS_FRC` and `PVS_SGN` have no such block.

Dynamic, Linux harness `chrome9_tcl_vs_oracle_probe` (mode 0), records before
the unchanged five-record finalizer:

```text
rcp r0, c0.x      00400000 00000000 00000000 0008080f   MOV scratch = c0.xxxx
                  00201b6c 00000000 00000000 0028010f   RCP r0 = scratch.wwww
rcp r0, c0.w      00401b6c 00000000 00000000 0008080f
                  00201b6c 00000000 00000000 0028010f
rcp r0.x, c0.y    00400924 00000000 00000000 0008080f
                  00201b6c 00000000 00000000 00280101
rcp r0, -c0.x     00400001 00000000 00000000 0008080f   NEG moves to the MOV
                  00201b6c 00000000 00000000 0028010f
rsq r0, c0.w      00401b6c ... 0008080f ; 00201b6c ... 002a010f
exp r0, c0.x      00400000 ... 0008080f ; 00201b6c ... 0020010f
expp r0, c0.x     00400000 ... 0008080f ; 00201b6c ... 0020010f
log r0, c0.x      00400000 ... 0008080f ; 00201b6c ... 0024010f
rsq r0.x,c0.x; rsq r0.y,c0.y
                  00400000 ... 0008080f ; 00201b6c ... 002a0101
                  00400924 ... 0008080f ; 00201b6c ... 002a0102
rcp r0, c0[a0.x].x
                  00400000 00000000 00000000 0408080f   relative nibble on MOV
                  00201b6c 00000000 00000000 0428010f   and copied to the RCP
```

Cross: the Windows UMD compiler under Wine produces the identical MOV/scalar
pairs for `rcp c0.x`, `rsq c0.w`, `exp c0.x`, `log c0.x` (followed by its
usual two depth-remap records).  The previously stored corpus shows the
complementary cases compile without the MOV: `rcp r0, v0.w` reads
`00081b6c` directly, `rcp r0, r1.y` reads `00100924` directly.

Every other vendor path that feeds the scalar unit already goes through a
non-constant register:

| construct | vendor records | scalar source |
|---|---|---|
| SINCOS | `MAD scratch = x * c233.w + 0`, then op 22 | scratch |
| NRM | `DP3 scratch`, `RSQ scratch.wwww`, `MUL` | scratch |
| POW | `MAX scratch = abs(x)`, `LOG`, `MUL`, `EXP` | scratch |
| finalizer | `RCP r0.w = 1 / scratch.w` | scratch |
| glxgears FF | `dp3 r0.w, r0, r0; rsq r0.w, r0.w` | temp |

`LIT` (op 24) and `FRC` are emitted with a direct constant source
(`lit r0, c0` gives `0040014c 0 0 0030010f`).  FRC is silicon-exact from a
constant; LIT from a constant is untested.

This lowering explains the silicon matrix exactly: scalar ops were wrong only
when the record addressed the constant file, and exact from temps, inputs
and (in the finalizer) scratch.  The collapsed-mantissa pattern (RCP of any
non-power-of-two in `[1,2)` returns 1/1.5, `RSQ(2.5) = 1/sqrt(3)`) is the
observed behaviour of an unsupported operand path; its internal mechanism is
not recoverable from the binaries (**inference**: the special-function path
receives a constant operand that is not the one the vector path would see).
The EXP and LOG sweeps fit the "mantissa collapsed to .5" description less
well (EXP of `-1.125..-1.75` gives .388, not the .354 that `2^-1.5` would
give; LOG returns 0 even for 2 and 4); they must be re-measured from a temp
before assuming the same cause (test S2 below).

## 3. op 14 and refinement

The vendor never emits a refinement sequence after RCP/RSQ/EXP/LOG in any
corpus case (`rcp_r0_v0w`, `rsq_r0_v0x`, `exp_r0_v0x`, `expp_r0_v0x`,
`log_r0_v0x`, `logp_r0_v0x`, `nrm_r0_v0`, `pow_r0_v0x_c0x`, `lit_r0_v0`,
`sincos_r0xy_v0x` in
[chrome9-hw-tcl-vs-corpus.json](chrome9-hw-tcl-vs-corpus.json); dynamic,
cross).  Each scalar op is exactly one record, and op 14 appears only in the
finalizer (and in the FF template `stmFFVS_Transform_H5i` 0x19f890):

```text
00201b6c 00000000 00000000 00280108   RCP  r0.w   = 1 / scratch.w
00200150 00101b6c 00201b6c 001c0107   op14 r0.xyz = f(scratch.xyz1, r0.wwww, scratch.wwww)
```

Its operands are the value to divide, the reciprocal estimate and the
original divisor.  That is exactly the operand set of either
`a * b` (plain perspective multiply, the third operand used only for a
zero/infinity guard) or one Newton step `a * b * (2 - b * c)`
(**inference**; neither form is proven).  Current silicon cannot
distinguish them: W = 1 and W = 2 make both forms exact, and the scratch-
sourced RCP is exact anyway.  Test S4 below separates them without relying
on the scalar unit.

`vs_1_1` `expp`/`logp` use ops 17/19 (the D3D partial-precision forms, whose
D3D definition returns different values per lane); `vs_2_0` maps both to
16/18.  Ops 17/19 are therefore not "higher precision" variants and should
not be used by the new driver.

## 4. Constant and input read ports

The vendor GL path (the "MS token regenerator" that turns GL programs into
D3D-style tokens) configures per-register-class read-port limits in
`stmDecideMSTokenRegeneratorInfoICD` (static):

```text
1cf338  cmp  $0xfffe0200,%ecx       # vs_2_0
1cf369  movl $0x1,0x5c(%edx)        # class 1 (input v#)    : 1 read port
1cf377  movl $0x1,0x60(%edx)        # class 2 (constant c#) : 1 read port
```

(The `ps_2_0` branch at 0x1cf388 sets temp 3, input 1, constant 1.)
`stmDefaultVSReplaceActionICD` (0x1d03e0) walks the instruction list and,
for D3D opcode 4 (MAD) and 0x12 (LRP), calls
`stmDefaultAvoidReadPortLimitICD` for classes 2 and 1.  That calls
`stmAvoidReadPortLimitICD` (0x1d02b0), which counts distinct register
numbers of the class with `stmGetSrcRegTypeCountICD` (0x1cf1a0: histogram by
register number, then count of nonzero buckets) and, when the count exceeds
the limit, moves the excess into a temporary with
`stmRemoveRegFromTokenNode`.  The same register used twice counts once,
which matches the silicon result that `MAD v, c0, c0` is correct.

Two caveats in the vendor code:

* only three-source MAD/LRP are processed, so a GL `ADD c0, c1` would not
  be split (vendor gap, or never produced by its front end; unmeasured);
* the histogram has 32 buckets on the stack but is indexed by
  `regnum & 0x7ff`, so constants `c32` and above are miscounted or overrun
  the buffer.  The limit values, not the vendor enforcement, are the useful
  evidence.

The D3D compilers (both drivers) emit two or three distinct constants in one
record without splitting (`mad_r0_c0_c1_c2`
`0040014c 0040414c 0040814c 0004010f`; newly probed `add r0, c0, c1`
`0040014c 00206490 0040414c 0004010f`, `dp4 r0, c0, c1`
`0040014c 0040414c 00000000 0006010f`; dynamic).  The D3D runtime validator
rejects such shaders for `vs_1_1`/`vs_2_0`, whose documented read-port
limits are one `c#` and one `v#` per instruction; that is external
specification knowledge, not repository evidence, and explains why the
D3D compiler needs no pass.

Internal rows are different.  The finalizer MAD
`0010014c 00380150 00384154 0004021f` reads unified 224 and 225 (internal
rows 256 and 257) in one record and is silicon-proven with distinct scale
and offset values (base oracle, 1024/1024).  **Inference:** unified
224..255 are a separate register class with their own port; the one-row
limit applies to user constants (unified 256..511) and inputs (32..47).
The FF corpus confirms the vendor never relies on more: 600 FF records
(`~/tmp/re-tcl/D/corpus2/*.raw`) contain no record with two distinct user
constants, only the finalizer's 224/225 pair.

No vendor code writes a register that could enable a second constant port;
there is no such bit to program.

## 5. Recommendations (ranked by confidence)

### R1 (high; dynamic + cross + silicon-consistent): lower scalar-from-constant

For ops 16, 17, 18, 19, 20 and 21, never place a constant (unified
256..511, and by the same rule internal rows 224..255) in a scalar record.
Emit the vendor pair instead:

```text
MOV  dword0 = (256 + n) << 14 | sel | neg   dword1 = dword2 = 0
     dword3 = 0x0008080f                    # op 4, dest 0x80 scratch, mask xyzw
     sel: .x 0x0000, .y 0x0924, .z 0x1248, .w 0x1b6c (replicated lane)
OP   dword0 = 0x00201b6c                    # unified 128, .wwww
     dword1 = dword2 = 0
     dword3 = op << 17 | sat << 14 | dest << 4 | mask
```

Negation belongs on the MOV (`00400001` for `-c0.x`).  For relative
addressing keep the relative nibble on the MOV; the vendor also copies it
onto the scalar record (`0428010f`), which is harmless only if the hardware
ignores it for a scratch source (unverified), so do not copy it.

Scratch hazard: the vendor writes a user `oPos` as destination `0xa0`
(scratch plus position), and the finalizer's RCP reads the clip position
from scratch.  A scratch MOV after such an `oPos` write would replace the
position.  The kernel currently writes position through a temporary and a
final `MOV` when `!position_direct` (`chrome9_vertex.c` near the finalizer
emission); use scratch for this lowering only on that path, or keep the
current temporary-based lowering.  The present kernel validator
(`chrome9_vertex.c:163..167`) rejects scalar ops with a constant source;
turning that rejection into this lowering (or keeping the Mesa-side MOV to a
temp) is equivalent in arithmetic.  Do not add a lookup-table upload or any
A61 change for this problem.

### R2 (high; static limits + silicon): one user-constant row and one input row per record

Keep `chrome9_vertex_validate_ports()` for every op, not only MAD/LRP, and
lower violations with a MOV of the extra constant/input into a temporary.
Temporaries are not limited: the VS replace action checks only classes 1
and 2, and distinct temps are silicon-exact.  The same register read twice
counts once.  Exempt only the vendor finalizer's
internal-row pair; do not generate user code that reads two internal rows or
an internal row plus a user constant until measured (test S5).

### R3 (high; static, cross): A61

Emit `A61 = 0x61000000 | (temps <= 8) << 23 | boolean mask` with bits 16,
17 and 18 clear, exactly as both vendors ship.  Remove or restrict the
`tcl_a61_or` debug knob to avoid reintroducing the bit-18 hang.

### R4 (medium; dynamic): keep one record per scalar op

No refinement records are needed or used by the vendor.  If S1 shows exact
results through a temp/scratch source, Mesa can map NIR `frcp`, `frsq`,
`fexp2`, `flog2` to single records (with R1) and keep the existing
fail-closed value domains of
[chrome9-rcp-rsq-scalar-audit.md](chrome9-rcp-rsq-scalar-audit.md) and
[chrome9-exp-log-scalar-audit.md](chrome9-exp-log-scalar-audit.md) until the
full sweep passes.

### R5 (low; untested): LIT

The vendor feeds LIT (op 24) directly from a constant.  Treat LIT from a
constant as unqualified until S6 passes; route LIT operands through a temp
in the meantime.

## 6. Minimal silicon test plan

All steps use the existing guarded `chrome9_tcl_oracle` op ladder
(`tools/re/chrome9_tcl_oracle.c`, one op per draw into flat T0, known-good
draw before and after, GPU lock, FE33, fence/timeout/guard checks) and stop
at the first retirement timeout, nonzero error status, persistent busy bit or
guard change.  One variable changes per step.

| step | variable | program (records before the finalizer) | expected | hang risk |
|---|---|---|---|---|
| S0 | none | existing `mov` and `rcp_tmp` cases | as before | none known |
| S1 | scalar source = scratch (vendor pair) | position via temp + final MOV; `MOV scratch = c2.xxxx` (`00408000 0 0 0008080f`), `RCP T0 = scratch.wwww` (`00201b6c 0 0 0028028f`); sweep c2.x = 1, 1.125, 1.25, 1.5, 1.75, 2, 2.5, 3, 4 via `C2X` | 1/x within 1 UNORM step in all four lanes | low: same op mix as proven `rcp_tmp`, only the register class differs |
| S2 | op = RSQ, then EXP, then LOG (separate runs) | as S1 with op 21/16/18; RSQ of 1, 1.5, 2, 2.5, 4, 16; EXP of -1, -1.125, -1.5, -2.5, -3, -4 (NEG on the MOV, `00408001`); LOG of 1, 1.25, 1.5, 1.75, 2 | exact within 1 step; LOG(2) = 1 | low |
| S3 | same S1/S2 values from a temp instead of scratch | `MOV r1 = c2.ssss`, `OP T0 = r1.wwww` | identical to S1/S2 | low |
| S4 | op 14 alone, all operands from temps | `MOV r1 = (0.5,0.5,0.5,1)`, `MOV r2 = 0.25`, `MOV r3 = 3`, `op14 T0.xyz = f(r1, r2.wwww, r3.wwww)` (dword3 `001c0287` for T0 mask xyz) | plain product .125 (32); Newton step .156 (40); true quotient .1667 (43) | medium: op 14 has never run outside the finalizer; run last in the session and only with the reset path armed |
| S5 | two internal rows in a user record | `MAD T0 = v1 * c256 + c257` using the loaded viewport rows (values rescaled into [0,1] by a separate row set) | both rows read distinctly, as in the finalizer | low |
| S6 | LIT from a constant versus a temp | `lit T0, c0` then `mov r1, c0; lit T0, r1` with (N.L, N.H, 0, exp) in [0,1] | both equal the D3D LIT result | low |
| S7 | two distinct constants in a 2-source op | `ADD T0 = c0 + c1` | expected to fail like MAD (reads c1 twice); confirms R2 covers all ops | none known |

Explicitly not recommended: A61 bit 17 alone, and A61 bits 16+18 together
(the actual vendor debug combination).  No vendor path uses them, bit 18
alone already hangs the engine, and neither is expected to change scalar
precision.  If ever tested, do so as the last step with engine reset
recovery prepared.

LOG of 4 (result 2) and EXP of positive inputs exceed the UNORM range; test
them only after S2 passes, with an exact `MUL` by 0.25 after the scalar
record.

## 7. Reproduction (offline)

The Linux harness and Wine harness from
[chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md) compile arbitrary `vs_2_0`
text through `tools/re/chrome9_tcl_vs_oracle.py`.  From the repository root:

```python
import sys; sys.path.insert(0, "tools/re")
import chrome9_tcl_vs_oracle as o
for prog in (["rcp r0, c0.x", "mov oPos, r0"],
             ["rcp r0, -c0.x", "mov oPos, r0"],
             ["rsq r0.x, c0.x", "rsq r0.y, c0.y", "mov oPos, r0"],
             ["lit r0, c0", "mov oPos, r0"],
             ["add r0, c0, c1", "mov oPos, r0"]):
    print(o.run_probe("<linux oracle>", o.assemble(prog), ["-m", "0"])["records"])
    print(o.run_wine("<probe.exe>", "<abs dir>", o.assemble(prog))["records"])
```

with `LD_LIBRARY_PATH` pointing at the 32-bit `libexpat`/`libdrm` copies
(`~/tmp/re-tcl/lib32`).  The DRI and UMD addresses above were read from
`objdump -d` of the listed binaries; the registry string at UMD VA
`0x100174e4` is UTF-16 at file offset `0x168e4`.

## Open items

* The internal cause of the wrong constant-sourced scalar results.
* op 14 arithmetic (S4) and its behaviour for W = 0.
* Whether internal rows 224..255 share the user constant port (S5).
* EXP/LOG accuracy from a temp over the full sweep (S2/S3).
* Semantics of ops 17/19 (vendor `vs_1_1` only).
