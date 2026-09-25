# Chrome9 HD blend, mask, and alpha-test packet recipes

This note is the packet-level companion to the first textured draw.  It keeps
three kinds of evidence separate:

- the VX900 Part II manual, PDF pages 90 and 100--104, defines A00 and
  A33--A37;
- the Windows D3D builder `VTGUModeDX32A:0x101fd9e0`, its OpenGL copy at
  `VTGOGL32A:0x10251220`, and Linux `STM_AlphaBlend_INV:0x196e30` independently
  implement the same equations and factor tables;
- the clamped `SRC_ALPHA/INV_SRC_ALPHA` recipe and the format-99 stored-alpha
  vectors below passed on the VX900, as recorded in
  [depth-blend-oracles.md](depth-blend-oracles.md) and
  [hardware-lab.md](hardware-lab.md).

The explicitly marked live rows are silicon results.  The wider tables remain
static recovery until each derivative receives its own guarded test.

## Enables and channel writes

A00 controls the four render targets independently:

```text
bits  7:4  alpha blend enables for RT3..RT0
bits 19:16 alpha test enables for RT3..RT0
bit     20 use RT0's alpha-test result for every RT
```

The proprietary blend builders set or clear all four blend bits with
`state[00] |= 0xf0` or `&= ~0xf0`.  The one-target live oracle only sets bit 4,
which is the narrower setting a one-RT Mesa path should use.  Preserve dither,
fog, alpha-test, packing, and all other A00 fields.

A53 controls RT0 writes.  Its upper fields must be preserved; only `[3:0]`
is the channel mask.  Gallium/public `R=1,G=2,B=4,A=8` maps to hardware
`A=8,R=4,G=2,B=1`:

```text
RGBA = 53010c0f   RGB = 53010c07   R = 53010c04
G    = 53010c02   B   = 53010c01   A = 53010c08
none = 53010c00
```

This remap agrees in Windows `0x101f9cd0`, OpenGL `0x1024d990`, and Linux
`STM_ColorWriteMode_INV:0x194df0`.  A5B/A63/A6B are the corresponding RT1--3
words.

## A34--A37 equation encoding

The hardware evaluates:

```text
RGBout = (FCa * Ca) Cop (FCb * Cb)
Aout   = (FAa * Aa) Aop (FAb * Ab)
```

The fields are:

| word | exact fields |
|---:|---|
| A34 | bit 16 no-clamp, RGB operand A `[15:10]`, RGB factor A `[9:4]` |
| A35 | RGB op `[15:14]`, RGB operand B `[13:8]`, RGB factor B `[7:2]` |
| A36 | bit 16 no-clamp, alpha operand A `[15:10]`, alpha factor A `[9:4]` |
| A37 | alpha op `[15:14]`, alpha operand B `[13:8]`, alpha factor B `[7:2]` |

The manual wording is important: A34/A36 bit 16 equal to zero **clamps** the
equation result to `[0,1]`; one disables that clamp.  Both proprietary
builders set bit 16.  The live Xorg-derived source-over oracle leaves it clear
and is therefore the safer initial UNORM-target setting.  Do not describe bit
16 merely as a generic “saturation” bit.

**Silicon (2026-09-25, rung `blend`,
[chrome9-queries-msaa-3d-flow-re.md](chrome9-queries-msaa-3d-flow-re.md)
§12.7):** the manual wording is inverted for negative results.  With bit 16
clear SUBTRACT stores `ff` for every negative channel (`ffa0ff40` instead of
`00a00040`); with bit 16 set all SUBTRACT/REVERSE_SUBTRACT cases are exact and
ADD above one still saturates (`c0ffffff`).  The kernel sets bit 16 only in
subtract lanes.

The operation code is `0=ADD, 1=SUBTRACT, 2=MAX, 3=MIN`.  The normal ADD and
SUBTRACT branches use source as operand A and destination as operand B.
REVERSE_SUBTRACT uses destination as A and source as B, then selects operation
1.  MIN/MAX also use destination as A and source as B, force both factors to
ONE, and select operation 3/2 respectively.

The six-bit factor codes are:

| API factor | RGB code | alpha code |
|---|---:|---:|
| ZERO | `05` | `09` |
| ONE | `15` | `19` |
| SRC_COLOR | `00` | `01` |
| INV_SRC_COLOR | `10` | `11` |
| SRC_ALPHA | `02` | `01` |
| INV_SRC_ALPHA | `12` | `11` |
| DST_ALPHA | `03` | `02` |
| INV_DST_ALPHA | `13` | `12` |
| DST_COLOR | `01` | `02` |
| INV_DST_COLOR | `11` | `12` |
| SRC_ALPHA_SATURATE | `08` | `19` |
| CONST_COLOR/ALPHA | `05` | `09` |
| INV_CONST_COLOR/ALPHA | `15` | `19` |

ZERO/ONE and CONST/INV_CONST deliberately share selectors.  Code `05/09`
reads a constant factor and code `15/19` reads its inverse.  ZERO and ONE are
obtained by clearing the relevant constant-factor registers; a nonzero blend
constant gives CONST and INV_CONST.  Thus A34--A37 alone do not close state for
ZERO/ONE once blend constants have ever been used in that context.

Place an A-term code at bit 4 and a B-term code at bit 2.  For normal
ADD/SUBTRACT, the operand selectors are RGB source/destination `0/1` and alpha
source/destination `1/2`.  Consequently the exact clamped pack is:

```text
A34 = 34000000 | (rgb_factor_A << 4)
A35 = 35000000 | (op << 14) | 00000100 | (rgb_factor_B << 2)
A36 = 36000000 | 00000400 | (alpha_factor_A << 4)
A37 = 37000000 | (op << 14) | 00000200 | (alpha_factor_B << 2)
```

