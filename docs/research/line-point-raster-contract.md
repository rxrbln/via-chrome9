# Chrome9 line and point raster contract

Status: exact static topology/state reconstruction and bounded oracle plan,
2026-09-21.  The current Mesa Chrome9 vbuf path accepts only triangle-derived
primitives; ordinary points and one-pixel lines reach its default rejection.
No line/point capability should be advertised until either Mesa expands the
primitive to proven triangles or the native packet below passes coverage,
depth, blend, and transition tests.

## Current software-path gap

`chrome9_vbuf_need_pipeline()` always returns true, but that alone does not
turn every primitive into triangles.  `chrome9_vbuf_draw_arrays()` and
`draw_elements()` handle triangles, strips, fans, polygons, quads, and quad
strips, then reject the default case.  Points and line families therefore do
not reach an ioctl.

Gallium draw already has useful expansion stages, but its defaults explain
the gap precisely:

* `wide_point_threshold` starts at a very large value, so an ordinary point
  passes through; setting it to zero can expand every positive-sized point;
* `wide_line_threshold` starts at one, so lines wider than one become two
  triangles; and
* the wide-line selection explicitly requires `line_width != 1.0`, so even a
  zero threshold cannot force a one-pixel line through that generic stage.

Thus a conservative correctness path can set the point threshold to zero and
enable draw's point-sprite expansion when needed.  Wide lines already use
draw.  Exactly-one-pixel lines need either a Chrome9-specific expansion stage
using the same half-pixel/major-axis rules or the native topology below.  Do
not perturb width 1 to a nearby float merely to trigger the wide-line stage;
that changes coverage and violates the API.

## Exact proprietary topology builders

The Windows D3D point-list builder is
`VTGUModeDX32A.dll:0x101f4eb0..0x101f4f44`.  Its exact payload changes at
`0x101f4ec5..0x101f4ee6` are:

```text
type-04 A20 &= ffc000ff             # clear reuse/cycle bits [21:8]
type-04 A23 &= ffff1fe0
type-04 A23 |= 00001e00             # point, flat-A state
```

From the current command-mode Gouraud state this gives:

```text
A20 = 20000000
A23 = 23001f00
A24 = 24000000 | point_count
```

The Linux proprietary point path at `via_chrome9_dri.so:0x1bbc32..0x1bbc53`
performs the same byte operations.

The Windows line-list builder begins at `0x101f4f50`.  Its normal non-AA
branch at `0x101f509c..0x101f50a4` clears the topology/edge low bits and ORs
`0x0e01`, producing:

```text
A20 = 20000000
A23 = 23004f01                  # line topology, Gouraud, all edges
A24 = 24000000 | (2 * line_count)
```

The independently compiled line-strip builder begins at `0x101f5140`; its
normal cycle at `0x101f52cc..0x101f52de` installs:

```text
A20 = 20206000
A23 = 23004f01
A24 = 24000000 | (line_count + 1)
```

The corresponding Linux builders emit the same words.  Specialized edge-AA
branches install additional A20 cycle state and are deliberately excluded
from the first gate.

The full topology table relevant to later work is:

| topology | A23 low nibble |
|---|---:|
| point | 0 |
| line | 1 |
| triangle | 2 |
| rectangle | 4 |
| wireframe triangle | 6 |
| point triangle | 7 |

## Common state that must remain closed

The established type-01 word `01c01540` already has A01 bits 23 and 22 set,
selecting the canonical line direction and diamond rule.  For the first
native test preserve it exactly.  In particular keep:

```text
A01 bit 21 = 0       point sprite disabled
A01 bit 5  = 0       edge anti-aliasing disabled
A01 bit 1  = 0       line stipple mode disabled
A01 bit 0  = 0       line pattern disabled
```

Blend, alpha test, color mask, scissor, cull, depth, and stencil state are
orthogonal packet namespaces, but the initial oracle disables all except
color.  Hardware culling has no effect on true point/line topology and should
remain disabled rather than relying on that assumption.

The immediate vertex record remains the established normalized six-dword
post-TNL layout:

```text
float x, float y, float z, float rhw, D3DCOLOR diffuse, D3DCOLOR specular
```

Keep declarations `020041d1/030052d1`; using the old unnormalized `d0`
reintroduces the measured nonzero-alpha saturation bug.

## Point dimensions without packet growth

Type-04 A2A and A2B carry default point width and height/minimum in signed
`s[8].15` form.  The proprietary builder calls its `FloatToP_E8_N(...,15)`
conversion and clamps the supported size to 1..2048.  Exact useful values are:

```text
1x1: A2A=2a008000  A2B=2b008000
2x2: A2A=2a010000  A2B=2b010000
```

The current type-04 raster/FVF group has 26 payload dwords followed by two
alignment pads before the next header.  Insert A2A/A2B after A28 and before
A40.  The group then has 28 payload dwords and consumes those two pads, so
every later packet index remains unchanged.  In the current untextured body:

