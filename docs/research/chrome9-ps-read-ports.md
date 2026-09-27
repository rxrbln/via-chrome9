<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 pixel-shader register read ports

Status 2026-09-24, host-only work.  Evidence labels:
**manual** (programming manual or external API specification),
**static** (disassembly of vendor binaries), **dynamic** (the vendor
compiler or the kernel compiler run offline on the host), **silicon**
(VX900 results recorded in this repository or reported by the user), and
**inference**.  Nothing here was run on the target.

## Result

1. **Rule:** a fragment ALU record may name **at most one distinct
   texture-class register**.  Texture-class means native source class 3:
   compact T varyings (`DRM_CHROME9_FRAGMENT_FILE_VARYING`) and the relocated
   MISC position input (`FILE_POSITION`).  The same register may appear in
   several slots with any swizzle, modifier or width.  The other classes
   keep their existing limits: TEMP three (one per slot), the single
   PRIMARY (v0) input, and constants as enforced today (two distinct are
   silicon-proven, three distinct alias C to A).
2. The kernel encoding of `ADD oC0,T0,T1` is **exactly what the Linux 5.75
   PS 2.0 compiler emits** for `add oC0,t0,t1`, byte for byte, including
   A90..A92 (`90058003 91000000 92000001`).  The hang is not caused by an
   encoding or control-word difference.  The vendor never executes this
   record: its GL front end splits it before the PS compiler (static), and
   the D3D runtime rejects it for `ps_2_0` (manual).  The vendor resource
   analyzer also sees only one texture-class source per record, so such a
   record also gets an undercounted FE A00 (`00000011` for two consumed T
   inputs).
3. `90058003` is correct.  The A90 low bits are the compact T-use mask of
   direct ALU reads (bit 0 = T0, bit 1 = T1).  Bits 15/16 are the resource
   bucket (`0x18000`) and bit 18 marks an ALU-first program.  The vendor
   emits exactly this word.

The draft kernel legalization follows the vendor GL pass: it copies each
excess varying into a fresh temporary with one identity MOV.  Every
legalized form checked matches the vendor compiler's output for the
equivalent split token stream, byte for byte.

## Silicon facts used

| case | result | label |
|---|---|---|
| `MOV oC0,T0` `01c00530 301c0053 00f2c005 000d0820`, A00=`11` | passes | silicon ([direct-T0 FE count](chrome9-direct-t0-fe-count.md)) |
| `ADD oC0,T0,T1` `02c10530 30140924 00f2c005 00000820`, post-TNL and HW TCL | job timeout, cold boot | silicon (user, 2026-09-24) |
| one-record `MAD oC0,T0,T1,T2` `02c20530 302c1053 00f2c005 00000820`, A00 `31` then `11` | timeout both times | silicon ([raster FE oracles](raster-front-end-oracles.md)) |
| three-record chain, at most one T per record, float4 T0..T2 | retired without timeout (silent black from stale A01) | silicon ([raster FE oracles](raster-front-end-oracles.md)) |
| `ABS oC0,T0`: T0 in slots A, C and B | passes (`804080bf`) | silicon ([CMP/ABS/SAT rung](chrome9-cmp-abs-sat-rung.md)) |
| MAD/CMP with three distinct constants | C reads alias A (wrong pixels) | silicon (kernel comment in `chrome9_fragment_validate`) |
| MAD/CMP with two distinct constants | correct | silicon (same) |

The kernel has always encoded a varying ABS operand as class 3 (the
source-class switch has not changed since `bb16ea9`).  The current kernel
emits `00c00538 300c0053 00f2c005 00090820`, so the ABS pass shows that one
texture-class register in all three slots is safe (dynamic, silicon).  The
comment in `chrome9_normalize_guard_smoke.c --abs` quotes the class-1
(`v0`) vendor row `00400538 30040053 00f04005 00090820`.  That comment is
mislabelled; the test submitted the class-3 form.

Every multi-T timeout above has two or more *distinct* class-3 registers
in one record.  Every class-3 pass has at most one distinct register per
record.

## Vendor static evidence

Linux `via_chrome9_dri.so` 5.75.32.87a (SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
i386, unstripped).

### GL front end: per-instruction read-port limits (static)

`stmDecideMSTokenRegeneratorInfoICD` (0x1cf250) fills a per-profile table
at `info+0x58+4*class`.  The class is the D3D register type: 0 TEMP,
1 INPUT, 2 CONST, 3 TEXTURE.

