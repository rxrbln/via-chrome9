# Chrome9 native pixel-shader upload transaction and safe ABI boundary

## Result

The Linux 5.75 UMD and both 2015 Windows UMDs agree on the complete native
pixel-shader transaction:

```text
type-01 H2: complete A90..A9b control object
type-03 H3: ALU_count * 4 dwords at selector 20030000, if nonempty
type-03 H3: TAU_count dwords at selector 21030000, if nonempty
```

Pixel constants are separate counted H3 stores at selectors rooted at
`22030000` and `22030080`; they must precede the draw which consumes them.
The upload order ALU-before-TAU is independent of execution order.  A90 bit
18 selects the first execution store and A93..A9a switch between the two
stores after particular records.  There is no executable END instruction.

The Mesa offline `struct chrome9_ps_program` contains exactly enough data to
identify a closed, compiler-finalized program object, but it is not suitable
as a kernel ABI and is not yet sufficient to validate arbitrary native code.
The safe first ABI is semantic: userspace supplies a stable program key,
job-local constants, and structured draw resources; the kernel regenerates a
byte-exact allowlisted object and emits its controls/stores.  Raw A90..A9b,
ALU, TAU, selectors, counts, and transition maps do not cross UAPI.

The smallest new silicon discriminator is an ALU-only `MOV oC0,c0` inside the
already-proven no-texture Xorg H6 envelope.  First run the six-dword derivative
which changes only c0.x/c0.w and the ALU.  Then qualify the real compiler packet form
with a counted H3 c0 store, all twelve A90..A9b words, and a counted H3 ALU
store.  The latter shortens the 188-dword Xorg body to 184 dwords because the
full twelve-word control object needs no Xorg `CC/DD` compatibility tail.

The local hardware-independent corpus was rebuilt during this audit and
reported:

```text
Chrome9 offline shader corpus: 15 PS + 8 VS templates and verifier passed
```

## Three-driver provenance

The store builders are independently present at these recovered locations:

| role | Linux 5.75 | Windows D3D UMD | Windows OpenGL ICD |
|---|---:|---:|---:|
| PS control/ALU/TAU update | `stmUpdatePS_inv:0x0017f710` | `0x1015b6d4..0x1015b909` | `0x10205df4..0x10206029` |
| PS user constants bank 0/1 | sparse and full paths at `0x0017ffc5..0x0018117d` | `0x1015c163..0x1015c21a` | `0x10206883..0x1020693a` |
| compiler post-finalizer | `stmPPS_PostTranslationSetting_invi(...,2)` | shared backend copy | shared backend copy |

The Windows builder emits the following literal H2 control header before
twelve control words:

```text
FE020440 0000043c 00010000 cc000000
90xxxxxx 91xxxxxx 92xxxxxx 93xxxxxx
94xxxxxx 95xxxxxx 96xxxxxx 97xxxxxx
98xxxxxx 99xxxxxx 9axxxxxx 9bxxxxxx
```

For each nonempty native store it pads the next header to a 16-byte boundary
and emits:

```text
FE03043c (ALU_count * 4) 20030000 cc000000
<ALU_count records, four dwords each>

FE03043c TAU_count       21030000 cc000000
<TAU_count records, one dword each>
```

The Linux symbolized implementation is field-for-field identical.  The
Windows D3D and OpenGL builders independently use the same `2003`, `2103`,
and `2203` selector families, ruling out a Linux-only convention.

The existing silicon-proven fixed texture streams use H2 for TAU and ALU and
place TAU before ALU.  That is a separate complete vendor oracle.  H2 and H3
must not be mixed casually: the generic compiler path uses the counted H3
form and ALU-before-TAU order above, while fixed paths retain their exact
Xorg packet order until the generic transaction is qualified.

## Exact control-object construction

The complete compiler-object mapping is:

