# Chrome9 signed wrap and nearest-mipmap contract

This note closes the immediate q3dm1 sampler-validation gap without treating
every finite IEEE coordinate as safe.  The supplied Windows Chrome9 driver
publishes an exact normalized-coordinate repeat limit of 128.  The q3dm1 BSP
needs only `[-19,21.5]` for its lightmapped base coordinates, while its
lightmap coordinates remain inside `[0,1]`.  The first two-texture
implementation therefore admits signed REPEAT coordinates through 32, retains
the old `[0,1]` rule for CLAMP_TO_EDGE and MIRRORED_REPEAT, and sets a bound
four times below the vendor limit while covering the measured map.

That exact REPEAT boundary is now live-proven on the two-stage path at signed
magnitudes through 31.109375.  The wider Windows limit, signed
MIRRORED_REPEAT, and mip selection remain static evidence plus bounded future
oracles; they must not inherit the live result by association.

## Exact Windows capability bound

`VTGUModeDX32A.dll`, SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
contains four literal `D3DCAPS9` records:

| file offset | analysis VA | `TextureCaps` at `+60` | width/height at `+88/+92` | `MaxTextureRepeat` at `+100` | anisotropy at `+108` |
|---:|---:|---:|---:|---:|---:|
| `30edf0` | `1030fbf0` | `00076d47` | `800/800` | `00000080` | `10` |
| `30eec8` | `1030fcc8` | `00076d47` | `800/800` | `00000080` | `10` |
| `3142b0` | `103150b0` | `0003484f` | `800/800` | `00000080` | `02` |
| `3143e0` | `103151e0` | `0003484f` | `800/800` | `00000080` | `02` |

The records are structurally unambiguous: device type is one, every preceding
capability word lines up with the public 304-byte structure, maximum texture
width and height are both 2048, and the following fields have plausible
volume, repeat, anisotropy, guard-band, shader-version, and constant-limit
values.  They are active templates rather than unrelated data.  The adapter
constructor at `0x100c6fad..0x100c6fcd` copies 53 dwords from `0x1030fbf0`
and then 76 dwords from `0x1030fcc8` into its capability images.

Both `TextureCaps` values contain
`D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE=0x40`.  Therefore the published
`MaxTextureRepeat=128` applies in the normalized repeat domain independent of
image width, rather than being multiplied or divided by the texture size.
The exact shipping API envelope is consequently `-128..+128`.  It is not
evidence that infinity, NaN, or every larger finite float is safe, and it does
not remove floating-point precision loss near the bound.

The VX900 manual independently fixes the address words but gives no numeric
coordinate limit:

```text
A32 = 32000040 | (wrap_t << 3) | wrap_s
clamp-to-edge   wrap=1, A32=32000049
repeat          wrap=2, A32=32000052
mirrored repeat wrap=3, A32=3200005b
```

The D3D and OpenGL address builders at `0x101d9250` and `0x10241a70`
select those codes for WRAP, MIRROR, and CLAMP without looking at image POT or
NPOT state.  Proprietary Linux `STM_TexAddressMode_INV:0x1b17b0` makes the
same independent S/T/R table lookup.  None of these state builders clamps
vertex coordinates.

The normal Windows vertex-buffer path also performs no hidden coordinate
inspection.  `SetStreamSource` at `0x100b3280..0x100b32bd` forwards the
resource, offset, and stride to `0x100ceda0`; that routine stores only stream
metadata in the context record at `state+0x467c+36*stream` and never reads the
vertex payload.  `DrawPrimitive` at `0x100b3360..0x100b33f3` constructs the
primitive/start/count tuple and calls `0x1010f290`, again without examining
the float data.  Together with the recovered FLOAT2 declaration code `0x42`,
this proves that the shipping user driver passes signed/out-of-range binary32
S/T to the GPU unchanged.  It does not prove arbitrary-large silicon safety.

Do not enable the apparently tempting general-texture A0D modulus control.
The live configuration word `0d810000` leaves all stage-enable bits
`A0D[7:0]` clear; `81` is merely the otherwise inactive exponent field.
Likewise, FE A0B is a separate, still-uncharacterized interpolation wrap-
correction control, not the sampler address mode.  Preserve the established
context value and change only each stage's A32 for this gate.

