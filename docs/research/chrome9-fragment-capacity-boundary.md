# Chrome9 fragment instruction and packet capacity boundary

## Result

Raising the Gallium fragment `max_instructions` claim from 32 to **96** is
capacity-safe with the current semantic ABI, Mesa arrays, kernel compiler,
64-KiB staging area, and persistent 64-KiB command ring.  Ninety-six is the
largest simple total semantic-instruction claim which remains safe for every
ALU/TEX mixture: every ALU consumes one native ALU record and every computed
TEX can add one native ALU bridge, so the native ALU high-water is no greater
than the total semantic instruction count.

The UAPI's 128 semantic-record storage is not an unconditional
128-instruction shader claim.  A legal 128-record stream must still satisfy:

```text
semantic ALU <= 96
semantic TAU <= 32
semantic ALU + computed-coordinate TEX bridges <= 96
physical live temporaries <= 12
```

Thus `96 ALU + 32 computed TEX` is deliberately rejected rather than encoded
as 128 native ALUs.  A finalized program containing the native maxima of 96
ALU and 32 TAU records does fit all packet and ring storage, however.  The
full worst-case fragment draw, including 32 constants, eight mipmapped texture
descriptors, eight float4 varyings, depth/blend state, and 128 triangles,
copies **63,376 bytes** into the 65,536-byte ring and leaves **1,264 bytes** at
the wrap append point.

This is a storage and bounds result.  It does not claim that every shader
under these scalar limits is allocatable in twelve physical temporaries or
that every varying/TAU topology is already execution-qualified.

## UAPI and kernel compiler bounds

The version-one UAPI limits are in `include/uapi/drm/chrome9_drm.h:959..966`:

```text
ALU records          96
TAU records          32
semantic records    128
constant vec4s       32
textures              8
varyings              8
triangles           128
texture levels       12
```

Each semantic record is fixed at 32 bytes and each full vertex at 152 bytes;
kernel compile-time assertions cover those layouts at
`chrome9_engine.c:265..274`.  The ioctl rejects counts above every UAPI limit
before copying user arrays (`chrome9_engine.c:7296..7324`) and uses overflow-
checked array copies for instructions, constants, textures, and vertices
(`:7355..7399`).

The compiler state has arrays indexed at their exact maxima:

- 128 source-count and per-instruction coordinate entries;
- 128 virtual temporary masks/maps;
- 96 ALU position-slot entries;
- eight varying/resource entries; and
- the finalized `chrome9_fragment_program` has `alu[96][4]`, `tau[32]`,
  `constants[32][4]`, and twelve control words.

See `chrome9_fragment.c:12..43` and `chrome9_fragment.h:9..24`.  Validation
checks 128 total records, 96 semantic ALUs, and 32 TAUs before encoding
(`chrome9_fragment.c:181..270`).  It separately counts computed-coordinate
bridges and rejects `alu_count + bridge_count > 96` at `:319..341`.  This
check occurs before the encoder indexes `program->alu`.

The control maps also cover the full stores.  TAU-to-ALU transitions use
control words A93/A94 (`controls[3 + tau/24]`) and ALU-to-TAU transitions use
A95 through A98 (`controls[5 + alu/24]`), so TAU index 31 and ALU index 95
remain inside `controls[12]`; see `chrome9_fragment.c:917..1031`.

### Why a total semantic cap of 96 composes

Let:

```text
A = ordinary semantic ALU instructions
T = semantic TEX instructions
K = the optional semantic KILL instruction
C = computed-coordinate TEX instructions, C <= T
N = A + T + K
```

KILL owns one native ALU bridge and one native TAU record, but it is already
counted once in `N`.  The finalized resource counts are therefore:

```text
native ALU = A + K + C <= A + K + T = N
native TAU = T + K <= 32              (independent validator bound)
```

Consequently `N <= 96` guarantees the 96-record native ALU store for every
direct/computed coordinate mixture.  In contrast, merely satisfying
`N <= 128`, `A + K <= 96`, and `T + K <= 32` does not suffice: computed TEX
bridges can still exceed the native ALU store, which is why the kernel has the
additional bridge check.

## Mesa-side storage

Mesa uses the same maxima in
`src/gallium/drivers/chrome9/chrome9_shader_offline.h:9..12`:

```text
CHROME9_PS_MAX_ALU          96
CHROME9_PS_MAX_TAU          32
CHROME9_PS_MAX_INSTRUCTIONS 128
CHROME9_PS_MAX_VIRTUAL_TEMPS 128
```

The semantic IR owns 128 instructions and the offline native object owns
`alu[96][4]`, `tau[32]`, and twelve controls (`:114..137`).  The NIR builder
has 128 value entries, checks total/ALU/TAU/temp counts before appending, and
uses an eight-bit count/index only for ranges no greater than 128
(`chrome9_context.c:2535..2562`, `:3091..3093`, `:3324..3329`).  A `uint8_t`
can represent the terminal count 128; loops promote it before arithmetic.

The ioctl translation array is exactly 128 records and rejects a larger IR
before conversion (`chrome9_fragment.c:269..282`).  The draw renderer owns
fixed arrays for 128 instructions, 32 constants, and eight textures
(`chrome9_draw.c:59..66`, `:1140..1146`).  Its production generic batch is
only 16 triangles (`chrome9_draw.c:31`, `:1331..1336`), substantially below
the kernel packet's audited 128-triangle maximum.

