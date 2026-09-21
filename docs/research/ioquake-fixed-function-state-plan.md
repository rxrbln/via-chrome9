# Chrome9 HD ioquake fixed-function state plan

This note turns the recovered blend, alpha, depth, polygon-offset, stencil,
and color-mask state into a ranked implementation plan.  It is deliberately
limited to structured state: userspace supplies normalized enums and values;
the kernel validates them and synthesizes every HPara word.

Unless a row says **live**, it is an exact static reconstruction and still
requires one bounded silicon discriminator.  A static word is not permission
to advertise the corresponding Gallium state.

## Evidence and confidence

The field definitions are in the *VX900 Series Chrome9 HD Open Graphics
Programming Manual*, preliminary revision 1.0, pages 90--104.  The executable
cross-checks are:

- Linux `via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`:
  alpha test/function `0x194c40/0x194ad0`, stencil `0x1955e0`, depth bias
  `0x1959d0`, depth write `0x195b90`, and blend `0x196e30`;
- Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`:
  depth enable/function `0x101fa910/0x101faa10`, alpha test
  `0x101fb090`, stencil `0x101fc060`, and blend `0x101fd9e0`;
- Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`:
  byte-equivalent depth, alpha, stencil, and blend builders at
  `0x1024e6d0`, `0x1024edc0`, `0x1024fa10`, and `0x10251220`.

The current live kernel has proved D16 LESS/write and LEQUAL/write, alpha-test
ALWAYS/NEVER/LESS including suppression of D16 writes, clamped ADD blending
with ZERO/ONE/SRC_ALPHA/INV_SRC_ALPHA, independent RGB/alpha source-over,
COPY and ONE/ONE ADD, BGRA storage, and A-only color masking.  The wider
tables below are binary/manual-exact but not yet live unless marked.

## One ownership rule and one state-closure rule

Depth or stencil surface mappings remain owned by the published job through
retirement.  A timeout quarantines every target, Z/S BO, texture, page-table
entry, and ring segment referenced by the command; no error unwind may unbind
them while the engine can still fetch.

Every successor closes the whole logical block it exposes.  Disabling a
feature clears its enable and writes neutral dependent state.  In particular:

```text
A16..A18 = 16000000 17000000 18000000       # no polygon offset
A19..A1B = 19800000 1a800000 1b803fbf       # depth clamp defaults
A1E..A20 = 1e000000 1f000000 20007fff       # wide pixel-Z interval
A23..A26 = 2300ffff 24070000 2500ffff 26070000
A38..A3F = 38000000 39000000 3a000000 3b000000
            3c000000 3d000000 3e000000 3f000000
A53       = 53010c0f                          # full RGBA write
```

The A38--A3F zero block is mandatory even for ZERO/ONE blending because the
native factor selectors are shared with constant factors.  The neutral
stencil block is mandatory before a later re-enable.  The wide A1E--A20 range
is mandatory: the apparent zero reset range clips every positive Z before
the depth function.

## Blend equations and factors

A00 bit 4 enables RT0 blend.  A34--A37 evaluate independent RGB and alpha
equations:

```text
RGB = (FCa * Ca) op (FCb * Cb)
A   = (FAa * Aa) op (FAb * Ab)

A34 = 34000000 | (rgb_factor_A << 4)
A35 = 35000000 | (op << 14) | 00000100 | (rgb_factor_B << 2)
A36 = 36000400 | (alpha_factor_A << 4)
A37 = 37000200 | (op << 14) | (alpha_factor_B << 2)
```

A34/A36 bit 16 disables clamp when set.  The proprietary builders set it,
but the live UNORM path leaves it clear and clamps to `[0,1]`; retain that
policy until a floating target exists.

The complete factor table is:

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
| CONST_COLOR or CONST_ALPHA | `05` | `09` |
| inverse constant | `15` | `19` |

On the alpha lane SRC_COLOR aliases SRC_ALPHA and DST_COLOR aliases
DST_ALPHA by definition.  SRC_ALPHA_SATURATE is legal only as the RGB source
factor; its alpha source factor is ONE.  Reject it in destination and alpha
factor fields.  No recovered selector implements SRC1/dual-source factors.

ZERO/ONE and CONST/INV_CONST use the same selectors.  Zeroed A38--A3F makes
`05/09` zero and `15/19` one.  Nonzero constant factors use unsigned U1.10:

```text
q(c) = min(floor(c * 1024), 0400)

A39[22:12]=Ra  A3A[22:12]=Ga  A3A[10:0]=Ba  A3E[10:0]=Aa
A3C[22:12]=Rb  A3D[22:12]=Gb  A3D[10:0]=Bb  A3F[10:0]=Ab
```

CONST_ALPHA replicates q(A) to all RGB factor lanes.  The inverse selectors
consume the same stored values; software must not pre-invert them.

