# Indexed draws, VBOs, topology, and the next application gate

This note separates three mechanisms which an API draw commonly presents
together but Chrome9 implements independently:

1. the application vertex and index buffers consumed by Mesa's software draw
   pipeline;
2. post-transform primitive decomposition, clipping, culling, and varying
   interpolation performed by that pipeline;
3. the Chrome9 command regulator's optional SF vertex-stream fetch plus
   inline index payload.

The first two are already the safer route for the current driver.  Native
Chrome9 vertex fetch is useful only after a native vertex shader can consume
the application's declaration, or after a separate post-TNL upload is shown
to outperform immediate vertices.  It is not required by glmark2 `build`.

## Evidence set

The proprietary images used here are:

```text
VTGUModeDX32A.dll  sha256 8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll      sha256 a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
via_chrome9_dri.so sha256 fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
```

The Windows D3D DDI exposes distinct `SetIndices`, `SetStreamSource`,
`DrawPrimitive`, and `DrawIndexedPrimitive` entries at
`VTGUModeDX32A.dll:10092bd0`, `100934e0`, `10092c30`, and `10092c70`.
Their implementations begin at `100b3210`, `100b3280`, `100b3360`, and
`100b3400`.  The ordinary hardware callback is `10156b20`; the byte-matching
OpenGL callback is `VTGOGL32A.dll:102019a0`.

The exact vertex-stream and inline-index construction is independently
recovered in [raster-front-end-oracles.md](raster-front-end-oracles.md).
The topology cycles and count semantics are in
[windows-draw-path.md](windows-draw-path.md).  This note derives the driver
boundary and application tests from those bit-level results.

## What glmark2 `build` actually submits

The matching glmark2 2023.01 source does not issue an indexed draw in the
`build` scene.  `Model::append_object_to_mesh()` expands every face into
three complete vertices.  The cube reports 12 faces, hence the mesh contains
36 vertices even though the source model reports only 20 vertices.
`Mesh::render_array()` and `Mesh::render_vbo()` both execute:

```c
glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
```

`use-vbo` changes only where the two input attributes live:

| option | position input | normal input | draw |
|---|---|---|---|
| `use-vbo=false`, default `interleave=false` | user float3 array | user float3 array | arrays, 36 vertices |
| `use-vbo=true`, default `interleave=false` | PIPE_BUFFER float3 | separate PIPE_BUFFER float3 | arrays, 36 vertices |
| `interleave=true` | offset 0 in one six-float record | offset 12 in the same 24-byte record | arrays, 36 vertices |

The scene does not pass texcoords to this shader.  The fragment shader log
shows its one smooth scalar `Color` input expanded to RGB with alpha one.
The current Chrome9 classifier therefore uses its bounded scalar-primary
path; no sampler, native index mode, or extra FVF element is implied.

The canvas establishes D16, `LEQUAL`, and back-face culling globally with
`glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`,
`glEnable(GL_CULL_FACE)`, and `glCullFace(GL_BACK)`.  This is the material
packet distinction from the earlier color-only triangles.  The build logs
already reached the scene with `use-vbo=false`; the last recorded successful
run was:

```text
[build] model=cube:nframes=1:use-vbo=false
FPS: 143  FrameTime: 7.009 ms
```

That is execution evidence, not a visual-conformance checksum.  The next
test must compare rendered images for all array/VBO/interleave forms.

## Current Mesa route is already indexed-draw capable

`chrome9_draw_vbo()` maps each user or resource vertex buffer and maps index
buffers of API element sizes 1, 2, or 4 bytes into Mesa draw.  Because
`chrome9_vbuf_need_pipeline()` always returns true, draw performs the vertex
shader, homogeneous clipping, viewport transform, culling, flat-shade fixup,
and primitive decomposition before the Chrome9 callback sees vertices.

The vbuf boundary deliberately uses 16-bit *post-transform* indices:

```text
max_indices             = UINT16_MAX
max_vertex_buffer_bytes = UINT16_MAX * 8 * sizeof(uint32_t)
```

