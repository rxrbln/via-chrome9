# Chrome9 bounded multi-ALU structured UAPI design

Status: implementation-ready design, cross-checked against Mesa `e34de89`,
the command-19 kernel encoder, and proprietary compiler output. No command or
capability number is allocated here. Existing command 19 and capability 41
remain the immutable one-record ABI.

## Smallest useful successor

Mesa `e34de89` has a semantic fragment compiler that accepts straight-line
MOV/MUL expression trees with at most three ALU records. It validates
destination lifetimes, derives temporary `last_use` markers in a second pass,
and emits native words. Current command 19 carries only one implicit-oC0
instruction, however, so userspace cannot submit those multi-record results.

The first successor should carry semantic instructions, never native words,
and expose only:

- one to three MOV or MUL records;
- primary input `v0`, float constant `c0`, and temporaries `r0` and `r1`;
- identity XYZW sources with modifier zero;
- full XYZW writes to SSA-style temporaries and one final `oC0`;
- the existing D16/RHW, blend, scissor, and destination-format envelope.

Keeping one constant is intentional. It reuses the live command-19 constant
store exactly and still admits nested forms such as `(v0*c0)*c0`. A later
constant-bank successor must be independently gated; it must not be smuggled
into this request.

## Proposed fixed-width structures

The following layout is a design input, not an allocation. The command and
capability values must be assigned centrally because capability bits 42--46
are already occupied.

```c
#define DRM_CHROME9_PS_V2_MAX_INSTRUCTIONS 3

#define DRM_CHROME9_PS_DEST_TEMP  0
#define DRM_CHROME9_PS_DEST_COLOR 1 /* semantic value; maps to native class 2 */

/* SOURCE_VARYING=0 and SOURCE_CONSTANT=1 retain their v1 values. */
#define DRM_CHROME9_PS_SOURCE_TEMP 2

struct drm_chrome9_ps_destination_v2 {
        __u8 file;
        __u8 index;
        __u8 writemask;
        __u8 reserved;
};

struct drm_chrome9_ps_instruction_v2 {
        __u32 op;
        struct drm_chrome9_ps_destination_v2 dst;
        struct drm_chrome9_ps_source src[2];
        __u32 reserved[2];
}; /* 32 bytes */

struct drm_chrome9_ps_program_v2 {
        __u32 instruction_count;
        __u32 reserved;
        struct drm_chrome9_ps_instruction_v2 instruction[3];
        __u32 constant_c0[4];
}; /* 120 bytes */
```

Append `drm_chrome9_ps_program_v2` after the existing draw request prefix
through `vertices_ptr`. That prefix is 104 bytes, making the complete request
224 bytes. Add compile-time size assertions for every structure. Do not resize
command 19: DRM ioctl size is part of its command encoding, and its documented
one-record semantics are already observable.

All `reserved` bytes and words must be zero. The semantic destination COLOR
value deliberately differs from native destination file value two. The kernel
owns that translation, just as it owns all source packing, release bits,
controls, and packet lengths.

Mesa's existing internal enums are intentionally compiler-oriented and do
not have the same numbers as the stable UAPI. Submission must translate them
field by field, never `memcpy` an internal instruction:

| Mesa internal | value | v2 UAPI | value |
|---|---:|---|---:|
| source TEMP | 0 | source TEMP | 2 |
| source INPUT | 1 | source VARYING | 0 |
| source CONSTANT | 2 | source CONSTANT | 1 |
| destination TEMP | 0 | destination TEMP | 0 |
| destination COLOR | 2 | destination COLOR | 1 |

The differing values are useful defense in depth: accidentally copying the
compiler object should fail validation or change meaning visibly rather than
becoming an undocumented raw-program ABI.

## Fail-closed validation

Validation should happen before BO lookup, vertex copying, or ring mutation:

1. Require `instruction_count` in 1..3.
2. Accept only MOV and MUL. MOV requires an all-zero second source.
3. Accept only identity XYZW, modifier zero, and source reserved zero.
4. VARYING must be `v0`; CONSTANT must be `c0`; TEMP must be `r0` or `r1`.
5. Every destination must have writemask `0xf` and reserved zero.
6. Only the last record may write COLOR, it must write semantic `oC0`, and the
   last record must write COLOR.
