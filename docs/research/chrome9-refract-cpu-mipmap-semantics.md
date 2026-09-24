# Chrome9 refract CPU mipmap semantics and ownership audit

## Result

The Chrome9 BGRA CPU generator is a conforming implementation for glmark2
`refract`.  For every generated texel it performs the same per-channel,
unsigned-byte, truncating 2x2 reduction as Mesa's software fallback in
`src/mesa/main/mipmap.c`.  Mesa 7.11.2 used the same rule.  The current code
does not need round-to-nearest or a proprietary-driver pixel match before the
scene can be enabled.

OpenGL 3.0 section 3.9 specifies repeated filtered reduction but does not
require a particular filter; a box filter is only recommended.  Therefore a
different result from the proprietary driver's hardware blit is not a GL
conformance failure.  The specification is available at
<https://registry.khronos.org/OpenGL/specs/gl/glspec30.pdf>.

The current worktree also contains the necessary producer flush at the start
of `chrome9_generate_mipmap()`.  The submit ioctl is synchronous and returns
the rendered object to CPU ownership before that function reads L0.  A later
consumer submission flushes the CPU-written complete texture object and
returns it to device ownership.  No additional CPU-side cache operation is
needed in the Gallium callback.

One validation defect must be fixed before accepting that flush change: the
offline mipmap test passes a fake `struct pipe_context`, but the new call
casts it to `struct chrome9_context` and reads `vbuf_render` beyond the fake
object.  This is a test-fixture bug, not a driver synchronization bug.

## Exact BGRA rule

For source size `sw x sh`, destination size is
`max(sw >> 1, 1) x max(sh >> 1, 1)`.  For each stored B, G, R, and A byte:

```text
x0 = sw == 1 ? 0 : 2*x
x1 = sw == 1 ? 0 : 2*x + 1
y0 = sh == 1 ? 0 : 2*y
y1 = sh == 1 ? 0 : 2*y + 1

dst[x,y,c] = floor((src[x0,y0,c] + src[x1,y0,c] +
                    src[x0,y1,c] + src[x1,y1,c]) / 4)
```

`PIPE_FORMAT_B8G8R8A8_UNORM` applies that rule to alpha as well as color.
`PIPE_FORMAT_B8G8R8X8_UNORM` filters the three color bytes and writes the
storage alpha byte as `0xff`, matching the format's sampled-alpha-one
semantics.  Averaging storage-order BGRA bytes is equivalent to unpacking
RGBA, averaging, and repacking because the operation is independent per
channel.  No supported Chrome9 sRGB format reaches this byte path.

Mesa's current `do_span_rgba_unorm8()` uses the same integer `/ 4`.  Its
`make_2d_mipmap()` advances by two source rows and `do_row()` consumes source
columns in pairs.  Mesa 7.11.2 `do_row()` has the same column stride, unsigned
byte sum, and truncating division.  Chrome9 is therefore exact against the
software fallback, not merely approximately similar.

The difference from a round-to-nearest box filter is at most one byte value
at L1.  Across ten recursively generated levels, the accumulated deficit
from truncation is strictly less than `10 * 0.75 = 7.5` byte values relative
to recursively unrounded arithmetic over the retained samples.  This bound
does not imply a visible error, and constant colors are preserved exactly.

## Exact 1600x1200 NPOT chain

The refract-sized chain is:

```text
L0  1600 x 1200
L1   800 x  600
L2   400 x  300
L3   200 x  150
L4   100 x   75
L5    50 x   37
L6    25 x   18
L7    12 x    9
L8     6 x    4
L9     3 x    2
L10    1 x    1
```

Like Mesa's fallback, each odd reduction consumes consecutive pairs from the
origin and leaves the final unmatched row or column unused.  The exact edge
losses are:

| transition | unused source edge | corresponding L0 range |
|---|---|---|
| `100x75 -> 50x37` | L4 row 74 | y `1184..1199` |
| `50x37 -> 25x18` | L5 row 36 | y `1152..1183` |
| `25x18 -> 12x9` | L6 column 24 | x `1536..1599` |
| `12x9 -> 6x4` | L7 row 8 | y `1024..1151` |
| `3x2 -> 1x1` | L9 column 2 | x `1024..1535` |