Set `00010000` in A34 and A36 to reproduce the proprietary no-clamp policy.
Constant factors are not encoded by changing only these selectors: A38--A3F
must also be populated.  Keep constant-color/alpha factors capability-gated
until those words and their dirty tracking are implemented.

### Exact first recipes

These recipes have the clamp bit clear and, where ZERO or ONE occurs, require
the A38--A3F constant state described below to be zero:

| equation | A34 | A35 | A36 | A37 | status |
|---|---:|---:|---:|---:|---|
| `src*ONE + dst*ZERO` | `34000150` | `35000114` | `36000590` | `37000224` | live on format 99 |
| `src*SRC_A + dst*(1-SRC_A)` | `34000020` | `35000148` | `36000410` | `37000244` | live |
| `src*ONE + dst*ONE` | `34000150` | `35000154` | `36000590` | `37000264` | live RGB destination factor/equation |
| `src*ONE - dst*ONE` | `34000150` | `35004154` | `36000590` | `37004264` | table-derived |
| `dst*ONE - src*ONE` | `34000550` | `35004054` | `36000990` | `37004164` | table-derived |
| `min(dst,src)` | `34000550` | `3500c054` | `36000990` | `3700c164` | table-derived |
| `max(dst,src)` | `34000550` | `35008054` | `36000990` | `37008164` | table-derived |

To obtain the exact Windows/Linux-builder forms, add `00010000` to A34 and
A36.  For example, proprietary source-over is `34010020 35000148 36010410
37000244`, whereas the already-live clamped form is `34000020 35000148
36000410 37000244`.

The old Xorg `PictOpSrc` state ends in `36000500 37000200`.  That pair is
sufficient for its XRGB target, whose stored alpha is unobservable, but it is
not the table-derived `src-alpha*ONE + dst-alpha*ZERO` equation.  Use
`36000590 37000224` for correct alpha writes to an ARGB target; the format-99
COPY readback below confirms this corrected pair.

### Exact equation discriminator matrix

The next equation extension can validate RGB and alpha together on the live
format-99 target without saturation ambiguity.  Prefill `80208040`, draw
source `40c04080`, keep ONE/ONE factors, and use the following complete words
and expected covered dwords:

| equation | A34 | A35 | A36 | A37 | expected BGRA dword |
|---|---:|---:|---:|---:|---:|
| ADD | `34000150` | `35000154` | `36000590` | `37000264` | `c0e0c0c0` |
| SUBTRACT | `34000150` | `35004154` | `36000590` | `37004264` | `00a00040` |
| REVERSE_SUBTRACT | `34000550` | `35004054` | `36000990` | `37004164` | `40004000` |
| MIN | `34000550` | `3500c054` | `36000990` | `3700c164` | `40204040` |
| MAX | `34000550` | `35008054` | `36000990` | `37008164` | `80c08080` |

ADD's RGB result is already covered by the live XRGB discriminator, but this
BGRA vector additionally observes its alpha equation.  SUBTRACT and
REVERSE_SUBTRACT intentionally clamp negative channels to zero and leave a
different positive channel, detecting an accidental operand swap.  MIN/MAX
use different source/destination values in every component and their
expected alpha bytes differ.  Keep A38--A3F zero, full RGBA mask, and run each
case as a separate guarded submission followed by a legacy COPY closure draw.
Only ADD is currently live; the remaining four rows are static packet-derived
test vectors until silicon execution.

### Size-preserving nonconstant factor/function completion

All nonconstant core GL factors fit the existing blend-state ABI: append enum
values for `SRC_COLOR`, `INV_SRC_COLOR`, `DST_ALPHA`, `INV_DST_ALPHA`,
`DST_COLOR`, `INV_DST_COLOR`, and `SRC_ALPHA_SATURATE`, and append functions
`SUBTRACT`, `REVERSE_SUBTRACT`, `MIN`, and `MAX`.  No ioctl structure or packet
length changes.  `SRC1_*` remains unsupported because the recovered hardware
path and bounded shaders expose only one fragment output.

Lowering must distinguish API source/destination from physical equation
operand A/B:

- ADD and SUBTRACT: A is source with the API source factor; B is destination
  with the API destination factor.
- REVERSE_SUBTRACT: A is destination with the API destination factor; B is
  source with the API source factor.  Swap both operand selectors **and**
  factor codes.
- MIN/MAX: A is destination, B is source, both factors are forced to ONE;
  Gallium already canonicalizes them, but the kernel should not rely on that.

The same transform is applied independently to RGB and alpha.  On the alpha
lane, `SRC_COLOR` is `SRC_ALPHA`, `DST_COLOR` is `DST_ALPHA`, and their inverse
forms follow the same rule; the recovered alpha factor-code table implements
exactly this GL semantic.  `SRC_ALPHA_SATURATE` is legal only as the RGB source
factor and its alpha-lane equivalent is ONE.  Reject it in either destination
field rather than accepting a table encoding which the API cannot generate.

A compact guarded factor sweep can use source `60c04020` and prefilled
destination `e02080e0`.  For each RGB **source** factor, use destination factor
ZERO and alpha COPY ONE/ZERO.  With round-to-nearest multiplication the
expected BGRA dwords are:

| source factor | expected |
|---|---:|
| ZERO | `60000000` |
| ONE | `60c04020` |
| SRC_COLOR | `60911004` |
| INV_SRC_COLOR | `602f301c` |
| SRC_ALPHA | `6048180c` |
| INV_SRC_ALPHA | `60782814` |
| DST_ALPHA | `60a9381c` |
| INV_DST_ALPHA | `60170804` |
| DST_COLOR | `6018201c` |
| INV_DST_COLOR | `60a82004` |
| SRC_ALPHA_SATURATE | `60170804` |

