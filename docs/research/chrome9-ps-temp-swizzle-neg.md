# Chrome9 multi-record TEMP swizzle and NEG boundary

Status: exact Linux 5.75 production output and a Windows D3D differential
using the recovered production H5 register-table builder/remap callbacks.
OpenGL has byte-identical static allocator, remap, MOV/MUL emitter, and
finalizer copies.  Swizzle encoding is closed at compiler level; NEG has a
real cross-driver native mismatch.  Neither is admitted by the current
identity-only semantic UAPI.

## Production-initialized Windows wrapper

The older identity-callback wrapper cannot encode a TEMP source location or
swizzle reliably.  It deliberately returns the D3D logical id and leaves
native source-location filler fields incomplete.  This audit instead uses
the recovered production sequence:

1. tokenize normally;
2. call the D3D usage collector at RVA `001da070`;
3. install the H5 get/release/remap/config callbacks and exact limits
   ordinary=12, special=4, texture=10, special-base=12;
4. call the byte-matched register-table builder at RVA `001da3e0` with
   `(texture_limit=8,temp_limit=12)`;
5. dispatch, schedule, and run configuration two at RVA `00202720`.

The isolated wrapper has SHA-256
`c56637fad7bfaeb994595e3363d439b7f0e35bcf78138bdc0d2bfdc70fdfa4e1`.
Because tokenizer and emitter objects are separate in the harness, it copies
only collected usage bitsets `+0x40/+0x44` before the production builder.
All resulting TEMP swizzle records matching Linux is an independent check
that the remap path is initialized correctly.

## Swizzle result

For `mov r0,c0; mov oC0,r0.S`, Windows raw/final and Linux configuration two
agree for each tested S:

| S | terminal Linux/Windows configuration-two record |
|---|---|
| `xxxx` | `01c00530 001c0053 00f20000 000d0820` |
| `yyyy` | `01c00530 901c0053 00f20024 000d0820` |
| `zzzz` | `01c00530 201c0053 00f20049 000d0820` |
| `wwww` | `01c00530 b01c0053 00f2006d 000d0820` |
| `wzyx` | `01c00530 801c0053 00f20068 000d0820` |

Configuration two changes only the ordinary TEMP last-use flag in d2.bit17;
the swizzle selectors remain in their normal A packing.  MUL agrees in both
operand orders too:

```text
mul oC0,r0.wwww,c1  0140b6d0 b0081053 00f2006d 00000820  # TEMP in A
mul oC0,c1,r0.wwww  0140b6d0 302006db 00f08105 00000820  # TEMP in C
```

The second form puts the swizzle in C's d1 packing and its last-use in
d1.bit21.  Every row has active tuple `(0,1,0,0)` and A90 `90058000`; source
swizzle does not change resource counting or controls.

These are byte-exact compiler facts, not numerical silicon proof.  The first
semantic UAPI should continue accepting identity swizzle only.  A future
swizzle rung may use this table but must first pass guarded pixels with
distinct c0 lanes and both A/C operand orders.

## NEG mismatch

NEG is not portable between the proprietary frontends even with production
TEMP allocation:

| form | Windows configuration two | Linux configuration two |
|---|---|---|
| `mov oC0,-r0` | `0140b6d0 38140924 00f20005 00100820` | `01c00530 381c0053 00f20005 000d0820` |
| `mul oC0,-r0,c1` | `0140b6d0 38081053 00f20005 00100820` | `0140b6d0 38081053 00f20005 00000820` |
| `mul oC0,c1,-r0` | `8140b6d0 30200053 00f08105 00100820` | `8140b6d0 30200053 00f08105 00000820` |

Windows changes MOV to a MUL-family record and retains d3.bit20 in all three
forms.  Linux uses the MOV-family record for MOV and omits d3.bit20 for both
MUL orders.  The arithmetic source NEG fields otherwise locate consistently
(A modifier in d1; C modifier in d0.bit31), but that is insufficient to pick
one executable convention.

Therefore TEMP NEG remains reject-only.  Do not infer that Windows d3.bit20
is required, that Linux intentionally suppresses it as redundant, or that
both streams are silicon-equivalent.  A future NEG admission needs separate
guarded execution of both vendor forms and a deliberate canonical choice.

## UAPI/NIR consequence

The first multi-ALU validator remains unchanged:

- full XYZW destinations;
- identity TEMP sources;
- no source modifier, including NEG/ABS;
- no WAW/read-before-definition/dead TEMP;
- kernel-rebuilt dense allocation and last-use bits.

NIR vector constructors and replicated channels may eventually use the
proved swizzle packer, but not until the multi-record silicon gate passes.
`fneg` must not be admitted through the TEMP path because its two proprietary
encodings disagree.  Constant sign folding may remain a separate one-record
optimization where its finite-domain contract already permits it.

All exact tokens and raw/final records are retained in
[`chrome9-ps-temp-swizzle-neg-corpus.tsv`](chrome9-ps-temp-swizzle-neg-corpus.tsv).
The production-initialized differential JSON SHA-256 is
`cfedb80856f3d179a80510ef248232b494c035f9f60d2c5077762012a0d64729`.

