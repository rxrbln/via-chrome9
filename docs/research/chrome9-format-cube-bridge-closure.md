# Chrome9 legacy format, cube image, and TEMP-coordinate closure

This note turns three independently useful pieces of the Chrome9 texture path
into bounded implementation contracts: legacy one- and two-byte image formats,
six-face cube image state, and the fragment compiler's shared varying/coordinate
namespace.  The measured corpus is
`chrome9-format-cube-bridge-corpus.json`; run
`tools/re/chrome9_format_cube_bridge_check.py` from the source root.

This is future generic texture support.  The current glmark2 `texture` scene is
a 2D image drawn on cube geometry, not a cube-map texture, and must not be used
as proof of the cube-map rows below.

## Production evidence

The exact binaries and SHA-256 identities are recorded in the corpus.  The
generation-specific format mappings come from:

- Windows D3D public/private format translation at
  `VTGUModeDX32A:0x100c7590`, sampler lookup at `0x1020a660`, and render-target
  lookup at `0x1020a4c0..0x1020a57d`;
- Windows OpenGL sampler lookup at `VTGOGL32A:0x1025d590` and render-target
  lookup at `0x1025d3f0..0x1025d4ad`;
- Linux OpenGL render-target lookup
  `stmLookupRenderTargetFormat_invi:0x1a6c20..0x1a6d53` and image builder
  `stmHWPrepareTextureRegs_invi:0x1b22f0`; and
- old Mesa `via_tex.c:449..473` plus `via_3d_reg.h:970..1010` as independent
  older-generation semantic corroboration.

The Windows D3D and OpenGL tables agree byte for byte on these rows.  A8 and L8
occur only in their sampler lookup.  L8A8 and RGB565 occur in both sampler and
render-target lookups.  This distinction is a hard bind-class rule, not a
missing table entry to infer.

## Exact sampler and render-target words

| logical view | bytes/texel | A30 image word | sampled RGBA | A52 at pitch 64 |
|---|---:|---:|---|---:|
| A8 | 1 | `301b0000` | `0,0,0,A` | unsupported |
| L8 | 1 | `30130000` | `L,L,L,1` | unsupported |
| L8A8 | 2 | `30150000` | `L,L,L,A` | `52150002` |
| RGB565 | 2 | `30890000` | `R,G,B,1` | `52890002` |

L8A8 memory is little-endian `AAAA AAAA LLLL LLLL`: L is the first byte and A
the second.  RGB565 is numeric `RRRRRGGG GGGBBBBB`; the corresponding Gallium
view is `PIPE_FORMAT_B5G6R5_UNORM`.  Missing alpha is exactly one.  A8's color
components are exactly zero.  Do not add shader-side legacy swizzles on top of
these decoder semantics; only an arbitrary sampler-view swizzle belongs in the
shader.

For a canonical linear level:

```text
pitch   = ALIGN(width * cpp, 32)
covered = pitch * ALIGN(height, 8)
base    = 256-byte aligned
```

There is no independent pitch register in this path.  Retain the logical A20
width only when the pitch is canonical.  At 32x32, A8/L8 have pitch 32 and
covered size `0x400`; L8A8/RGB565 have pitch 64 and covered size `0x800`.  All
four use `A20=20020020`, `A21=21000001`, `A22=22008055`; only A30 and the
kernel-derived byte geometry differ.

Linear packed render targets use:

```text
A50 = 50000000 | (sf_address >> 8)
A51 = 51100000
A52 = 52000000 | (format_byte << 16) | (byte_pitch >> 5)
A53 = 53010c0f
```

The static format lookup alone is not a live render-to-texture proof.  RGB565
is the safest first packed target because destination alpha is implicitly one.
For L8A8, only gray output (`R=G=B=L`) is initially sound: the binaries prove
the stored bit layout but not which unequal RGB component is reduced to L.
A8/L8 must remain sample-only.

The corpus fixes the first sampler readback values for a BGRA8888 destination:

| A30 | source quadrants | expected packed BGRA |
|---:|---|---|
| `301b0000` | `00,55,aa,ff` | `00000000,55000000,aa000000,ff000000` |
| `30130000` | `00,55,aa,ff` | `ff000000,ff555555,ffaaaaaa,ffffffff` |
| `30150000` | `0000,ff00,0080,ffff` | `00000000,ff000000,00808080,ffffffff` |
| `30890000` | `f800,07e0,001f,ffff` | `ffff0000,ff00ff00,ff0000ff,ffffffff` |

Use the already bounded one-texture REPLACE program, nearest/clamp, four guards,
and no blend/depth for this gate.  Then test LINEAR and REPEAT separately.