| control | source | closed meaning |
|---:|---|---|
| A90 | `object+0x2c OR object+0x6c` | bit 18 selects ALU-first; remaining resource/finalizer fields are template-owned |
| A91 | `object+0x70` | finalizer/runtime dependency state; exact per template, not generally decoded |
| A92 | `object+0x18 OR (object+0x24 << 16)` | ALU count low 16, TAU count high 16 |
| A93,A94 | `object+0x60,+0x64` | TAU-to-ALU transition bitmap |
| A95..A9a | `object+0x48..+0x5c` | ALU-to-TAU transition bitmap |
| A9b | low 24 bits of context runtime state | kernel-owned; zero in initial canonical objects |

Each transition word carries 24 positions.  A93 covers TAU 0..23 and A94
covers 24..31.  A95..A9a cover ALU 0..95.  A set bit switches to the other
store after the indexed record.  The compiler appenders establish the
algorithm precisely:

```text
phase 0 = empty, phase 1 = ALU, phase 2 = TAU
first ALU: set A90.bit18
first TAU: clear A90.bit18
append TAU while in ALU: set A2T[ALU_count - 1]
append ALU while in TAU: set T2A[TAU_count - 1]
```

The mandatory configuration-two finalizer modifies more than controls.  For
one `TEXLD` followed by an ALU it changes semantic TAU `201001b0` to finalized
`301001b0`, sets a dependency field in the consuming ALU, changes A90 from
zero to `90018000`, sets A91 to `91000001`, and retains T2A bit zero.  Two
independent samples and a MUL instead finalize as:

```text
A90..A95 90018000 91000003 92020001 93000002 94000000 95000000
TAU       301001b0 311111b1
ALU       0140b6d0 30201053 00f20005 00000820
```

Therefore neither transition words nor TAU dependency bits can be rebuilt by
a constant OR.  Controls, finalized ALUs, finalized TAUs, counts, and phase
order are one indivisible program object.

The first three compiler-exact schedule shapes are:

```text
# ALU-only, N canonical records
A90=90058000 A91=91000000 A92=920000NN, all maps zero

# one TAU followed by N canonical ALUs
A90=90018000 A91=91000001 A92=920100NN A93=93000001

# two independent TAUs followed by one ALU
A90=90018000 A91=91000003 A92=92020001 A93=93000002
```

The former `90000000/91000001`, `a01001b0 311001b1`, and `3003f000`
combination reproduces only with the probe's swapped non-production callback
order.  It remains valid where cited as a fixed, live Xorg object; it is not a
generic production-compiler transaction.

These are safe only for the exact closed records which produced them.  For
example, another ALU-only fog object has a different A90 low resource bit.
Schedule shape and counts alone do not derive all of A90/A91.

## Constant stores and selector boundaries

PS 2.0 constants are unmodified IEEE-754 dwords.  API constants are split by
hardware-owned gaps:

```text
c0..c15  -> native ids  0..15 -> selector 22030000 + first_dword
c16..c31 -> native ids 32..47 -> selector 22030080 + (first_dword - 64)
```

The exact H3 form is:

```text
FE03043c payload_dwords selector cc000000
<payload>
```

A dirty run crossing c15/c16 must become two packets.  The conservative full
upload is two 64-dword packets at `22030000` and `22030080`.  Never expose or
coalesce across internal selectors `22030040/5c/c0/dc`.  Never accept c32+:
c32..c47 collide with the second internal/gap range and c48..c63 alias the
first user bank.

Constants precede A90..A9b and the program stores in the initial kernel
transaction.  Every program change must overwrite every constant it consumes,
including driver-owned constants such as RGBX c0.x=1.  Persistent set-then-
draw constant state is not a safe multi-file ABI.

## Required surrounding draw state

A valid PS object alone is not executable state.  A self-contained draw must
emit or prove this order before primitive launch:

1. destination format/base/pitch, color mask, blend and depth/alpha state;
2. every referenced texture image and sampler block;
3. producer retirement and the subtype-FE texture-cache clear when a rendered
   BO becomes a sampler source;