## What the current paths actually do

The old Unichrome Mesa path provides architectural corroboration that GL
coordinates were not CPU-clamped.  `via_tris.c:787..799` selects `EMIT_2F`
for ordinary texture units; `tnl/t_vertex_generic.c:247..254` copies both
floats verbatim, and the SSE path does the same at
`tnl/t_vertex_sse.c:419..423`.  `via_state.c:776..810` independently maps GL
repeat, mirrored repeat, and clamp to hardware state.  This is not Chrome9
bit-position evidence, but it rules out a legacy Mesa-side `[0,1]` contract.

Before the signed-repeat successor, the Chrome9 implementation was
deliberately asymmetric:

- one-texture ioctls require finite S/T and copy their IEEE words unchanged;
- two-texture ioctls presently require every S/T in `[0,1]`;
- Mesa repeats the same two-texture rejection before submission; and
- the two-texture packet copies S0/T0/S1/T1 unchanged into vertex dwords
  6--9 and emits independent stage-0/stage-1 A32 words.

At the time of this audit the relevant kernel locations are
`chrome9_engine.c:3882..3891`, `3994..4009`, and `1245..1259`; Mesa's are
`chrome9_draw.c:983..1000` and `1033..1049`.  The two-stage hardware layout
already carries signed floats correctly.  The successor changes only that
fail-closed proof boundary for REPEAT.

## Exact q3dm1 demand

The measured asset is `maps/q3dm1.bsp`, SHA-256
`3d2dad8e6ba05914fc90619a6517ecdee7e5eb50f86013a14a84bb3c61cd702e`,
from `pak0.pk3`, SHA-256
`e77abad2466f45a0a7ea018445528f9b95a0fe7789fa1abc1a7718bbf0754b08`.
It is Quake 3 `IBSP` version 46.  Vertex lump 10 has 13,978 44-byte records;
base S/T are the two floats at record offset 12 and lightmap S/T are at offset
20.  These are already repeat-domain coordinates; no texture-dimension divide
was applied.

For the 2,022 faces with a lightmap:

| coordinate | minimum | maximum |
|---|---:|---:|
| base S | `-3.7753200531` | `17.1171875` |
| base T | `-19.0` | `21.5` |
| lightmap S | `0.00390625` | `0.99609375` |
| lightmap T | `0.00390625` | `0.98828125` |

Of 10,982 referenced lightmapped vertices, 4,762 have a base coordinate
outside `[0,1]`; 1,331 of 2,022 faces do.  Type-1 polygon faces contain 5,308
triangles through the mesh-vertex lump, and 3,483 of them, **65.62%**, contain
at least one base vertex rejected by the current gate.  Runtime-tessellated
patch faces were deliberately omitted from that triangle percentage.

The parser's type counts, 1,942 polygon faces, 113 meshes, and 42 trisurfs,
match the retained ioquake log.  That log also records
`GL_LINEAR_MIPMAP_NEAREST`, making signed two-texture coordinates and nearest
mip selection consecutive real blockers rather than speculative features.
The four lightmapped extrema have no effective `tcMod`; the all-face minimum
T of `-28.5` belongs to unlightmapped turbulent lava and also remains within
32 before its small turbulence amplitude.

The retained 387-frame run says `HACK: using vertex lightmap approximation`,
so it is not evidence that the true two-texture coordinates above executed.
The BSP measurement and the later isolated two-stage sweep are the relevant
proofs.  Generated dlight/fog passes and arbitrary material `tcMod` sequences
remain separate workload questions.

## First validator boundary

For the first capability-gated two-texture successor, the contained rule is:

```text
all coordinates: finite IEEE binary32
REPEAT:             abs(coord) <= 32.0
CLAMP_TO_EDGE:      0.0 <= coord <= 1.0, with -0 accepted
MIRRORED_REPEAT:    retain 0.0 <= coord <= 1.0 until its signed oracle
RHW:                finite and strictly positive, unchanged
```

