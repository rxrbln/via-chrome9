# Chrome9 refract resource-descriptor and scheduling oracle

## Scope and correction

This note isolates the non-shader state needed by glmark2 `refract`.  It is
based on the installed 2023.01 source, the supplied Linux 5.75 UMD, both
Windows UMDs, and the existing guarded-resource corpora.  It does not claim
that the full refract fragment program is compiled or live.

The important lifecycle correction is that there is no draw which both
renders to the generated color/depth pair and samples those resources.  One
frame has three distinct operations:

1. render the back faces to **L0** of one BGRA color texture and one X8Z24
   depth texture, with no fragment samplers;
2. unbind that FBO and generate the BGRA color mip chain; then
3. render to the ordinary canvas BGRX+Z16 framebuffer while sampling X8Z24
   depth at stage 0, the generated BGRA mip chain at stage 1, and the plain
   512x512 RGBA image at stage 2.

`scene-refract.cpp:165..265` constructs and binds the FBO, and
`:397..474` performs that exact transition.  The source and asset checksums
are:

```text
scene-refract.cpp   a45f827710dcb6796107d8a96dbc2a83bd2353c459461fa3a305214fd741f141
light-refract.frag  5d60dd85933ef77a7bc89eb5d567fb9791fd9b14170ce82b06985da718a5e001
nasa1.png           28682a0aaa5c40c47a11657dc0524a0024058927f7236a8a1559d377ca0f97a0
```

Consequently sampling an attached resource in the producer, binding a
nonzero color mip as an RT, or keeping the D24 resource active as the
consumer's depth destination would all be incorrect workarounds.  The
existing no-feedback-alias rejection is required.

## Proprietary descriptor builders

The executable evidence agrees across the three supplied UMDs:

| role | Linux 5.75 | Windows D3D | Windows OpenGL |
|---|---:|---:|---:|
| texture image/mip builder | `stmHWPrepareTextureRegs_invi:0x001b22f0` | `0x101d7c10` | `0x10240740` |
| render target builder | `STM_RenderTarget_INV:0x00195e90` | `0x101f9f90` | corresponding relocated copy |
| mip LOD builder | `STM_MipMapLOD_INV:0x001b1a50` | `0x101d9a40` | `0x10242260` |
| full state serialization | `stmFullUpdateHardware_invi:0x00177490` | dirty gather `0x10150be0` | dirty gather `0x101fc050` |

Binary checksums are:

```text
via_chrome9_dri.so fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
VTGUModeDX32A.dll  8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll       a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
```

Windows D3D computes the image record as
`state + 0x290 + stage*0xc4` at `0x101d7c1b..0x101d7c38`.  Exact dimensions
come from resource `+0x144/+0x148`; the POT test is
`0x101d7c8a..0x101d7cb3`; A20/A21/A22 are built at
`0x101d7f83..0x101d8041`.  The address low 24 bits are cleared for relocation
at `0x101d7f72`.  Mip resources are first limited to twelve levels at
`0x101d7c66..0x101d7c6b`, then to nine explicitly addressed total levels at
`0x101d8112..0x101d8122`.  The L1--L8 loop obtains the subresource at
`0x101d8134`, shifts its address by eight at `0x101d814a`, installs the
location bits at `0x101d8166..0x101d81b9`, and clears unused entries at
`0x101d81c7..0x101d81ec`.

Linux independently caps twelve levels at `0x1b2316..0x1b232a`, derives
exact dimensions at `0x1b2340..0x1b2365`, installs the texture/depth-format
bytes at `0x1b245d..0x1b2488`, and builds the log2/POT fields beginning at
`0x1b251d`.  Its depth texture lookup
`INV_D3DFmt_DDPFToDepthTX:0x001a7030` returns Z mode four for private
D24X8/D24S8 `2032/2033`; with sampler format byte `fe` this gives exact image
word `30fe2000`.

The Windows D3D texture-format lookup is `0x1020a660`.  Its depth-mode helper
at `0x1020a960` is merged into A30 `[13:11]` at
`0x101d7f57..0x101d7f7d`.  For VX900 the Linux value four is the qualified
choice.  The alternate Windows return value one is undocumented and must not
be guessed to mean comparison sampling.