## Exact cube image state

Linux `stmHWPrepareTextureRegs_invi` resolves two manual label errors and packs
the six A18 location fields as `[1:0]`, `[3:2]`, `[5:4]`, `[7:6]`, `[15:14]`,
and `[17:16]`.  Domain value one is SF.  Bit 12 makes mip-table entries offsets
relative to the selected face root, and `[11:8]=3` selects cube shape.  The
complete six-SF word is therefore `18015355`.

The builder writes six face L0 addresses divided by 256 to A00--A05 and creates
one shared subtype-10 L1--L8 table from face zero.  Every face consequently has
the same dimensions, format, level count, and relative level layout.  The
bounded 32x32 XRGB image group is:

```text
FE020440 0000043C 00020000 CC000000
00AAAAAA 01BBBBBB 02CCCCCC 03DDDDDD
04EEEEEE 05FFFFFF 18015355 20020020
21000001 22008055 30980000 CC000000
```

Every `AAAAAA..FFFFFF` is a separately relocated face address shifted right by
eight.  `stmGetCubeFaceRoot:0xee2d0` and
`stmGetCubeFaceLevelNext:0xee350` are the production walkers.  The allocator's
single-object face-stride fields are not sufficiently identified to expose a
user offset contract.  The first safe successor should therefore take six GEM
handles, derive and validate identical layouts in the kernel, pin all six for
the whole job, and insert six relocations.  No raw face offset or stride enters
the UAPI.

Windows OpenGL `TexImage2D` at `0x10148ab0` maps consecutive targets without a
permutation:

```text
A00 8515 +X    A01 8516 -X    A02 8517 +Y
A03 8518 -Y    A04 8519 +Z    A05 851a -Z
```

The upload path does not rotate or mirror rows.  This is strong static evidence
for native GL face orientation, but not silicon proof: a hidden sampler
coordinate transform remains possible.  Gate exposure on one nearest test
where all six faces and all four corners have unique colors.

The manual-derived coordinate tuple for texture 0 is
`A01=01002522`, `A08=08010201`: perspective mode one, source texture A,
three-dimensional STR, and one texture-coordinate register.  This follows the
manual fields exactly, but no retained executable cube general-state packet or
live result independently confirms it.  Keep it behind the cube capability and
orientation gate.  Do not substitute four-dimensional/projective Q state.

The fragment lookup itself is now production-exact.  Linux OpenGL
`__glEnableDisable:0x28d5fc..0x28e010` handles public
`GL_TEXTURE_CUBE_MAP=8513`, stores its enable at per-unit byte `+0x6023`, and
calls `__glSetTexEnableDimension`.  That function selects internal dimension
four at `0x28c5ab..0x28c5bb`.  The fixed-function program builder then:

- maps dimension four to a four-lane `Tn.xyzw` declaration through the exact
  table `via_chrome9_dri.so:0x3f8e60 = 00010000,00030000,00070000,000f0000`;
- recognizes key four at
  `__glDpGenerateTexStageBlendMSToken:0x206620..0x20662e`; and
- emits source token `03010042` (TEXLDP) instead of ordinary `03000042`
  (TEXLD).

Compiling otherwise identical `TEX(s0,T0); MOV oC0` objects gives:

| production backend | TEXLD TAU | cube/TEXLDP TAU | stable delta |
|---|---:|---:|---:|
| Linux 5.75 | `301001b0` | `307001b0` | `00600000` |
| Windows D3D | `201001b0` | `207001b0` | `00600000` |

The Windows result was generated headlessly from the exact DLL with the
private tokenizer/compiler/finalizer wrapper; token and output hashes are in
the corpus.  Bit 28 differs only because the Windows wrapper has a synthetic
register manager and does not mark this direct coordinate's last use.  Both
backends independently prove TAU operation field `[22:20]`: one is ordinary
TEXLD and seven is the TEXLDP form selected by the cube fixed-function path.
The image A18 shape still distinguishes cube face selection from a projected
2D lookup.  A first cube implementation must therefore emit operation seven,
not reuse the current ordinary `...1...` TAU template.

## Shared varying/coordinate allocation

Three fresh Linux 5.75 production compiler objects close an allocator ambiguity:

| source | active tuple | A90 | A91 | TAU |
|---|---|---:|---:|---:|
| `MOV oC0,T0` | `(1,0,1,0)` | `90058001` | `91000000` | none |
| `TEX(s0,T0); MOV oC0` | `(1,1,1,1)` | `90018000` | `91000001` | `301001b0` |
| `TEX(s0,T1); MUL oC0,r0,T0` | `(2,1,2,1)` | `90018001` | `91000002` | `301011b0` |

