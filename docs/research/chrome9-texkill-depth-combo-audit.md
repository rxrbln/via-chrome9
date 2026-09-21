# Chrome9 combined TEXKILL and depth-output boundary

The initialized `MOV r0,c0; TEXKILL r0` sequence can coexist in the compiler
object with full oC0 and oDepth outputs.  Both compiler families set TEXKILL
and depth-output metadata simultaneously and produce the same record set, but
their output scheduler policy differs: Windows always places depth before
color, while Linux preserves the requested color/depth token order.  The
program remains rejected until silicon proves that killed fragments suppress
both color and shader depth.

## Exact schedule

For token order color then depth, Windows emits:

```
01c00530 301c0053 00f08005 000d0020  # r0 = c0
01c00530 301c0053 00f00005 000d0420  # kill address transfer
01c00530 b01c0053 00f0816d 000d0920  # oDepth = c1.w
01c00530 301c0053 00f04005 000d0820  # oC0 = v0
TAU 214001b1
```

Linux configuration 2 preserves color then depth and adds the kill dependency:

```
01c00530 301c0053 00f08005 000d0020
01c00530 301c0053 00f20005 000d0420
01c00530 301c0053 00f04005 000d0820
01c00530 b01c0053 00f0816d 000d0920
TAU 314001b1
```

Reversing the two output tokens leaves the Windows stream unchanged; Linux
reverses only its final color/depth records.  Thus Windows canonicalizes depth
before color while Linux preserves token order.  Both variants have phase 1,
ALU count 4, TAU count 1, both side-effect flags set, and one color output.

Windows uses controls:

```
90040000 91000000 92010004 93000001 94000000 95000002
```

Linux configuration 2 uses:

```
90058000 91000001 92010004 93000001 94000000 95000002
```

A93 bit 0 returns from TEXKILL's TAU phase to the output ALUs; A95 bit 1 moves
from the two leading ALUs to TEXKILL.  The exact two-case JSON SHA-256 is
`9a2bea70b4bdeb0e51f062b81cd45d33ebabc5ba07a82983400f02c5cd5e5b8b`.
The compact corpus is
[`chrome9-texkill-depth-combo-corpus.tsv`](chrome9-texkill-depth-combo-corpus.tsv).

## Sideband and live ordering gate

The depth flag still requires A13.bit23 `HZSrcPS`; with LESS the exact state is
`13810000`.  TEXKILL metadata and TAU/control words must be installed in the
same transactional program update.  Returning to ordinary color-only drawing
must clear both the TEXKILL program/metadata and A13.bit23.  Fixed fog from Z
is excluded for the first gate.

Use command 19's mandatory fresh D16, depth clear 1, LESS/write, and guarded
color/depth destinations.  First draw a near red candidate with negative
finite c0.x, geometric z=0.75, and shader depth c1.w=0.25.  Then draw green
with positive c0, geometric z=0.5, and shader depth 0.5.  Correct kill-before-
depth behavior leaves green: the killed red fragment writes neither color nor
0.25 depth.  A failed kill leaves red; color-only kill with an erroneous 0.25
depth write leaves the original sentinel because green fails depth.  Read back
both color and D16, repeat negative X/Y/Z/W separately, and guard surrounding
pixels.

Run the Linux color-then-depth and depth-then-color streams as separate
experiments; equality would establish schedule-order tolerance, not permission
to mix Windows predependency words.  Until kill-vs-depth timing, A13/TEXKILL
enable and clear transitions, reset restoration, and both orderings are
proven, reject every combined discard plus fragment-depth NIR shape.