| profile | temp | input v# | const c# | texture t# | code |
|---|---:|---:|---:|---:|---|
| `ps_2_0` (`ffff0200`) | 3 | 1 | 1 | **1** | 0x1cf388..0x1cf3e9 |
| `ps_1_4` (`ffff0104`) | 3 | 1 | 2 | 1 | 0x1cf3f0..0x1cf451 |
| `vs_2_0` (`fffe0200`) | – | 1 | 1 | – | 0x1cf338..0x1cf37e |

The shared prologue stores `stmDefaultPSReplaceActionICD` at `info+0xc4`
(0x1cf290).  The chip layer's PS hook `stmReplaceMSTokenPSICD_inv`
(0x1762e0, installed through the `R_386_32` relocation at `0x428a08`) calls
`*(info+0xc4)` unconditionally.  `stmReplaceMSTokenICD` (0x1cfa00) reaches
it from `stmCreateFragmentProgramICD`, the GL fragment-program path.

`stmDefaultPSReplaceActionICD` (0x1d0350) visits **every** instruction.
Unlike the VS action, which only visits MAD and LRP, it calls
`stmDefaultAvoidReadPortLimitICD` for classes 2, 1 and **3** (0x1d0378..
0x1d03af).  That calls `stmAvoidReadPortLimitICD` (0x1d02b0).  It counts
*distinct register numbers* of one class with `stmGetSrcRegTypeCountICD`
(0x1cf1a0).  When the count exceeds the limit, it calls
`stmRemoveRegFromTokenNode` (0x1cff30).  That function allocates a
`stmGetSaveToUseTempRegisterICD` temporary and inserts a token node of
length 3 with opcode 1 (MOV).  The MOV has an `.xyzw` destination and the
removed register as its source.  It walks the histogram from register 0
upward, so it appears to move the lowest-numbered registers and keep the
highest (inference from the loop at 0x1d0040).

Consequences:

* the vendor GL driver never gives its PS compiler an instruction with two
  distinct `t#` (or two distinct `v#`, or two distinct `c#`) sources;
* repeated use of one register counts once; swizzle and write mask do not
  matter;
* the constant limit (1) is stricter than silicon needs (two distinct
  constants pass), so these values are conservative front-end limits.  For
  class 3, the silicon timeouts show the limit is required.

**Manual (external, not repository evidence):** Microsoft's `ps_2_0`
register table allows one read port for `t#` and for `v#`, two for `c#`,
and three for `r#`.  The D3D runtime validator therefore rejects
`add r0,t0,t1` before the UMD sees it.  This is why the Windows D3D
compiler needs no split pass.  A bounded byte-pattern search of
`VTGUModeDX32A.dll` and `VTGOGL32A.dll` did not find the Linux limit table
(static negative, not an absence proof).

### Resource analyzer (static, previously documented)

`stmGetActiveTexTmpNum` (0x19a410; Windows 0x10201e60) reads at most **one**
texture-class source per ALU record, in the order A, then C, then B.  See
[chrome9-direct-t0-fe-count.md](chrome9-direct-t0-fe-count.md).  Its third
output becomes FE A00, and its first output drives the A90 bucket and the
MISC relocation (`stmUpdateConfig`).  For `ADD T0,T1` it returns `(1,0,1,0)`
although A90 names two T inputs.  If each record reads at most one class-3
register, the analyzer sees every coordinate the ALU consumes.  The MISC
location then always falls above the highest direct coordinate (ids
3/4/7/15 for `active0` ≤ 4/5/8), so the vendor's MISC relocation never
collides.  The kernel's `-EOPNOTSUPP` MISC/direct-coordinate alias check is
reachable only through multi-class-3 records (inference, checked with the
patched self-test below).

### PS optimizer co-issue limits (static)

`PSOPT_CheckCoissueRWPortRestriction_H5i` (0x1cb920) uses
`PSOPT_CountReadPort` (0x1c0dc0) on native records.  It counts distinct
native sources across **two co-issued records**, deduplicating within a
record by the 9-bit class|id field and across records with `ISSAMESRC`.
It rejects the pair when any of these are exceeded:

| native class | limit per co-issued pair |
|---|---:|
| 3 (T/MISC) | 2 |
| 1 (INPUT) | 2 |
| 0 TEMP id 0..11 | 3 |
| 0 TEMP id 12..15 | 2 |
| 2 CONST id ≤ 23 | 1 |
| 2 CONST id 32..55 | 1 |

