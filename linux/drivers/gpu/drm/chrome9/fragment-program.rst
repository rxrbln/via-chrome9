.. SPDX-License-Identifier: GPL-2.0-only

Chrome9 semantic fragment programs
==================================

The ``DRAW_FRAGMENT_PROGRAM`` ioctl accepts fixed-width semantic records.  It
never accepts native command or shader words.  The kernel validates SSA use,
component definitions, resources, object layouts, and then emits the native
ALU, TAU, transition maps, and resource controls.

Temporary and coordinate allocation
-----------------------------------

Virtual temporary indices range from 0 through 127.  Disjoint component
writes may assemble a virtual temporary; overlapping definitions and reads of
undefined components are rejected.  One physical r0..r11 allocation is kept
from the first component definition through the final component read.

Direct interpolated texture coordinates are compacted by logical varying
index into coordinate slots 0..7.  A computed TEMP coordinate is copied by a
kernel-generated MOV into a class-one coordinate slot immediately before its
TAU.  Computed slots cycle over the slots above the persistent direct-input
prefix.  A program needing both eight direct coordinates and a computed
coordinate is rejected.  Lifetime markers are derived only after physical
allocation and cover the last read of a physical slot across the whole
program.

The ``FRAGMENT_EXTENDED_LINKAGE`` capability means that each structured draw
also writes the ordered direct-input FE fields A01--A03 and accepts up to
eight descriptor slots.  Direct ALU and TAU reads share the same ascending
logical-T order; the sampler selected by a TAU does not determine its FE
field.  Textured draws cap each consumer width at three components, while
the independently qualified ALU-only path retains a four-component width.
Every draw writes the inactive A02/A03 baseline to prevent stale high fields
after a program with fewer inputs.  This capability is absent from older
kernels whose FE image only programs the first consumer field.

The smallest computed-coordinate production oracle is::

  ALU 008106d0 d0140924 00308006 00000020
      01c00530 c01c0053 00f20006 000d0420
      01c00530 301c0053 00f20105 000d0820
  TAU 301101b0
  CTL 90058000 91000001 92010003 93000001
      94000000 95000002 96000000 97000000
      98000000 99000000 9a000000 9b000000

The compiler self-test compares every word against this oracle and also
compiles a two-write partial temporary before engine initialization.

Texture layout and LOD
----------------------

Each active level has an explicit byte offset and canonical 32-byte-aligned
pitch.  The selected first level begins after the leading guard page.  Levels
must be ordered, non-overlapping, and 256-byte aligned; the exact BO ends in a
final guard page.  BGRX/BGRA chains may contain up to twelve levels.  Entries
L1..L8 relative to the selected base are generated into the stage-specific H3
table.  Relative levels beyond eight must follow the validated consecutive
256-byte tail rule.  A8 and X8Z24 remain level-zero-only.

LOD bias uses signed S5.5 in ``[-512, 511]``.  Version one requires minimum
and maximum LOD clamps to be whole-level S5.5 values and encodes them relative
to the selected first level.  The texture-state self-test checks a two-level
trilinear chain, negative half-level bias, H3 offset, sampler words, and exact
guarded allocation size.

Unproven state fails closed: control flow, coordinate slots 8 and 9, border
addressing, fractional LOD clamps, and mipmapped A8/X8Z24 are rejected.