4. type-04 declaration, raster FVF, VS-output/resource linkage, and primitive
   state compatible with every consumed PS varying/TAU coordinate;
5. PS user constants at `2203xxxx`;
6. VS constants, program and complete A61/A62/A60 activation transaction;
7. the complete A90..A9b PS control object;
8. the selected PS ALU and TAU stores in the selected vendor packet order;
9. immediate/indexed primitive payload, retirement cookie, PAUSE and closure.

All destination and source BO PTEs must already be published, their DMA
ownership transferred to the device, and all relevant reservation fences
waited before the ring tuple is published.

An ALU-only program following a texture program must still rewrite all twelve
controls with TAU count zero and every transition map zero.  It may omit the
zero-length `21030000` packet because A92 makes stale TAU storage unreachable.
A one-texture program following two textures must rewrite A92/A93, ALU, TAU,
texture resource masks, FE active-stage state, and linkage.  No previous
program state may be inherited merely because its store bytes remain in the
hardware array.

## Mapping `chrome9_ps_program`

The current Mesa offline object is laid out as:

```c
struct chrome9_ps_program {
        uint32_t control[12];
        uint32_t alu[96][4];
        uint32_t tau[32];
        uint32_t constant_mask;
        uint32_t constant_x_one_mask;
        uint16_t varying_mask;
        uint16_t sampler_mask;
        uint8_t alu_count;
        uint8_t tau_count;
};
```

Its native C size is currently 1,728 bytes, but that size, its enum keys, and
its padding are implementation details.  Do not copy this structure into a
DRM header.  Map its members as follows:

| Mesa member | kernel/UAPI treatment |
|---|---|
| `control[0..10]` | regenerated from the selected canonical kernel template and byte-compared in Mesa offline tests; never supplied by userspace |
| `control[11]` A9b | always kernel-owned; initial value `9b000000` after its no-draw gate |
| `alu[][]` | selected from immutable kernel table for the semantic key; no raw UAPI words |
| `tau[]` | selected from the same immutable table and cross-checked against bound sampler/coordinate resources |
| `constant_mask` | exact expected mask derived from key; UAPI `used_mask` must equal it, not be a superset |
| `constant_x_one_mask` | kernel-enforced driver-owned constant lanes; userspace cannot override them |
| `varying_mask` | derived from key and required to equal the supported VS/FVF linkage mask |
| `sampler_mask` | derived from key and required to equal the submitted texture descriptor set |
| `alu_count`,`tau_count` | derived from the immutable arrays; also checked against A92 and packet counts |

The actual constant values are not in `chrome9_ps_program`; they belong to
the draw job.  The semantic key is likewise not stored in the object, so the
caller must retain it for canonical re-generation and verification.

## Minimal safe UAPI

The already reserved command-16 semantic-program design is the correct model.
A reusable fragment record is:

```c
#define DRM_CHROME9_FRAGMENT_CONSTANT_COUNT 32

struct drm_chrome9_fragment_constants_v1 {
        __u32 used_mask;
        __u32 reserved0;
        __u32 value[DRM_CHROME9_FRAGMENT_CONSTANT_COUNT][4];
        __u64 reserved[2];
};
```

The enclosing structured draw carries a fixed-width `__u32 program` semantic
id plus destination/depth/blend/scissor, texture descriptors, and vertices.
Do not expose `enum chrome9_fs_op` or `enum chrome9_fs_leaf`; C enum size and
value assignments are not a stable ABI, and commutative/internal forms should
collapse to one public key.

For each semantic key the kernel performs this fail-closed sequence before BO
binding or ring publication:

1. validate request version, flags, reserved fields, formats, sizes, guards,
   vertex finiteness, and object alias rules;
2. select a kernel-owned canonical PS object by the semantic id;
3. require the constant `used_mask` to equal its exact public mask and every
   unused dword to be zero;
4. fill and validate any driver-owned constant lanes, and initially reject
   non-finite consumed user lanes;