It also allows at most one write port per destination class.
`PSOPT_CheckReadPortLimitation_invi` (0x1c28a0) applies the same limits
when `PSOPT_RemoveRedundantMOV_invi` would copy-propagate into a co-issued
neighbour (node flags `+0xc4` bits 6/7, calls at 0x1c68b5 and 0x1c691d).
It performs no check for non-co-issued records, because the front end has
already enforced one `t#` per instruction.  Two T reads per two-record pair
is consistent with one T read port per record slot (inference).  The kernel
does not co-issue, so these pair limits need no kernel rule today.

## Vendor dynamic evidence (offline compiler)

Production chip-5 callbacks via `tools/re/chrome9_pps_probe.c`,
`CHROME9_POSTPROCESS=2`.  Tokens omit the `ffff0200` header and the
`0000ffff` end token.

| D3D PS 2.0 | final ALU records | A90 / A92 | active tuple |
|---|---|---|---|
| `mov oC0,t0` | `01c00530 301c0053 00f2c005 000d0820` | `90058001/92000001` | `1 0 1 0` |
| `add oC0,t0,t1` | `02c10530 30140924 00f2c005 00000820` | `90058003/92000001` | `1 0 1 0` |
| `mul oC0,t0,t1` | `0140b6d0 302c1053 00f2c005 00000820` | `90058003/92000001` | `1 0 1 0` |
| `mad oC0,t0,t1,t2` | `02c20530 302c1053 00f2c005 00000820` | `90058007/92000001` | `1 0 1 0` |
| `add oC0,t1,t2` | same as `add t0,t1` (compacted) | `90058003` | `1 0 1 0` |
| `add oC0,t0,t0` | `02c00530 30140924 00f0c005 00000820` | `90058001` | – |
| `dp3 oC0,t0,t0` | `01c00530 502c0055 00f0c005 00020820` | `90058001` | `1 0 1 0` |
| `abs oC0,t0` | `02c00538 300c0053 00f0c005 00090820` | `90058001` | `1 0 1 0` |
| `abs oC0,v0` | `00400538 30040053 00f04005 00090820` | `90058000` | `0 0 0 0` |
| `add oC0,v0,v1` | `00410530 30140924 00f04005 00000820` | `90058000` | – |
| `mul oC0,t0,v0` | `0140b6d0 30040053 00f2c005 00000820` | `90058001` | `1 0 1 0` |
| `mov r0,t0; add oC0,r0,t1` | `01c00530 301c0053 00f2c005 000d0020` `02c10530 30140924 00f20005 00000820` | `90058003/92000002` | `2 1 2 0` |
| `mov r0,t0; mov r1,t1; mad oC0,r0,r1,t2` | `01c00530 301c0053 00f2c005 000d0020` `01c00530 301c0053 00f2c105 000d0060` `02c20530 30201053 00f20005 00000820` | `90058007/92000003` | `3 2 3 0` |
| `mov r0.xyz,t0; dp3 oC0,r0,t1` | `01c00530 501c0053 0072c005 000d0020` `01c00530 502c1055 00f20005 00020820` | `90058003/92000002` | `2 1 2 0` |
| `mov r0,t0; sub oC0,r0,t1` | `01c00530 301c0053 00f2c005 000d0020` `82000530 30140924 00f2c105 00000820` | `90058003/92000002` | `2 1 2 0` |
| `mov r0.xy,t0; mad oC0,r0.xxxx,t1,r0.yyyy` | `01c00530 d01c0053 0032c006 000d0020` `00002490 002c1053 00f20000 00000820` | `90058003/92000002` | `2 1 2 0` |
| `mov r0.xy,t0; mul oC0,vPos.xyxy,r0.xyxy` | `01c00530 d01c0053 0032c006 000d0020` `0140b6d0 10200041 00f2c304 00000820` | `90058009/92000002` | `1 1 1 0` |

Observations:

* The PS compiler itself has **no** read-port legalization.  It emits two
  or three distinct T sources in one record.  The record, A90 and A92 are
  identical to the kernel's.
* Distinct T registers are compacted, so `add t1,t2` equals `add t0,t1`.
* The vendor sets selectors of lanes outside the destination mask to ZERO
  in every slot, e.g. `mov r0.xy,t0` gives `d01c0053 0032c006`.
* With one T per record the active tuple counts every T.  A00 becomes `21`
  or `31`, matching the A08 coordinate count.

## Answers to the three questions

### 1. Per-record limits and hazards

