# Chrome9 command 0x13 two-texture D16 blend audit

Status: static implementation audit, 2026-09-21. This note checks the current
uncommitted kernel candidate against the independently recovered Linux and
Windows state builders. It does not claim a command-0x13 silicon result.

## Audited inputs

The exact candidate files inspected were:

| file | SHA-256 |
|---|---|
| `drivers/gpu/drm/chrome9/chrome9_engine.c` | `adc4b3e9e578bce631e1d86f1be8dc37e0caba7fe734342103ae3c5b8ef1af3c` |
| `drivers/gpu/drm/chrome9/chrome9_drv.c` | `c320f254cb97ca82d34ea2fc9dc6457ded278ce45872a59631e6d0e28555b606` |
| `include/uapi/drm/chrome9_drm.h` | `dfe7f8920294bb79ca51f7919a857ad41896082650352591d88dc19730278272` |

The hardware-state cross-checks are:

* Linux 5.75 `via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`:
  `STM_ZFunc_INV:0x192cf0`, `STM_ZWBEnable_INV:0x195b90`,
  `STM_AlphaFunc_INV:0x194ad0`, `STM_AlphaTest_INV:0x194c40`,
  `STM_AlphaBlend_INV:0x196e30`, `STM_TextureMap_INV:0x1b2df0`, and
  `STM_FilterMode_INV:0x1b32d0`;
* Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`:
  depth enable/function at `0x101fa910/0x101faa10`, alpha-test state at
  `0x101fb090`, and blend at `0x101fd9e0`;
* Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2`:
  byte-equivalent depth, alpha and blend builders at `0x1024e6d0`,
  `0x1024edc0`, and `0x10251220`; and
* Windows KMD `VTGKModeDX32A.sys`, SHA-256
  `618129ac7db7eb406e898471f5a4965ce1c84f1938188dc42a14b15858fe1087`:
  `S3gPatch` at analysis address `0x921c0` and the resource-ID patch table.

Command `0x13` is a Linux private ioctl, not a native hardware opcode present
in those binaries. The relevant binary result is more useful: the Windows and
Linux UMDs maintain depth, texture, alpha-test and blend as independently dirty
parts of one resident state image. The normal draw serializer gathers the
dirty ranges into ordinary H2/H3 packets. No alternate two-texture-plus-depth
command stream, special enable sequence, or state-order interlock was found.

## UAPI and scalar validation

`DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND` is private command `0x13`.
The fixed record is 296 bytes and has x86 ioctl value `0x41286453`. Its offsets
are:

| bytes | member |
|---:|---|
| `0` | destination handle |
| `4` | D16 handle |
| `8` | depth flags |
| `12..24` | target width, height, color pitch, D16 pitch |
| `28` | reserved zero |
| `32..103` | two 36-byte texture states |
| `104..143` | normalized blend state |
| `144..263` | three two-texture vertices |
| `264..275` | scissor |
| `276` | destination format |
| `280..295` | four reserved zero dwords |

The current candidate correctly requires an **explicit nonzero** compare
field before calling the shared depth validator. This is deliberately stricter
than the generic UAPI comment and older D16 requests, where zero retains
implicit LESS. Legal command-0x13 depth words are therefore exactly:

| flags | operation |
|---:|---|
| `00000100` | LESS, writes enabled |
| `00000300` | LEQUAL, writes enabled |
| `00000900` | LESS, read-only |
| `00000b00` | LEQUAL, read-only |

Every other flag bit is rejected. In particular, the old canonical
source-over bit is not accepted because blend state has one normalized source
of truth. This explicit-compare requirement is also present in the command
`0x0d` and `0x12` candidate handlers.

The remaining scalar masks match the design:

* blend flags contain only `BLEND_ENABLE` and `ALPHA_TEST_ENABLE`;
* all four blend factors are `0..10`, both functions are ADD, and the color
  mask is four bits;
* disabled blend requires canonical ONE/ZERO for independent RGB and alpha;
* enabled alpha test accepts function `0..7` and reference `0..255`; disabled
  alpha test requires both dependent fields to be zero;
* destination and both samplers accept only BGRX/BGRA, each sampler requires
  canonical `ALIGN(width*4,32)` pitch, dimensions `1..2048`, nearest/linear,
  and the three proven wraps;
* the D16 pitch is canonical `ALIGN(width*2,32)`; all five reserved dwords are
  zero; and
* coordinates, positive finite RHW, safe normalized S/T, Z, triangle area,
  integer overflow and total 64-MiB aperture use the existing bounded checks.

## Exact 270-dword body

The base two-texture body is 262 dwords. Command `0x13` has mandatory
destination/blend growth `B=4` and D16 growth `D=4`, so its body is exactly
270 dwords. The candidate builder's emission order and assertions reduce to:

| body | state |
|---:|---|
| `0..3` | H2 `fe020440 0000043c 00010000 cccccccc` |
| `4..19` | destination: A50/A51/A52, zero A38--A3F, A34--A37, A33 |
| `20..31` | texture-0 image, base at 24 and image A30 at 29 |
| `32..39` | texture-0 sampler |
| `40..51` | texture-1 image, base at 44 and image A30 at 49 |
| `52..59` | texture-1 sampler |
| `60..75` | common target/depth state |
| `76..87` | general state |
| `88..119` | H6 FVF/linkage |
| `120..127` | four zero pixel constants |
| `128..147` | VS float/integer constants |
| `148..155` | clip; A80--A83 at `152..155` |
| `156..183` | six-instruction VS |
| `184..191` | VS activation |
| `192..211` | pixel controls |
| `212..219` | two TAUs |
| `220..233` | two-factor RGBA program and terminators |
| `234..235` | alignment |
| `236..269` | immediate header plus three ten-dword vertices |

