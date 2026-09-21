# Chrome9 ioquake3 packed-lightall BGRA-unit1 object

Status: implementation-ready static packet design and exact fixed-Linux/Windows
compiler differential, 2026-09-21.  The two-texture resource path, D16,
blend, alpha test, and the shorter XRGB-unit1 three-factor object have
independent evidence.  This four-ALU object has not yet executed on VX900
silicon, so capability bit 39 must remain unadvertised until the guarded
oracle below retires.

## Closed semantic key

The linked ioquake3 GL2 fast-light fragment shader uses packed coordinates
and separate primary color:

```text
VAR0.xy = texture 0 S,T
VAR0.zw = texture 1 S,T
VAR1    = normalized primary RGBA

RGB = texture0.rgb * texture1.rgb * primary.rgb
A   = texture0.a   * primary.a
```

Texture 1 alpha is deliberately absent.  This is the exact NIR dataflow in
the live GLSL175 and GLSL199 objects recorded in
[ioquake3-gl2-linked-texturecolor-lightall.md](ioquake3-gl2-linked-texturecolor-lightall.md).
It is not the full four-channel product when texture 1 is BGRA.  The shorter
three-ALU object selected by program flag bit 0 is exact for this shader only
when texture 1 is translated as XRGB/A30 format 98 and therefore samples
alpha one.  A BGRA/A30-format-99 texture 1 requires the complete four-ALU
object below.

The classifier must prove, independently per lane:

```text
RGB leaf multiset = {TEX0[c], TEX1[c], VAR1[c]}, c = x,y,z
alpha leaf multiset = {TEX0.w, VAR1.w}
```

Both samples are ordinary, non-array, non-shadow 2D operations on matching
texture/sampler units.  Their coordinates are exactly VAR0.xy and VAR0.zw.
There is no projector in this linked form: VAR0.w is texture-1 T, not Q.
Require finite S/T for both units, but do not require either packed z/w lane
to equal one.  The three RGB FMUL leaves may be reassociated only when both
participating NIR instructions report `nir_alu_instr_no_reassoc() == false`.
The exact integer `u_AlphaTest` tree and its direct constant-buffer-zero load
remain part of the key.

Initially select this object only when unit 1 is BGRA.  Unit 0 may be XRGB or
BGRA: format 98 produces primary alpha, while format 99 produces stored
texture-0 alpha times primary alpha.  The destination may use either already
validated A52 format.  A shader whose alpha includes texture 1, omits texture
0, or is otherwise unobservable is a different key and must not select this
program.

## Indivisible physical program

The historical Linux 5.75 wrapper plus configuration-two finalizer reproduced
this complete object only with its validator/processor callbacks swapped:

```text
A90..A9a  90000000 91000001 92020004 93000002
           94000000 95000000 96000000 97000000
           98000000 99000000 9a000000
TAU        a01001b0 311001b1
ALU        0140b6d0 3003f000 00f00005 000003e0
           0140b6d0 50040055 00703f00 000003e0
           0140b6d0 b0040b6b 008200b6 200003e0
           01c00530 001c0053 00f03f00 000d0820
```

The first record forms the common two-texture value, the second writes its
primary-modulated RGB, the third writes texture-0-alpha times primary alpha,
and the fourth moves the completed temporary to oC0.  The physical ALU-store
SHA-256 is
`d8af75a0c403ff40be0c98d40584f8af9ba5652ed6ead1afa7ae448abce268c3`.

These words are one indivisible fixed allocation and schedule.  In
particular, do not append the third ALU to the current Xorg-scheduled object,
retain its `301001b0 311211b1` TAUs, or change only A92 to four.  A90/A91,
both TAUs, all four ALUs, masks, temporary ids, transition word A93, and the
four-ALU count travel together.

The independently executed Windows D3D compiler emitted four ALUs and two
ordinary virtual TAUs `201001b0 211111b1` for the same semantic program.  Its
finalized virtual-ALU SHA-256 is
`1c179d215b9c7c98728473310a1ad3a6c99a53cd0e7530a5d66ac6612199edf6`.
Those virtual words prove instruction count, operation, masks, and alpha
source, but are not uploadable physical allocation words.

The fixed-object Linux oracle is
`binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
executed through `stmPPSCompile_invi` and
`stmPPS_PostTranslationSetting_invi(..., 2)` with the historical nonproduction
callback order.  The production-callback replacement family is documented in
[`chrome9-production-four-alu-provenance.md`](chrome9-production-four-alu-provenance.md).
The Windows oracle is
`VTGUModeDX32A.dll`, SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
executed under Wine through tokenizer `0x101dbd30`, dispatcher `0x101dca80`,
and finalizer `0x10203530`.  The supplied OpenGL ICD, SHA-256
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`,
has byte-matched backend routines at RVAs `0x244530`, `0x245280`, and
`0x256550`.  Legacy open-source UniChrome
`Mesa-7.11.2/src/mesa/drivers/dri/unichrome/via_texcombine.c:140..225` and
`:285..360` independently supplies the stage-order rule (stage-zero
`GL_PREVIOUS` selects diffuse, later stages select current) and separate RGB
and alpha source selection, but it does not supply these Chrome9 native
words.

