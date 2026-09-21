# Chrome9 HD D16 read-only and polygon-offset contract

This note defines the next bounded depth-state extension after the proven D16
and D16-RHW paths.  Read-only and polygon offset remain a static
implementation plan, not a claim that those new states have run on hardware.
The existing LESS/LEQUAL write-enabled ABI and packets must remain unchanged
until each new-state oracle below passes.

The most important result is that depth read-only and polygon offset are
orthogonal:

- read-only changes only type-01 A01 bit 2, from the established test/write
  word to the corresponding test/no-write word;
- the depth surface, format, comparison, clear-data/mantissa state, and pixel-Z
  clip words do not change;
- polygon offset changes A16--A18 only.  It is not encoded in A14/A15.

## Independent evidence

The *VX900 Series Chrome9 HD Open Graphics Programming Manual*, preliminary
revision 1.0, pages 91--94, defines A01 bit 3 as Z-test enable, A01 bit 2 as
Z-write enable, and A10--A18 as the Z surface, compare, clear/mantissa, and
bias words described below.

The Linux proprietary driver independently supplies executable evidence:

- `via_chrome9_dri.so:STM_ZWBEnable_INV` at `0x195b90`, especially
  `0x195e40..0x195e7f`, conditionally sets or clears A01 bit 2 and then sets
  bit 3.  The disabled/unbound path at `0x195bd7` clears both bits with byte
  mask `f3`.
- `STM_ZFunc_INV:0x192cf0` replaces A13 bits 18:16 with the public comparison
  enum minus one.
- `STM_DepthBias_INV:0x1959d0` copies context `+0x1de4` bit-for-bit as the
  scale float, converts context `+0x1de8` units to the offset float, and
  splits those two IEEE-754 words across A16--A18 at
  `0x195ad9..0x195b18`.  The inactive path at `0x195a5e..0x195a70` zeros all
  payload bits of A16--A18.

The Windows D3D builder at `VTGUModeDX32A.dll:0x101fa910` and Z-function
builder at `0x101faa10` perform the same enable/write and comparison updates.
The Windows OpenGL copy at `VTGOGL32A.dll:0x1024e6d0` is an independent
compiler build of the same state transition.  Old Mesa 7.11.2
`via_state.c:viaChooseDepthState` is only policy evidence, but agrees that a
disabled depth test also disables depth writes.

## Exact register meanings and deltas

For the established linear SF D16 path, the complete relevant words are:

```text
A01 affine, test/write       01c0154c
A01 affine, test/read-only   01c01548
A01 RHW, test/write          01ce154c
A01 RHW, test/read-only      01ce1548

A10                          10000000 | (depth_sf_va >> 8)
A11                          11000001 | ((depth_pitch >> 5) << 13)
A12                          12380000
A13                          13000000 | (depth_func << 16)
A16/A17/A18, no offset       16000000 17000000 18000000
A19/A1A/A1B                  19800000 1a800000 1b803fbf
A1E/A1F/A20                  1e000000 1f000000 20007fff
```

`depth_func` is the native three-bit value:

```text
0 NEVER    1 LESS       2 EQUAL      3 LEQUAL
4 GREATER  5 NOTEQUAL   6 GEQUAL     7 ALWAYS
```

The delta table is deliberately explicit about fields that do **not** move:

| transition | A01 | A10 | A12 | A13 | A14 | A15 | A20 |
|---|---|---|---|---|---|---|---|
| write to read-only | clear bit 2 | same address | `12380000` | same function | unchanged | unchanged | `20007fff` |
| read-only to write | set bit 2 | same address | `12380000` | same function | unchanged | unchanged | `20007fff` |
| LESS to EQUAL | unchanged | same address | `12380000` | `13010000` to `13020000` | unchanged | unchanged | `20007fff` |
| zero to nonzero offset | unchanged | same address | `12380000` | unchanged | unchanged | unchanged | `20007fff` |

For affine or RHW packets, the read-only delta is therefore exactly `-4` in
A01.  A10 and A12 continue to bind the same D16 resource and format because a
read-only test still reads the depth surface.  A13 remains live because the
test still runs.  A20 must remain `20007fff`; reset value `20000000` collapses
the pre-depth-test pixel-Z interval to zero and can reject ordinary nonzero Z
before A13.

### A14/A15 are not depth bias

The manual resolves an important ambiguity:

- A14 bits 23:0 are the low three bytes of Z/W clear data;
- A15 bits 7:0 are the highest byte of Z/W clear data;
- A15 bits 23:8 are the negative mask applied to the low 16 mantissa bits of
  rendered Z before format conversion, comparison, and writeback.

Thus the shorthand that A14/A15 are only clear data is incomplete.  A15's
high half can alter ordinary rendered depth even when no fast clear occurs.
Neither read-only nor polygon offset requires an A14/A15 update.  The first
successor must not introduce writes to these registers: retain the proven
context image, keep fast-Z clear A27 bit 0 disabled, and vary only the words
under test.  If a later state-complete context image explicitly closes them,
`14000000/15000000` is the manual-neutral clear value and zero mantissa mask,
but that is a separate guarded delta rather than a prerequisite for this ABI.

## Polygon-offset encoding

The hardware equation is:

```text
Zbias = max(dZ/dX, dZ/dY) * scale + offset
```

The exact split of the two IEEE-754 binary32 words is:

```text
A16 = 16000000 | (offset_bits & 00ffffff)
A17 = 17000000 | ((scale_bits >> 8) & 00ff0000)
                  | (offset_bits >> 24)
A18 = 18000000 | (scale_bits & 00ffffff)
```

For the address-free first discriminator, use scale `+0.0` and offset `-0.25`
(`be800000`):

```text
16800000 170000be 18000000
```

Closing the feature is exactly:

```text
16000000 17000000 18000000
```

A19--A1B remain the proprietary initialization defaults shown above.  They
reconstruct -1.0/+1.0 clamp bounds and enable enhanced Z precision; they are
context prerequisites, not part of each bias toggle.

The public polygon `factor` becomes `scale` without conversion.  Public
`units` does not become `offset` directly: the Linux builder multiplies it by
`0x38000080` when bound-resource field `+0x158` is `0x10`, and by
`0x34000001` otherwise.  `sfmGetPatchColorKey:0xb6afc..0xb6b52`
independently proves that field is bits per pixel: values 8, 16, and 32 select
byte, word, and dword loads from the surface.  Therefore D16 uses
`0x38000080` (approximately `3.0518044e-5`), while the 32-bpp D24S8, D24X8,
and D32 family uses `0x34000001` (approximately `1.1920930e-7`).

Fill, line, and point enables are separate proprietary context booleans at
`+0x3c40`, `+0x3c44`, and `+0x3c48`, selected against polygon mode at
`+0x1dfc`.  The first implementation is triangle-fill only.

## Bounded ABI plan

Do not reinterpret the original D16 ioctls silently.  Existing unknown flag
validation provides a useful fail-closed boundary, and old userspace expects
zero comparison bits to mean legacy LESS even though native comparison zero
means NEVER.

The smallest independently shippable extension is a capability-gated
`DEPTH_READ_ONLY` flag on the existing structured D16 successors:

```text
CAP_D16_DEPTH_READ_ONLY present + flag clear -> established write path
CAP_D16_DEPTH_READ_ONLY present + flag set   -> clear A01 bit 2
cap absent or non-D16 ioctl                  -> reject flag with EINVAL
```

This adds no fields and changes no old packet.  It must be combined only with
an accepted explicit depth function; the current implementation may continue
to limit that set to LESS and LEQUAL until the eight-function oracle passes.
Depth-test disable should continue through the color-only draw path.  Do not
encode write-on/test-off, because both proprietary implementations suppress
writes when the API depth test is disabled.

### Read-only code-level integration map

At the current UAPI snapshot, flags 0/1 belong to per-ioctl batch/blend state
and bits 8--10 hold the depth function.  Bit 11 is free in every D16 request,
so the concrete non-colliding definition is:

```text
DRM_CHROME9_DEPTH_READ_ONLY = 1U << 11
DRM_CHROME9_CAP_DEPTH_READ_ONLY = 1ULL << 23
```

Capability bit 23 is reserved in the canonical
[UAPI allocation ledger](chrome9-uapi-allocation.md); bits 21 and 22 belong to
the mip and RHW-run successors.  The request flag bit remains 11 once
published.  Capability absence keeps the existing unknown-bit rejection and
guarantees old kernels fail with EINVAL rather than execute a write-enabled
approximation.

One helper should derive the A01 word so emission and assertions cannot
diverge:

```text
affine base       01c01540
perspective base  01ce1540
+ test            00000008
+ write unless READ_ONLY 00000004
```