7. Earlier records must each define a previously undefined temporary. Reject
   read-before-definition, WAW, dead definitions, and any temporary not used.
   Do not require numerical definition order: the recursive Mesa lowering can
   validly define r1 before r0 for a three-level left-deep expression. After
   validation, require the set of defined physical ids to be dense: one temp
   must be `{r0}`, and two temps must be `{r0,r1}`. Reject a lone r1.
8. Count all reads first, then assign release bits on the last read in a
   second pass. For the present A/C operand set, if both slots make the last
   read of the same temporary, set A only. This is the vendor A-over-C rule.
9. If any instruction reads c0, require every lane's sign-cleared IEEE word
   to be at most `0x3f800000` (`abs(c0) <= 1.0`). Require all four words zero
   otherwise. Retain the existing destination/depth/vertex, blend, and
   scissor checks unchanged.
10. Rebuild every native word in kernel memory. Never accept A90--A9b,
    destination bits, source file numbers, temporary-release bits, or a native
    instruction blob from userspace.

The validator should track `defined_temps`, `temp_uses[2]`, and
`remaining_uses[2]`, mirroring the compiler's two passes. Validation and
encoding must use the same physical-slot mapping: MOV source zero is A; MUL
source zero is A and source one is C. A future SUB or MAD admission cannot
reuse logical-source ordering blindly because those operations have different
physical slots.

The UAPI temp index is a bounded allocated physical slot, not a NIR SSA id or
a D3D token register number. Proprietary Linux configuration two maps a shader
that mentions only logical D3D r1 to physical r0, while the research Windows
identity-callback harness preserves id one before production allocation. This
proves logical ids cannot be copied directly. Mesa may serialize only the
post-allocation ids in `native_fs_ir`; alternatively a future kernel compiler
could allocate from a genuinely logical graph, but the two models must never
be mixed.

## Mesa submission gate

The draw path may select this request only when all of the following hold:

- `native_fs_from_nir` is true and semantic IR count equals native ALU count
  in 1..3;
- native TAU count and sampler mask are zero;
- varying mask has no bit except v0, constant mask has no bit except c0, and
  `constant_x_one_mask` is zero;
- every semantic instruction passes the exact opcode/source/destination rules
  above after explicit enum translation;
- its post-allocation temp-id set is canonical and dense even if definition
  order is reverse numerical order;
- runtime c0 satisfies the kernel's `abs(c0)<=1` bound; and
- the selected vertex path supplies normalized primary color v0 and no
  unrepresented varying.

If any check fails, do not truncate to the first record or silently fall back
to command 19. Keep the draw on an independently correct path or reject it.
The native words cached in `shader->native_fs` remain an offline comparison
oracle; the v2 ioctl carries `shader->native_fs_ir` semantics only.

## Exact controls and packet growth

For this bounded two-temporary, ALU-only subset the proprietary compilers and
the Mesa offline encoder agree on:

```text
A90 90058000
A91 91000000
A92 92000000 | instruction_count
A93 93000000
A94 94000000
A95 95000000
A96 96000000
A97 97000000
A98 98000000
A99 99000000
A9a 9a000000
```

No TAU packet is present. The current H6 template already supplies the
one-record form. After the existing blend (`delta=0/4`) and D16 insertion, the
first ALU record is at `body[160 + delta]`, and the immediate-vertex H2 header
starts at `body[164 + delta]`. For `N` records:

```text
program_extra = 4 * (N - 1) dwords
ALU payload   = body[160 + delta .. 160 + delta + 4*N)
vertex header = body[164 + delta + program_extra]
vertex data   = body[168 + delta + program_extra]
body length   = 170 + delta + program_extra + vertex_count*6
```

Move the four-word vertex header before copying the enlarged ALU payload, then
write vertices at the shifted location. The post-D16 A92 word is
`body[142 + delta]`; patch it to `0x92000000 | N` before publication.
The H2 parameter type remains `20030000`; the ALU payload length is determined
by A92 and contains exactly `4*N` dwords. Recalculate every internal packet
assertion from `program_extra`; do not leave one-record literal offsets in the
multi-record builder.

The c0 H2 store stays four dwords and byte-identical to command 19. Keeping
that store unchanged is a safety property of this first successor.