This does not limit application index elements to 16 bits.  Mesa draw fetches
8-, 16-, or 32-bit application indices, applies the draw start and index bias,
executes primitive restart by splitting runs, shades/clips vertices, and
builds a bounded temporary vertex array addressed by the callback's `uint16_t`
indices.  Its vbuf allocator splits work before that temporary namespace can
overflow.

The current callback accepts these triangle-producing modes and expands them
to explicitly ordered independent triples:

| mode | emitted triples |
|---|---|
| triangles | `(i,i+1,i+2)` in steps of three |
| triangle strip | even `(i,i+1,i+2)`, odd `(i+1,i,i+2)` |
| triangle fan or polygon | `(first,i,i+1)` |
| quads | `(0,1,3)`, `(1,2,3)` per four vertices |
| quad strip | `(i,i+1,i+3)`, `(i,i+3,i+2)` per pair |

The odd strip swap preserves winding.  The quad split matches the established
glamor diagonal and must remain stable because interpolation along a different
diagonal is observably different.  Points, lines, line loops, patches, and
adjacency modes are currently rejected at this callback and must not be
advertised as accelerated merely because hardware topology words exist.

The equivalent native triangle words are fully decoded, but only the list is
needed after software decomposition:

| topology | A20 immediate | A20 16-bit indexed | A23 Gouraud | A24 |
|---|---:|---:|---:|---|
| triangle list | `20000000` | `20000001` | `23004f02` | `3 * primitives` |
| triangle strip | `201c7800` | `201c7801` | `23004f02` | `primitives + 2` |
| triangle fan | `201c5c00` | `201c5c01` | `23004f02` | `primitives + 2` |

For 32-bit indexing bit 1 joins bit 0, so the indexed low bits are `3` rather
than `1`.  A24 always counts vertices or indices exactly; it never counts
packed payload dwords.  Keeping only the native list form means strip reuse,
fan anchoring, and their transition state cannot leak into application draws.

The software pipeline also gives the safest culling contract.  It uses the
Gallium `front_ccw` and `cull_face` state after clipping and before vbuf
delivery.  The hardware packet remains Gouraud triangle-list state with
hardware back-face culling disabled.  This avoids coupling API winding to
the framebuffer Y orientation or to the still-static A23 orientation bit.

## Exact native Chrome9 stream format

Native buffer-fed draws use two packet namespaces:

1. H3 type-03 subtype `30` supplies up to 16 stream base/pitch words.
2. H2 type-04 A00--A10 describes elements and the global memory domain.

For stream `s`:

```text
stream[s] = (byte_address & 0xfffffc00) | (byte_pitch >> 2)
```

The base is 1-KiB aligned.  Pitch is a nine-bit dword count, so directly
encodable pitches are multiples of four through 2044 bytes.  An element word
A01--A10 contains stream ID `[23:20]`, byte-address bits 9:2 as `[19:12]`, VS
input register `[11:8]`, and format `[7:0]`.  A00 `[16:15]=01` selects SF,
`[11:8]` names the last element, and `[6:0]` is total vertex dwords.

The currently proven post-TNL record is six dwords:

```text
00008206  # SF stream, elements D0..D2, six dwords
010000c2  # D0 +0: float4 position/RHW -> IR0
020041d1  # D1 +16: normalized D3DCOLOR -> IR1
030052d1  # D2 +20: normalized D3DCOLOR -> IR2
```

The associated stream-0 packet for a 24-byte record is:

```text
fe03043c 00000001 30030000 cc000000
<1-KiB-aligned SF VA | 00000006> cc000000 cc000000 cc000000
```

Do not confuse this with raw glmark2 vertex data.  Its separate position and
normal float3 inputs need two streams and a native VS that transforms position
and evaluates lighting.  The existing hardware program instead consumes
already transformed D0 plus packed D1/D2.  Pointing that program at the raw
application VBO would reinterpret normals or adjacent positions as colors and
RHW.  Uploading software-TNL output into a new SF BO merely to fetch it back
usually adds a copy and ownership transition compared with the live immediate
payload.

