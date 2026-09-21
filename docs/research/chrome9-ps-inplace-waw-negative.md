# Chrome9 pixel TEMP in-place WAW negative boundary

Status: exact Linux 5.75 production compiler and Windows D3D private
emitter/configuration-two differentials, with static OpenGL agreement for the
same emitters and finalizer.  These rows document permissive vendor behavior;
they remain rejected by the first structured multi-ALU UAPI.

## Exact behavior

The proprietary compiler accepts all of these three-record programs:

```text
mov r0,c0; mul r0,r0,c1; mov oC0,r0     # destination aliases A
mov r0,c0; mul r0,c1,r0; mov oC0,r0     # destination aliases C
mov r0,c0; mul r0,r0,r0; mov oC0,r0     # destination aliases A and C
mov r0,c0; mov r0,c1;    mov oC0,r0     # overwrite without reading old value
```

Every Linux row uses active tuple `(0,1,0,0)`, A90 `90058000`, and A92
`92000003`.  The in-place middle instruction retains destination p0.  It has
no A or C final-use bit even when it reads the old p0 value.  Only the final
`mov oC0,r0` gets A d2.bit17.

The pure-overwrite discriminator is more important than acceptance alone.
The first `mov r0,c0` is semantically dead, but configuration two does **not**
set its destination d3.bit29.  Both TEMP destinations remain `000d0020` and
the terminal read alone is marked.  The vendor pass tracks a physical
register id across the whole stream, not distinct value versions.  It cannot
serve as SSA validation or DCE.

The duplicate A/C middle MUL likewise has no marker.  This is not a
contradiction of A-over-C last-use precedence: p0 is read again by the final
MOV, so neither occurrence in the middle record is its register-id last use.

A logical-r1 overwrite compacts to the identical p0 Linux stream.  The
Windows identity-callback harness retains id 1, then marks only its terminal
read; that is field/finalizer evidence rather than production allocation.

Exact records for all five rows are in
[`chrome9-ps-inplace-waw-negative-corpus.tsv`](chrome9-ps-inplace-waw-negative-corpus.tsv).

## Why the first semantic UAPI must still reject it

No admitted one-record NIR shape needs a TEMP WAW.  A short MOV/MUL NIR graph
is already SSA: every producer defines a distinct semantic value.  The
bounded UAPI can represent those values with dense `t0,t1`, and the kernel
can produce the already proved non-WAW p0/p1 schedules.

Coalescing a dying source with the destination could reduce pressure to one
physical slot, but that is an optional later compiler optimization.  The
offline compiler records do not prove on silicon that every destination/A/C
alias combination is hazard-free, nor do they recover an instruction-level
read-before-write timing contract.  The pure-overwrite row also demonstrates
that accepting WAW would require versioned liveness and dead-definition
checks which the vendor finalizer does not provide.

Therefore the first validator should:

- reject a TEMP destination whose semantic name is already defined;
- reject all read-before-definition and dead TEMP destinations after DCE;
- allocate distinct surviving SSA values canonically, even where a future
  coalescer could reuse a dying source's slot;
- rebuild register-id last-use bits only after validation/allocation; and
- include an in-place WAW ioctl as a negative test which fails before BO
  reservation, command publication, or fence/counter advance.

Future WAW/coalescing support requires an explicit alias-safe opcode/slot
matrix and guarded silicon pixels for A, C, and duplicate A/C.  It must not be
enabled merely because the legacy compiler emitted the records.

## Cross-driver provenance

The D3D identity harness calls the real finalizer at RVA `00202720`; its
middle/terminal marker behavior matches Linux for all five rows.  The full
D3D/OpenGL finalizers are normalized-identical with SHA-256
`0640c204fb3fe1e7b425215e95b246846e7b83571a5024727fdf9e90f35e738a`.
The MOV and MUL emitter pairs are independently normalized-identical with
SHA-256 values
`509e9fd9a986c51b4fb58f41642c2108c5dbe08dfa03e4744c2f4395df7890e6`
and
`fed69f474b142a18875c438b6406281df4adeb8def152f8c14a266cdf4ce333c`.
Thus OpenGL has the same static WAW encoding/finalizer policy, although its
DLL still cannot complete process initialization under headless Wine.

The combined differential JSON SHA-256 is
`66d6959070b6cfb08a98f0d8208377bf5ff6e0f82f418c9533a856cb4f8a396d`;
the corrected Windows harness SHA-256 is
`e9386bd70a8b1a0857135c9b3117e767e297c7b5c72184df557f3c06cbceed1b`.

