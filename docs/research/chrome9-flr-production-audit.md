# Chrome9 FLR production mismatch

Status: internal opcode 103 was invoked through production Windows and Linux
H5 register management and configuration two. Both drivers emit the expected
two-record `x - FRC(x)` expansion, but they choose different scratch-register
classes/ids and different A90 control buckets. FLR is therefore a strict
cross-driver rejection boundary, not a portable compiler key.

Exact rows are retained in
[`chrome9-flr-production-corpus.tsv`](chrome9-flr-production-corpus.tsv).

## Invocation provenance

The Windows wrapper tokenizes an FRC-shaped instruction, changes only
normalized opcode 19 to internal opcode 103, then uses the recovered
production H5 table builder, scheduler, and configuration-two finalizer.
Wrapper SHA-256 is
`4062a929ad5188cf75c53aaee1ed369353f3829369ff3cccace75f6dd4caa986`.
The four-case JSON SHA-256 is
`9545deb52dd8f24a3b347c27dd900383fea45650f9e87a580daa6847ef46fee5`;
the separate v0.wwww JSON SHA-256 is
`dcc0ed3336dfb88246dd78f28af17935d746c40917b51ed6255104247272f64f`.

Linux exports `stmPPS_FLR_ICD_PS20_invi` at VA `00164290`. Direct invocation
on an initialized production H5 instance, followed by
`stmPPS_PostTranslationSetting_invi(instance+0x10,2)`, succeeds for terminal
v0, c0, and v0.wwww. The bounded helper SHA-256 is
`8e774cf9d7d42b65935ec30a26b2341dcb92cafde4ba39013fa46a0d1f1fdc63`.

## Arithmetic closure

Both emitters implement exactly two records:

1. FRC subop e writes a scratch TEMP.
2. An arithmetic record computes original source minus that scratch, with
   A=original, B=NEG scratch, C=literal ONE, and destination oC0.

Configuration two sets B's last-use flag in d0.bit25. The v0.wwww case
proves that the source swizzle is propagated to both the FRC source and the
second record's original-source A slot.

## Exact portability mismatch

For terminal FLR(v0), Windows produces:

```text
raw:
01c00530 301c0053 00f04005 000e0320
000c0538 30140924 00f04005 00000820
config2:
01c00530 301c0053 00f04005 000e0320
020c0538 30140924 00f04005 00000820
```

The scratch is special TEMP id12 (`...0320`, B id12),
`temporary_write_mask=0`, A90=`90040000`, and A92=`92000002`.

Linux produces:

```text
raw:
01c00530 301c0053 00f04005 000e0020
00000538 30140924 00f04005 00000820
config2:
01c00530 301c0053 00f04005 000e0020
02000538 30140924 00f04005 00000820
```

The scratch is ordinary physical id0, `temporary_write_mask=0`,
A90=`90058000`, and A92=`92000002`. c0 and v0.wwww reproduce exactly the
same id/control mismatch. Although both streams express the same arithmetic,
neither may be selected as a portable runtime contract without independent
silicon proof and a deliberate allocator/control policy.

## Explicit-TEMP timeout boundary

Two Windows production compositions reached the bounded 30-second wrapper
deadline:

- `mov r0,c0; flr oC0,r0`;
- `flr r0,c0; mov oC0,r0`.

The terminal internal cases finish in about four seconds. Preserve the two
timeouts as allocator/composition negative evidence and do not retry them
unboundedly. Linux public FRC success for analogous tokens is not internal
FLR evidence.

## Policy

Reject native FLR and any generic `ffloor` mapping. The current structured
UAPI cannot encode the internal scratch lifetime, the vendor streams disagree
on that scratch and A90, and no silicon test has established negative-floor,
signed-zero, denormal, NaN/Inf, or precision behavior. Terminal finite c0 may
still be constant-folded on the CPU into the existing MOV(c0) path under its
separate finite/FP contract.
