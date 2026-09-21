# Chrome9 pixel MRT compiler boundary

The production Windows and Linux compilers byte-match for individual oC0
through oC3 MOVs and for a combined oC0+oC1 program.  Native destination id is
dword-3 bits 7:6, producing `000d0820`, `000d0860`, `000d08a0`, and
`000d08e0`.  Windows metadata records highest color destination id plus one,
not an active-target mask.  This closes compiler encoding only; MRT remains
rejected because secondary render-target attachment and memory state have not
been proven.

## Exact destination sequence

With identity c0, the four terminal records are:

```
oC0 01c00530 301c0053 00f08005 000d0820
oC1 01c00530 301c0053 00f08005 000d0860
oC2 01c00530 301c0053 00f08005 000d08a0
oC3 01c00530 301c0053 00f08005 000d08e0
```

Every row is phase 1, depth-output-present false, and one ALU.  Windows uses
A90=`90040000`; Linux configuration 2 uses A90=`90058000`; both use
A92=`92000001`.  No source/destination dependency bit differs.

A single oC0/oC1/oC2/oC3 store gives Windows color-output count 1/2/3/4.
Therefore the field is a high-water count and cannot say that only oC2 is
active.  A validator must derive the actual written-target mask from stores,
then require every lower slot demanded by the count to have intentionally
defined attachment state rather than treating the count as a mask.

The combined `oC0=c0; oC1=c1` records retain token order and byte-match:

```
01c00530 301c0053 00f08005 000d0820
01c00530 301c0053 00f08105 000d0860
```

Both use A92=`92000002`; Windows reports color-output count 2.  The exact
five-case differential JSON SHA-256 is
`30930cd295620d1ec311ed45159a6209d528fef4ba1cd02cdfc1f41d5d167154`.
All rows are retained in
[`chrome9-mrt-output-corpus.tsv`](chrome9-mrt-output-corpus.tsv).

## Runtime boundary

Keep the structured UAPI and Mesa exposure at oC0 only.  Compiler destination
ids do not establish the Axx state, base address, pitch, format, tiling, write
mask, blend equation, cache flush/invalidate, aliasing rules, or fault bounds
for RT1..RT3.  They also do not prove simultaneous stores on VX900 silicon.
The invalid oC4 token aliases native depth destination `000d0920` while not
setting depth metadata, so kernel/Mesa validators must bound color ids to 0..3
and must never infer depth from the native destination alone.

A future first MRT oracle should attach two separate guarded linear BOs with
distinct canaries, disable blending, emit full XYZW oC0/oC1 colors, and verify
both images plus untouched guards.  It must use command 19's mandatory fresh
D16, depth clear 1, LESS/write, passing z=0.5, and reset-on-timeout.  Follow
with swapped attachments, independent write masks, blend, cache transitions,
and an oC1-only hole test.  No MRT capability or native key is admitted until
the render-target state and all those memory-safety checks are independently
closed.