| source class (native) | distinct registers per record | basis |
|---|---:|---|
| T varying / MISC (3) | **1** | silicon (3 timeouts, all passes ≤1), static GL limit 1, manual `ps_2_0` 1 |
| PRIMARY v0 (1) | 1 (UAPI admits only v0) | static GL limit 1, manual 1; two distinct unmeasured |
| CONST (2) | 2 (three distinct aliases) | silicon; vendor front end is stricter (1) |
| TEMP (0) | 3 | static (temp limit 3); silicon MAD/CMP with TEMP sources |
| TAU (sampler) results | TEMP class; no separate limit | dynamic (`texld` results are class 0) |
| literal ONE/ZERO (5) | not a read port | static (`PSOPT_CountReadPort` ignores class 5) |

Hazards:

* **Same T in several slots:** safe.  The rule counts distinct registers.
  `ABS T0` (A, C and B) passed on silicon, and the vendor front end
  deduplicates.
* **T with swizzles or partial width:** unaffected.  The GL limit counts
  register numbers only.  `add oC0,t0.x,t1.y` still has two distinct T.
* **T + MISC in one record:** two class-3 registers, so this is under the
  rule (inference; the vendor front end cannot see MISC as `t#`, and the
  shape has never run on silicon).
* **T + PRIMARY, T + constant, T + TEMP:** different classes, legal under
  the D3D and GL limits.  Silicon: T + TEMP runs in the three-record chain;
  T + PRIMARY and T + two constants are unmeasured.
* **Co-issue:** at most 2 T and 2 inputs across a co-issued pair (static,
  not used by the kernel).

### 2. Is the kernel ADD the vendor's ADD?

Yes, it is byte-exact (dynamic).  Controls A90..A92 also match.  The
kernel FE derives A00 from the same one-source-per-record analysis
(`chrome9_fragment_resource_note_alu`), so the FE image is the one the
vendor compiler object implies.  What differs is the vendor's *front end*:
it would never submit the record.  Nothing else in A90..A9b, A00 or A08
explains the hang.  The two remaining explanations, one physical
texture-class read port per record and an FE/interpolator undercount (A00
counts one input while two are read), are both removed by the same
legalization.  Only a deliberately unsafe discriminator can separate them
(ladder rung 7).

### 3. `90058003`

Correct (dynamic, vendor-identical).  Bit 1 is set because compact T1 is
read by an ALU.  After legalization the word is the same (`90058003`)
and only A92 changes to two ALUs.

## Kernel legalization rule

Before validation, for each ALU instruction with two or more sources
(TEX and KILL excluded):

1. The kept register is the MISC position if the instruction reads it,
   otherwise the varying with the highest logical index (the vendor GL
   pass moves the lowest numbers first).
2. For every other varying named by the instruction, insert one
   `MOV Tn.mask, Vj` immediately before it, with an identity swizzle and
   ZERO outside the mask.  `mask` is the union of the components that all
   operands naming `Vj` select in the lanes the consumer uses
   (`chrome9_fragment_alu_component_mask`); a literal-only operand copies
   `.x`.
3. Rewrite those operands to `Tn`.  Keep their swizzle in used lanes and
   their NEG modifier, and select ZERO in unused lanes (vendor convention).
4. `Tn` is the highest logical temp index the program does not use.
   Allocation, liveness, release bits, bucket and A00 then follow the
   existing compiler unchanged.  The pass returns `-E2BIG` if the program
   would exceed 128 instructions, has no free logical temp, or later
   exceeds the 12 physical temps or 96 ALU records.

Because the MOV copies only referenced components, varying and ALU widths
(A01/A08/A09 consumer and producer widths) are unchanged.  Programs that
already read at most one texture-class register per record are unchanged
byte for byte.

Rejecting instead (`-EINVAL` in `chrome9_fragment_validate` when two
sources name different class-3 registers) is the smaller alternative.  It
would push the same split into Mesa, which can reuse its own register
allocation.

### Draft patch

`~/tmp/agent-psports/0001-drm-chrome9-one-texture-class-read-per-ALU-record.patch`
(-p1 against `linux-7.2`, signed off).  It applies cleanly with a dry run,
builds W=1 clean out of tree against the prepared kernel tree, and passes
`checkpatch` with 0 errors and 0 warnings (`--strict` only flags the file's
existing `(`-line style).  The host self-test
(`tools/re/chrome9_kfrag_selftest.c`) passes.  Self-test changes:

* new `chrome9_fragment_texture_read_selftest`: `ADD T0,T1`,
  `MAD T0.x,T1,T0.y` and the unchanged `DP3 T0,T0`, all against the vendor
  goldens above;
* the `resource_mad` golden becomes the vendor three-record split with
  active count 3;
* `MUL vPos,T0` / `MUL T0,vPos` become the vendor two-record split;
* the old MISC-alias rejection program now compiles (MISC at location 4,
  `A90=900d001f`).