Allow one LSB per multiplied RGB component while first characterizing silicon
rounding.  The chosen high destination alpha makes SATURATE select
`1-DST_ALPHA`; repeat it with destination alpha `20` to make SATURATE select
`SRC_ALPHA`.  Both cases are required to validate the native minimum rather
than accidentally aliasing either ordinary factor.

For each RGB **destination** factor, use source factor ZERO and alpha
destination-preserve ZERO/ONE.  The same input pair gives:

| destination factor | expected |
|---|---:|
| ZERO | `e0000000` |
| ONE | `e02080e0` |
| SRC_COLOR | `e018201c` |
| INV_SRC_COLOR | `e00860c4` |
| SRC_ALPHA | `e00c3054` |
| INV_SRC_ALPHA | `e014508c` |
| DST_ALPHA | `e01c70c5` |
| INV_DST_ALPHA | `e004101b` |
| DST_COLOR | `e00440c5` |
| INV_DST_COLOR | `e01c401b` |

Do not include SRC_ALPHA_SATURATE in that destination sweep.  Follow every
new factor/function submission with canonical COPY so a bad word cannot leave
an inherited factor or operand order hidden for the next test.

Exercise the alpha table separately on format 99 with an A-only write mask;
RGB must remain the destination `2080e0`.  With source alpha `60` and
destination alpha `e0`, source-factor/alpha-destination-ZERO results are
ZERO=`00`, ONE=`60`, SRC_COLOR/SRC_ALPHA=`24`,
INV_SRC_COLOR/INV_SRC_ALPHA=`3c`, DST_COLOR/DST_ALPHA=`54`, and their inverse
forms=`0c`.  Source-ZERO/destination-factor results are ZERO=`00`, ONE=`e0`,
SRC_COLOR/SRC_ALPHA=`54`, inverse=`8c`, DST_COLOR/DST_ALPHA=`c5`, and
inverse=`1b`.  These paired aliases are required GL alpha-lane behavior, not
missing hardware distinctions.  Keep SRC_ALPHA_SATURATE rejected in alpha
factor fields.

Constant factors are the only core factor family that does **not** fit the
current ABI because `drm_chrome9_blend_state` contains no blend-color value.
They need a new size/versioned successor (or a separately versioned immutable
state object), not reinterpretation of alpha-test or draw-tail fields.  A
normalized four-component value must be lowered independently into the
physical A-side or B-side constant registers; REVERSE_SUBTRACT swaps which
side receives the API source and destination constants.

### Constant-factor state and closure

The factor constants use unsigned 1.10 fixed point.  For an API blend color
component already clamped to `[0,1]`, the proprietary converter produces:

```text
q(c) = min(floor(c * 1024), 0x400)
```

The A-term RGB factor is held in A39/A3A and its alpha factor in A3E low;
the B-term RGB factor is in A3C/A3D and its alpha factor in A3F low:

```text
A39[22:12] = q(Ra)
A3A[22:12] = q(Ga)    A3A[10:0] = q(Ba)
A3E[10:0]  = q(Aa)

A3C[22:12] = q(Rb)
A3D[22:12] = q(Gb)    A3D[10:0] = q(Bb)
A3F[10:0]  = q(Ab)
```

A38/A39 low, A3B/A3C low, and A3E/A3F high are separate operand-constant
fields.  The complete manual-defined allocation, including those fields, is:

```text
A38[22:12] Ca.G       A38[10:0] Ca.B
A39[22:12] FCa.R      A39[10:0] Ca.R
A3A[22:12] FCa.G      A3A[10:0] FCa.B
A3B[22:12] Cb.G       A3B[10:0] Cb.B
A3C[22:12] FCb.R      A3C[10:0] Cb.R
A3D[22:12] FCb.G      A3D[10:0] FCb.B
A3E[22:12] Aa         A3E[10:0] FAa
A3F[22:12] Ab         A3F[10:0] FAb
```

Here C/A names are operand constants selected by A34--A37 operand selector
5/9, whereas FC/F names are blend-factor constants selected by factor
selector 5/9; they are independent despite sharing each physical word.
Preserve both halves in a general state tracker.  A first implementation that
exposes no operand constants can safely emit all of A38--A3F as zero on every
blend-state change.  That makes ZERO/ONE self-contained and matches the
clearing performed by the proprietary builders.

`CONST_COLOR` and `CONST_ALPHA` share the same native selector but require
different population of these registers.  For CONST_COLOR, put q(R/G/B) in
the RGB factor lanes and q(A) in the alpha factor lane.  For CONST_ALPHA, put
q(A) in **all three** RGB factor lanes as well as the alpha lane.  The inverse
forms use the same stored values and select the inverse native factor code;
do not pre-invert the constants in software.  Populate the physical A-side
lanes for the equation's A operand and B-side lanes for its B operand.  This
means REVERSE_SUBTRACT places the API destination factor on A and the API
source factor on B, matching its operand swap.  Leave all Ca/Cb/Aa/Ab operand
constant halves zero until a shader path actually selects constant operands.

For blend color `(R,G,B,A)=(0.25,0.5,0.75,1.0)`, both A- and B-factor copies
are exactly:

```text
39100000 3a200300 3e000400
3c100000 3d200300 3f000400
```

An exact no-rounding constant-factor discriminator uses blend color
`(R,G,B,A)=(1,0,1,0)`, source `ff112233`, and destination `80445566`:

```text
A38..A3F = 38000000 39400000 3a000400 3b000000
           3c400000 3d000400 3e000000 3f000000
A34..A37 = 34000050 35000154 36000490 37000264
expected BGRA = 80115533
```

This selects `src*CONST + dst*(1-CONST)` independently for RGB and alpha.
The R/B result comes from the source, G/alpha from the destination, so every
factor-register component and both direct/inverse selector paths are
observable without fractional store rounding.  Follow it immediately by a
canonical COPY draw.  A legacy body which clears only A38/A3B/A39/A3C leaves
stale G/B/alpha factor halves and is not closed after this test; either expand
legacy closure to all A38--A3F or keep every later draw on the fully closed
successor before advertising constant factors.