The control store likewise stays in command 19's silicon-proven Xorg H2
compatibility form: A90--A9a followed by `CC/DD` padding. It does **not** add
an A9b write and does not switch the ALU or constant store to counted H3.
Although the proprietary generic builder proves a complete twelve-word
A90--A9b object and counted H3 stores, that transaction is a separate,
still-ungated silicon feature. Mixing it into the first multi-record gate
would change three variables at once.

This omission is safe only while every advertised runtime path preserves the
same compatibility contract and no path writes nonzero kernel-owned A9b.
Qualifying the generic transaction later requires its documented
no-draw/old-new-old gates and recomputed packet offsets; it is not an in-place
upgrade of this ABI.

### Why finite-only c0 is unsafe here

Command 19's single `v0*c0` cannot overflow when normalized v0 is in `[0,1]`
and c0 is finite. That argument does not extend to an arbitrary three-record
DAG. For example:

```text
r0  = c0*c0
r1  = r0*r0
oC0 = r1*r1
```

forms `c0^8`, which can overflow even though every input c0 lane is finite.
The fixed `abs(c0)<=1` bound makes every MOV/MUL intermediate finite with
magnitude at most one for every accepted topology because D3DColor v0 is
normalized. This also admits signed zero and finite subnormals. A wider bound
requires kernel-side interval/exponent propagation and a separate numerical
gate; do not infer it merely from command 19's finite check.

## Golden encoder rows

These rows were regenerated with the Mesa `e34de89` offline encoder and match
the recovered proprietary A/C destination and last-use rules.

They are also reproduced byte for byte by the proprietary Linux 5.75
production configuration-two compiler. The four exact token streams and a
Windows raw-stage comparison are retained in
[chrome9-multi-alu-cross-driver-corpus.tsv](chrome9-multi-alu-cross-driver-corpus.tsv).
Static D3D/OpenGL finalizer provenance, normalized body hashes, destination
ids, active-resource tuples, and eight additional short-chain rows are in
[chrome9-mov-mul-short-chain-three-driver.md](chrome9-mov-mul-short-chain-three-driver.md).

Two records, one temporary:

```text
MOV r0,v0       01c00530 301c0053 00f04005 000d0020
MUL oC0,r0,c0   0140b6d0 30080053 00f20005 00000820
controls        90058000 91000000 92000002 93000000
masks           varying=0001 constant=00000001
```

Three records, a linear lifetime:

```text
MUL r0,v0,c0    0140b6d0 30080053 00f04005 00000020
MOV r1,r0       01c00530 301c0053 00f20005 000d0060
MUL oC0,r1,c0   0140b6d0 30080053 00f20105 00000820
controls        90058000 91000000 92000003 93000000
```

Three records, simultaneous A/C releases:

```text
MUL r0,v0,c0    0140b6d0 30080053 00f04005 00000020
MOV r1,r0       01c00530 301c0053 00f00005 000d0060
MUL oC0,r0,r1   0140b6d0 30201053 00f20005 00000820
controls        90058000 91000000 92000003 93000000
```

Three records in the reverse-id order produced by a left-deep recursive NIR
tree are also byte-identical between Linux configuration two and Mesa:

```text
MUL r1,v0,c0    0140b6d0 30080053 00f04005 00000060
MUL r0,r1,c0    0140b6d0 30080053 00f20105 00000020
MUL oC0,r0,c0   0140b6d0 30080053 00f20005 00000820
controls        90058000 91000000 92000003 93000000
```

The final row proves two different temporaries may be released together: A
release is dword2 bit17 and C release is dword1 bit21. A separate duplicate
source oracle sets only A's marker:

```text
MOV r0,v0       01c00530 301c0053 00f04005 000d0020
MUL oC0,r0,r0   0140b6d0 30000053 00f20005 00000820
controls        90058000 91000000 92000002 93000000
```

## Live admission sequence

Add the new capability only after all kernel unit vectors byte-match the rows
above. Then use isolated BGRA8+D16 draws, with guard pages and the normal
timeout quarantine:

1. `MOV r0,v0; MUL oC0,r0,c0`, with c0=(0.5,1,1,1), checks two-record order.
2. The linear three-record row checks r0 release before r1 consumption.
3. The reverse-id row checks that semantic validation does not impose a false
   r0-before-r1 rule on compiler output.