```text
body[48] A20
body[49] A23
body[50] A24
body[51] A25
body[52] A28
body[53] A2A            # newly occupies old alignment capacity
body[54] A2B
body[55] A40            # shifted inside the same aligned group
...
next H2 header remains body[68]
immediate H2 header remains body[160]
```

For a blend-aware body add the existing four-dword destination delta to these
indices; the immediate header remains body 164.  The same no-net-growth rule
applies to the one- and two-texture raster/FVF arrays because they use the same
26-word group shape.

Every point packet must emit A2A/A2B.  Triangle/line packets may leave the
last point size inert, but a later point must never inherit an earlier size
accidentally.

## Exact color-only immediate bodies

Starting from the current untextured builder and retaining all shader/FVF/
viewport state, change only A20/A23/A24, insert the point-size words for point
topology, and emit the required number of six-dword vertices.

Without blend, the immediate header remains body 160..163 and vertices begin
at 164:

| primitive | vertex count | A20 | A23 | A24 | body end including two pads |
|---|---:|---:|---:|---:|---:|
| one point | 1 | `20000000` | `23001f00` | `24000001` | 172 |
| one line | 2 | `20000000` | `23004f01` | `24000002` | 178 |
| two-point line strip | 2 | `20206000` | `23004f01` | `24000002` | 178 |
| reference triangle | 3 | `20000000` | `23004f02` | `24000003` | 184 |

With the existing blend closure, the state and immediate headers move by four
dwords, so point/line/triangle ends are 176/182/188.  These variable ends are
well within the current ring bound and do not alter retirement framing.

Do not encode a zero vertex count.  For line list require an even count; for
line strip require at least two; for points require at least one.  Cap the
count from the same page/read-ahead equation as triangle batches rather than
trusting A24.  The kernel generates A24 and all H2 lengths.

## Safe first live coverage oracles

Use a 32x32 guarded BGRX target, no depth/blend/scissor, opaque unique colors,
and the same post-viewport pixel-center convention as the live triangle path.

### Point

1. Emit isolated 1x1 points at integer and half-integer coordinates.  Record
   the single covered pixel for each; every point must change exactly one
   pixel and no neighboring byte.  This measures rather than assumes the
   point-center phase.
2. Repeat the measured center with `2a010000/2b010000`; require an exact 2x2
   footprint around the same phase and no overlap with a second point.
3. Run `1x1 -> 2x2 -> 1x1` without reinitializing state to prove A2A/A2B
   closure.

Point sprites, per-vertex PSIZE, smoothing, and point attenuation remain off.
They add shader-varying and interpolation requirements absent from this gate.

### One-pixel line

1. Draw horizontal, vertical, x-major diagonal, and y-major diagonal lines
   well inside the target.  Repeat every segment with endpoints reversed;
   the coverage set must be identical.
2. Join two segments at one endpoint using line-list and then line-strip
   topology.  Compare the shared-pixel ownership and require no crack or
   double blend.  This distinguishes A20 list versus strip reuse state.
3. Repeat a horizontal and diagonal segment at integer and half-integer
   offsets to measure the diamond-rule phase selected by A01 bits 23/22.
4. Run `line list -> triangle -> line strip -> point -> triangle` and verify
   that A20 reuse, A23 topology, A24 count, and point size do not leak.

The first oracle uses COPY.  Then repeat source-over with alpha 0x80 over a
known target; any doubly covered shared endpoint is visible immediately.
Only after color coverage passes add scissor, D16 LESS/read-only, and
interpolated Z/color.

At every step check all guards, exact unchanged pixels, retirement cookie,
work-mask clear, and `040c==0`.  A pixel mismatch keeps the native path
disabled; a timeout or guard mutation invokes quarantine.

## Offset, stipple, and AA boundaries

Windows selects point, line, and fill polygon-offset enables independently
before producing the common A16--A18 bias words.  Native line/point topology
therefore must not inherit `offset_tri`.  Until point/line bias has its own
oracle, either expand through draw (which selects `offset_point/offset_line`)
or reject those native combinations.

Line stipple uses A84 low 16 as the LSB-first pattern and A85 low 16 as repeat;
A85 bit 23 resets the counter and self-clears.  A first future pattern is
`8400aaaa 85800001` on a 16-pixel horizontal line.  Close A01 bit 0 and reset
A84/A85 on disable.  Smooth/AA lines use the proprietary special-cycle branch
and remain separate from both stipple and the normal `23004f01` recipe.

The safest immediate software/native split is therefore:

* points and point sprites: draw expansion to triangles first;
* wide lines: existing draw expansion;
* one-pixel non-smooth, non-stippled lines: native only after the coverage
  oracle, otherwise a Chrome9-specific draw expansion;
* native point/line depth bias, smoothing, stipple, and sprites: deferred.

No command or capability number is reserved by this note.  The exact packet
is ready for an internal raw diagnostic, but a public structured ABI should
be allocated only after that diagnostic proves coverage and after its state
envelope (color-only versus depth/blend/textured) is chosen from an observed
application blocker.