The resulting exact words remain `01c0154c/01c01548` and
`01ce154c/01ce1548`.  Apply that helper at every structured D16 emission site:

| builder/path | current A01 location | perspective selector |
|---|---:|---|
| untextured D16, blend, batch, RHW batch | `body[19 + delta]` | RHW-batch kind |
| one-texture D16 | `common[4]`, asserted at `body[44 + delta]` | MODULATE program |
| two-texture D16 | `common[4]`, asserted at `body[64]` | retain its established affine word |

Do not alter the fixed depth oracle's hard-coded write-enabled word; it is a
regression reference, not a general UAPI path.  No memmove, body-length, A10,
A12, A13, A14, A15, or A20 assertion changes are needed for read-only.

Kernel validation must accept bit 11 only on a D16 request.  In the generic
batch ABI, reject `READ_ONLY` when `TRIANGLE_BATCH_DEPTH` is clear, exactly as
for a depth-function field without a depth BO.  If the capability is global,
all advertised D16 ioctl variants must implement it before the cap is set;
otherwise textured or two-textured draws could silently lose an API write
mask transition.

Mesa needs one cached capability boolean.  Its draw-state gate changes from
requiring `depth_writemask` to requiring either the writemask or that cap, and
sets bit 11 in every selected D16 argument: plain/blend, ordinary/RHW batch,
one-texture, and two-texture.  It never sets the bit on the color-only path.
The state object already retains `pipe_depth_stencil_alpha_state`, so no CSO
layout change is required.

Polygon offset needs two binary32 values and should use a new structured
successor rather than consume or rename reserved fields in an old ioctl.  A
minimal depth-state record is:

```text
u32 flags;               /* TEST, WRITE, BIAS_FILL; all others rejected */
u32 func;                /* native 0..7, explicit even for NEVER */
u32 bias_offset_float;   /* exact IEEE-754 binary32 */
u32 bias_scale_float;    /* exact IEEE-754 binary32 */
```

The first advertised version should be narrower than the record permits:

1. require TEST and a retained, distinct guarded D16 BO;
2. accept WRITE independently; reject write without test;
3. accept only triangle fill, linear D16, and the already proven destination
   formats/layouts;
4. when BIAS_FILL is clear, require both bias words to be positive zero and
   emit neutral A16--A18 on every draw;
5. in the first diagnostic capability, require `scale == +0.0` and allow only
   `offset == +0.0` or `offset == -0.25`; reject NaN, infinity, subnormal,
   negative zero, and every other bit pattern;
6. after the raw oracle passes, a production D16-only capability may accept
   finite values in a deliberately small range.  Mesa can lower GL `units`
   with the proved D16 multiplier `0x38000080`; because kernel code cannot use
   floating point, userspace should pass the resulting finite binary32 offset
   and the kernel should validate its bits and bounds;
7. kernel generation owns all packet words.  Userspace never supplies A01 or
   A10--A20 directly.

This split permits read-only depth to land without coupling it to an
unmeasured units conversion.  The full successor should preserve the current
128-byte batch envelope only if a new layout can do so naturally; ABI safety
is more important than reusing old `reserved[]` bytes.  A new ioctl number and
capability leave all existing structure sizes and semantics untouched.

### Polygon-offset packet-growth map

Unlike read-only, a state-complete bias successor grows the type-01 group.
For the current untextured D16+blend body, replace indices 26--31 with:

```text
26..29  A10 A11 A12 A13
30..35  A16 A17 A18 A19 A1A A1B
36..38  A1E A1F A20
39      A33 or padding
40      next header
```

This is exactly eight additional dwords.  The same logical closure adds eight
dwords to one-texture D16 because the existing five-word A10--A13/A20 block
grows by A16--A1B plus A1E/A1F.  Keep A14/A15 absent and inherited for the
first gate; adding their nominal zero values would combine an unmeasured
mantissa-mask transition with bias.  Every enabled and disabled draw emits
all A16--A1B and A1E--A20 words, so offset cannot leak into a legacy COPY.

## Live oracles

Every case uses separate guarded color and D16 BOs, canonical pitches,
CPU-to-device ownership transfer before submission, bounded synchronous
retirement, post-retirement CPU sync, guard verification, and an engine-error
check.  Run each as one isolated state delta after the established D16-RHW
suite.  On timeout or nonzero engine error, quarantine 3D until reboot; do not
attempt an in-place retry with another state.

### 1. Near/equal/far comparison matrix