The unused halves still need explicit preservation or zeroing.  For example,
`src*CONST_COLOR + dst*INV_CONST_COLOR` uses `34000050 35000154`; its alpha
pair is `36000490 37000264`.  Those equation words look identical to a
ZERO/ONE selection because the A39/A3A/A3E and A3C/A3D/A3F contents supply
the semantic difference.

## Alpha test

A33 is independent of the blend equation:

```text
A33[14:12] = hardware compare selector 0..7
A33[10:0]  = ref == 255 ? 0x400 : (ref << 2) | (ref >> 6)
```

The proprietary builder's D3D compare input is one-based and is therefore
decremented; the resulting hardware selector and Gallium `PIPE_FUNC_*` enum
are both directly `NEVER..ALWAYS = 0..7`.  Exact examples are:

```text
NEVER,  ref 00 = 33000000
LESS,   ref 80 = 33001202
GEQUAL, ref fe = 330063fb
ALWAYS, ref 80 = 33007202
ALWAYS, ref ff = 33007400
```

Windows `0x101fab20` and Linux `STM_AlphaTest_INV:0x194c40` both implement the
normal GL/D3D enable by toggling A00 bit 20.  They clear it for bound
render-target private formats `1028`, `102c`, and `203c..203e`; the Linux
routine obtains that surface from the same `context+0x3aac` RT0 array used by
`STM_RenderTarget_INV`.  Do not generalize that format restriction to ordinary
UNORM targets.  A one-RT implementation could use documented
A00 bit 16 instead, but bit 20 is the independently matching proprietary
recipe and should be the first guarded test.

Alpha test consumes the fragment output alpha.  It is valid for the recovered
REPLACE/MODULATE shaders, but the state also participates in the proprietary
fragment-program key.  A general Mesa path must not advertise alpha test by
emitting only A33/A00 while allowing shaders that do not produce a defined
alpha channel.

### Exact first alpha-test discriminator

The original color-triangle body has two padding words at type-01 body indices
22 and 23, immediately before the next packet.  Replace index 22 with A33 and
retain index 23 as padding; no packet length or later body index needs to
change.  Enable the proprietary one-result-for-all-RTs path by changing only
A00 bit 20:

```text
body[18]: 00000011 -> 00100011
body[22]: cc000000 -> 33001202    # incoming alpha LESS than ref 0x80
```

The blend-aware successor expands A38--A3F and shifts the following state by
four dwords.  In that body A00 is at index 22 and must become `00100011`; put
A33 in the shifted padding at index 26, **not** index 22.  Body length again
stays unchanged.

The combined D16+blend successor has a different final slot.  Blend first
expands the destination state by four dwords, then the five depth words consume
the old two-word alignment gap and move the following header by four dwords.
The resulting first type-01 group is exactly:

```text
body[6]       A52 destination format/pitch
body[7..14]   A38..A3F closed constant state
body[15..18]  A34..A37 blend equations
body[19..25]  A86, A87, A53, A00, A01, A88, A89
body[26..30]  A10, A11, A12, A13, A20 depth/clip state
body[31]      A33 when alpha test is enabled; cc000000 otherwise
body[32]      next aligned type-02 header
```

Thus A33 consumes body index 31, not the color-only blend slot 26.  It does
not change the 192-dword three-vertex body length or shift any later state:
A23/A24 remain at 57/58, A80..A83 at 108..111, the immediate header at
168..171, vertices at 172..189, and final padding at 190/191.  A00 remains at
index 22 and gains bit 20.  This placement is safe because every type-01 word
contains its own subaddress.  The byte-identical Windows builders
`VTGUModeDX32A:0x101fb090` and `VTGOGL32A:0x1024edc0`, plus Linux
`STM_AlphaFunc_INV:0x194ad0`, update the ordinary A33 state-image slot; none
establishes an ordering dependency relative to A10--A20.

The now-live combined alpha/depth discriminator keeps blending disabled
with canonical COPY words and use LESS for both tests.  Clear XRGB to zero and
D16 to `ffff`, then cover the same interior pixels twice:

```text
draw 1: z=4000, color=c0ff0000, alpha LESS 80 -> killed
draw 2: z=8000, color=4000ff00, alpha LESS 80 -> passes
expected covered color/depth: 0000ff00 / 8000
```

Correct alpha kill suppresses both color and depth writes from draw 1, so the
farther green draw still passes against the clear value.  If alpha test is
disabled or reversed, draw 1 stores red/depth `4000` and rejects draw 2.  If
the color kill occurs but a killed fragment incorrectly writes depth, the
final pair is zero/`4000`.  This therefore validates the A33 slot, A00 bit 20,
and alpha-test ordering relative to D16 writes in one guarded test.  ALWAYS
and NEVER controls also passed before this LESS case; all guards remained
intact.

Submit two otherwise-identical overlapping COPY draws against a zero target:

```text
draw 1: A86/A87 = 86ff0000 87000040   # 0x40 < 0x80, passes, red
draw 2: A86/A87 = 8600ff00 870000c0   # 0xc0 < 0x80 is false, killed
expected covered XRGB = 00ff0000
```

If the comparison direction is accidentally reversed the covered result is
green; if alpha test is disabled the second draw also makes it green; if both
draws are killed it remains zero.  This one result therefore distinguishes
the three common implementation errors.  First run `ALWAYS, ref=80`
(`33007202`) as a no-output-change retirement test and `NEVER, ref=80`
(`33000202`) as the full-kill control.

After LESS retires, all eight functions can be covered by three nonoverlapping
triangles with incoming alpha bytes `40`, `80`, and `c0` against reference
`80`.  The exact A33 words and required passing alpha sets are:

