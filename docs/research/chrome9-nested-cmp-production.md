# Chrome9 nested CMP production differential

Status: exact hardware-free Linux production compiler/configuration-two
packing and lifetime evidence.  It does not prove the compare sense or signed
zero boundary on silicon and does not map general NIR boolean selection.

Every row begins with `mov r0,v0` and terminates in a full-XYZW CMP to `oC0`.
The corpus places r0 in each one of the three logical positions and every
duplicate combination:
[chrome9-nested-cmp-production-corpus.tsv](chrome9-nested-cmp-production-corpus.tsv).

CMP uses exactly the same physical source placement as MAD:

| D3D logical operand | native slot |
|---:|---|
| condition/source 0 | A |
| selected source 1 | C |
| selected source 2 | B |

Its one-record discriminator is d3 subop 9 (`00090820`).  Moving the temporary
through the three positions moves its configuration-two marker through A
d2.bit17, C d1.bit21, and B d0.bit25.  Duplicate forms again close the complete
A-over-C-over-B precedence: A+C selects A, A+B selects A, C+B selects C, and
A+C+B selects A.  Controls are the standard two-record ALU-only transition
`90040000 -> 90058000`, with A92 `92000002`.

The current two-source Mesa semantic IR cannot represent general CMP.  More
importantly, NIR `bcsel` is not automatically equivalent to the D3D floating
sign comparison.  A future three-source representation must preserve logical
condition/true/false order, assign A/C/B before lifetime finalization, and use
only an explicitly proved CMP-shaped NIR key.  Keep CMP rejected until an
asymmetric signed-zero pixel oracle proves the hardware boundary; compiler
byte closure alone is insufficient.

Evidence used production callbacks parser/validator/processor at backend
offsets `+0x137c/+0x1380/+0x1384`, configuration value two, and the serialized
`/tmp/chrome9-shader-probe.lock`.  No GPU was touched.