## Exact native inline-index format

Chrome9 does not take an index-buffer address in this path.  Type-04 A20
selects index mode, then a type-00 command packet carries indices inline:

| application element size | A20 `[1:0]` | payload |
|---:|---:|---|
| no native indexing | `00` | immediate vertex records |
| 16 bit | `01` | two little-endian indices per dword |
| 32 bit | `11` | one index per dword |

A24 is the exact number of indices, not the number of payload dwords.  The
payload length is `ceil(A24/2)` for 16-bit indices and `A24` for 32-bit
indices.  A final unused 16-bit lane must be deterministic zero; it lies
outside the A24 count but remains inside command memory read by the engine.

The allowed vertex-index bounds are not addresses:

```text
A2C = 2c000000 | (max_index & 00ffffff)
A2D = 2d000000 | (min_index & 00ffffff)
A2E = 2e000000 | ((max_index >> 16) & 0000ff00)
                 |  (min_index >> 24)
```

For `{0,1,2}`, the vendor-exact bounds and payload packets are:

```text
fe020440 0000043c 00040000 cc000000
2c000002 2d000000 2e000000 cc000000

fe03043c 00000002 00000000 cc000000
00010000 00000002 cc000000 cc000000
```

`VTGUModeDX32A.dll:101b0a20..101b0abd` emits the A24-only type-04 packet,
aligns, then emits the H3 header.  The neighboring helper
`101b0ad0..101b0b9a` emits A23+A24 before the same H3 framing.  The Linux H6
triangle-list emitter independently chunks indexed lists, re-emits A24, and
copies packed inline indices.  Therefore native indexing reduces repeated
vertex payload but does not eliminate command-side index copying.

## Kernel validation boundary for native streams

A future native-stream ioctl must remain handle-based and kernel-derived.  It
must not accept raw SF addresses, declaration words, A20, or A2C--A2E.
Before ring publication the kernel must:

1. copy and bound the inline index list; accept only 16 or 32 bit elements;
2. compute the true minimum and maximum after any supported bias, never trust
   userspace extrema;
3. reject restart values unless the kernel itself splits the list before
   forming A24 and payloads;
4. for every element, check `base + element_offset + max_index*pitch + width`
   against that stream BO, with checked arithmetic;
5. require the one-KiB stream-base alignment and fold address bits 9:2 into
   each element instead of truncating them;
6. derive format, stream number, VS input, SF domain, A24, A20, and all bounds
   words from a small enumerated ABI;
7. pin every stream mapping through synchronous retirement or its fence, then
   perform the established DMA/GART ownership transitions before unpinning;
8. assert exact packet body end, alignment, PAUSE, STOP, and read-ahead limit.

The first ABI should support only the six-dword post-TNL declaration above,
one SF stream, triangle lists, and index bias zero.  Multi-stream raw API
formats belong behind native-VS validation, not in the first live gate.

## Implementation order

### Immediate correctness milestone

Keep Mesa draw in the mandatory software pipeline.  It already covers user
arrays, VBO resources, 8/16/32-bit application indices, clipping, culling,
primitive restart, index bias, and triangle-producing topology decomposition.
Keep hardware culling off and continue submitting only independent triangle
lists.  Extend batching before native fetch: untextured glmark2 `build` fits
inside the established 34-triangle batch cap even before software culling,
because the cube is only 12 triangles.

The implementation audit should assert these fail-closed conditions:

- every mapped vertex/index range has a real size for resource buffers;
- the post-TNL callback index is below `vertex_count` and its complete record
  lies within `capacity`;
- position X/Y, required Z/RHW, and consumed varyings are finite;
- clipped/viewport-converted 16.16 area is nonzero;
- batch state is immutable and every appended triple passed the same checks;
- unsupported point/line/patch modes increment a diagnostic rejection rather
  than silently succeeding.

