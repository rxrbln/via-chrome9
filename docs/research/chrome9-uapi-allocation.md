# Chrome9 private UAPI allocation ledger

This is the canonical command-number and capability-bit ledger for the
bring-up driver.  The two namespaces are independent: a feature can consume a
capability without adding an ioctl, and several semantic capabilities can
describe one ioctl.  Update this table in the same commit that reserves a new
number; design notes must link here rather than independently claiming the
"next free" value.

## Ioctl command numbers

| command | state | public name |
|---:|---|---|
| `00` | merged | `GET_PARAM` |
| `01` | merged | `GEM_CREATE` |
| `02` | merged | `FIXED_QUAD` |
| `03` | merged | `DRAW_TRIANGLE` |
| `04` | merged | `DRAW_TRIANGLE_D16` |
| `05` | merged | `DRAW_TEXTURED_TRIANGLE` |
| `06` | merged | `DRAW_TRIANGLE_BLEND` |
| `07` | merged | `DRAW_TEXTURED_TRIANGLE_BLEND` |
| `08` | merged | `DRAW_TRIANGLE_D16_BLEND` |
| `09` | merged | `DRAW_TRIANGLE_BATCH` |
| `0a` | merged | `DRAW_TWO_TEXTURED_TRIANGLE` |
| `0b` | merged | `DRAW_TEXTURED_TRIANGLE_D16` |
| `0c` | merged | `DRAW_TRIANGLE_D16_RHW_BATCH` |
| `0d` | merged | `DRAW_TWO_TEXTURED_TRIANGLE_D16` |
| `0e` | reserved | `DRAW_TEXTURED_TRIANGLE_MIP` |
| `0f` | implemented, live-proven | `DRAW_TRIANGLE_D16_RHW_RUN` |
| `10` | reserved | `DRAW_CLIP_TRIANGLE` |
| `11` | reserved | `DRAW_CLIP_TRIANGLE_LIST` |
| `12` | reserved | `DRAW_TEXTURED_TRIANGLE_D16_BLEND` |
| `13` | reserved | `DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND` |
| `14` | reserved | `DRAW_TEXTURED_TRIANGLE_RUN` |
| `15` | reserved | `DRAW_TWO_TEXTURED_TRIANGLE_RUN` |
| `16` | reserved | `DRAW_TEXTURED_PROGRAM_TRIANGLE` |
| `17` | reserved | `DRAW_TRIANGLE_D24S8_STENCIL` |
| `18` | reserved | `DRAW_FOG_TRIANGLE` |
| `19` | implemented | `DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH` |

The next unreserved command is `0x1a`.  A reservation is not an advertised
ABI: the command enters `chrome9_ioctls[]` only with its implementation, and
userspace must still require the associated capability.

On x86, the proposed 128-byte RHW-run request at private command `0x0f` has
ioctl value `0x4080644f`: direction WRITE, DRM type `0x64`, command number
`DRM_COMMAND_BASE + 0x0f = 0x4f`, and payload size `0x80`.

Command `0x0f` has a complete render-node handler, a separate 32-KiB CPU-only
staging allocation, and the audited one-list 272-triangle builder.  On VX900
it passed counts 1/34/35/64/128/272, rejected 273, verified alternating D16
cells and all guards, survived four consecutive maximum lists and ring wrap,
then passed the full legacy/cmd19 smoke.  Capability bit 22 is therefore
advertised; Mesa keeps the old 34-triangle fallback for older kernels.

## Capability bits