## Producer: mip-capable BGRA L0 plus X8Z24

Let `C` and `Z` be the translated SF byte addresses of color L0 and depth L0,
and let `P=ALIGN(width*4,32)`.  Binding a complete mipmapped color resource as
the L0 render target does not add a mip-table packet to the producer.  The
render and depth descriptors are simply:

```text
A50 = 50000000 | (C >> 8)
A51 = 51100000
A52 = 52990000 | (P >> 5)       # BGRA8888

A10 = 10000000 | (Z >> 8)
A11 = 11000001 | ((P >> 5) << 13)
A12 = 123e0000                  # D24X8/D24S8 storage
A13 = 13030000                  # LEQUAL
```

Linux constructs RT0's base, pitch/format and translated location at
`0x195fec..0x196078`; its state-image words are at `+0x140/+0x144/+0x148`.
Windows uses the same RT0 record and shifts the address at
`0x101fa123..0x101fa1bf`.  Mip capability is a resource property, not an RT0
format bit.  The producer also rewrites A80--A83 for the doubled dimensions,
uses full BGRA write mask, depth test/write, and FRONT culling as required by
the source, but those do not alter the resource descriptor.

For an 800x600 canvas, the FBO is 1600x1200 and `P=0x1900`, so the exact
address-independent words are:

```text
A52 = 529900c8
A11 = 11190001
A12 = 123e0000
A13 = 13030000
A80 = 800004b0
A81 = 81000640
```

The color resource still owns its complete L0--L10 allocation through the
producer even though only L0 is writable in this draw.  X8Z24 remains
single-level.

## Consumer: exact dense three-stage envelope

Every active stage has an independent image block and sampler block:

```text
image H2 selector(stage n)   = (n << 24) | 00020000
sampler H2 selector(stage n) = (n << 24) | 20020000
mip H3 selector(stage n)     = 10030000 | (n << 3)
```

Therefore stage 1's mip selector is **`10030008`**, not `10030000` and not
`11030000`.  The first form would overwrite stage 0's table; the second names
a different H3 state class.

For the 1600x1200 producer pair and default 512x512 `nasa1`, the consumer
resource image is:

```text
# stage 0: DistanceMap, raw D24 L0, nearest/clamp
selector 00020000
<Z>>8 18001101 204b0640 21000001 220000aa 30fe2000
selector 20020000
30000000 31100000 32000049 cccccccc

# stage 1: NormalMap, dynamic BGRA L0--L10, trilinear/clamp
selector 01020000
<C>>8 18001101 204b0640 21000001 220000aa 30990000
FE03043C 00000008 10030008 CC000000
40007540 4000928c 400099df 40009bb4
40009c2e 40009c4f 40009c58 40009c5b
selector 21020000
30000280 31102492 32000049 cccccccc

# stage 2: ImageMap, plain 512x512 BGRA, bilinear/clamp
selector 02020000
<I>>8 18001101 20200200 21000001 22008099 30990000
selector 22020000
30000000 31102490 32000049 cccccccc
```

Each `selector` line above is the third word of a normal
`FE020440 0000043c <selector> cc000000` H2 header.  The stage-1 H3 offsets
come from the current guarded 1600x1200 allocation corpus and are relative to
L0 in 256-byte units.  Other canvas dimensions must derive their own words;
for example `last=floor(log2(max(width,height)))`, not a scene constant.

All three S/T modes are `CLAMP_TO_EDGE`, hence A32=`32000049`.  The isolated
D24 interior-sample corpus used `32000040`; that was sufficient to qualify
raw depth decoding but leaves S/T in border mode and is not the glmark edge
contract.  Do not propagate `32000040` into refract.

The real `light-refract` fragment program has three active texture resources
and three VS varying outputs of widths `{3,4,4}`.  With the current FE builder
its general-texture prefix/linkage is:

```text
A00 = 00000031
A01 = 01002521
A08 = 08030203
A09 = 09000003
```

A00 is the compiler-active texture-resource count.  A08 is the independent
VS-coordinate-output count.  TAU sampler nibbles must remain logical stages
0, 1, and 2; A91 is the allocated coordinate-slot mask, not a sampler mask.
The bounded resource oracle below may intentionally use one simpler varying;
its FE image is not the production refract FE image.

