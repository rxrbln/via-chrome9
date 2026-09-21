# Chrome9 line/polygon stipple and unfilled-mode contract

Status: exact line-pattern packet reconstruction and fail-closed polygon
stipple/unfilled policy, 2026-09-21.  This follows the AA/line-cycle work in
[msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md).  It
does not reserve a UAPI command or capability.

## Result

Chrome9 has a small native line-stipple block, but the counter reset is part
of every primitive packet and cannot be omitted.  Polygon stipple is not the
same mechanism: it uses a 32-dword type-03 table plus A01 bit 1, and both old
Mesa and the proprietary ICD contain fallback/patch machinery around it.
The current Chrome9 Gallium `set_polygon_stipple()` no-op must not be treated
as support.

The safest order is:

1. prove ordinary native one-pixel lines;
2. add line stipple to that exact line command, with explicit reset;
3. let Gallium draw lower unfilled triangles to the proved line/point paths;
4. lower polygon stipple through a verified fragment discard/texture key; and
5. investigate the native 32x32 table only as an optimization.

## Exact line-stipple words

The manual defines:

```text
A01 bit 0       line-pattern enable
A84[15:0]       16-bit pattern, consumed least-significant bit first
A85 bit 23      reset repeat counter; hardware self-clears it
A85[15:0]       number of pixels for which each pattern bit is repeated
```

Linux independently implements all three pieces:

* `STM_LineStippleEnable_INV:0x194530..0x194574` clears/sets A01 bit 0;
* `STM_LinePattern_INV:0x194d80..0x194de3` copies two 16-bit semantic values
  to hardware-image offsets `+0x210/+0x214`, exactly A84/A85; and
* both primitive builders set byte `+0x216` bit 7, complete-word A85 bit 23,
  before emission.  Line strip does so at `0x1bbdd1`; line list at
  `0x1bc0b1`.

The Windows builders match.  `VTGUModeDX32A:0x101f50aa` sets A85 bit 23 for
line list and `0x101f52a0` does so for line strip.  The reset is emitted even
on the normal non-AA path.  Therefore the complete first test state is:

```text
A01 = 01c01541                    # baseline plus line-pattern enable
A84 = 8400aaaa                    # alternating, first pixel uses bit 0
A85 = 85800001                    # reset plus repeat factor 1

line list:  A20=20000000 A23=23004f01
line strip: A20=20206000 A23=23004f01
```

Do not combine this first test with smooth lines.  The recovered smooth path
changes A23 to `23004506` and strip A20 to `202c6c00`; stipple plus that
wireframe cycle needs its own image oracle.

The proprietary GL entry point `__glim_LineStipple:0x3a83c0` clamps the API
repeat factor to `1..255` before storing it.  The manual field is 16 bits, but
the initial structured validator should mirror the executable evidence:

```text
pattern: any 16-bit value
factor:  1..255
```

Gallium stores `line_stipple_factor` as the API factor minus one; its draw
stipple stage adds one.  Mesa must perform that conversion exactly once.
Pattern zero can be discarded as a software no-op.  Pattern `ffff` can use
the ordinary solid line path, but only if doing so also preserves the API's
counter/reset boundary.

### Packet growth

The current type-01 clip packet contains sequential A80--A83 and ends on a
16-byte boundary.  Extending the same packet with A84/A85 consumes two
payload dwords plus two alignment pads.  Everything after the clip packet
moves by exactly four dwords:

```text
color, no blend: immediate header 160 -> 164; vertices 164 -> 168
color, blend:    immediate header 164 -> 168; vertices 168 -> 172
```

For one two-vertex line the complete body end consequently moves from
`178 -> 182` without blend and `182 -> 186` with blend.  A semantic line
successor must generate those positions and assert them before publication;
it must not splice A84/A85 into a hard-coded triangle body without shifting
the downstream headers.

Every submitted stippled primitive must emit all of A01, A84, and A85.  On
disable, rebuild A01 with bit 0 clear and emit neutral A84/A85 state rather
than relying on self-clearing bit 23.  A suggested closure tuple is
`8400ffff 85800001` even though A01 disables its use.

## Counter semantics and bounded oracles

Use a 64x32 guarded BGRA target with integer-aligned horizontal lines away
from boundaries.  Keep A01 direction/diamond bits 23/22 set, A23 last-pixel
bit 15 clear, smoothing off, and use COPY first.