Operations are `ADD=0`, `SUBTRACT=1`, `MAX=2`, `MIN=3`.  Normal ADD and
SUBTRACT place source in A and destination in B.  REVERSE_SUBTRACT also uses
op 1 but swaps both operands and their API factors.  MIN/MAX place destination
in A, source in B, and force both factors to ONE.

Exact clamped recipes are:

| semantic | A34 | A35 | A36 | A37 | status |
|---|---:|---:|---:|---:|---|
| COPY, including alpha | `34000150` | `35000114` | `36000590` | `37000224` | **live** BGRA |
| canonical source-over | `34000020` | `35000148` | `36000590` | `37000244` | **live** BGRA |
| ONE+ONE ADD | `34000150` | `35000154` | `36000590` | `37000264` | **live** RGB |
| ONE-ONE SUBTRACT | `34000150` | `35004154` | `36000590` | `37004264` | static |
| reverse subtract | `34000550` | `35004054` | `36000990` | `37004164` | static |
| MIN | `34000550` | `3500c054` | `36000990` | `3700c164` | static |
| MAX | `34000550` | `35008054` | `36000990` | `37008164` | static |

The older same-factor source-over alpha words `36000410 37000244` calculate
`As*As + Ad*(1-As)`.  They are valid only when the API requested those alpha
factors; canonical source-over needs `36000590 37000244`.  The live BGRA
result `80807f00` distinguishes them from the stale approximately-`40` alpha.

For the four unproved operations, prefill BGRA `80208040`, draw source
`40c04080`, use ONE/ONE and expect ADD `c0e0c0c0`, SUBTRACT `00a00040`,
REVERSE_SUBTRACT `40004000`, MIN `40204040`, and MAX `80c08080`.  Each is a
separate guarded submission followed by live COPY closure.

## Alpha test

Alpha test remains fixed function.  A00 bit 20 enables the RT0 result for all
render targets, and A33 contains:

```text
A33 = 33000000 | (func << 12) | encode_ref(ref8)
encode_ref(255) = 0400
encode_ref(v)   = (v << 2) | (v >> 6), v < 255
```

Functions are `NEVER..ALWAYS = 0..7`.  For reference `80`, the complete
words are:

| function | A33 | passing source alpha among `40,80,c0` |
|---|---:|---|
| NEVER | `33000202` | none |
| LESS | `33001202` | `40` |
| EQUAL | `33002202` | `80` |
| LEQUAL | `33003202` | `40,80` |
| GREATER | `33004202` | `c0` |
| NOTEQUAL | `33005202` | `40,c0` |
| GEQUAL | `33006202` | `80,c0` |
| ALWAYS | `33007202` | all |

ALWAYS, NEVER, and LESS are **live**, including a LESS case proving killed
fragments suppress D16 writes.  Disable clears A00 bit 20, and every enable
must emit a complete A33 rather than reuse an inherited word.  A
state-complete successor can emit `33007000` (ALWAYS/ref 0) in the otherwise
padding slot while disabled.  A33 consumes an existing alignment slot, so
alpha test changes no body length.

## Depth compare and write control

For the established 32x32 linear SF D16 surface:

```text
A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11004001
A12 = 12380000

NEVER=13000000  LESS=13010000  EQUAL=13020000  LEQUAL=13030000
GREATER=13040000 NOTEQUAL=13050000 GEQUAL=13060000 ALWAYS=13070000
```

A01 controls test and write independently:

```text
disabled:    01c01540
read-only:   01c01548              # bit 3 test, bit 2 clear
test+write:  01c0154c              # bits 3 and 2
```

The API effective write is `depth_test_enable && depth_write_enable`.  Never
set A01 bit 2 while bit 3 is clear.  LESS/write is live.  LEQUAL/write is also
live: equal `000000ff/8000` passed, far retained that value, near produced
`00ff0000/4000`, and MMIO `040c` remained zero on kernel
`8a16b931bacdaa292d81ea30c8c99b7a369b2cad` with Mesa
`b607b9ca59588dad3787d860e84bcacbceeba6fb`.  Read-only and the remaining
functions still need their isolated compare oracles.

The complete compare oracle CPU-fills D16 with `8000` and tests incoming
`4000/8000/c000`.  Expected passing sets are none, `4000`, `8000`,
`4000+8000`, `c000`, `4000+c000`, `8000+c000`, and all for the eight
functions in order.  Reset both color and depth before every row.

For read-only, start at `8000`, draw red at `4000` with LESS/read-only, then
green at `8000` with EQUAL/read-only.  The final pixel is green and the depth
word remains byte-identical `8000`.  This catches an A01 bit-2 leak.

