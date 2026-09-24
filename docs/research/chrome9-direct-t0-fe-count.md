# Chrome9 programmable front-end texture-resource count

Status: exact Linux production-compiler differential, exact Linux/Windows
finalizer provenance, and exact Windows D3D runtime writer.  The initial
direct-T0 correction is live-qualified.  The generalized scheduled-record
resource rule is implemented and host-selftested, but is pending a bounded
silicon check.

## Result

A pixel program that reads an interpolated texture-coordinate register
directly in ALU requires a nonzero type-FE A00 count even when the program has
no TAU instruction and no sampler.  However, A00 bits 7:4 are **not** the
logical-coordinate high-water, coordinate population, sampler high-water, or
the maximum of those quantities.  The programmable Windows path copies the
compiler's active texture-temporary resource count to A00.

Consequently neither the old generic rule

```text
A00[7:4] = fragment_texture_count
```

nor the former generic rule

```text
A00[7:4] = max(fragment_texture_count, compact_coordinate_count)
```

matches the production programmable pipeline.  The source-backed rule is:

```text
A00[7:4] = compiled active texture-temporary resource count
```

A00 bit zero remains the independent cache-clear pulse.  The resource count
must be derived from the scheduled native program; it does not require a
dummy image, sampler, TAU instruction, or texture BO.

## Finalizer and runtime provenance

