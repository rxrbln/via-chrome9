# Chrome9 pixel SGN rejection boundary

The apparent one-record SGN evidence is vertex-shader-only.  A bounded
ps_2_0 pixel probe using opcode 0x22 and the documented three-source form is
rejected by Linux and faults in the Windows tokenizer before any ALU record
is produced.  The exact negative result is retained in
[`chrome9-pixel-sgn-rejection.tsv`](chrome9-pixel-sgn-rejection.tsv).

The differential JSON SHA256 is
`719102d79e723e2a96743a0e0686796e7e11360fcd6713ff20cdb0e20cbf91fa`.

## Evidence

The tested token stream is:

```text
ffff0200 05000022 000f0800 20e40000 20e40001 20e40002 0000ffff
```

The Windows tokenizer faults at module RVA `0x001dc0f7` while reading beyond
the normalized input and creates only the compiler object, with no tokenizer
object or native ALU/TAU stream.  The Linux production probe returns 4 with
`compiler rejected shader`.  This mirrors the unsupported-token failure
class already observed for pixel SLT; it is not an emitted instruction whose
bytes can be recovered from the failed run.

The symbolized Linux driver exports `PVS_SGN` at `0x0018ba60`, and the vertex
corpus contains the one-record PVS form.  There is no corresponding
`stmPPS_SGN*` symbol, and the recovered pixel programmable dispatcher has no
public opcode-0x22 handler.  Vertex primary selector 12 and its record format
must not be transplanted into the distinct pixel ALU ISA.

## Policy

Reject pixel NIR sign operations as a native SGN key.  Do not infer a PPS
record from the PVS word, the API opcode number, or the Windows crash.  A
future implementation may lower finite sign semantics through separately
proven pixel comparisons and selects, but that is a multi-record compiler
task with signed-zero and NaN behavior to define; it is not one-record SGN.

No live silicon oracle is justified without compiler bytes.  If an OpenGL
internal pixel path is later located, it must first be called with a
production allocator/finalizer, cross-checked against Linux, and only then
tested under command 19's mandatory fresh D16 and guarded submission
contract.  Until then SGN remains a frontend rejection, not a candidate rung.