| bit | state | public name |
|---:|---|---|
| 0 | merged | `GEM` |
| 1 | historical, not advertised | `GEM_DMA` |
| 2 | merged | `GEM_SHMEM_CACHED` |
| 3 | merged | `FIXED_QUAD` |
| 4 | merged | `DRAW_TRIANGLE` |
| 5 | merged | `DRAW_TRIANGLE_D16` |
| 6 | merged | `DRAW_TEXTURED_TRIANGLE` |
| 7 | merged | `DRAW_TRIANGLE_BLEND` |
| 8 | merged | `DRAW_TEXTURED_TRIANGLE_BLEND` |
| 9 | merged | `BLEND_BGRA8888` |
| 10 | merged | `ALPHA_TEST` |
| 11 | merged | `SCISSOR` |
| 12 | merged | `DRAW_TRIANGLE_D16_BLEND` |
| 13 | merged | `DRAW_TRIANGLE_BATCH` |
| 14 | merged | `DRAW_TWO_TEXTURED_TRIANGLE` |
| 15 | merged | `DEPTH_FUNC` |
| 16 | merged | `DRAW_TEXTURED_TRIANGLE_D16` |
| 17 | merged | `TEXTURED_D16_BGRA8888` |
| 18 | merged | `TEXTURED_D16_SOURCE_OVER` |
| 19 | merged | `DRAW_TRIANGLE_D16_RHW_BATCH` |
| 20 | merged | `DRAW_TWO_TEXTURED_TRIANGLE_D16` |
| 21 | reserved | `DRAW_TEXTURED_TRIANGLE_MIP` |
| 22 | implemented, live-proven | `DRAW_TRIANGLE_D16_RHW_RUN` |
| 23 | merged | `DEPTH_READ_ONLY` |
| 24 | reserved | `DRAW_CLIP_TRIANGLE` |
| 25 | reserved | `TWO_TEXTURE_PRIMARY_MODULATE` |
| 26 | reserved | `DRAW_CLIP_TRIANGLE_LIST` |
| 27 | reserved | `DRAW_TEXTURED_TRIANGLE_D16_BLEND` |
| 28 | reserved | `DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND` |
| 29 | merged | `BLEND_EXTENDED_FACTORS` |
| 30 | reserved | `DRAW_TEXTURED_TRIANGLE_RUN` |
| 31 | reserved | `DRAW_TWO_TEXTURED_TRIANGLE_RUN` |
| 32 | reserved | `DEPTH_FUNC_EQUAL` |
| 33 | reserved | `DRAW_TEXTURED_PROGRAM` |
| 34 | reserved | `FRAGMENT_CONSTANTS_32` |
| 35 | reserved | `DEPTH_ALWAYS` |
| 36 | reserved | `DRAW_TRIANGLE_D24S8_STENCIL` |
| 37 | reserved | `DRAW_FOGPASS` |
| 38 | reserved | `D24S8_TWO_SIDED_STENCIL` |
| 39 | reserved | `TWO_TEXTURE_PRIMARY_TEX0_ALPHA` |
| 40 | reserved, live-proven | `TWO_TEXTURE_SIGNED_REPEAT_COORDS` |
| 41 | implemented, awaiting silicon validation | `DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH` |
| 42 | reserved | `PS_MOV_SWIZZLE` |
| 43 | reserved | `DRAW_TEXTURED_TRIANGLE_BATCH` |

The next unreserved capability bit is 44.  `DEPTH_READ_ONLY` deliberately has
no command allocation: it adds request flag bit 11 to existing structured D16
successors only when capability bit 23 is present.

The command ordinal and capability-bit namespaces are not numerically paired.
In particular, the native list successor is private command `0x11` but
capability bit 26.  Bit 25 belongs to the two-texture primary-color semantic
extension documented by commit `a8589b4`; that extension reuses existing
two-texture commands and therefore consumes no ioctl ordinal.

The merged boundary in this table is kernel Chrome9 commit
`f56ac02` and kernel UAPI commit `940be59`.  The reservations describe
reviewed designs, not code present in those commits.

Commands `0x14` and `0x15` and capability bits 30 and 31 are the bounded
one-/two-texture list/run successors specified in
[textured-triangle-run-list-design.md](textured-triangle-run-list-design.md).
They retain existing structured shader/state envelopes and do not expose raw
command streams.

Capability bit 32 adds no command or request field. It says the existing
depth-function field accepts explicit EQUAL in addition to the baseline
LESS/LEQUAL subset, as designed in
[ioquake-fog-dlight-successors.md](ioquake-fog-dlight-successors.md).