5. require bound texture descriptors to equal `sampler_mask`, and supported
   VS/FVF outputs to equal `varying_mask` plus the required TAU coordinates;
6. verify one full oC0 output, no depth output, no TEXKILL, and no side-effect
   flag for every admitted key;
7. assert `alu_count <= 96`, `tau_count <= 32`, A92 equals both counts, every
   transition bit is below the corresponding count, and all unused map bits
   and object slack are zero;
8. copy the complete selected object and constants into job-owned staging;
9. emit only kernel-selected selectors and exact packet counts; and
10. retain the existing fence, guard, timeout, and quarantine rules.

A Mesa-side byte hash is useful for detecting a mismatched table, but it is
not a security boundary.  The kernel must own or regenerate every native
word.

For the first ALU-only key, userspace needs only semantic id `C0_REPLACE` and
`used_mask=1`; the kernel emits the canonical `MOV oC0,c0` object.  A later
untextured semantic-program ioctl can reuse the same constant record.  No new
command or capability number is reserved by this documentation-only audit.

## Why a raw-program ABI is not safe yet

The offline object's complete-byte verifier is strong for known keys: it
regenerates the expected object, checks resource masks, controls, used native
records, counts, and every zeroed slack byte.  It does not turn arbitrary
native input into a validated program.

The object lacks or leaves unresolved all of the following security and
correctness facts:

- A90 resource fields and A91 meaning are compiler-exact per template but not
  derivable for arbitrary instructions;
- A9b is context runtime state, not shader-owned data;
- finalized TAU high/dependency bits and ALU dependency fields require the
  full allocator/scheduler/finalizer history;
- the object records masks but not the exact TAU-coordinate-to-VS-output/FVF
  linkage or interpolation qualifier;
- `varying_mask` is only a logical summary; generic v1+, flat,
  noperspective, centroid/sample, fog, point sprite, and two-sided routes are
  not live-proven;
- no PS depth-output or TEXKILL context-side state is represented; both have
  side effects beyond their native records;
- the ALU decode is incomplete for unknown/reserved bits, modifiers, result
  shifts, multi-record lowerings, exceptional arithmetic, and partial/MRT
  output interactions;
- TAU opcodes TEXLDB/TEXLDP/TEXKILL are compiler-encoded but not generally
  live-qualified; gradients, explicit LOD, offsets, shadows, and arrays are
  outside the recovered subset;
- sampler ids must be validated before encoding because invalid s16 can
  collide numerically with finalized s0;
- the proprietary compiler silently truncates ALU 97 and TAU 33 and crashes
  beyond its 128-record normalized IR; successful vendor compilation is not
  proof of a complete object;
- constant values, texture state, BO lifetime, cache invalidation, render
  state, and primitive state are outside the object and must be atomic with it;
- the C structure contains implementation padding and no version/size field.

A future Level-1 native ABI would need a decoded semantic IR or phase list,
not merely raw controls.  The kernel would have to range-check every operand,
operation, modifier, mask, temporary, constant, varying and sampler; derive
all counts and transition maps; reconstruct A90/A91; own A9b; and cross-check
all draw resources.  Until A90/A91 derivation and numerical operation gates
are closed, a general native blob is not reviewable even if packet bounds are.

## Smallest ALU-only live sequence

Use the exact 32x32 no-texture Xorg fan envelope, COPY blend, no depth, the
proven post-transform VS/FVF/linkage, a guarded BGRA8888 target, and finite
W=1 vertices.  Initialize the target data to a non-output sentinel and both
guard pages to the canonical guard word.

### Gate A: minimum six-dword derivative

Retain the complete 188-dword proven Xorg body.  Change only c0.x/c0.w to one
and the four ALU dwords:

```text
# existing Xorg H2 c0 packet, opaque logical red
FE020440 0000043c 22030000 cc000000
3f800000 00000000 00000000 3f800000

# existing Xorg controls remain byte-identical
90058000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 cccccccc dddddddd

# existing Xorg H2 ALU packet, compiler-finalized MOV oC0,c0
FE020440 0000043c 20030000 cc000000
01c00530 301c0053 00f08005 000d0820
```

Every covered pixel must be packed BGRA `ffff0000`; uncovered target bytes,
both guard pages, cookie, PAUSE, non-CR idle, and error status must be exact.
Run `old varying pass-through -> constant MOV -> old pass-through` without an
engine reset.  This proves ALU constant selection while changing the minimum
number of live words.

### Gate B: generic compiler transaction

After Gate A, use the same surrounding state but replace only the three PS
packets with the exact generic form:

```text
# body[68..75], same eight-dword footprint as old c0
FE03043c 00000004 22030000 cc000000
3f800000 00000000 00000000 3f800000

# body[132..147], complete A90..A9b
FE020440 0000043c 00010000 cc000000
90058000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000

# body[148..155], counted one-record ALU store
FE03043c 00000004 20030000 cc000000
01c00530 301c0053 00f08005 000d0820
```

There is no `21030000` packet because TAU count is zero.  The immediate H2
header moves from body 160 to body 156 and the complete body becomes 184
dwords.  Recompute all builder indices and closure; do not delete four words
from a prebuilt live ring in place.

Before rasterizing Gate B, emit these packets in an otherwise idle job with no
primitive/vertex words and require target/source/guards unchanged, CR paused,
non-CR work idle, and zero error status.  Then execute the red draw and
the same old/new/old closure.  This separates H3 selector/count and A9b
qualification from numerical ALU behavior.

On any post-publication timeout, preserve all mappings and use the existing
reboot-scoped quarantine; do not retry Gate A, omit A9b, or switch packet forms
inside the same boot to make the failure disappear.

### Multi-record count and termination re-audit

The three-coordinate silent-black result prompted a fresh instruction-level
audit of the generic store rather than another packet-field experiment.  The
Linux 5.75 implementation in `via_chrome9_dri.so` (build ID
`f140f76cfb18e3f368245054629d1ef2e67be129`) is conclusive:

| range | operation |
|---|---|
| `stmUpdatePS_inv:0x17fc29..0x17fc62` | reserves space from the ALU and TAU counts |
| `0x17fc92..0x17fd8f` | emits the full A90--A9b H2 control object |
| `0x17fd9e..0x17fe06` | if ALU count is nonzero, aligns, writes H3, and copies the ALU array |
| `0x1805a0..0x18060d` | if TAU count is nonzero, aligns, writes H3, and copies the TAU array |
| `0x17fe17..0x17fe3c` | releases exactly the emitted command span |

At `0x17fc35..0x17fc3c`, the ALU count is shifted left two before packet
construction.  That same dword count is written at `0x17fdc8`; the payload
copy size is independently formed as ALU count shifted left four bytes at
`0x17fde2..0x17fdfc`.  The fixed header words are written literally:

```text
0x17fdc2  FE03043C
0x17fdc8  ALU_count * 4
0x17fdcb  20030000
0x17fdd2  CC000000
```

The TAU path analogously writes its unscaled record count and copies four
bytes per record.  There is no END record, payload delimiter, last-record
flag, or post-program marker.  Execution length comes from A92, while the H3
count bounds the store transaction.  The next packet, if any, performs its
own 16-byte alignment.  Consequently the current generic tuple

```text
FE03043C 0000000C 20030000 CC000000 <three 4-dword ALUs>
```

is the exact proprietary form for three ALUs.  A static change to count,
selector, padding, a terminal instruction, or a trailing TAU packet would
contradict the production writer rather than repair a recovered omission.