1. Pattern `aaaa`, factor 1, 16-pixel line: bit 0 is zero, so the first
   candidate pixel should be absent and coverage should alternate.  Record
   the measured pixel-center phase rather than assuming which framebuffer
   pixel is candidate zero.
2. Pattern `0001`, factors 1, 2, 3, and 255: require runs of exactly the
   requested factor followed by the appropriate off span.  Keep the factor
   255 line long enough to observe the transition.
3. Reverse the endpoints.  A01 bit 23 should canonicalize horizontal
   direction, so the framebuffer pattern must not reverse.
4. Submit two independent line-list members in one A24 list and then in two
   submissions.  This determines whether hardware resets automatically per
   member or only from A85.23.  Until they match GL semantics, accept only one
   independent line per request.
5. Submit a connected three-vertex strip once.  Do not split a stippled strip
   between command segments: there is no recovered readable counter or
   semantic carry field, and re-emitting A85.23 would restart its phase.
6. Run `stipple -> solid -> stipple` and source-over alpha `80` to expose both
   stale A01 state and duplicate endpoint ownership.

The validator permits line topology only, rejects zero vertices, requires an
even list count or at least two strip vertices, and bounds the whole stippled
strip to one command segment.  It rejects smoothing, polygon stipple, point
topology, non-unit line width, and a changing pattern/factor inside a strip.

## Polygon stipple is a separate, unsafe native path

The hardware's polygon mask is the type-03 subtype `14h` **Stipple Palette**,
32 dwords, selected by A01 bit 1.  Linux
`STM_PolygonStippleEnable_INV:0x1944e0..0x194524` toggles precisely that bit.
`STM_PolygonStipplePattern_INV:0x194450..0x1944da` walks 32 source rows,
byte-swaps each dword, stores rows in descending hardware-image order, and
marks the whole table dirty.  That is not enough to define OpenGL's window
origin and bit orientation because the API storage path can preconvert the
source before this routine runs.

The fallback evidence is material:

* old Mesa 7.11.2 has the native palette upload under `#if 0` and states that
  patterns such as `f0f0f0f0` can make the hardware refuse the stipple;
* that disabled code reverses all bits within each row and emits rows 31 to
  0, showing the API/hardware orientation problem;
* the proprietary ICD has `__glDpLoadPolygonStippleImage` and
  `__glDpEnablePolygonStipple` patch paths which construct/upload an image,
  in addition to the low-level A01/table state; and
* current Chrome9 Mesa installs `set_polygon_stipple()` as a no-op and does
  not install Gallium draw's polygon-stipple stage.  A rasterizer with
  `poly_stipple_enable` can therefore currently reach an unmasked triangle.

Initial Mesa must reject/fallback such a draw, not silently ignore the mask.
Once general fragment lowering exists, the safest implementation is Mesa's
polygon-stipple texture/discard lowering on a reserved internal sampler.  It
can be tested like an ordinary guarded alpha-kill shader and avoids exposing
the table hang erratum to the kernel.  Native table promotion requires all of
these exact tests:

* all-zero, all-one, checkerboard, `f0f0f0f0`, and asymmetric single-bit rows;
* nonzero drawable and viewport origins to close the 32x32 window phase;
* front/back orientation and scissor clipping;
* depth-write, stencil, alpha test, blend, and color-mask-zero interactions;
* `enabled -> disabled -> enabled` with full table re-emission; and
* clean retirement plus a following 2D fill for the historically failing
  `f0f0f0f0` pattern.

Any hang or engine error permanently disables native polygon stipple for the
module lifetime.  There is no value in assigning a public capability before
the shader fallback or all native-table tests pass.

## Unfilled front/back polygon modes

A23 supports native triangle wireframe (`PMType=6`), triangle point
(`PMType=7`), and different physical CW/CCW modes through bit 4 plus bits
`[6:5]`.  Those fields interact with edge flags, culling, the smooth-line
special cycle, and the physical-winding mapping.  They are unnecessary for
correctness now.

Gallium draw's unfilled stage already resolves API front/back after
`front_ccw`, emits lines or points, and applies offset-line/offset-point in
the matching stage.  Once the bounded line/point vbuf outputs work, use that
software lowering.  This avoids a second two-sided physical-orientation ABI
and naturally preserves flatshade/provoking-vertex behavior.

Until then, a draw whose effective `fill_front` or `fill_back` is not FILL
must be rejected if the resulting line/point primitive cannot be submitted.
Do not let the original filled triangle through.  Native A23 two-face mode is
an optimization rung after line/point coverage, cull, stipple, AA, and bias
oracles are all live.
