# Chrome9 MOV/MUL short-chain three-driver audit

Status: exact Linux 5.75 production-compiler output, exact Windows D3D
private-emitter/finalizer differentials, and static byte-normalized agreement
between the D3D and OpenGL Windows compiler copies.  This constrains a future
structured multi-record compiler; it is not new runtime or silicon coverage.

## Result

For full-width MOV/MUL chains of at most three ALUs, the native destination
id is always d3 bits 9:6.  The surrounding d3 template remains
operation/destination-class specific:

| operation and destination | physical r0 | physical r1 | oC0 |
|---|---:|---:|---:|
| MOV | `000d0020` | `000d0060` | `000d0820` |
| MUL | `00000020` | `00000060` | `00000820` |

Therefore a compiler may insert `physical_id << 6`, but must not reuse the
whole MOV d3 template for MUL or vice versa.  The logical D3D temp number is
not itself the physical id: a lone logical r1 producer is encoded as physical
r0 by the Linux production allocator, while the Windows identity-callback
emitter harness deliberately preserves id 1.  Multi-temp rows in which r0
and r1 overlap in lifetime map to physical 0 and 1 in both.

MUL places its two operands in native A and C.  Configuration two applies the
same physical-temp last-use policy in all three proprietary compiler copies:

- final use in A sets d2.bit17 (`0x00020000`);
- final use in C sets d1.bit21 (`0x00200000`);
- two distinct temps dying in A and C set both bits;
- the same physical temp in A and C is marked once, in A only.

The last rule is the exact A-over-C same-record precedence.  It applies to
every occurrence after allocation, not to a NIR source ordinal or producer
edge.  A two-step r0-to-r1 chain releases r0 in the middle MUL and r1 in the
terminal MOV.  A middle `MUL r1,r0,r0` still marks only its A occurrence, and
a terminal `MUL oC0,r1,r1` does the same independently.

## Exact Windows static provenance

The D3D programmable dispatcher maps MOV and MUL to these private emitters;
the OpenGL dispatcher maps the same opcodes to its compiler copy:

| operation | D3D VA / RVA / file offset / size | OpenGL VA / RVA / file offset / size |
|---|---|---|
| MOV | `0x1021ab10` / `0x0021ab10` / `0x00219f10` / `0x140` | `0x1027a040` / `0x0027a040` / `0x00279440` / `0x140` |
| MUL | `0x1021ac50` / `0x0021ac50` / `0x0021a050` / `0x300` | `0x1027a180` / `0x0027a180` / `0x00279580` / `0x300` |

After zeroing only direct CALL/JMP relative-immediate fields, the paired
function bodies are otherwise byte-identical.  The normalized hashes are
`509e9fd9a986c51b4fb58f41642c2108c5dbe08dfa03e4744c2f4395df7890e6`
for MOV and
`fed69f474b142a18875c438b6406281df4adeb8def152f8c14a266cdf4ce333c`
for MUL.  Raw function hashes differ because their helper call targets are at
different image locations.

The complete configuration-two finalizers are D3D
`0x10202720..0x10203204` (RVA `0x00202720`, file offset `0x00201b20`) and
OpenGL `0x10255740..0x10256224` (RVA `0x00255740`, file offset
`0x00254b40`).  After zeroing direct branch/call immediates and absolute
table-address displacements, all remaining `0xae5` bytes match,
with normalized SHA-256
`0640c204fb3fe1e7b425215e95b246846e7b83571a5024727fdf9e90f35e738a`.

The exact marker action blocks are:

| source | D3D | OpenGL | mutation |
|---|---:|---:|---|
| A | `0x10203153` | `0x10256173` | OR d2 with `0x00020000` |
| C | `0x10203164` | `0x10256184` | OR d1 with `0x00200000` |
| B | `0x10203175` | `0x10256195` | OR d0 with `0x02000000` |

This makes the OpenGL agreement stronger than an address analogy: its MOV,
MUL, and whole last-use finalizer logic are independently present and equal
after relocation normalization.

Binary SHA-256 values are
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
(D3D),
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`
(OpenGL), and
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`
(Linux).

## Dynamic differential boundary

The eight exact rows are retained in
[`chrome9-mov-mul-short-chain-corpus.tsv`](chrome9-mov-mul-short-chain-corpus.tsv).
The D3D harness calls the real finalizer at RVA `0x202720` with configuration
two.  Its final-use deltas, destination ids, ALU counts, and A90--A93 controls
match the Linux production rules.

The Windows harness installs identity register callbacks so individual
emitter fields can be attributed without a device object.  It is not a
production allocator.  Its unassigned source-template words remain zero in
places where Linux production processing inserts fixed fields such as
`0x30000053` and low `0x5`.  Thus the corpus compares only the proved
destination, last-use, count, and control fields; the complete Linux records
remain authoritative executable objects.  A Windows row must not be uploaded
as a shader merely because its relevant marker bits match.

The OpenGL DLL loads and then returns failure from its process initialization
under the headless Wine environment, so no dynamic OGL row is claimed.  The
static byte-normalized compiler/finalizer equality above closes the requested
field logic without disguising that environment limit.

The retained D3D/Linux result JSON SHA-256 is
`9d1e66c9033bb80b8172d974d6639b537718d6786841eb56222425cb235f7539`;
the corrected private D3D wrapper SHA-256 is
`e9386bd70a8b1a0857135c9b3117e767e297c7b5c72184df557f3c06cbceed1b`.
The durable TSV contains all result words and is the repository evidence;
neither temporary artifact is a runtime dependency.

## Structured compiler consequence

All eight programs report active resource tuple `(0,1,0,0)` or `(0,2,0,0)`.
They therefore remain in the first resource bucket and close to the same
controls:

```text
90058000 91000000 92000002/92000003 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000
```

There is no A90 resource-threshold conflict for this bounded two-/three-ALU
MOV/MUL subset.  The implementation requirements are instead ordering
requirements:

1. keep values in SSA through scheduling;
2. allocate physical temporaries and reject read-before-definition/WAW;
3. encode each operation's destination template with the physical id;
4. find each physical temp's last scheduled occurrence;
5. on duplicate A/C use, mark A only;
6. emit the complete Linux-closed record templates and controls;
7. keep the runtime disabled until a guarded multi-record silicon oracle
   succeeds.

This agrees with the current proposed structured semantic compiler.  It also
rules out exposing logical register numbers or raw d3/source-marker bits in a
UAPI: those are post-allocation compiler products.