Both Windows clients independently confirm every field.  The D3D UMD tests
the ALU count at `VTGUModeDX32A.dll:0x1015b809`, aligns at
`0x1015b816..0x1015b831`, writes `FE03043C` at `0x1015b836`, writes
`ALU_count*4` at `0x1015b845..0x1015b84c`, then writes `20030000` and
`CC000000` at `0x1015b857/0x1015b866`.  Its memcpy at
`0x1015b872..0x1015b880` copies `ALU_count<<4` bytes from the compiler
object.  Its TAU path at `0x1015b893..0x1015b8fb` writes the unchanged TAU
count, selector `21030000`, and copies `TAU_count<<2` bytes.  The OpenGL ICD's
separate builder is field-for-field identical: ALU header/count/selector/pad
at `0x10205f56/0x10205f65..0x10205f6c/0x10205f77/0x10205f86`, followed by
the copy at `0x10205f92..0x10205fa0`; its TAU path is
`0x10205fd6..0x1020601b`.  Neither client emits a zero-length TAU store or a
program terminator.

The smallest remaining silicon discriminator is a three-record constant-only
MOV chain in the already-live no-varying envelope.  It preserves the proven
c0 store, target, raster state, geometry, and one-record opcode while changing
only the temporary chain and ALU/A92 counts:

```text
# PS 2.0 semantics; c0 = (1,0,0,1)
MOV r0, c0
MOV r1, r0
MOV oC0, r1

# Exact token stream
ffff0200
03000001 000f0000 20e40000
03000001 000f0001 00e40000
03000001 000f0800 00e40001
0000ffff

# Linux production configuration-2 output
01c00530 301c0053 00f08005 000d0020
01c00530 301c0053 00f20005 000d0060
01c00530 301c0053 00f20105 000d0820

A90..A92 = 90058000 91000000 92000003
A93..A9b = 93000000 ... 9b000000
active-resource-counts = 0 2 0 0
```

This is a single causal fork.  A red result qualifies multi-record H3,
temporary forwarding and count-three controls together, moving the
three-coordinate failure back to producer/consumer linkage.  A black result
with normal retirement reproduces the missing terminal color write without
any varying state; a timeout identifies an execution/store boundary instead.
Do not alter H3 framing between outcomes.  This discriminator is compiler-
exact but has not been submitted to hardware in this audit.

## Acceptance and transition tests

After the single-record gate passes, expand only through closed objects:

1. `v0*c0`, proving varying plus constant ALU-only MUL;
2. `c0*c1`, proving two simultaneous constants;
3. `tex0*c0`, adding the one leading finalized TAU and complete matching
   texture/linkage/cache state;
4. `tex0*tex1`, adding the exact independent two-TAU object;
5. ALU-only after two-texture, proving A92 TAU count and maps fully clear old
   stores; and
6. texture after ALU-only, proving the inverse transition.

Negative requests must reject before PTE or ring publication: unknown key,
mask mismatch, nonzero unused constant, reserved field, missing sampler,
extra sampler, missing varying linkage, non-finite constant in the initial
policy, ALU/TAU count mismatch, transition bit at/above count, nonzero slack,
output other than full oC0, depth output, TEXKILL, or an unrecognized native
record in a debug verifier.

Passing this ladder proves the bounded compiler transaction.  It does not
justify accepting arbitrary ALU words, advertising general GLSL execution,
or enabling unsupported interpolation, flow, derivatives, discard, shadow,
or LOD semantics.

## Evidence boundary

- **Silicon-proven:** the complete 188-dword varying pass-through and
  232-dword one-texture streams; their fixed H2 PS stores and linkage; and
  the generic counted-H3 one-record constant MOV transaction.
- **Compiler- and builder-exact:** all twelve A90..A9b sources, H3 counts and
  selectors, ALU-before-TAU upload order, configuration-two finalized
  `MOV/MUL` objects, 96/32 store limits, and c0..c31 bank split.
- **Hardware-independent checked:** fifteen canonical Mesa PS objects and
  complete-byte verifier.
- **Still requiring silicon gate:** generic counted-H3 programs with more
  than one ALU, general A90/A91 derivation, and every arithmetic/sampling
  behavior outside the already executed fixed keys.
