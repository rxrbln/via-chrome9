# Chrome9 FLR production allocation closure

Status: internal opcode 103 was invoked through matching production Windows
and Linux H5 register-table initialization and configuration two. Both
drivers byte-match the two-record `x - FRC(x)` expansion, special scratch r12,
and A90=`90040000`. An earlier apparent Linux r0/A90=`90058000` result was a
direct-emitter harness artifact caused by omitting `stmSetRegMgr(instance,8,12)`.

Exact corrected rows are retained in
[`chrome9-flr-production-corpus.tsv`](chrome9-flr-production-corpus.tsv).

## Invocation provenance

The Windows wrapper tokenizes an FRC-shaped instruction, changes only
normalized opcode 19 to internal opcode 103, then uses the recovered
production H5 table builder, scheduler, and configuration-two finalizer.
Wrapper SHA-256 is
`4062a929ad5188cf75c53aaee1ed369353f3829369ff3cccace75f6dd4caa986`.
The four-case JSON SHA-256 is
`9545deb52dd8f24a3b347c27dd900383fea45650f9e87a580daa6847ef46fee5`;
the v0.wwww JSON SHA-256 is
`dcc0ed3336dfb88246dd78f28af17935d746c40917b51ed6255104247272f64f`.

Linux exports `stmPPS_FLR_ICD_PS20_invi` at VA `00164290`. The corrected
helper initializes the production H5 callbacks, calls exported
`stmSetRegMgr_invi(instance,8,12)`, invokes the emitter, then calls
`stmPPS_PostTranslationSetting_invi(instance+0x10,2)`. Its SHA-256 is
`67bd937b2dbe16f29c25c5963ff52b9a63308455f72196ccae0ecdb9a0e3508d`.

## Allocator call and callback provenance

Both emitters request the same scratch class:

- Windows `10216a2e..10216a37` loads callback `backend+0x138c`, pushes
  `special_ok=1` and the compiler object, then calls it;
- Linux `001642e8..001642f9` loads the same `backend+0x138c` callback and
  passes `(instance,1)`;
- both release the returned physical id through `backend+0x1394` at Windows
  `10216bfe..10216c0c` and Linux `001644c2..001644cc`.

The Windows H5 allocator is at VA `1020ec70`; Linux
`stmPPS_GetFreeTmpSlot_h5i` is at `00162000`. The recovered Windows D3D and
OGL copies of the allocator are raw-identical (size `0x1aa`, SHA-256
`d58a71a8b82e059d29103b4e588d8026cbe47259eab0a40a57377844fe8d0a8f`).
Thus there is no proprietary-driver algorithm mismatch to explain.

H5 limits match in both setups:

```text
backend+1354 = 12  ordinary TEMP limit
backend+1358 = 4   special TEMP count
backend+1360 = 12  special TEMP base
```

The production table builder at Linux `001b3f20` (byte-matched Windows
builder RVA `001da3e0`) sets the scratch cursor `backend+0x1350` to special
base 12 at `001b40aa..001b40b3`, resets ordinary slot activity through
`001b40c8..001b40df`, and clears special activity through
`001b40e1..001b4110`. With no logical ordinary TEMP in terminal FLR, the H5
allocator's ordinary dynamic window is empty and `special_ok=1` selects r12.
After allocation and release, the observed cursor is 13 and r12 is inactive.

The incomplete earlier helper called only `stmPPSSetRegManager_invi`; its
ordinary scan window still exposed r0. The identical allocator therefore
returned r0 and configuration two derived a different resource bucket. That
result is retained only as a harness-initialization warning, not compiler
evidence.

## Exact byte-matched expansion

Both production paths emit terminal FLR(v0) as:

```text
raw:
01c00530 301c0053 00f04005 000e0320
000c0538 30140924 00f04005 00000820
config2:
01c00530 301c0053 00f04005 000e0320
020c0538 30140924 00f04005 00000820
```

The first record is FRC subop e to special r12. The second computes
A=original plus B=NEG r12 times C=literal ONE, i.e. `x - FRC(x)`. Configuration
two sets the B last-use flag in d0.bit25. c0 and v0.wwww byte-match too; the
swizzle propagates to the FRC source and the second record's original-source
A slot.

`temporary_write_mask` remains zero because r12 is compiler scratch rather
than a logical program TEMP. `stmGetActiveTexTmpNum` reports `(0,13,0,0)`:
active1 is the highest used scratch location plus one. This exceeds the
resource-bucket thresholds, so configuration two adds no `0x18000/0x10000/
0x08000` tier and leaves A90=`90040000`; A92 is `92000002`. The earlier r0
harness artifact reported active1=1 and therefore A90=`90058000`.

## Explicit-TEMP timeout boundary

Two Windows production compositions reached the bounded 30-second wrapper
deadline:

- `mov r0,c0; flr oC0,r0`;
- `flr r0,c0; mov oC0,r0`.

Terminal internal cases finish in about four seconds. Preserve the timeouts
as allocator/composition negative evidence and do not retry them unboundedly.

## Policy

Reject native FLR and any generic `ffloor` mapping. Compiler portability is
now closed for the tested terminal sources, but the current structured UAPI
does not expose compiler-created scratch lifetime and no silicon test has
established negative-floor, signed-zero, denormal, NaN/Inf, or precision
behavior. Terminal finite c0 may still be constant-folded on the CPU into the
existing MOV(c0) path under its separate finite/FP contract.