CPU-fill depth with `8000` and color with black.  With writes disabled, draw
three non-overlapping triangles at normalized 16.16 Z `4000`, `8000`, and
`c000`.  These are respectively near, equal, and far relative to stored
`8000`.  The exact expected passing triangles are:

| function | expected |
|---|---|
| NEVER | none |
| LESS | near |
| EQUAL | equal |
| LEQUAL | near, equal |
| GREATER | far |
| NOTEQUAL | near, far |
| GEQUAL | equal, far |
| ALWAYS | near, equal, far |

After every draw, all depth samples must still be `8000`.  EQUAL and NOTEQUAL
are useful operand-order checks; NEVER and ALWAYS expose accidental function
aliasing.

The write-enabled LEQUAL subset is **Measured** on live VX900.  With depth
cleared to `8000`, the Gallium oracle observed equal blue as
`000000ff/8000`, retained it for the farther draw as `000000ff/8000`, and
accepted near red as `00ff0000/4000`.  Kernel commit
`8a16b931bacdaa292d81ea30c8c99b7a369b2cad` and Mesa Chrome9 commit
`b607b9ca59588dad3787d860e84bcacbceeba6fb` completed with engine-error MMIO
`040c=0`.  This proves native A13 `13030000` and its existing write path; it
does not prove the A01 read-only delta or any other compare row.

### 2. Independent read/write discriminator

Use one overlapping interior sample and clear depth to `8000`:

1. red at `4000`, LESS/read-only: color becomes red, depth stays `8000`;
2. green at `6000`, LESS/write: color becomes green, depth becomes `6000`;
3. blue at `7000`, LESS/write: it is farther than stored `6000`, so both
   color and depth remain green/`6000`.

This proves that read-only does not make the test inert, does not secretly
write depth, and does not prevent a later write-enabled transition.

### 3. Coplanar constant-offset discriminator

Clear depth to `ffff` and color to black.  Keep A14/A15 unchanged and fast-Z
clear disabled throughout.

1. Draw red at Z=0.5 (`8000`) with LESS/write and neutral A16--A18.  It must
   pass and establish the stored plane.
2. Draw the identical green triangle at the same Z with LESS/write and neutral
   A16--A18.  It must fail because equality does not pass LESS.
3. Change only A16--A18 to the exact `-0.25`, scale-zero words and draw green
   again.  It must replace red, and sampled D16 must move toward near (below
   `8000`; the expected ideal is approximately `4000`).
4. Restore neutral A16--A18 and draw an identical blue triangle at Z=0.5.  It
   must fail against the biased stored value, proving that the bias state was
   closed rather than inherited.

Use Z=0.5 rather than zero: A12 bit 19 clamps negative post-bias values to
zero, which would make a Z=0 case a poor discriminator.  Before treating an
exact `4000` result as ABI truth, record the actual D16 readback and allow a
tight one-ulp implementation tolerance for the manual's fixed-format scaling
rule.

After that large raw-offset gate, validate the production D16 units lowering
with factor zero and `units = -1`.  Its exact post-format offset bits are
`b8000080`, the sign-flipped D16 multiplier.  Against stored `8000`, an equal
incoming plane should move approximately two D16 steps toward near, pass
LESS, and store `7ffe` (allow one adjacent code until rounding is measured).
The `units = +1` control uses `38000080`, moves toward far, and must fail LESS
against `8000`.  This small-offset pair distinguishes the proved multiplier
from an accidental direct use of integer units without relying on a large
quarter-range displacement.

### 4. Combined read-only bias control

Only after the three isolated gates pass, repeat the coplanar green draw with
negative offset and read-only state.  Green must win the color comparison,
while the stored red depth remains exactly `8000`.  Restore neutral bias and
draw blue at Z=0.5 with EQUAL/read-only; blue must then pass.  This detects an
implementation that incorrectly applies bias to stored depth or couples the
write mask to the compare input.

## Mesa exposure gate

Until live validation completes, report polygon-offset and broad depth-state
support as unavailable.  After the read-only gate passes, Mesa may lower
`pipe_depth_state.writemask == false` only when the kernel advertises the new
capability; otherwise reject/fallback before submission.  After the raw bias
gate passes, the statically proved D16 `units` multiplier permits a truthful
D16-only GL/Gallium lowering, but advertise it only after factor, units, and
state-closure cases pass live.  A diagnostic raw-offset ioctl is evidence
tooling, not by itself truthful GL polygon-offset support.