Thus direct ALU and TAU varying references share one persistent coordinate
namespace.  For the mixed case, ALU T0 reserves slot zero and TAU T1 uses slot
one.  A91 marks slots actually read by TAU, while A90's low varying bits mark
direct ALU references not also consumed by TAU.  The implementation rule is:

1. collect the union of logical varying IDs referenced by ALU or direct TAU;
2. sort by logical ID and assign dense persistent coordinate slots;
3. encode each direct TAU with its assigned slot and set that slot in A91;
4. set A90 low bits for direct ALU varying references excluded from the direct
   TAU set; and
5. allocate computed TEMP-coordinate bridge slots only after the persistent
   union, using the proven circular lifetime allocator.

Two sparse production rows prove that this is physical compaction, not merely
slot reservation.  `MOV oC0,T5` compiles identically to `MOV oC0,T0`: logical
usage metadata is bit 5, but the ALU source and A90 use physical slot/bit zero.
`TEX(s0,T0); MUL oC0,r0,T5` compacts union `{T0,T5}` to slots `{0,1}`: TAU is
`301001b0`, the ALU source ID is one (`...302c1053...`), A90 is `90018002`, and
A91 is `91000001`.  Therefore every direct ALU varying source must be rewritten
through the same logical-to-physical map used by TAU.

The current kernel fragment allocator only discovers direct coordinates while
walking TEX instructions, and `chrome9_fragment_source_id()` returns a varying's
logical index unchanged.  It would both compact T1 incorrectly in the first
mixed case and encode sparse ALU T5 as physical five in the second.  Fix union
allocation and ALU source rewriting before admitting arbitrary packed
varying-plus-texture shaders.

The exact computed-coordinate object remains:

```text
# r0.xy=c0.xy+c1.xy
008106d0 d0140924 00308006 00000020
# full TEMP0 -> coordinate slot 0 bridge
01c00530 c01c0053 00f20006 000d0420
# sampled TEMP1 -> oC0
01c00530 301c0053 00f20105 000d0820
TAU 301101b0
A90..A96 90058000 91000001 92010003 93000001 94000000 95000002 96000000
```

For coordinate slot `j`, the bridge destination is
`000d0420 | (j << 6)`.  A95 bit 1 transitions ALU to TAU after the bridge;
A93 bit 0 transitions back after the TAU.  TAU bit 28 is set on the last read
of that physical coordinate slot.  This object is production-compiler exact,
not yet a silicon result; the kernel must continue rebuilding every native word
from structured instructions.

The combined computed-coordinate plus cube/TEXLDP differential is also closed.
Replacing only the source TEXLD token with TEXLDP leaves all three Linux ALU
records and A90--A9B byte-identical and changes only TAU
`301101b0 -> 307101b0`.  The Windows compiler independently produces
`207101b0`; again, only its synthetic-wrapper last-use bit differs.  Therefore
TEMP-coordinate scheduling needs no cube-specific bridge or phase transition:
use the ordinary full TEMP-to-coordinate bridge, then select TAU operation
field seven.  The token SHA and Windows TAU SHA in the corpus prevent this
combined path from being inferred merely by ORing undocumented bits.

## Smallest safe implementation order

1. Add Mesa resource cpp/layout helpers and sampler-only A8 first; its kernel
   enum and A30 lowering already exist, but capability discovery and Mesa
   format support do not.
2. Add normalized L8, L8A8, and RGB565 sampler enums under one versioned
   capability.  Kernel validation derives cpp and canonical per-level pitch;
   userspace supplies handles and structured level descriptions only.
3. Enable packed render targets under a separate capability: RGB565 first,
   L8A8 gray-only until the reduction rule is measured.  Never infer A52 from
   A30 and never advertise A8/L8 render binding.
4. Correct the shared direct-varying allocator before dependent-coordinate
   shaders are generalized.  Retain the exact control-map checker and fail
   closed above the proven coordinate/temp limits.
5. Add cube as a separate six-handle successor only after 2D format gates pass.
   Require identical faces, derive all mip offsets, and run the asymmetric
   orientation oracle before reporting cube-map support.

For every transition, close unused image roots, A18 shape/location fields,
subtype-10 tables, sampler words, A90--A9B controls, and program records.  A
cube-to-2D draw must restore A18=`18001101`, emit only A00, and clear A01--A05;
a computed-coordinate draw followed by a direct draw must rebuild A90/A91 and
all phase bitmaps rather than retaining the prior object.