### Performance milestone

Add textured and D16+textured triangle batching using the existing immediate
post-TNL records.  This attacks the synchronous ioctl cost relevant to
glmark2 without exposing new address-bearing state.  Only if profiles still
show vertex payload/copy cost should the guarded one-stream native-index
oracle be promoted to a structured UAPI.

### Native VS/VBO milestone

Once native VS translation accepts bounded float3 position/normal layouts,
add two independently pinned streams and the exact declaration generated
from pipe vertex elements.  At that point native indexing can retain shared
vertices and make glmark2-style model VBOs useful.  Until then, advertising a
native VBO path changes no API capability and risks a new GART fetch wedge.

## Tests required before changing the route

### Software-path corpus

1. Draw the same uniquely colored two-triangle square using user arrays and
   PIPE_BUFFER VBOs, first as separate float3 streams and then as one
   interleaved 24-byte stream.  Require identical target hashes.
2. Repeat through `glDrawElements`/Gallium indexed draws with 8-, 16-, and
   32-bit elements, nonzero `draw.start`, positive and negative valid index
   bias, and resource-backed versus user indices.  Require the same ordered
   post-TNL triples.
3. Exercise triangle list, four-vertex strip, four-vertex fan, quad, and quad
   strip with a different color at every logical vertex.  The strip oracle
   must prove the odd `(i+1,i,i+2)` swap; the quad oracle must prove the fixed
   `1--3` diagonal.
4. Insert primitive-restart indices into strips and fans.  Compare with two
   explicit API draws so a restart cannot accidentally bridge primitives.
5. Clip a triangle against each of the six homogeneous planes, including a
   varying-RHW textured case.  Require finite generated attributes, in-range
   16.16 coordinates, correct interpolation, and no degenerate submission.
6. Run paired CW/CCW triangles under cull-none, cull-front, and cull-back.
   Verify software rejection counts and keep hardware A01 cull disabled.
7. Cross both `UINT16_MAX` temporary limits and every kernel batch cap to
   prove Mesa splits without changing order, provoking vertex, or coverage.

### glmark2 gate

Run one deterministic cube frame for all four combinations of
`use-vbo=false/true` and `interleave=false/true`.  Capture the render target
before presentation and require:

- identical nonzero image hashes and changed-pixel counts;
- identical accepted/rejected triangle counts;
- D16 guards, color guards, and source VBO guards intact;
- engine idle and MMIO error `040c=0` after every variant.

This test distinguishes VBO ingestion from the known X/Present path.  A score
or clean process exit alone is insufficient.

### Native inline-index live gate

Only after the software corpus passes, use a private guarded SF BO containing
three known six-dword post-TNL records and inline 16-bit indices `{0,1,2}`.
Start from the live immediate triangle and change only:

```text
FVF A00       00000206 -> 00008206
primitive A20 20000000 -> 20000001
add bounds    A2C=2c000002 A2D=2d000000 A2E=2e000000
replace vertex payload with the stream-0 packet and packed index packet
```

First use three distinct primary colors, then permutation `{0,2,1}` to prove
that vertex fetch and winding both follow the indices.  Follow with 32-bit
index mode in a separate boot-bounded experiment.  Out-of-range and overflow
cases are validator unit tests only: never publish an intentionally invalid
fetch to live hardware.  Stop on any retirement timeout, nonzero `040c`,
persistent CR/VP/SF busy bit, guard change, or loss of the known PAUSE state.

## Decision

For the immediate post-glxgears milestone, indexed application draws and VBOs
remain software-fetched through Mesa draw and hardware-submitted as immediate
triangle lists.  This is not a fallback in API semantics: it preserves
indices, clipping, culling, restart, bias, and VBO/user-buffer equivalence.
The native Chrome9 stream/index protocol is sufficiently decoded for a later
guarded oracle, but it is neither necessary for glmark2 `build` nor safe to
connect to raw application declarations before native vertex-shader linkage
and complete kernel range validation exist.