Apply the wrap-specific rule independently to S and T and independently to
both units, subject to the command's narrower capability tuple where one is
defined.  Do not make acceptance depend on POT versus NPOT dimensions;
the image builders and live one-level NPOT tests already use the same address
state.  The limit is intentionally four times smaller than the Windows cap,
yet covers every measured q3dm1 coordinate.  This does not advertise a new
Gallium capability and needs no UAPI layout change.

If a later workload exceeds the chosen bound, a common per-primitive rebase
is semantics-preserving and keeps derivatives unchanged:

- REPEAT permits subtracting the same integer from all three S values, and
  independently from all three T values;
- MIRRORED_REPEAT permits subtracting the same **even** integer; an odd shift
  reverses the mirror phase and is not equivalent; and
- never reduce each vertex independently, because that changes interpolation
  and derivative LOD across a triangle.

Perform any such rebase before float precision has already discarded the
fractional texel.  For a 2048-wide image binary32 spacing first reaches one
texel at magnitude 4096; the Windows 128 bound is comfortably below that
binary32 limit, but the hardware's internal interpolation precision is not
documented.  The staged silicon sweep, not float format alone, remains the
proof.  The first REPEAT sweep has now established only the bounded 32 rule;
128 and signed mirror remain future tests.

## Signed two-stage wrap oracle

Use guarded 32x32 BGRX sources and the already-live componentwise two-texture
program.  Fill the tested source's columns with 32 distinct exact colors and
fill the other source opaque white.  Hold T at `0.5`, make S constant across
all vertices, use nearest/no-mip A31=`31100000`, and test the units separately
by swapping which source is white.

Let `p=(3.5/32)=0.109375`, binary32 `3de00000`.  The expected nearest texels
are:

| A32/mode | S | binary32 | expected X |
|---|---:|---:|---:|
| `32000052` repeat | `-30.890625 = -31+p` | `c1f72000` | 3 |
| `32000052` repeat | `31.109375 = 31+p` | `41f8e000` | 3 |
| `3200005b` mirror | `-30.109375 = -30-p` | `c1f0e000` | 3 |
| `3200005b` mirror | `30.109375 = 30+p` | `41f0e000` | 3 |
| `3200005b` mirror | `-31.109375 = -31-p` | `c1f8e000` | 28 |
| `3200005b` mirror | `31.109375 = 31+p` | `41f8e000` | 28 |

The general expected functions, avoiding exact integer boundaries, are:

```text
repeat(s) = s - floor(s)
mirror(s) = 1 - abs(1 - (s mod 2)), with mod in [0,2)
texel(s)  = floor(reduced(s) * 32)
```

The first live sweep covered REPEAT at `+/-1.109375`, `+/-16.109375`, and
`+/-31.109375`, with exact expected samples, all guards intact, and clean
retirement/status.  It establishes the current `abs(coord)<=32` REPEAT gate
on the real two-stage path.  Keep the table's signed MIRRORED_REPEAT cases as
the next extension oracle; they are not live-proven.

For that extension, start with `p`, `1+p`, `-p`, and `-1-p`; then test
magnitudes 16 and 31.
After each draw verify both complete source images, all six source guard
pages, destination guards and outside sentinel, retirement cookie, CR PAUSE,
and clear engine work bits.  Run this closure sequence without a context
reset:

```text
[0,1] clamp -> signed repeat unit 0 -> signed repeat unit 1
            -> signed mirror unit 0 -> signed mirror unit 1 -> [0,1] clamp
```

Any timeout takes the established quarantine/reboot path.  A wrong color with
clean status is a coordinate/address-mode failure, not permission to widen
the bound.  The REPEAT half of this sequence has succeeded.  The mirror
transitions still gate any signed-mirror capability; 128 remains the shipping
Windows cap and a later power-of-two sweep target.

## `GL_LINEAR_MIPMAP_NEAREST` packet

The exact sampler lowering is independent of signed wrapping:

```text
A30 = 30000040                         # bias 0, physical levels L0..L1
A31 = 31102491                         # linear min/mag in S/T, nearest D
A32 = 32000052 or 3200005b             # repeat or mirrored repeat
```