## Structured UAPI selector

Capability bit 25 and program flag bit 0 retain their existing full-RGBA
meaning:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_PRIMARY_MODULATE (1ULL << 25)
#define DRM_CHROME9_TWO_TEXTURE_PRIMARY_MODULATE     (1U << 0)
```

Reserve capability bit 39 and program flag bit 1 for the texture-0-alpha
object:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_PRIMARY_TEX0_ALPHA (1ULL << 39)
#define DRM_CHROME9_TWO_TEXTURE_PRIMARY_TEX0_ALPHA     (1U << 1)
```

Both bits reuse commands 0x0a, 0x0d, and 0x13; no structure grows and no
native word enters userspace.  Decode the low two flag bits as an enum:

| bits `[1:0]` | operation | required capability |
|---:|---|---:|
| 0 | existing two-texture full-RGBA product | base command only |
| 1 | three-factor full-RGBA product | 25 |
| 2 | RGB three-factor, alpha TEX0*primary | 39 |
| 3 | invalid; reject | none |

For program flag bit 1 (selector value 2), initially require texture 1 format
BGRA8888.  Texture 0 keeps the existing XRGB/BGRA set.  This deliberately
narrow kernel rule makes every accepted TEX0-alpha request exercise the
reason for the new object; XRGB unit 1 continues to use the shorter program
flag bit 0 object.  Format validation is still independent for texture 0 and
destination.

Command 0x0a accepts only this two-bit program field.  Commands 0x0d and
0x13 additionally retain the existing depth-function bits `[10:8]` and
capability-gated read-only bit 11.  Command 0x13 retains its separate nested
blend flag namespace.  Reject both program bits set, every unknown top-level
bit, missing capability, or a selector-value-2 request whose texture 1 is not
BGRA.
All existing BO distinctness, actual-size, leading/trailing guard, pitch,
filter, wrap, scissor, finite-coordinate, depth, blend, fence, quarantine,
and retirement rules remain unchanged.

The validation masks are therefore exact:

```text
program_mask       = 00000003
cmd-0a allowed     = 00000003
cmd-0d/13 allowed  = 00000003 | DEPTH_FUNC_MASK | DEPTH_READ_ONLY

(flags & program_mask) == 3                  -> EINVAL
(flags & program_mask) == 1 and !cap25       -> EINVAL
(flags & program_mask) == 2 and !cap39       -> EINVAL
(flags & program_mask) == 2 and tex1 != BGRA -> EINVAL
flags & ~allowed                               -> EINVAL
```

The existing depth validator still independently gates LESS/LEQUAL, EQUAL,
ALWAYS, and read-only on their existing capabilities.  The new program field
does not imply any depth or blend capability.

An old kernel rejects bit 1.  A new kernel must not advertise capability 39
until all three packet forms below use the exact four-ALU object and their
body/index assertions pass.

## Exact FE, VS, PS, and packet indices

The immediate vertex record remains ten dwords:

```text
x y z rhw diffuse specular s0 t0 s1 t1
```

Primary color is normalized D1.  Common A01 is `01ce1540` without depth,
`01ce154c` with depth writes, or `01ce1548` with read-only depth.  The FVF
must retain A02 `020041d1`, D3/D4 declarations A04/A05
`04006342/05008442`, resource word A62 `624081ff`, and VS control A60
`60800005`.  The final two VS records remain the existing D3/D4-to-oT0/oT1
routes:

```text
0008c14c 00000000 00000000 0008028f
0009014c 00000000 00000000 0008029f
```

The relevant body indices are exact relative to the current
`chrome9_build_two_textured_triangle()` body start:

| state | cmd 0a, no depth | cmd 0d, D16 | cmd 13, D16+blend |
|---|---:|---:|---:|
| texture 0 image address / A30 | 20 / 25 | 20 / 25 | 24 / 29 |
| texture 1 image address / A30 | 40 / 45 | 40 / 45 | 44 / 49 |
| common A01 | 64 | 64 | 68 |
| depth A10..A13/A20 | absent | 67..71 | 71..75 |
| FVF A00 / A02 | 84 / 86 | 88 / 90 | 92 / 94 |
| FVF A04 / A05 / A62 | 88 / 89 / 92 | 92 / 93 / 96 | 96 / 97 / 100 |
| VS D3 route | 168..171 | 172..175 | 176..179 |
| VS D4 route | 172..175 | 176..179 | 180..183 |
| VS A60 | 180 | 184 | 188 |
| PS A90 / A91 / A92 / A93 | 188/189/190/191 | 192/193/194/195 | 196/197/198/199 |
| PS A9a | 198 | 202 | 206 |
| TAU payload | 208..211 | 212..215 | 216..219 |
| ALU H2 header | 212..215 | 216..219 | 220..223 |
| four ALUs | 216..231 | 220..235 | 224..239 |
| program CC/DD | 232..233 | 236..237 | 240..241 |
| alignment poison | 234..235 | 238..239 | 242..243 |
| immediate H2 header | 236..239 | 240..243 | 244..247 |
| three vertices | 240..269 | 244..273 | 248..277 |
| complete body | 270 | 274 | 278 |

