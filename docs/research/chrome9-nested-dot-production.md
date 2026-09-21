# Chrome9 nested DP3/DP4 production differential

Status: exact hardware-free Linux production compiler/configuration-two
differential.  It closes source packing and temporary final-use behavior for
a MOV producer followed by terminal DP3 or DP4, but not scalar-result
broadcast or numerical behavior on silicon.

Each program is `mov r0,v0` followed by full-XYZW `dp3` or `dp4` to `oC0`
with operands `r0,c0`, `c0,r0`, or `r0,r0`.  All six raw and finalized streams
are in
[chrome9-nested-dot-production-corpus.tsv](chrome9-nested-dot-production-corpus.tsv).

The control closure matches the other two-record ALU-only programs exactly:

```text
raw     90040000 91000000 92000002 93000000 ... 9a000000
config2 90058000 91000000 92000002 93000000 ... 9a000000
```

Both dot operations map logical source 0 to native A and logical source 1 to
native C.  Forward order therefore marks a final r0 use at d2.bit17; reverse
order marks d1.bit21.  Duplicate r0 occupies A and C but marks only A, matching
the recovered A-over-C-over-B release priority.

DP4 uses the ordinary identity A/C packers and changes only subop to d3
`00030820`.  DP3 changes subop to `00020820` and forces both operands' W
selectors to literal ZERO.  This produces the characteristic d1 bases
`50080055`, `50000055`, or `50000055`, rather than DP4's
`30080053`/`30000053`.  A future encoder must apply that DP3 lane rule itself;
the semantic sources remain identity XYZW at the NIR boundary.

The existing Mesa semantic IR can represent the two logical inputs and its
current MOV/MUL A/C final-use pass has the correct physical ordering for these
forms.  It still lacks DP opcodes/templates, and direct full-mask native output
does not prove that a scalar dot is broadcast to every rendered channel.
Admission therefore remains fail closed until a guarded pixel oracle proves
replication and precision.  For `v0`-derived r0 with c0, the previously
derived rounding-independent conservative constant bound remains
`(bits(c0[i]) & 0x7fffffff) <= 0x7dffffff`; duplicate normalized r0 needs no
job constant but still needs the same broadcast/numerical gate.

Evidence used production callbacks parser/validator/processor at backend
offsets `+0x137c/+0x1380/+0x1384`, configuration value two, and the serialized
`/tmp/chrome9-shader-probe.lock`.  No GPU was touched.