| function | A33 | passing source-alpha bytes |
|---|---:|---|
| NEVER | `33000202` | none |
| LESS | `33001202` | `40` |
| EQUAL | `33002202` | `80` |
| LEQUAL | `33003202` | `40,80` |
| GREATER | `33004202` | `c0` |
| NOTEQUAL | `33005202` | `40,c0` |
| GEQUAL | `33006202` | `80,c0` |
| ALWAYS | `33007202` | `40,80,c0` |

The reference expander maps byte `80` to internal fixed-point `202`; packed
D3DCOLOR normalization is expected to map the equal input consistently, which
is why the EQUAL/NOTEQUAL rows are useful follow-ups rather than the first
safety test.
Also test the endpoint encodings `ref=00 -> 000` and `ref=ff -> 400` before
advertising the complete enum.

To prove that alpha test consumes fragment alpha **before** blending, use a
format-99 target prefilled `ff00ff00`, source `40ff0000`, LESS/ref `80`, and
enable blending with RGB COPY but alpha destination-preserve:

```text
A00       = 00100011
A33       = 33001202
A34..A37 = 34000150 35000114 36000490 37000264
expected covered BGRA = ffff0000
```

The fragment alpha `40` passes, RGB COPY stores red, and the independent
alpha equation `src*ZERO + dst*ONE` retains destination alpha `ff`.  An
incorrect post-blend alpha test instead compares `ff`, kills the fragment,
and leaves the original `ff00ff00`.  This exact vector also validates the
opposite alpha ZERO/ONE factor direction after the basic alpha-test and
canonical COPY/source-over cases are live.

For the textured builders, append A33 to the common
A86/A87/A53/A00/A01/A88/A89 type-01 group.  That group currently has seven
payload words, so the aligned packet emitter inserts one `cc000000` before
the following header.  A33 consumes that alignment slot: the exact non-blend
body counts remain REPLACE/MODULATE 224/228, the blend-aware counts remain
228/232, and the later packet indices do **not** move.  Do not overwrite the
type-02 sampler's explicit trailing `cc000000`, which belongs to a different
parameter type.

Concretely, A33 occupies body index 47 in the original textured builder and
51 in the blend-aware builder (whose closed destination state adds four
dwords).  Their A00 indices are 43 and 47 respectively.

The first textured discriminator should keep a format-98 destination and
REPLACE shader, use nearest/clamp with a uniform BGRA texture, and retain the
same LESS/ref `80` state.  Draw uniform `40ff0000` first, then overwrite the
texture with uniform `c000ff00` only after the first job has retired and draw
again over the same pixels.  Correct output remains red: sampled alpha `40`
passes and sampled alpha `c0` kills the second draw.  Disabled alpha test
ends green, reversed comparison also ends green, and both-killed remains the
prefill.  Check texture preservation and both source/destination guards after
each retirement.  This isolates fragment-program texture alpha from vertex
COLOR0 normalization and validates A33 at textured body index 51.

## Safe implementation order

1. Keep the live clamped source-over words and RT0-only A00 bit 4.
2. Add packet-exact channel masks by changing only A53 `[3:0]`.
3. Zero A38--A3F and test table-derived COPY on an ARGB target so stored alpha
   validates A36/A37 without inherited constant-factor state.
4. Test ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, and MAX separately with guards.
5. Add A33 plus A00 bit 20 only with a fragment program whose output alpha is
   defined; exercise both passing and killed pixels.
6. Defer nonzero blend constants and separate-alpha state until A38--A3F are
   emitted and independently tested.

## Minimal next DRM/Mesa ABI plan

Do not expose A34--A3F values to userspace.  Add normalized, validated blend
state and let the kernel lower it to the words above.  The current triangle
and textured-triangle ioctl structures have only two reserved 64-bit words;
that is not enough for explicit independent RGB/alpha state.  Preserve their
sizes and add blend-aware successors rather than silently changing an ioctl's
encoded size.

The shared state should use full-width UAPI fields, not C bitfields:

```c
struct drm_chrome9_blend_state {
        __u32 flags;             /* bit 0: RT0 blend enable */
        __u32 rgb_src_factor;
        __u32 rgb_dst_factor;
        __u32 alpha_src_factor;
        __u32 alpha_dst_factor;
        __u32 rgb_func;
        __u32 alpha_func;
        __u32 color_mask;        /* API order: R=1,G=2,B=4,A=8 */
        __u32 reserved[2];       /* must be zero */
};
```

The blend-successor tail can add the destination format without changing
either ioctl size.  Replace its first formerly-zero tail dword by
`destination_format` and retain three zero `reserved[]` dwords:

```text
0 = BGRX8888 -> A52 format 98
1 = BGRA8888 -> A52 format 99
```

This is backward compatible: every old caller was required to pass zero in
that location and therefore continues to request BGRX.  The kernel must range
check the normalized enum and synthesize A52; userspace must not submit a
packed hardware format.  Gate BGRA render-target exposure on a distinct
capability bit.  Mesa must force the blend-aware successor for a BGRA target
even when blending is disabled and the channel mask is RGBA, because the
legacy ioctl has no format field and always emits A52 format 98.

Initially advertise BGRA only for guarded internal render targets.  Do not
copy the BGRX binding mask wholesale: it also includes display, scanout,
shared, and PRIME paths, while current KMS/import validation accepts only
XRGB8888.  A BGRA render-target-plus-sampler binding additionally requires
the independently advertised texture capability.  CPU clear may use Mesa's
normal B8G8R8A8 pack/fill path and must preserve the stored alpha byte.

The first factor enum needs only `ZERO`, `ONE`, `SRC_ALPHA`, and
`INV_SRC_ALPHA`.  The first function enum needs only `ADD`.  Assign stable
values for the wider recovered factor/function set if desired, but reject it
with `-EOPNOTSUPP` until its guarded test passes.  A disabled blend state must
require canonical `ONE/ZERO/ADD` fields or ignore them after validation; it
must not accept arbitrary values which happen not to be consumed.

