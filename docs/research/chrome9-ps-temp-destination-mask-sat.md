# Chrome9 multi-record TEMP destination mask and SAT boundary

Status: exact production-initialized Windows D3D and Linux 5.75 compiler
differential for two-record MOV/MUL chains.  Partial TEMP destinations and
TEMP destination saturation are byte-closed at compiler level, but remain
reject-only for the first structured semantic UAPI.

## Scope and provenance

The Windows side used the production H5 register-table builder, physical
TEMP remap, scheduler, and configuration-two finalizer described in
[`chrome9-ps-temp-swizzle-neg.md`](chrome9-ps-temp-swizzle-neg.md).  The
wrapper SHA-256 is
`c56637fad7bfaeb994595e3363d439b7f0e35bcf78138bdc0d2bfdc70fdfa4e1`.
Every case was compiled independently by the Linux 5.75 compiler and the
Windows D3D compiler; all finalized ALU records match byte for byte.

The retained JSON objects are:

- live producer/consumer matrix SHA-256
  `2febba0307bd260038f16343d61c8fb983567f782af838af32b0dea664b19d41`;
- dead-producer matrix SHA-256
  `468cf4e7a86bf799f1e2c84e1c91f9b1024bee868a99f45b22358ac0cfe2d072`.

Both use PS 2.0 token streams and an ordinary TEMP r0.  The tested producer
forms are MOV and MUL, masks X, XY, XYZ, W, and XYZW where applicable, and
SAT both with a full mask and with XYZ.

## Exact destination fields

The differential confirms the existing field decode in a multi-record TEMP
context:

- destination mask is d2 bits `23:20`;
- destination SAT is d3.bit12;
- ordinary TEMP r0 destination is d3 low word `0020` before other controls;
- SAT and the mask are independent fields.

Representative producer records are:

| operation | mask/SAT | producer record |
|---|---|---|
| MOV r0,c0 | X | `01c00530 d01c0053 00108016 000d0020` |
| MOV r0,c0 | XYZ | `01c00530 501c0053 00708005 000d0020` |
| MOV r0,c0 | XYZW+SAT | `01c00530 301c0053 00f08005 000d1020` |
| MOV r0,c0 | XYZ+SAT | `01c00530 501c0053 00708005 000d1020` |
| MUL r0,v0,c0 | X | `0140b6d0 d008016d 00104016 00000020` |
| MUL r0,v0,c0 | XYZ | `0140b6d0 50080055 00704005 00000020` |
| MUL r0,v0,c0 | XYZW+SAT | `0140b6d0 30080053 00f04005 00001020` |
| MUL r0,v0,c0 | XYZ+SAT | `0140b6d0 50080055 00704005 00001020` |

The apparent changes outside d2 mask bits are source-selector packing for
lanes the operation does not write.  Therefore a future packer must rebuild
the complete record from the operation and mask rather than patching only
d2 bits `23:20` into a full-mask template.

## Partial-write consumer rewrite

For `producer r0.M,...; mov oC0,r0`, the compiler changes the terminal TEMP
source selector according to the producer mask:

| producer mask | terminal source selector | finalized terminal MOV |
|---|---|---|
| X | `x001` | `01c00530 c01c0053 00f20016 000d0820` |
| XY | `xy01` | `01c00530 c01c0053 00f20006 000d0820` |
| XYZ | `xyz1` | `01c00530 401c0053 00f20005 000d0820` |
| W | `000w` | `01c00530 b01c0053 00f200b6 000d0820` |
| XYZW | `xyzw` | `01c00530 301c0053 00f20005 000d0820` |

Selectors `0` and `1` in this table are the native literal ZERO and ONE,
not component names.  This is a compiler dataflow rewrite, not merely a
destination-field encoding.  The corpus does not establish the general
merge rule for two partial writes to one TEMP, nor does it prove these
literal-fill semantics on silicon.  Partial TEMP writes must therefore
remain rejected until a complete per-component definition/merge algorithm
and guarded hardware oracle exist.

Destination SAT does not otherwise disturb the consumer.  For full XYZW the
terminal source remains identity; for XYZ+SAT it gets the same `xyz1`
rewrite as unsaturated XYZ.

## Last-use and dead-destination finalization

With a live producer, configuration two changes only the terminal TEMP read:
its A-slot last-use marker is d2.bit17.  It does not mark the producer
destination.  Mask and SAT do not change this rule.

With a dead producer followed by `mov oC0,v0`, configuration two sets
d3.bit29 on the dead TEMP destination.  It composes directly with SAT:

```text
dead MOV r0.x,c0       ... 000d0020 -> ... 200d0020
dead MOV_SAT r0.xyz,c0 ... 000d1020 -> ... 200d1020
dead MUL r0.x,v0,c0    ... 00000020 -> ... 20000020
dead MUL_SAT r0.xyz    ... 00001020 -> ... 20001020
```

Windows and Linux match in all four rows.  This reinforces that d3.bit29 is
a physical-TEMP dead-destination/lifetime marker independent of writemask
and saturation.

## Allocation and controls

Every row retains:

- physical TEMP allocation r0;
- `temporary_write_mask = 00000001` even for an X-only or W-only write;
- active tuple `(0,1,0,0)`;
- A90 `90058000` and A92 count `92000002`.

Thus TEMP resource accounting is per physical register, not per component,
and neither partial masks nor SAT alter allocation or resource controls.

## Fail-closed contract

The first structured multi-record UAPI must continue to require:

- full XYZW TEMP destinations;
- no destination SAT or result scale;
- one complete definition before every TEMP read;
- no partial-write merge, read-before-definition, WAW, or dead TEMP;
- kernel-rebuilt physical allocation and lifetime flags.

The compiler-level byte match is useful future evidence, not authorization
to expose these forms.  Destination SAT still requires guarded pre-RT lower
and upper clamp silicon tests.  Partial masks require both the full merge
algorithm and pixels that distinguish native literal-fill behavior from a
normal component-preserving write.

Exact token streams and raw/final records are in
[`chrome9-ps-temp-destination-mask-sat-corpus.tsv`](chrome9-ps-temp-destination-mask-sat-corpus.tsv).