The API-facing fields currently remain conservative at
`max_instructions=max_alu_instructions=32` and `max_temps=8`
(`chrome9_screen.c:326..333`).  The state tracker copies these values directly
to the GL program limits (`st_extensions.c:206..229`).  A bounded promotion
for refract is:

```text
max_instructions     = 96
max_alu_instructions = 96
```

The existing sampler qualification can retain its smaller
`max_tex_instructions` and sampler limits.  Raising `max_temps` is a separate
claim and is not required by the instruction-store calculation.

## Counted H3 program payload

`chrome9_emit_header3()` aligns the cursor to 16 bytes and writes a four-dword
header followed by exactly the supplied payload count
(`chrome9_engine.c:464..478`).  At the finalized maxima:

| Payload | Data dwords | Header dwords | Transaction bytes |
|---|---:|---:|---:|
| 96 four-dword ALU records | 384 | 4 | 1,552 |
| 32 TAU records | 32 | 4 | 144 |
| constants c0..c15 | 64 | 4 | 272 |
| constants c16..c31 | 64 | 4 | 272 |

The builder emits ALU first with count `alu_count * 4`, then TAU with count
`tau_count` (`chrome9_engine.c:2157..2164`).  Both maximum counts are ordinary
H3 payload counts and fit without truncation.  The two constant banks are
separate H3 transactions at `:2134..2143`.

## Exact 64-KiB staging calculation

The packet builder uses a 64-KiB zeroed staging allocation
(`CHROME9_RUN_STAGING_SIZE` at `chrome9_engine.c:40`, allocated at
`:3743..3751`).  The following bound follows the exact order and 16-byte
header alignment in `chrome9_build_fragment_program()` (`:2089..2197`).
It deliberately selects every size-increasing option: blend destination
state, depth plus stencil state, eight descriptors with mip packets, 32
constants, eight varyings, MISC location state, 96 native ALUs, 32 TAUs, and
128 triangles.

| Region | Dwords | Bytes | Derivation |
|---|---:|---:|---|
| initial sync + destination | 28 | 112 | H0(2), alignment, H2(16) |
| 8 texture image/mip/sampler groups | 256 | 1,024 | 8 × (10 + 2-pad + 12 + 8) |
| common/config/raster | 83 | 332 | H2(25), padding, H2(8), H2(35) |
| two constant banks | 137 | 548 | one alignment dword + 68 + 68 |
| VS constants/clip/program/control | 85 | 340 | maximum 8-varying VS program |
| MISC + 12 PS controls | 27 | 108 | includes header alignment |
| 96 ALU + 32 TAU H3 transactions | 424 | 1,696 | 388 + 36 dwords |
| state through TAU | **1,040** | **4,160** | cumulative |
| raw vertex header | 4 | 16 | aligned H2 header |
| 128 × 3 full vertices | 14,592 | 58,368 | 384 × (6 + 8×4) dwords |
| sync/status/null/cookie tail | 26 | 104 | includes status alignment |
| before ring trailer | **15,662** | **62,648** | cumulative |

`chrome9_finish_ring_sized()` requires the pre-trailer cursor to be at most
`capacity - CHROME9_CR_READ_AHEAD`, or 64,992 bytes, so the worst packet has
2,344 bytes of pre-trailer margin (`chrome9_engine.c:481..517`).  Its exact
alignment results are:

```text
PID00 pause offset       62,704
PID10 stop offset        62,832
copied read-ahead closure 62,832 + 0x220 = 63,376 bytes
64-KiB ring headroom      65,536 - 63,376 = 2,160 bytes
wrap append headroom      65,536 - 0x380 - 63,376 = 1,264 bytes
```

`chrome9_append_segment()` overflow-checks `stop + 0x220`, rejects a copy
larger than either the staging source or ring, and on wrap requires the copy
to fit after `CHROME9_WRAP_APPEND=0x380`
(`chrome9_engine.c:2610..2643`).  The exact worst case passes all three
checks.  The wrapped ring window is the first storage limit: with all other
maxima unchanged, 130 triangles produce a 64,272-byte copy and still fit the
64,640-byte window, while 131 triangles produce 64,784 bytes and do not.
Retaining the public 128-triangle cap leaves a two-triangle alignment margin.
Mesa's normal 16-triangle batch uses only 7,296 vertex bytes at the same
maximal varying width, leaving much larger operational margin.

## Promotion gate

The smallest defensible change is to raise only the two Gallium instruction
claims to 96 and add offline tests which construct:

1. 96 total semantic instructions with every TEX coordinate computed, proving
   the native ALU count never exceeds 96;
2. a valid finalized 96-ALU/32-TAU program, proving H3 counts and control-word
   endpoints; and
3. the exact 128-triangle maximum packet, asserting the offsets
   `pause=62704`, `stop=62832`, and copied length `63376`.

Keep 128 as an internal semantic-array bound, not the advertised generic
instruction limit.  Any future increase in varyings, triangle count, texture
state size, or program records must update the packet-size proof before the
unchecked emit helpers write into staging.