## Exact packet differential

Relative to a dense two-stage, single-level resource body:

* making stage 1 mipmapped adds exactly twelve dwords:
  `FE03043C,8,10030008,CC000000` plus eight L1--L8 words;
* adding plain stage 2 adds a twelve-dword aligned image block and an
  eight-dword sampler block, exactly twenty dwords;
* general A00 changes `00000021 -> 00000031`; A08 does **not** derive from
  sampler count and remains the shader-I/O-dependent `08030203`; and
* no RT0 or depth descriptor changes in the consumer because its destination
  is the independent canvas BGRX+Z16 pair.

The total resource-state increment is therefore 32 dwords.  No dummy fourth
descriptor, mip table for stage 0/2, alternate BGRA RT format, or texture
count encoded as popcount of another mask is needed.

The proprietary Linux *full replay* chooses a category-wide order.  Its
sixteen-stage image loop begins at `0x177908`, builds each H2 selector at
`0x177923..0x17792f`, increments/tests the stage at
`0x1779ec..0x1779fa`, and is followed by the sixteen-stage sampler loop
`0x177a20..0x177abc`.  After one FE/general block at
`0x177ae1..0x177b2c`, the sixteen-stage H3 mip loop is
`0x177b48..0x177bd3`; exact selector construction is
`10030000 | (stage << 3)` at `0x177b59..0x177b72`.

The Windows D3D dirty gather is also category-wide, but orders the resource
classes differently.  It appends all dirty sampler chunks, by outer stage
0--15, in `0x101518e0..0x10151ce8`; all dirty image chunks in
`0x10151d10..0x10152118`; and all dirty H3 mip chunks in
`0x10152150..0x101524d1`.  Each stage can contribute several subranges, so
these are typed dirty-range loops rather than one already-flat packet per
stage.  They nevertheless establish Windows' all-samplers -> all-images ->
all-mip-tables append order, in contrast to Linux full replay's all-images ->
all-samplers -> FE/general -> all-mip-tables order.  The Windows OpenGL copy
independently has the same three loops at `0x101fcd50..0x101fd158`,
`0x101fd180..0x101fd588`, and `0x101fd5c0..0x101fd941`.

That cross-driver difference, together with the older Xorg compact
`image -> optional mip -> sampler` path, is strong evidence that this ordering
is canonical software serialization rather than a descriptor-latch sequence.
Image, sampler and H3 selectors are self-addressed and must all be complete
before the primitive which consumes them.  It does **not** prove that
cache/ownership barriers may move across producer retirement, nor that state
may be changed after a consuming primitive.  The current compact order should
therefore be retained while tested; replaying sixteen inactive stages or
reshuffling to resemble either UMD adds risk without a recovered hardware
requirement.

## Current implementation mapping and pre-GPU blockers

The current kernel builder already expresses the recovered compact resource
model in `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/`
`drivers/gpu/drm/chrome9/chrome9_engine.c`:

* `:1602..1615` builds A30--A32, including clamp/repeat bits in A32;
* `:1798..1813` keeps A00's texture count independent of A08's varying count;
* `:2093..2127` emits image, optional H3 mip table, then sampler for every
  active stage, with `10030000 | i << 3` at `:2121`; and
* `:7041..7136` validates every level and the consecutive `+0x100` implicit
  tail after the ninth explicitly addressed level.

The 1600x1200 producer is nevertheless blocked before any GPU command.  Mesa's
canonical private mip layout at `chrome9_resource.c:131..160` is
`[leading guard][L0][internal L0 guard][L1..Ln][final guard]`.  For BGRA
1600x1200, L0 plus two guards requires `0x755000` bytes, but the full L0--L10
BO is `0x9c8000`: the internal guard is `0x754000..0x754fff`, L1 begins at
`0x755000`, and the final guard is `0x9c7000..0x9c7fff`.  The render ioctl
recomputes only L0 plus two guards at kernel `chrome9_engine.c:7294..7299`,
stores that as the destination size at `:4698..4699`, then demands exact GEM
size equality at `:4719..4722`.  The full-size mip-capable RT therefore fails
with `-EINVAL`; its destination-size contract must accept and validate the
complete guarded BO without weakening either guard.