The exact packet-owned image selectors are retained:

```text
texture 0 image   00020000       sampler   20020000
texture 1 image   01020000       sampler   21020000
```

The combined state indices are:

```text
body[6]       A52 = 5298xxxx BGRX or 5299xxxx BGRA
body[7..14]   A38..A3F = 38000000 .. 3f000000
body[15..18]  lowered A34..A37
body[19]      lowered A33 or disabled 33007000
body[64]      A86
body[65]      A87
body[66]      A53 = 53010c00 | hardware color mask
body[67]      A00 = 00000001 | blend bit4 | alpha-test bit20
body[68]      A01 = 01c0154c write or 01c01548 read-only
body[69]      A88
body[70]      A89
body[71]      A10 = 10000000 | D16_base>>8
body[72]      A11 = 11000001 | (D16_pitch/32)<<13
body[73]      A12 = 12380000
body[74]      A13 = 13010000 LESS or 13030000 LEQUAL
body[75]      A20 = 20007fff
```

This agrees with the Windows/Linux builders. D3D `0x101faa10` and Linux
`STM_ZFunc_INV:0x192d0a..0x192d25` both subtract one from a one-based API
compare and insert three bits into the cached A13 field. Thus public
LESS=`1` and LEQUAL=`3` produce the shown native words. D3D `0x101fa910`
separates depth-test and depth-write enable, agreeing with the single A01 bit-2
delta. The Windows alpha and blend builders update the A33 and A34--A37 cached
ranges independently, while the common serializer emits all dirty ranges
before the draw. Nothing in either binary makes A33 placement after A13
mandatory; self-addressed A33 at body 19 is transition-complete and avoids the
old one-texture `common[7]` collision with A10.

For the currently important ioquake tuple, LEQUAL/read-only plus
DST_COLOR/ZERO and all channels, the fixed discriminator is:

```text
flags       00000b00
A34..A37    34000010 35000114 36000420 37000224
A53         53010c0f
A00         00000011
A01         01c01548
A33         33007000
A12/A13/A20 12380000 13030000 20007fff
```

The shader portion is also unchanged from the live two-texture product:

```text
controls    90018000 91000003 92020002 93000002
TAU         301001b0 311211b1 cccccccc dddddddd
ALU         0140b6d0 30202053 00f20005 0000007f
            01c00000 301c0000 00f20105 000d083f
            cccccccc dddddddd
```

There is no hidden program selector in the request. It always means the
two-sampler RGBA product, with blend/alpha-test/depth/scissor layered around
that closed program.

## Four-object role and lifetime audit

The internal names are historical and initially misleading, but the roles are
correct:

| internal lookup | command-0x13 object | use |
|---|---|---|
| `obj` | target | WRITE |
| `depth_obj` | texture 0 | READ |
| `texture2_obj` | texture 1 | READ |
| `texture3_obj` | D16 | WRITE conservatively, including read-only depth |

All four handles must be nonzero and pairwise distinct. The submit path locks
all four reservation objects, waits for dependencies, pins/maps all four,
installs one contiguous GART span, flushes/synchronizes all four, attaches the
four fence usages, and passes addresses to the builder as target, texture 0,
texture 1, D16. Its PTE order matches those address calculations. On a
post-publication failure the same four mappings, references, PTEs and ring
segment are quarantined together.

The asymmetric size tests are intended: target and D16 are exact canonical
guarded allocations; texture 0 and texture 1 may be larger only for the
existing mip-capable allocation convention, while the command still samples
level zero. Both the canonical L0 guard and terminal allocation guard remain
part of submission safety.

## No cross-device PRIME requirement

No recovered Windows draw path requires cross-adapter or PRIME-like behavior.
The UMD puts ordinary local-adapter resource references in the allocation and
patch lists. The KMD's `S3gPatch` resolves render targets `RID_RT0..RT3`
(`37..40`), texture base `RID_TXB` (`44`), cube faces (`45..50`), and texture
levels `RID_TXL` (`51`) to addresses used in the same command buffer. The
address-format patch selectors change only how an allocation address is packed
into command words. No device selector, migration operation, peer aperture, or
cross-adapter fence accompanies the two-texture/depth/blend state.

This is an absence result scoped to the recovered rendering path. Windows may
support shared resources through other OS interfaces, but that is not part of
the Chrome9 packet contract and is not required for command `0x13`.

The Linux policy is therefore coherent:

* reject an object belonging to a different `drm_device`;
* reject a true dma-buf import proxy (`drm_gem_is_imported(obj)`); and
* accept a same-Chrome9 dma-buf round trip, because
  `drm_gem_shmem_prime_import_no_map()` recognizes its own export and returns
  the original GEM object with a reference. Such an alias has the same
  `drm_device` and reservation object and no `import_attach`, so it is not an
  imported proxy.

Same-device dma-buf handle sharing is important for DRI3/EGL presentation, but
it is object identity and lifetime management, not cross-device offload. No
PRIME transport field belongs in this draw ABI.

## Remaining gate

The packet map, masks, four-object role wiring and binary state semantics pass
static audit. Before advertising capability bit 28, retain the staged guarded
silicon vectors in `two-texture-primary-uapi-design.md`: first the
LEQUAL/read-only DST_COLOR discriminator, then alpha-test/scissor transitions,
then write-enabled depth. Record the exact body length and all four guards on
both success and timeout; static agreement is not a substitute for the first
combined live run.