Add this structure to new color-only and textured draw successors.  Both use
one kernel lowering helper.  That helper must:

1. reject unknown flags, enums, color-mask bits, or nonzero reserved words;
2. support RT0 only and reject logic-op/blend combinations;
3. keep A34/A36 bit 16 clear for the initial clamped policy;
4. emit a self-contained zero constant block before the equation:

   ```text
   FE020440 0000043c 00010000 cc000000
   38000000 39000000 3a000000 3b000000
   3c000000 3d000000 3e000000 3f000000
   ```

5. synthesize A34--A37 from the validated enums, never from userspace words;
6. remap the public color mask into A53 `[3:0]` while retaining `53010c00`;
7. set only A00 bit 4 when enabled and clear only bit 4 when disabled,
   preserving the live dither bit and every unrelated A00 field.

The preserved bit 0 is not currently a selectable Gallium state.  All legacy
and blend-aware packets leave it set, whereas Chrome9 advertises
`pipe_caps.dithering=false`; Mesa's state tracker consequently never copies
the GL dither enable into `pipe_blend_state::dither`.  This makes ordinary GL
default state submit successfully but also means `glDisable(GL_DITHER)` is
not distinguishable at the driver.  Do not advertise the cap merely because
the packet default is enabled.  First add a normalized UAPI bit which controls
A00 bit 0, then compare fractional color and blended output on XRGB8888 with
the bit both clear and set.  A53 `53010c0f` selects mode `[16:15]=10`, which
the manual calls rounding with multiplication by `2^n-1`; the current packet
does not select a spatial dither table.  This mode makes the bit potentially
observable for fractional blend results despite the 8-bit target.

The initially useful independent RGB/alpha combinations are:

| semantic state | RGB A34/A35 | alpha A36/A37 | evidence |
|---|---|---|---|
| disabled | canonical COPY words, A00 bit 4 clear | `36000590 37000224` | live, including format-99 alpha storage |
| COPY | `34000150 35000114` | `36000590 37000224` | live on format 99 |
| source-over, same RGB/alpha factors | `34000020 35000148` | `36000410 37000244` | live on XRGB; alpha store unobservable |
| canonical source-over with independent alpha | `34000020 35000148` | `36000590 37000244` | live on format 99 |
| additive ONE/ONE | `34000150 35000154` | `36000590 37000264` | live on XRGB |

The existing live source-over result proves the RGB equation and that A00 bit
4 reaches RT0.  It does **not** prove stored alpha because target format 98 is
XRGB.  Notice that the live same-factor alpha pair would calculate
`Asrc*Asrc + Adst*(1-Asrc)`, whereas canonical source-over alpha is
`Asrc*ONE + Adst*(1-Asrc)` and therefore requires `36000590 37000244`.
The format-99 readback now proves the first independent-alpha combination:
canonical source-over stored `80` alpha, whereas the stale same-factor
equation would have stored about `40`.  Factors outside the bounded live enum
remain unverified.

### Gallium lowering boundary

Mesa 26.2's factor values are not the UAPI values.  The first lowering must
use explicit cases, not casts:

| Gallium state | Gallium value | blend UAPI value |
|---|---:|---:|
| `PIPE_BLENDFACTOR_ONE` | `1` | `ONE = 1` |
| `PIPE_BLENDFACTOR_SRC_ALPHA` | `3` | `SRC_ALPHA = 2` |
| `PIPE_BLENDFACTOR_ZERO` | `17` | `ZERO = 0` |
| `PIPE_BLENDFACTOR_INV_SRC_ALPHA` | `19` | `INV_SRC_ALPHA = 3` |
| `PIPE_BLEND_ADD` | `0` | `ADD = 0` |

Apply the factor conversion independently to all four RGB/alpha factor
fields.  Reject enabled blending with every other Gallium factor or with
`SUBTRACT`, `REVERSE_SUBTRACT`, `MIN`, or `MAX`.  Also reject independent-RT
blend, logic operation, alpha-to-coverage (including its dither control),
alpha-to-one, a nonzero `max_rt`, advanced blend, or coherent advanced blend.
The standalone dither field is the exception described above: the packet
currently leaves dither enabled regardless of its value, while the GL state
tracker suppresses the field because the screen cap is false.

When blending is disabled, unused Gallium equation and factor fields have no
semantic effect.  Canonicalize them to independent `ONE/ZERO/ADD` in the
UAPI rather than rejecting a CSO merely because it contains stale disabled
fields.  A partial color mask still selects the blend-aware successor even
with blending disabled.  Full RGBA writes with blending disabled should keep
using the original ioctl, both to minimize the live delta and to prove that a
successor draw closes its state before a legacy draw.

That transition is closed only for the present zero-constant ABI.  Both
historical legacy builders clear the partial set A38/A3B/A39/A3C, whereas the
successor clears all of A38--A3F.  If a later successor writes a nonzero blend
constant, a following legacy COPY can consume stale A3A/A3D/A3E/A3F through
its ZERO/ONE selectors.  Before adding constant factors, either expand both
legacy bodies to close all eight words or route every draw through a closed
successor.

Keep `blend_equation_separate=false`, `indep_blend_enable=false`,
`indep_blend_func=false`, `max_dual_source_render_targets=0`, and
`max_render_targets=1`.  There is no Gallium cap for ordinary single-target
blending; adding this bounded UAPI does not justify raising an API version.
Mesa's compatibility extension table marks `EXT_blend_color`,
`EXT_blend_func_separate`, `EXT_blend_minmax`, `EXT_blend_subtract`, and
`NV_blend_square` as `dummy_true`.  The new ABI provides useful independent
RGB/alpha factors for `EXT_blend_func_separate`, but constant factors,
MIN/MAX/SUBTRACT, color-square factors, and core destination-color/alpha
factors remain unimplemented and must fail closed at draw time.  Pipe caps
cannot suppress those extension strings in this Mesa version.

