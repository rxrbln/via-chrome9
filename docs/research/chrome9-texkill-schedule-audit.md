# Chrome9 TEXKILL schedule and discard boundary

Fresh production differentials close the compiler schedule for isolated,
output-followed, and initialized TEXKILL programs.  TEXKILL is not a lone TAU
word: the compiler first moves its logical temporary into texture/address
destination 0, then issues TAU operation 4.  Linux configuration 2 adds the
ALU-to-TAU dependency bits and changes the control base.  Discard remains
rejected until a guarded live test proves sign comparison and color/depth
suppression.

## Exact records and finalization

The common pre-finalizer kill pair is:

```
ALU 01c00530 301c0053 00f00005 000d0420
TAU 214001b1
```

Linux configuration 2 rewrites it to:

```
ALU 01c00530 301c0053 00f20005 000d0420
TAU 314001b1
```

The ALU is MOV-family subop d into texture/address destination 0.  The TAU has
operation nibble 4.  Linux adds dword-2 bit `00020000` to the ALU source-A
dependency and bit `10000000` to the TAU.  These are required schedule fields,
not alternate TEXKILL semantics.

For initialized `mov r0,c0; texkill r0; mov oC0,v0`, Linux emits:

```
01c00530 301c0053 00f08005 000d0020
01c00530 301c0053 00f20005 000d0420
01c00530 301c0053 00f04005 000d0820
TAU 314001b1
A90..A95 90058000 91000001 92010003 93000001 94000000 95000002
```

Windows has the same three arithmetic records except kill-transfer dword 2 is
`00f00005`, uses TAU `214001b1`, and controls
`90040000 91000000 92010003 93000001 94000000 95000002`.
The v0-driven initialized form changes only the first/last MOV source classes.

The isolated kill has phase 2 and one ALU/one TAU.  Adding an output makes
phase 1 and adds A93 bit 0 for TAU-to-ALU.  Adding the leading initializer
changes A95 from bit 0 to bit 1, so the transition maps must be carried from
the finalized program rather than reconstructed merely from counts.  Every
case sets TEXKILL-present metadata.  Linux reports temporary mask 1; this
Windows wrapper reports zero, another reason not to transplant raw metadata
between compiler families.

## TEXKILL metadata is not a context sideband

Public instance `+0x7c` is a compiler/rebuild flag, not a separately emitted
hardware-context enable.  `stmPPS_TEXKILL_PS20_invi:0x167903` sets it to one;
initialization clears it at `stmInitializePixelShaderInstance_inv:0x1b81d7`.
`INVPS_HwRebuildInstBuffDesc:0x198f21..0x198f3d` reconstructs the same flag
from TAU operation 4 (its object-relative `+0x6c` is public-instance
`+0x7c`).

Neither production draw updater consumes that flag.  Linux
`stmUpdatePS_inv:0x17f710..0x180c00` reads public `+0x78` at `0x17f77d` to
update the fragment-depth context bit, but contains no public `+0x7c` read.
The Windows D3D programmable update at
`VTGUModeDX32A:0x1015b000..0x1015c600` likewise reads `+0x78` at
`0x1015b057` and never reads `+0x7c`.  It uploads the TEXKILL ALU/TAU records
and A90--A9A transition image through the ordinary program path.

The resource counter provides an independent consistency check.  Pre-final
`texkill r0; mov oC0,c0` reports `(1,1,0,0)`: the kill-transfer ALU defines
all four components of internal texture/address register zero, and the kill
TAU consumes that internally defined register.  Its external texture map is
empty, so the A00-producing third output remains zero.  TEXKILL must
therefore neither set a separate context bit nor increment A00 merely because
it uses the texture/address path.  Its required per-program state is the
ordered transfer ALU, operation-4 TAU, finalized transition controls, and the
normal replacement/clear of that complete program on the next draw.

The fresh isolated/output JSON SHA-256 is
`05bf89b34993acde688ca3da640c24f29477d131ede494c946e15f498a50f035`.
The two initialized cases have SHA-256
`d738992836734b637083a28ce201644fbf09ff32e3785fdb54b5c0d3c4538649`.
All records and A90..A95 tuples are retained in
[`chrome9-texkill-schedule-corpus.tsv`](chrome9-texkill-schedule-corpus.tsv).

## Guarded live discard oracle

The constant-driven initialized program is the preferred first live shape:
job-local c0 can contain finite positive or negative values while v0 supplies
the visible output.  First probe all-positive c0, then one negative lane at a
time, including W rather than assuming which lanes participate.  Include +0
and -0 boundary cases only after ordinary positive/negative behavior works.

Use command 19's mandatory fresh D16, depth clear 1, LESS/write, and guarded
color destination.  Draw a near red candidate at z=0.25 with one negative c0
lane, followed by a farther green candidate at z=0.5 with all-positive c0.
Correct discard of both color and depth ends green.  Failure to discard ends
red; color-only suppression with an incorrect depth write leaves the original
sentinel because the green draw then fails depth.  Repeat for X/Y/Z/W and
guard surrounding pixels.

Compiler evidence alone does not establish the comparison boundary, affected
lanes, or ordering relative to depth/stencil.  Keep all NIR discard/demote
forms and TEXKILL native keys rejected until the complete live matrix passes.
The fixed A33 alpha-test path remains preferable for alpha-test emulation
because it is already silicon-proven and avoids this new shader side effect.