Mesa also still rejects the eventual consumer before submission:
`chrome9_draw.c:696..744` restricts mip sampling to square POT BGRX resources
no larger than 512, while refract's stage 1 is 1600x1200 BGRA and extends to
L10.  That gate must be generalized from the proven layout/format contract,
not bypassed for the scene.

Finally, the CPU mipmap callback is not independently synchronized against a
retained BGRA producer.  `chrome9_resource.c:524..529` flushes a pending draw
from `transfer_map` only for X8Z24, whereas `:745..805` maps and reads BGRA
directly during mip generation.  Refract itself avoids this race because its
FBO unbind reaches `chrome9_set_framebuffer_state`, which flushes the retained
batch and render target at `chrome9_context.c:4340..4349`, before
`glGenerateMipmap`.  The callback still needs a self-contained producer flush
for callers which generate mipmaps without that framebuffer transition.

## Bounded guarded oracle

The smallest useful test separates descriptor correctness from the full
refract math while retaining the real ownership transition:

1. Allocate guarded 32x32 BGRA L0+L1 and guarded 32x32 X8Z24 L0 resources.
   Render two triangles to BGRA L0 and X8Z24 together, storing opaque green
   near `0x80` and depth 0.25.  Verify producer retirement, both guards and
   the full BGRA/D24 payloads.
2. With the producer unbound, generate L1 through the intended Mesa CPU
   mipmap path.  Byte-check L0, L1, padding and guards, then publish the
   complete resource for device reads.
3. Bind a third guarded 4x4 BGRA source containing opaque blue `0xc0`.
   Render to a separate guarded BGRX+Z16 target with dense stages 0/1/2.
   Force stage 1 to L1 with min=max=1 and nearest-D for the first test; use
   its exact selector `10030008`.  A simple generic fragment program copies
   depth into R, generated-mip green into G, plain-image blue into B and one
   into A.  The safely interior expected host word is approximately
   `ff4080c0`, allowing one LSB only for depth conversion.
4. Run `three-stage -> one-stage -> three-stage` without context reset.  Use
   distinct source colors after the transition so stale stage 1/2 state or a
   wrong FE high-water cannot accidentally pass.
5. Repeat producer/consumer with depth 0.75 and a different L0 color for at
   least 64 alternating iterations.  Require no feedback alias, exact
   retirement/idle, unchanged source allocations, all guards intact, and a
   final known-good legacy draw.
6. Only after forced L1 passes, enable A31=`31102492` and exercise an exact
   L0/L1 trilinear blend.  Then extend separately through L8 and the implicit
   L9--L11 tail.

This oracle proves the combined BGRA+X8Z24 producer, stage-1 H3 namespace,
three dense descriptor slots, mixed raw-depth/color sampling, and old/new/old
state closure.  It deliberately does not depend on `normalize`, `refract`,
`pow`, or the production shader schedule.

## Remaining risks

* The current 1600x1200 refract chain reaches L10.  Only L1--L8 have explicit
  H3 addresses.  The proprietary allocator proves consecutive 256-byte
  L9--L11 storage after L8, but silicon selection of that implicit tail still
  needs the existing POT and asymmetric NPOT oracle.
* The three-nearest-sampler smoke source exists, but no retained live result
  was found in the documentation.  Three texture descriptors must therefore
  remain a live qualification item even though the compiler/packet namespace
  is statically exact.
* The synchronous D24 writer/reader transition is live, including an
  unrelated active Z16 destination.  It has not yet been qualified in the
  same draw as two additional color samplers and a stage-1 mip table.
* Producer-to-consumer CPU mip generation is a real ownership transition.
  Wait the producer before CPU access, acquire CPU ownership, generate and
  flush every reachable color level, then reacquire device READ ownership.
  A texture-cache pulse cannot replace those CPU/DMA steps.
* A wrong color with clean idle status is a descriptor/filter/slot failure;
  timeout or engine error requires the normal context quarantine.  Neither
  outcome justifies sampling the attached producer resources or forcing L0.