The smallest discriminating Gallium smoke sequence is:

1. Draw disabled/full-mask COPY through the legacy ioctl and retain the
   existing pixel and guard result.
2. Prefill green `0000ff00`; draw 50%-alpha red `80ff0000` with RGB
   `SRC_ALPHA/INV_SRC_ALPHA`, alpha `ONE/INV_SRC_ALPHA`, and ADD.  A robust
   interior sample is `007f7f00` through `00808000`; blue stays zero.
3. Without recreating the context, bind disabled/full-mask state and draw an
   opaque COPY.  This catches stale A00, A34--A3F, or A53 state left by the
   successor.
4. Prefill a nonuniform XRGB value and exercise masks R, G, B, RGB, and NONE
   separately.  Alpha-only writes cannot be proven on the format-98 target.
5. Repeat step 2 with a uniform BGRA texture `80ff0000` and REPLACE.  This
   proves sampled alpha reaches the blend unit; require target padding,
   texture padding, both objects' guards, and source texels to remain intact.
6. Negative direct-pipe cases should leave the submission count unchanged
   and increment the rejected count for each unsupported factor, each
   non-ADD function, logic-op, independent RT, multisample blend flag,
   `max_rt=1`, and advanced/coherent state.  Repeat one accepted draw after
   the rejection matrix to prove no invalid command reached the engine.

Format 98 alone cannot validate alpha-factor state or an alpha channel mask.
The live format-99 target now validates canonical independent-alpha
source-over, COPY alpha, and the A-only mask.  Other alpha-factor combinations
remain table-derived until separately exercised.

### Packed vertex color normalization prerequisite

The blend equation can expose a vertex-declaration error which opaque COPY
draws hide.  The historical Xorg FVF words use A02 `020041d0` and A03
`030052d0`.  In the VX900 format byte, bit 0 is Normalize: `d0` maps BGRA byte
order but leaves each component in the numeric range 0..255.  The manual's
D3DCOLOR table and the Windows OpenGL declaration mapper at
`VTGOGL32A:0x10204320..0x102043ea` both select `d1`, division by 255.

A live raw successor sweep made this bit observable.  Source-over with vertex
alpha 00 preserved the green destination, proving A00 and the blend equation
were active; alpha 40, 7f, and 80 all produced opaque red because every
nonzero value was far above 1.0 and saturated downstream.  This rules out an
A00 packet-index failure.  Changing only A02 from `020041d0` to
`020041d1` then produced the expected `00807f00` source-over result.  General
Gouraud and MODULATE paths must use:

```text
A02 = 020041d1  # normalized COLOR0 / diffuse
A03 = 030052d1  # normalized COLOR1 / specular
```

The older 00/ff RGB tests and the solid-color blend oracle could not catch the
mistake: full byte values saturate to the same endpoints, while solid shading
takes alpha from A87 instead of the vertex declaration.

### Live bounded blend-successor result

Kernel commits `2b2425e` and `8efc38c` added validated color and textured
blend-aware successors and the normalized COLOR0 declaration.  The live raw
suite on `1106:7122` established all of the following without an engine error
or guard corruption:

- untextured red A=`80` over green produced `00807f00`;
- the A=`00/40/7f/80` sweep isolated the `d0` saturation and the `d1` change
  fixed it;
- blend disabled with only the hardware R write bit selected produced
  `00ffff00` from the nonuniform source/destination pair;
- a blend-successor draw followed immediately by a legacy COPY produced the
  expected blue result, proving the present zero-constant successor leaves no
  stale A00/A34--A3F/A53 state consumed by the legacy body;
- BGRA8888 texture REPLACE with source-over produced `00807f00`, proving
  sampled alpha reaches the blend unit.

The matching Mesa lowering, nested commit `6c6d99d` and parent commit
`2181d02`, passed the complete Gallium suite.  Its untextured half-alpha
sample was `007f8000`, the equally valid opposite one-LSB rounding from the
raw result; textured BGRA blending, D16, REPLACE, MODULATE, varying-RHW and
PRIME tests all passed in the same run.  This promotes the first four UAPI
factor enums, ADD, RT0 color masks, the color and textured successor packet
layout, and A02 normalization from static recipes to live evidence.  The
later format-99 run below adds stored destination alpha; A03/COLOR1
normalization remains outside the current shader/readback proof.

The old Xorg H5 emitter has now also been executed with its destination setter
fed the `a8r8g8b8` private format (`0x20028888`) instead of its normal
`x8r8g8b8` (`0x20020888`).  The emitted 160-dword draw changed only the RT0
format byte in the destination group:

```text
x8r8g8b8: 5007fd00 51000000 52980004
a8r8g8b8: 5007fd00 51000000 52990004
```

The following blend and color-mask words remained
`34000150 35000114 36000500 37000200 53010c0f`.  This is executable,
independent confirmation that format 99 is accepted by the Xorg state builder
and is a one-word derivative of format 98.  The guarded silicon run has now
confirmed the same derivative with stored-alpha readback.

The implementation uses a distinct capability bit for verified BGRA/separate-
alpha storage in addition to the two blend-successor capabilities.  Mesa
should submit only combinations covered by the advertised bits; kernel enum
validation remains mandatory even for an older or malicious userspace.

### Safest live deltas

Run each as a complete guarded body followed by the known variable-triangle
regression, preserving the timeout/reset procedure:

1. Add only the eight zero A38--A3F words to the existing COPY draw; output
   must remain identical.  This establishes constant-state closure.