Private command `0x16` and capability bit 33 belong to the version-one
one-texture semantic-program successor in
[ioquake3-gl2-generic-texturecolor.md](ioquake3-gl2-generic-texturecolor.md).
It transports job-local constants but never native instructions. Capability
bit 34 is deliberately separate: only it permits Mesa to advertise the full
32-vector fragment constant store. The first command implementation can use
the live-shaped c0 H2 packet without claiming the two-bank H3 path.

Capability bit 35 adds no command or request field. It says the existing
depth-function bits `[10:8]` accept `ALWAYS=7`, as specified in
[ioquake-depth-always-mask-zero.md](ioquake-depth-always-mask-zero.md). The
first observed consumer is an untextured D16 write with RT0 color mask zero;
it still requires the existing state-complete command-08 capability.

Private command `0x17` and capability bit 36 belong to the first structured
untextured D24S8/stencil successor in
[ioquake-next-raster-state-ladder.md](ioquake-next-raster-state-ladder.md).
The 208-byte request carries semantic physical-CW/CCW stencil faces; the
kernel translates operations and generates every native packet word. Native
cull and polygon offset deliberately consume no allocation at this stage
because Gallium draw already supplies both semantics in software.

Private command `0x18` and capability bit 37 belong to the exact ioquake3 GL2
fogpass semantic successor in
[ioquake3-gl2-fogpass-native-design.md](ioquake3-gl2-fogpass-native-design.md).
It carries one job-local color constant and one post-TNL float fog scale per
vertex. It does not accept native code or advertise general shader ALU.

Capability bit 38 adds no command or request field. It permits unequal
physical CW/CCW face blocks in command `0x17` after the exact orientation
matrix in
[two-sided-stencil-orientation.md](two-sided-stencil-orientation.md) passes.
Base capability 36 remains sufficient for mirrored one-sided stencil.

Capability bit 39 adds two-texture program flag bit 1 to existing commands
`0x0a`, `0x0d`, and `0x13`.  It selects the complete four-ALU
texture-0-alpha-times-primary object needed when packed ioquake3 lightall
uses a BGRA unit 1 but does not consume its alpha.  Program flag bit 0 retains
the bit-25 full-RGBA object; bits 0 and 1 are mutually exclusive.  The packet,
format gate, and silicon oracle are specified in
[ioquake3-gl2-lightall-bgra-object.md](ioquake3-gl2-lightall-bgra-object.md).

Capability bit 40 adds no command or request field.  For either texture it
permits a signed/out-of-range coordinate on an axis set to REPEAT, with finite
magnitude at most 32; CLAMP and MIRROR axes retain the normalized rule.  The
unchanged IEEE float payload, sampler ordering, command-15 selector
separation, and passed guarded silicon gate are specified in
[ioquake3-signed-repeat-command15.md](ioquake3-signed-repeat-command15.md).

Private command `0x19` and capability bit 41 are the first path where Mesa's
NIR compiler result controls the fragment program executed by a draw.  The
144-byte request is a D16, perspective-correct triangle batch with one
structured full-vector MOV or MUL instruction.  Version one exposes only
primary varying `v0` and job-local constant `c0`; Mesa sends logical opcode,
file, and identity swizzle fields, while the kernel validates them and
rebuilds the exact four native ALU words.  The modifier byte must be zero.  No
native control, instruction, TAU, temp, or
packet word crosses the UAPI.  The kernel fixes the one-ALU resource controls,
uploads only c0, and rejects unused or non-finite fields.  The guarded v0/c0
exact-pixel test is the required silicon acceptance gate before deployment.

Capability bit 42 is reserved for the first independently gated command-19
source-swizzle extension.  Its initial semantic key is terminal full-XYZW
`MOV(v0.wwww)` only; it does not authorize arbitrary selectors, partial
writes, source modifiers, or arithmetic swizzles.  The exact record and
mandatory D16 oracle are specified in
[chrome9-swizzle-neg-boundary.md](chrome9-swizzle-neg-boundary.md).

Capability bit 43 is reserved exclusively for
`DRAW_TEXTURED_TRIANGLE_BATCH`, the structured one-texture batch used by the
same-device Present path.  This reservation assigns no command ordinal by
itself; the implementing UAPI commit must add the selected command to the
ioctl table above and define the complete request contract before advertising
the bit.