The current depth flag field gives zero its legacy meaning LESS, so it cannot
encode explicit NEVER.  Never silently reinterpret zero.  Either add an
`EXPLICIT_DEPTH_FUNC` presence bit or use the state-complete successor below.

## Polygon offset

The manual equation and exact packing are:

```text
Zbias = max(dZ/dX, dZ/dY) * scale + offset

A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000) | (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

Linux `STM_DepthBias_INV:0x1959d0` copies API factor directly to `scale` and
multiplies API units by a depth-allocation constant before producing
`offset`.  The formerly opaque resource field is now resolved.  At
`0x195a10` it compares surface `+0x158` with 16; independent function
`sfmGetPatchColorKey:0xb6afc..0xb6b52` tests the same field for 8, 16, and 32
and performs byte, word, and dword loads.  Therefore it is bits per pixel:

| depth allocation | units multiplier bits | approximate value |
|---|---:|---:|
| D16, 16 bpp | `38000080` | `3.0518044e-5` |
| D24S8/D24X8/D32, 32 bpp | `34000001` | `1.1920930e-7` |

Fill, line, and point offset enables are independent proprietary context
fields.  The ABI must preserve all three rather than expose one global bit.
For a first hardware discriminator, `offset=-0.25`, `scale=0` is exactly:

```text
16800000 170000be 18000000
19800000 1a800000 1b803fbf
```

Draw red and then coincident green at Z=0.5 with LESS.  Zero offset retains
red; negative offset admits green.  Then emit the zero A16--A18 closure and
repeat.  Do not start at Z=0 because negative-Z clamping can hide the bias.

The UAPI should carry semantic IEEE-754 `scale` and post-format-conversion
`offset` bits, not A16--A18 words.  This follows the existing RHW/S/T float-bit
precedent while avoiding floating point in the kernel.  Validate finite
values and a conservative exponent range, then split them into native words.
Mesa computes `offset = units * format_multiplier` selected from the bound Z
format.  The first kernel implementation should restrict the accepted depth
format and float bit patterns exactly as described in
[polygon-offset-depth-readonly-plan.md](polygon-offset-depth-readonly-plan.md),
rather than pretending it can reconstruct arbitrary API units.  A later ABI
may instead carry units and implement an integer-only IEEE multiply, but raw
register words are never acceptable.

## Stencil functions, operations, and masks

Stencil is valid only with D24S8:

```text
A10 = 10000000 | (zs_sf_va >> 8)
A11 = 11008001                  # 32 pixels * 4 bytes
A12 = 123e0000                  # D24 in [31:8], S8 in [7:0]
A01 bit 4 = stencil enable
```

Physical clockwise and counter-clockwise faces are:

```text
A23 = 23000000 | (cw.ref << 16) | (cw.value_mask << 8) | cw.write_mask
A24 = 24000000 | (cw.func << 16) |
                  (cw.fail << 6) | (cw.zfail << 3) | cw.zpass
A25 = 25000000 | (ccw.ref << 16) | (ccw.value_mask << 8) | ccw.write_mask
A26 = 26000000 | (ccw.func << 16) |
                  (ccw.fail << 6) | (ccw.zfail << 3) | ccw.zpass