2. Change corrected COPY alpha words to `36000590 37000224` on XRGB.  RGB must
   remain identical and the engine must retire cleanly.
3. Test additive RGB with a sub-saturating source and prefilled destination,
   for example source `00400000` plus destination `00002000`, expecting
   `00402000` inside.  Use `34000150 35000154` and keep the alpha pair
   `36000590 37000264`.  This exact discriminator is now live-proven.
4. Test R, G, B, RGB, and NONE masks individually against a nonuniform
   destination.  Alpha-mask behavior is not observable on XRGB.
5. For the first textured blend, sample uniform BGRA `80ff0000` with REPLACE
   over a prefilled green `0000ff00`, nearest/clamp.  With the already-live
   source-over words the covered result should be approximately `00807f00`
   using the live round-to-nearest behavior.  This directly validates texture
   alpha through the fragment output into RGB blending without requiring an
   ARGB destination.
6. Add a format-99 ARGB render target and check alpha readback for independent
   A36/A37 factors.  This step is now live-proven as described below.

The format-99 discriminator can be exact rather than approximate.  Clear the
ARGB target to zero, draw solid `A=80,R=ff,G=00,B=00`, and use the live RGB
source-over pair with canonical independent source-over alpha:

```text
A52 = 52990004
A34..A37 = 34000020 35000148 36000590 37000244
A86/A87 = 86ff0000 87000080
expected covered dword = 80800000
```

The red byte is exactly `ff * 80/ff = 80`; canonical output alpha is exactly
the source alpha because destination alpha is zero.  Accidentally retaining
the live same-factor alpha pair `36000410 37000244` instead produces alpha
near `40`, so the expected result becomes `40800000`.  That is a strong
single-draw discriminator for A36's independent source-alpha factor.

The guarded run passed that discriminator with covered dword `80807f00` over
transparent green.  It also passed an ARGB COPY to a zero target: with
`34000150 35000114 36000590 37000224`, source `80ff0000` stored exactly
`80ff0000`.  For channel-mask validation, prefill `44332211`, draw COPY source
`80a0b0c0`, and expect:

```text
mask R    (A53 low = 4): 44a02211
mask G    (A53 low = 2): 4433b011
mask B    (A53 low = 1): 443322c0
mask A    (A53 low = 8): 80332211
mask RGB  (A53 low = 7): 44a0b0c0
mask NONE (A53 low = 0): 44332211
```

These values also expose an API-order-to-hardware-order mask reversal without
depending on edge pixels or fractional blend rounding.

### Live format-99 destination result

Kernel commit `edd8da0` adds normalized destination-format selection to the
unchanged-size blend successors and advertises the separately gated BGRA
capability.  On VX900 silicon, A52 format 99 passed all three exact raw
discriminators with guards intact:

```text
blend-disabled full-mask COPY:             80ff0000
A-only COPY, prefill/source 44332211/80a0b0c0: 80332211
canonical independent-alpha source-over:   80807f00
```

The source-over result was measured over transparent green, so it proves RGB
`SRC_ALPHA/INV_SRC_ALPHA` and stored alpha `ONE/INV_SRC_ALPHA` in one draw;
the obsolete same-factor alpha pair would have produced an alpha byte near
`40`.  Mesa nested commit `79e0882` and parent commit `3c9197c` then passed
the guarded Gallium BGRA render-target path as well.  Its float 0.5 vertex
color packs to byte `7f`, the expected one-LSB quantization difference from
the raw byte-`80` vector.

### Alpha-test ABI extension without another size change

After the guarded alpha-test discriminator passes, the blend-aware successor's
two reserved state dwords can become `alpha_test_func` and `alpha_test_ref`
without changing the ioctl size; zero-initialized older users remain valid.
Add a separate bit to the existing state `flags` for alpha-test enable.  Use
normalized enums
`NEVER..ALWAYS = 0..7` and a reference restricted to `0..255`; the kernel then
packs `(func << 12) | encode_ref(ref)` into A33 and toggles only A00 bit 20.
Do not expose packed A33 or A00.

Mesa 26.2 provides an exact fixed-function route once
`pipe_caps::alpha_test` is true.  `st_update_depth_stencil_alpha()` copies the
test into `pipe_depth_stencil_alpha_state::{alpha_enabled,alpha_func,
alpha_ref_value}`; `PIPE_FUNC_NEVER..ALWAYS` are already numbered 0..7.  The
state tracker deliberately passes `AlphaRefUnclamped`, so userspace must use
`float_to_ubyte()` (clamp plus round to nearest) before filling the byte UAPI,
not a C cast.  The kernel's exact byte-to-A33 reference conversion is:

```text
encode_ref(255) = 0400
encode_ref(v)   = (v << 2) | (v >> 6), for v=0..254
```

With the cap false, `st_update_fp()` instead keys a fragment-shader variant
and `st_nir_lower_alpha_test()` inserts a compare plus `nir_discard_if`.
Chrome9's bounded shader classifier does not accept that form.  Therefore the
cap must change only together with the DSA/UAPI route; merely recognizing A33
in the kernel while leaving the cap false will not make desktop GL alpha test
reach it.

When disabled, require both fields to remain zero so older callers which knew
them only as reserved words continue to pass validation.  Advertise a
distinct alpha-test capability only after the solid-color pass/fail oracle
and one REPLACE texture-alpha oracle both retire cleanly.  This is independent
of ARGB destination support:
alpha test consumes fragment alpha before the format-98 XRGB store, so it can
be validated on the already-live target.

One more exposure boundary is important: setting the Gallium cap true claims
the fixed test for every otherwise supported draw.  The alpha-aware D16
successor is now live, and both raw and Gallium alpha-plus-D16 tests passed.
The driver's texture+depth combination remains explicitly unsupported and is
rejected before submission, so it does not invalidate the cap for the bounded
draw combinations which the driver accepts.