Command 0x13 additionally keeps destination A52 at body 6 and alpha-test A33
at body 19.  Its texture-1 A30 at body 49 must be `30990000` for this first
key.  The PS assertions must cover every control word, both TAUs, all sixteen
ALU dwords, both CC/DD words, both alignment words, the following immediate
header, and first/last vertex payload.  A four-ALU packet accepted by a
three-ALU length assertion is not safe.

The general size is `270 + 12*H + B + D`, where `H` is a future mip-table
count, `B=4` for the command-13 closed destination/blend envelope, and `D=4`
for D16.  Current level-zero commands use `H=0`; therefore the only admitted
sizes are the three rows above.

## Guarded silicon sequence

1. Submit command 0x0a to an XRGB destination.  Unit 0 is BGRA logical RGBA
   `(128,64,255,128)`, unit 1 is BGRA `(128,255,64,0)`, and primary is
   `(255,128,128,128)`.  Expected logical RGBA is approximately
   `(64,32,32,64)`, packed `0x40402020`.  The three-ALU full product instead
   produces alpha zero, so this is a direct program discriminator.
2. Change only unit-1 stored alpha from `00` to `ff`; output must remain
   `0x40402020`.  Change unit-0 alpha to `ff`; output alpha must become about
   `80`.  This proves the selected alpha leaf is texture 0, not texture 1 or
   primary alone.
3. Repeat through command 0x0d with D16 LESS then LEQUAL, first write and then
   read-only.  Verify accepted/rejected depth pixels and byte-identical D16
   in the read-only pass.
4. Repeat through command 0x13 into BGRA with source-alpha-observing blend,
   then with alpha modes 1, 2, and 3.  Killed fragments must preserve color
   and depth.  This prevents an XRGB target from hiding wrong fragment alpha.
5. Run flag1 -> legacy two-factor -> flag0 -> flag1 without an engine reset.
   The first and last flag1 outputs must match.  After every job verify all
   four BO guards and source bytes, retirement cookie, CR PAUSE, work-mask
   clear, and error registers.

Negative cases cover both program bits, missing capability 39, XRGB unit 1
under flag1, swapped unit coordinates, non-finite S/T, any Q interpretation
of VAR0.w, source/target/depth aliasing, malformed guards, short objects, and
every unknown flag.  Rejection occurs before BO mapping or packet emission.

## Exact next runtime boundary

The live capture
`/root/tmp/chrome9/ioquake3-gl2-semantic-nextdump-v1.log` binds only two
unsupported fragment objects after `UnnamedPlayer entered the game`:

| pointer in capture | source name/hash | runtime binds | semantic |
|---|---|---:|---|
| `0x55a382684480` | GLSL175, `1478d42b ... f291dc37` | 124 | packed lightall key above |
| `0x55a3839f2e70` | GLSL199, `82b0eeb5 ... 9d297fa9` | 93 | byte-semantic-identical packed lightall key |

They alternate immediately after the first world bind.  One classifier and
one native object therefore close both; pointer or GLSL source name must not
be part of the key.

The complete `source_blake3` vectors are:

```text
GLSL175 1478d42b 1a41c331 6700f05a 4cf4983b
        d59174c5 d23b3d1f f924acd4 f291dc37
GLSL199 82b0eeb5 beb816bc 55233f66 a0bcfd1d
        71e1f3a8 5387e207 ef9ba26f 9d297fa9
```

From the first post-load runtime bind through the end of the captured
interval there are 1,304 fragment binds: 217 are those two unsupported
lightall objects, 1,025 are already-supported packed generic objects
(`program=23` in that build), 61 are already-supported constant-color
objects (`program=6`), and one is the already-supported texturecolor object
(`program=18`).  No third unsupported fragment pointer appears.  The first
binds after the initial lightall cluster return to the packed generic family;
their exact shader is the existing one-sample TEX0*primary plus the same
integer alpha-test tree.

Consequently there is no evidence for another fragment-program addition
immediately after this one.  Once capability 39 executes, the next live run
must capture draw-state rejection reasons and a fresh bind trace.  The next
blocker may be depth/blend/layout/state composition rather than a new shader;
do not preemptively admit fog, default seven-sampler lightall, or arbitrary
ALU on the strength of this trace.