```

Compare functions use `NEVER..ALWAYS = 0..7`.  Native operation values are:

```text
KEEP=0 ZERO=1 REPLACE=2 INCRSAT=3 DECRSAT=4
INVERT=5 INCR_WRAP=6 DECR_WRAP=7
```

Gallium's last three values differ.  The exact Gallium-to-hardware lookup is
`{0,1,2,3,4,6,7,5}`; range-check before indexing.  A raw three-bit copy would
exchange INVERT and both wrap operations.

Resolve API front/back to physical screen-space CW/CCW after framebuffer Y
orientation and front-face state.  With one-sided stencil, mirror the active
face into both pairs.  When disabled, clear A01 bit 4 and emit the neutral
A23--A26 block.  Reject stencil with D16.

The first four bounded oracles are:

1. compare mask: stencil `a1/b5`, ref `a5`, value mask `f0`, EQUAL, REPLACE;
   only `a1` passes;
2. write mask: start `3c`, ref `a5`, ALWAYS, write mask `0f`, REPLACE;
   final stencil is `35` and the upper depth 24 bits are unchanged;
3. operations: `INCRSAT(ff)=ff`, `INCR_WRAP(ff)=00`,
   `DECRSAT(00)=00`, `DECR_WRAP(00)=ff`, `INVERT(55)=aa`;
4. Z-fail routing: stencil ALWAYS, fail KEEP, Z-fail REPLACE, pass KEEP,
   force depth failure; only the stencil byte becomes the reference.

Only after these pass should opposite-winding triangles with distinct refs
prove physical face selection.

## Color-mask interactions

A53 low bits are hardware `B,G,R,A = 1,2,4,8`, whereas the public mask is
`R,G,B,A = 1,2,4,8`.  Exact closed words are:

```text
none=53010c00 B=53010c01 G=53010c02 R=53010c04
A=53010c08 RGB=53010c07 RGBA=53010c0f
```

With BGRA destination prefilled `44332211` and COPY source `80a0b0c0`, expect
R `44a02211`, G `4433b011`, B `443322c0`, A `80332211`, RGB `44a0b0c0`, and
none `44332211`.  A-only is live.

A53 masks only the color store; it must not suppress depth or stencil.  Keep
A01 global color enable bit 6 set for ordinary masked rendering and use A53,
including mask NONE.  Prove these interactions before optionally clearing
bit 6 for a dedicated depth/stencil-only path:

1. mask NONE plus depth write at Z=`4000`, followed by full-mask EQUAL at the
   same Z, proves the invisible first draw updated depth;
2. mask NONE plus D24S8 ALWAYS/REPLACE must preserve color while changing
   stencil;
3. alpha-test kill plus enabled depth and stencil must leave all three
   destinations unchanged;
4. blend with A-only and RGB-only masks must preserve every masked channel,
   rather than applying the mask to an equation input.

## Exact packet growth and A33 placement

In the live untextured D16+blend body, indices 19--25 are
A86/A87/A53/A00/A01/A88/A89, indices 26--30 are A10/A11/A12/A13/A20,
index 31 is A33 or padding, and the next header starts at 32.

A depth-bias-complete D16 group replaces indices 26--31 with:

```text
26..29 A10 A11 A12 A13
30..35 A16 A17 A18 A19 A1A A1B
36..38 A1E A1F A20
39     A33 or padding
40     next header
```

That is exactly +8 dwords.  A D24S8/stencil-complete group inserts A23--A26
at 39--42, puts A33/padding at 43, and starts the next header at 44: exactly
+12 dwords over the current body.

For one three-vertex draw, the resulting body lengths are:

| state | no blend | blend |
|---|---:|---:|
| current untextured D16 | 188 | 192 |
| complete bias/clamp D16 | 196 | 200 |
| complete D24S8 + stencil | 200 | 204 |

For current one-texture D16, REPLACE/MODULATE are 228/232 dwords without
blend and 232/236 with blend.  Adding full bias/clamp closure makes those
236/240 and 240/244 respectively.  D16 cannot add the stencil block.

Alpha test itself adds zero dwords in every case because A33 replaces the
final alignment word.  Packet assertions must be updated as a single change;
do not append A33 after the aligned group and accidentally shift every later
shader or immediate-vertex index.

## Ranked ABI successors

The ordering below ranks safety first and ioquake usefulness second.

1. **Depth read-only and complete compare enum.**  Add an explicit-presence
   flag to disambiguate NEVER from legacy zero/LESS, plus a READ_ONLY flag.
   No structure-size or packet-length change.  Promote one function at a time
   after the compare/read-only oracles.  This is the smallest immediate game
   blocker.
2. **Nonconstant blend completion.**  Extend existing factor/function enums;
   the current state already has independent RGB/alpha fields.  No ioctl-size
   or packet-length change.  Gate it behind a new cap and accept only rows
   which passed the operation/factor sweeps.  Keep SRC1 rejected.
3. **Constant blend-color successor.**  Existing blend state has no spare
   color.  Add a versioned state carrying four semantic U1.10 values
   `0..1024`; the kernel places them in A/B factor lanes according to equation
   operand order.  Expand legacy closure to all A38--A3F before enabling it.
   Native packet length is unchanged because the successor already emits all
   eight words.
4. **Polygon-offset successor.**  Add fill/line/point enable bits and validated
   IEEE scale plus post-format offset.  Close A16--A1B and A1E--A20, costing
   exactly eight body dwords.  Initially permit D16 fill only; line/point and
   D24 wait for their respective raster/Z oracles.
5. **D24S8/stencil state-complete successor.**  Add a D24S8 BO binding plus
   normalized physical-CW/CCW compare/op/mask/ref blocks.  Kernel translates
   enums and writes A23--A26; userspace cannot supply words.  This costs four
   more dwords than polygon/depth closure and requires new BO validation,
   format-aware clear/readback, and face-orientation tests, so it is the
   highest-risk state expansion despite its game value.

Alpha test and color mask need no new ABI: all alpha functions already fit
the existing blend state, A33 has a stable slot, and the color mask is live.
Their remaining work is promotion testing and transition coverage.

For every tier, run `new state -> fully disabled neutral state -> new state`
in one context, then a legacy COPY draw.  Verify target and Z/S readback,
every guard, the retirement cookie, CR PAUSE, and engine work bits.  Any
timeout is a quarantine event, not a pixel-test failure.