Consequently L10 is derived from the L0 rectangle `[0,1024) x [0,1024)`,
with equal nominal sample weights but intermediate truncation.  This looks
surprising, but it is the established Mesa software policy and remains legal
because OpenGL does not define exact generation-filter taps or rounding.
Glmark2 does not have a byte-exact reference check for these generated
levels, so this cannot explain a scene validation failure.

## Proprietary-driver comparison

The supplied Linux 5.75 UMD does not use a host integer averaging loop:

* `__glS3InvGenerateMipmaps:0x234c50..0x234c8f` obtains each face's
  sub-mipmap surface and calls `sfmGenerateMipLevels(..., 2)`;
* `sfmGenerateMipLevels:0x000b7fb0..0x000b819d` dispatches to the inverse
  surface-manager implementation;
* `sfmGenerateMipLevels_inv:0x000e5f00..0x000e67e1` walks adjacent mip
  surfaces, copies their exact logical widths and heights from `+0x178` and
  `+0x17c` into source/destination rectangles, propagates filter argument two
  into the 3D-blit request, calls `sfm3DBlt_inv` at `0xe6399`, then kicks DMA
  and waits idle at `0xe63b5..0xe63c5`.

Thus the vendor result is hardware filtered and its final byte rounding and
NPOT edge taps cannot be derived from the UMD alone.  It is useful evidence
for the resource transition and per-level scheduling, but not a required
pixel oracle.  The Windows public GenerateMipmap wrapper requires a complete
ICD context and offers no safer hardware-free route to those hidden hardware
rounding details.

## Producer/CPU/consumer ownership proof

For the private guarded refract resource, the intended sequence is complete:

1. `chrome9_generate_mipmap()` calls `chrome9_draw_flush_batch()` before
   mapping L0.  This covers a caller which did not first change framebuffer.
   Refract already changes from its FBO to the canvas, whose framebuffer
   callback flushes the same retained batch.
2. The fragment submit ioctl waits for retirement, performs
   `dma_sync_sgtable_for_cpu()`, flushes the mapped range, and executes a read
   barrier before returning.  Its synchronous return therefore establishes
   CPU ownership of L0.
3. The generator reads level N and writes the nonoverlapping mapped payload
   of level N+1 in order.  The guarded resource layout keeps all levels in the
   same pinned BO, and no GPU work overlaps this loop.
4. A subsequent fragment consumer locks/waits on the same GEM reservation,
   flushes the full actual object mapping, calls
   `dma_sync_sgtable_for_device()`, and records the resource as a read-only
   texture.  This publishes every CPU-written mip level before sampling.

The direct-map fallback after `pipe_transfer` allocation failure remains
safe because the producer flush is performed once before any mapping attempt.

## Fixture-safe offline validation

`chrome9_mipmap_test.c` currently defines a fake object beginning with only
`struct pipe_context`.  After the producer-flush addition,
`chrome9_context(pipe)->vbuf_render` is outside that object.  A rebuilt test
was run from a `/tmp` copy with the incompatible T2 RPATH removed; Valgrind
reported the invalid read in:

```text
chrome9_generate_mipmap
  -> chrome9_draw_flush_batch
     -> chrome9_flush_triangle_batch
```

and the test terminated with SIGSEGV (a direct run terminated with SIGBUS).

The smallest safe correction is to make `struct test_context` begin with a
real `struct chrome9_context`, use `test.context.base` as the pipe context,
and leave `test.context.vbuf_render == NULL`.  The existing callback casts
remain valid because both nested base objects are at offset zero.  This fixes
the OOB while preserving the arithmetic, layout, map-contract, padding, and
guard checks.  The existing asymmetric `9x6 -> 4x3 -> 2x1 -> 1x1` vector
already exercises both odd-width and odd-height discard semantics.

That fixture does not prove a pending batch was submitted.  The live guarded
producer -> CPU mipgen -> consumer ladder remains the synchronization proof;
the offline test should only prove the pure mapping and byte-generation
contract.