In `VTGUModeDX32A.dll` (SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`),
the programmable pixel-shader draw update selects the compiled-object view in
`esi`.  At `0x1015b255..0x1015b26b` it performs exactly:

```text
mov ecx,[esi+24h]
shl ecx,4
xor ecx,[edi+1c90h]
and ecx,000000f0h
xor [edi+1c90h],ecx
```

The hardware image at `edi+0x1c90` is type-FE subaddress A00.  The initializer
at `0x101549a8..0x101549c0` writes consecutive A00--A13 headers there.  Thus
the writer copies compiled-object `+0x24`, equivalently public instance
`+0x34`, into A00 bits 7:4.

The field's producer is also exact.  Linux
`stmPPS_PostTranslationSetting_invi:0x19b075..0x19b094` passes four output
pointers to `stmGetActiveTexTmpNum`, then stores the third output at
`0x19b645..0x19b648` to compiled-object `+0x24`.  Windows D3D does the same at
`0x10202824..0x10202831` and `0x102031ed..0x102031fa`; the OpenGL finalizer is
the independently relocated equivalent.  The checked-in probe reports those
four outputs in call order as `active-resource-counts`.

The D3D runtime later consumes A00 at `0x1015b3b4..0x1015b42e`: it loops over
that many dimension entries, maps widths one through four to masks `8,c,e,f`,
and accumulates a vertex-output mask.  This does not make the compiler field a
logical-coordinate count; the exact differentials below disprove that
interpretation.

## Production compiler differential

The exact Linux binary is
`binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, build ID
`f140f76cfb18e3f368245054629d1ef2e67be129`.  Running its PS 2.0 compiler,
production register manager, resource query, and configuration-two finalizer
gives:

| program | resource tuple | ALU/TAU | instance `+0x34` | logical-use `+0x40` |
|---|---:|---:|---:|---:|
| `MOV oC0,c0` | `(0,0,0,0)` | 1/0 | `0` | `0` |
| `MOV oC0,v0` | `(0,0,0,0)` | 1/0 | `0` | `0` |
| `MOV oC0,T0` | `(1,0,1,0)` | 1/0 | `1` | `1` |
| `MOV oC0,T5` | `(1,0,1,0)` | 1/0 | `1` | `20` |
| `ADD oC0,T0,T1` | `(1,0,1,0)` | 1/0 | `1` | `3` |
| `MAD oC0,T0,T1,T2` | `(1,0,1,0)` | 1/0 | `1` | `7` |
| three-record distinct `T0/T1/T2` chain | `(3,2,3,0)` | 3/0 | `3` | `7` |
| three-record repeated `T0` chain | `(1,2,1,0)` | 3/0 | `1` | `1` |
| `TEX(s5,T5); MOV oC0,r0` | `(1,1,1,6)` | 1/1 | `1` | `20` |

The one-record MAD is decisive against coordinate population and high-water:
three distinct logical and encoded coordinates produce `+0x40=7` but only
one active texture-temporary resource.  The two three-record chains prove
that ALU count alone is not the rule.  The sparse stage-five TEX is decisive
against sampler high-water: its fourth resource output is six and its TAU
retains sampler five, while `+0x34` and therefore A00 remain one.  The runtime
does not take a maximum with bound texture state.

`stmGetActiveTexTmpNum:0x19a410..0x19ab9d` derives the tuple by inspecting the
scheduled 16-byte ALU and TAU records and four per-location activity maps.  It
is a native resource/lifetime analysis, not a source-level varying counter.
The H5 callback table independently identifies texture-slot allocation with
`stmPPS_GetFreeTexSlot_invi`, `stmPPS_ReleaseTexSlot_invi`, and
`stmPPS_GetReMapTexSlot_invi`.  `CHROME9_TRACE_REGS=1` in the checked-in probe
traces these callbacks as well as arithmetic-temporary callbacks.

## Exact PS 2.0 third-output algorithm

The third output is now statically closed for PS 2.0.  The independently
compiled Windows routine at `VTGUModeDX32A:0x10201e60..0x102024d6` makes the
four 16-entry maps particularly clear:

```text
A[16] at ebp-f8   arithmetic temporary use counts
B[16] at ebp-64   externally required texture-register component masks
C[16] at ebp-b8   internally defined texture-register component masks
D[16] at ebp-138  sampled-stage presence
```

The Linux routine uses the equivalent maps at `ebp-48`, `ebp-88`, `ebp-c8`,
and `ebp-108`.  For each scheduled ALU record it handles at most one
texture-class source, in native operand priority A, then C, then B.  Windows
tests source A at `0x1020200d`, source C at `0x1020229b`, and source B at
`0x10202348`; each successful branch joins the common path before either
lower-priority source can be considered.  Linux has the same three-way
control flow at `0x19a550 -> 0x19a870`, `0x19a562 -> 0x19aa58`, and
`0x19a572 -> 0x19aae8`.

For the selected source, the routine derives a four-bit component-use mask
from its native swizzle.  Literal-zero and literal-one selectors do not set a
component bit.  For every selected component not already present in `C[id]`,
it sets the corresponding bit in `B[id]`.  A texture/address-class ALU
destination performs the converse operation at
`VTGUModeDX32A:0x1020216b..0x10202221`: every written component not already
required in `B[id]` is added to `C[id]`.  Thus a read before a later write
remains externally required, while a write before a read makes the later read
internal.

A TAU record marks all four components of its coordinate/address register in
`B` unless that component is already in `C`
(`VTGUModeDX32A:0x10201f15..0x10201f77`).  Non-TEXKILL TAUs also mark the
sampled stage in `D`; operation 4 does not.  The instruction walk follows the
actual ALU/TAU phase and transition maps, so these masks describe scheduled
execution order rather than source-token order.

For PS 2.0, the third output is exactly:

```text
limit = compiled_object[0x2c] ? compiled_object[0x2c] : 16
third = 1 + highest i below limit for which B[i] != 0, else 0
```

Windows implements the limit and descending scan at
`0x1020248d..0x102024b9`, then stores the result through the third output
pointer at `0x102024bc..0x102024bf`.  Linux performs the relocated equivalent
at `0x19a758..0x19aa2c`.  `compiled_object+0x2c` is the declaration-derived
bound (public instance `+0x3c`); the zero-declaration path deliberately scans
all 16 entries.

This also explains the otherwise surprising differential.  A single native
MAD with three texture-class operands enters the source-A branch and creates
only `B[0]=0xf`, so its third output is one.  The three-record chain selects a
different texture-class source in each record and creates
`B[0..2]={0xf,0xf,0xf}`, so its third output is three.  Debugger inspection of
the Linux binary produced those exact maps and return tuples `(1,0,1,0)` and
`(3,2,3,0)`.

There is no source-backed conservative substitution.  The failed guarded MAD
used three where the compiler requires one, so rounding up to varying count,
texture count, population, or high-water is not known safe.  The backend must
run this scheduled-record analysis exactly, or reject a shape it cannot
analyze.  In particular, count only the first texture-class ALU source in
native A/C/B priority; counting every texture operand changes the vendor
contract.

The finalized direct-T0 object remains:

```text
ALU 01c00530 301c0053 00f2c005 000d0820
TAU count 0
A90..A92 90058001 91000000 92000001
```

A valid PS 2.0 `dcl T0` changes declaration metadata but produces the same
native instruction, controls, and `+0x34=1`.  The Linux DCL parser increments
instance `+0x3c` at `stmPPSTokenParser20_invi:0x1b7310..0x1b7344`; the H5 DCL
emitter is a no-op at `stmPPS_DCL_PS20_invi:0x162c60..0x162c66`.

## Former mismatch and current validation boundary

The original direct-T0 packet's A00=`00000001` was incomplete.  A00=`11`
subsequently passed the guarded direct-T0 test, proving the one-resource case.
That result does not validate the later `max(texture_count,varying_count)`
generalization.

The earlier kernel `chrome9_fragment_fe_config()` computed
`max(texture_count, varying_count)`.  It emitted A00=`31` for the one-record
`MAD oC0,T0,T1,T2` discriminator, although the byte-exact vendor object
requires resource count one and A00=`11`.  That guarded draw timed out after
reaching the post-draw closure.  The mismatch is exact; causality still
requires a single corrected cold-boot test.

Kernel commits `eb8524b` and `5d70f4b` now carry an independently computed
`active_texture_count` in `chrome9_fragment_program` and use it for FE A00.
The compiler computes the count from scheduled native records with the
`B/C` component-mask analysis above; A08 still uses the independent producer
coordinate high-water.  Host selftests cover the contradictory pairs:

```text
MOV T0                          -> 1
MAD T0,T1,T2                    -> 1
three-record distinct T0/T1/T2 -> 3
three-record repeated T0       -> 1
TEX s5,T5                      -> 1, sampler high-water 6 separately
```

Changing all multi-varying programs to one would be equally wrong.  The host
selftests establish the compiler-to-FE contract, not silicon behavior: rerun
the guarded MAD discriminator with A00=`11` after a cold boot, and keep any
shape the native analyzer cannot represent unsupported rather than falling
back to varying count, texture count, or their maximum.

That rerun must use the later ordered-FE implementation in kernel commit
`d359ed1`.  Every earlier guarded three-varying MAD, split-output, and chain
submission predates that commit: those packets wrote only the first A01
consumer field (for example `01002523`) while the proprietary writer packs
the subsequent physical T1/T2 fields into the remaining A01 bytes.  For
three float4 ALU inputs and no sampler, current setup emits
`A01=012a2523` and explicitly resets A02/A03.  The earlier timeouts therefore
do not qualify the corrected A00 plus ordered-consumer tuple.  The next live
discriminator is still the byte-checked one-record MAD, but on a module
containing both fixes; stop after the first timeout rather than replaying the
older split-write matrix.