If GL magnification is nearest rather than linear, change only magnification
fields and use A31=`31100411`.  Do not mirror the minification choice into the
magnification fields.  Windows D3D `0x101d9380`, Windows OpenGL
`0x10241ba0`, Linux `STM_MipMapLOD_INV:0x1b1a50`, and the manual agree on
the independent fields.  The six exact min-filter words are already tabulated
in `ioquake-texture-format-sampler-plan.md`.

For the current guarded 32x32 L0 plus 16x16 L1 object, L0 begins at object
offset `1000`, the retained L0 guard occupies `2000..2fff`, and L1 begins at
`3000`.  The SF-relative L1 delta is therefore `2000`, not the vendor-packed
`1000`.  Emit the complete table before the sampler:

```text
FE03043C 00000008 10030000 CC000000
40000020 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

Keep image A18=`18001101`; write all eight table entries plus A30/A31/A32 on
every mip draw.  The table selector for stage 1, if a later command makes
that stage mipmapped, is `10030008`, **not** `11030000`.  The proprietary
full-state loop at `stmFullUpdateHardware_invi:0x177b59..0x177b72` forms
`10030000 | ((stage << 3) & ffff)`.

For the first q3-shaped combined packet only unit 0 needs a chain.  Insert the
12-dword stage-0 table after image 0 and before sampler 0; retain single-level
unit 1 with its ordinary image/sampler selectors `01020000/21020000`.  Against
the current three-vertex two-texture builder this changes the base body from
262 to 274 dwords and moves every existing word after image 0 by 12.  Existing
blend, depth, and longer-program deltas remain orthogonal.  Do not allocate a
new ioctl number merely from this note; first land the reserved one-texture
mip command and then version the combined semantic request through the UAPI
ledger.

## Nearest-mipmap oracle and wrap interaction

Use solid opaque red L0 and green L1, the guarded layout above, REPLACE for
the first hardware gate, and safely interior pixels:

1. Force L0 and L1 independently with the existing address-table tests.
2. Set A30/A31=`30000040/31102491`; map normalized 32 source texels onto 16
   destination pixels.  Interior pixels must be green.  A red result means
   nearest-D derivative selection did not engage.
3. Render 1:1 with the same words; interior pixels must be red.
4. Add a common integer repeat offset `+16` to every S/T vertex and repeat
   steps 2--3.  Images must be byte-identical to the zero-offset runs.  Repeat
   at `-16`, then at `+31/-31` only after the signed constant-coordinate gate.
5. For mirrored repeat, use even common offsets `+16/-16/+30/-30` so mirror
   phase is unchanged.  An odd common offset is a deliberate phase-reversal
   test, not a closure comparison.
6. Transition mip nearest -> single-level/no-mip -> mip nearest without a
   context reset.  Require green/red/green in the forced-address companion
   test and rewrite the table and all sampler words each time.

The common offsets leave mathematical derivatives unchanged, so any LOD or
color change isolates coordinate precision, pre-wrap derivative ordering, or
stale sampler state.  Never begin with large coordinates and an incomplete
mip chain: a wrong LOD could fetch an unmapped address and turn a pixel oracle
into an engine recovery event.

## Confidence and remaining hazards

| claim | confidence |
|---|---|
| A32 clamp/repeat/mirror codes and independent stages | high, manual plus three executable builders and live one-stage tests |
| normalized repeat cap 128 | high, four active Windows `D3DCAPS9` records |
| q3dm1 fits `abs(coord)<=32` | measured exactly from the BSP; tcMod caveat checked for extrema |
| signed two-texture REPEAT through 32 | live at `+/-1.109375`, `+/-16.109375`, and `+/-31.109375`, with guards/status clean |
| signed two-texture MIRRORED_REPEAT through 32 | static/API evidence only; oracle pending |
| `LINEAR_MIPMAP_NEAREST` A31 words | high static cross-driver/manual evidence |
| H3 table address and selector | high static evidence; first actual mip fetch still pending |

The important hazards are finite-but-unbounded input, independent per-vertex
modulo, changing FE A0D/A0B while testing A32, confusing stage-1 table selector
`10030008` with `11030000`, omitting unused H3 entries, and allowing A30 to
name any level whose pages and guard ownership are not retained through
retirement.  None of the signed-coordinate work relaxes the existing BO,
pitch, format, RHW, alias, reservation, or quarantine invariants.