4. The branch row checks simultaneous A/C release without a wedge.
5. A duplicate-temp row checks A-over-C release precedence.
6. A `c0*c0; square; square` negative ioctl with any lane just above 1.0
   checks that finite-but-unsafe constants fail before publication.
7. Other negative ioctls cover count 0/4, early COLOR, final TEMP,
   undefined/dead temps, lone-r1 allocation, WAW, nonidentity swizzle,
   nonzero modifier/reserved, NaN/Inf c0, and a nonzero unused c0.

Each positive oracle must verify exact pixels, untouched outside pixels,
color/depth guards, cookie retirement, 0400/040c idle state, and a clean
follow-up command-19 one-record draw. Keep the feature disabled by default
until the full transition sequence passes on VX900.

## Patch touch points

Keep version one and version two mechanically separate until both have passed
their own tests:

1. In `include/uapi/drm/chrome9_drm.h`, add the fixed-size v2 structures and a
   newly coordinated ioctl/capability. Do not add TEMP or destination fields
   to the meaning accepted by command 19.
2. In `chrome9_engine.c`, retain `chrome9_ps_source_valid()` and
   `chrome9_encode_ps_instruction()` as the v1 validator. Add a distinct v2
   whole-program validator/encoder with two passes and compile-time size
   assertions.
3. Extend the internal `chrome9_draw_request` to carry 12 kernel-built ALU
   words plus `ps_program_count`; zero the unused tail. Do not infer count by
   scanning words.
4. Thread that explicit count through `chrome9_draw_triangle_batch()`. The v1
   ioctl passes one; v2 passes its validated 1..3 count. Validation errors
   return before `memdup_array_user()`, BO reservation, or publication.
5. In `chrome9_build_depth_triangle()`, replay the normal complete state,
   including the exact existing H2 compatibility PS control/store form, patch
   `body[142+delta]`, shift the immediate-vertex header, copy exactly
   `count*4` ALU dwords, and adjust every following offset by
   `program_extra`. The per-draw replay ensures a following v1 draw restores
   A92=one; never rely on inherited program count. Do not add A9b or H3 in
   this patch.
6. Advertise the new capability from `chrome9_drv.c` only behind a distinct
   default-off module gate until all live oracles pass. Timeout quarantine and
   teardown ownership remain unchanged.
7. In Mesa `chrome9_drm.c`, add a wrapper for the new fixed-size request. In
   `chrome9_draw.c`, select it only through the submission gate above and
   perform explicit enum translation. Preserve command 19 as the preferred
   one-record route until the v2 capability is present and proven.

The maximum 544-triangle run grows by only eight dwords at three ALUs. Each
extra record is four dwords, so all following H2 headers retain 16-byte
alignment. The builder must still use its existing staging-capacity and
read-ahead checks rather than relying on this arithmetic informally.

## Explicit exclusions

This design does not admit textures, more than one constant, partial writes,
SAT, source modifiers, arbitrary swizzles, B-slot operations, control flow,
discard, depth output, raw native programs, or the generic A9b/H3 upload
transaction. It also does not reserve an ABI number. Those are separate
successors with separate corpus and silicon gates.

## Windows wrapper boundary

The headless D3D Wine wrapper independently accepts all four programs above
and produces the same operation and destination topology. Its injected
identity/allocation callbacks do not reconstruct temporary source locations,
however: for example its second record for the two-record case remains
`0140b6d0 00080053 00f00000 00000820` before and after the wrapper's
post-translation call. It is not an executable native record.

Therefore the corpus keeps the Windows raw column separate. The executable
golden is the byte-identical Linux configuration-two and Mesa result; no test
may substitute the unresolved Windows wrapper row. This is a wrapper-context
limit, not evidence that the shipped Windows KMD submits unresolved words.

Reproduction identities:

```text
Wine wrapper PE32     a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c
Linux probe binary    a22c873536dc4b6c45c43a00a48dbc183e40ddb89ed5e1461890efad2d21e6ba
Linux probe source    4299ac3cb182c9e26688dd40e63e9aed4fb86ce47c829717b2663b734f040c13
Mesa encoder source   dabd63db42ad5939cdac2da6195556f6caa3c97b65a829e233797e36a927107e
```