Kernel outputs checked against the vendor with the same tokens: ADD, SUB,
MAD (three T), MAD with a constant, CMP, DP3, MAD with a repeated
swizzled T, and both vPos orders all match exactly.  One pre-existing
difference is not caused by this patch.  In the five-record position
program the kernel's allocator reuses physical r0 for a later copy, while
the vendor keeps r3.  The earlier last read of r0 therefore has no release
bit.  This follows the existing allocator and finalizer policy for any
user program of that shape.

## Silicon test ladder (after a cold, healthy engine)

Use the usual guarded protocol: GPU lock, known-good baseline first, one
submission per rung, stop and quarantine on the first timeout.  Load the
patched module for rungs 2 onwards; rung 1 needs no module change.

| rank | program (kernel UAPI) | expected native image | purpose | hang risk |
|---:|---|---|---|---|
| 1 | `MOV oC0,T0` | `01c00530 301c0053 00f2c005 000d0820`, A00=`11` | baseline | low (passed) |
| 2 | `ADD oC0,T0,T1` (patched → `MOV r,T0; ADD oC0,r,T1`), T0=(.25,0,0,.5), T1=(0,.5,0,.5) | vendor split above, A90 `90058003`, A92 `92000002`, A00=`21`; expect (.25,.5,0,1) | proves the rule and the two-input FE | low–medium |
| 3 | `ADD oC0,T0,T0` (one record, same T in B and A) | `02c00530 30140924 00f0c005 00000820` | same-register repeat in the ADD form | low (ABS precedent) |
| 4 | `MAD oC0,T0,T1,T2` (patched three-record split), T0=T1=0, T2=red | vendor three-record golden, A00=`31` | reopens the timed-out MAD | medium |
| 5 | `MUL oC0,T0,v0` (T + PRIMARY) | `0140b6d0 30040053 00f2c005 00000820` (vendor-identical) | mixed class 3 + 1 | medium (unmeasured) |
| 6 | `MAD oC0,T0,c0,c1` | `00810530 30080053 00f2c005 00000820` (vendor-identical) | T + constant ports | low–medium |
| 7 | *not recommended:* unsplit `ADD T0,T1` with A00 forced to `21` | – | separates port limit from FE undercount | high (cold boot) |

Only rung 2 is needed to qualify the patch.  Rungs 3–6 widen the envelope
for Mesa's NIR lowering.  Rung 7 answers only the mechanism question and
can stay unanswered.

## Open items

* The kernel `ABS` varying release marks slot A; the vendor marks B
  (`02c00538 300c0053 00f0c005`).  The silicon ABS pass used the kernel
  form, so this is a golden mismatch, not a known fault.
* PRIMARY + another input class, and two distinct inputs, are unmeasured.
* MISC + T in one record is kept out by the patch but has never run on
  silicon either way.
* Mesa could split in NIR and use its own register allocation; the kernel
  pass is the safety net.

## Reproduction

```sh
cd /home/ai/src/openchrome
W=~/tmp/agent-psports; L=$W/lib; mkdir -p $L
gcc -m32 -shared -fPIC -Wl,-soname,libexpat.so.1 tools/re/chrome9_stub.c -o $L/libexpat.so.1
gcc -m32 -shared -fPIC -Wl,-soname,libdrm.so.2   tools/re/chrome9_stub.c -o $L/libdrm.so.2
gcc -m32 -O1 tools/re/chrome9_pps_probe.c -ldl -o $W/pps
# add oC0,t0,t1 and the vendor split form
LD_LIBRARY_PATH=$L CHROME9_POSTPROCESS=2 CHROME9_DUMP_CONTROLS=1 CHROME9_RESOURCE_COUNTS=1 \
  $W/pps ffff0200 04000002 000f0800 30e40000 30e40001 0000ffff
LD_LIBRARY_PATH=$L CHROME9_POSTPROCESS=2 CHROME9_DUMP_CONTROLS=1 CHROME9_RESOURCE_COUNTS=1 \
  $W/pps ffff0200 03000001 000f0000 30e40000 04000002 000f0800 00e40000 30e40001 0000ffff
# static
objdump -d --no-show-raw-insn binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so > $W/dri.dis
grep -n -A60 '<stmDecideMSTokenRegeneratorInfoICD>:' $W/dri.dis
```

The kernel host harness is `~/tmp/agent-psports/kdump.c`, built by
`~/tmp/agent-psports/build.sh <dir> <suffix>` with the `tools/re/kshim`
headers.  The same script builds the self-test from
`tools/re/chrome9_kfrag_selftest.c`.
