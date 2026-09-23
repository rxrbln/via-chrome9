# VX900 bounded mipmap successor oracle

## Scope and status

This note records the next texture feature after the live-proven level-zero
nearest and linear glmark2 cube.  The packet layout and allocation arithmetic
below are cross-checked against the Chrome9 manuals and proprietary Linux and
Windows drivers.  The implicit level-nine tail is not yet live-proven, so the
driver must not advertise mipmap filtering from this evidence alone.

## Current Mesa 512x512 BGRX layout

For a representative BO whose total allocation size is `0x159000`, Mesa's
guarded resource layout is:

| level | BO offset | pitch | data bytes |
|---:|---:|---:|---:|
| 0 | `0x001000` | `0x800` | `0x100000` |
| 1 | `0x102000` | `0x400` | `0x040000` |
| 2 | `0x142000` | `0x200` | `0x010000` |
| 3 | `0x152000` | `0x100` | `0x004000` |
| 4 | `0x156000` | `0x080` | `0x001000` |
| 5 | `0x157000` | `0x040` | `0x000400` |
| 6 | `0x157400` | `0x020` | `0x000100` |
| 7 | `0x157500` | `0x020` | `0x000080` |
| 8 | `0x157600` | `0x020` | `0x000040` |
| 9 | `0x157700` | `0x020` | `0x000020` |

The final data padding reaches `0x158000`; the final guard is
`0x158000..0x158fff`.  Relative to L0, L1..L8 therefore encode exactly as:

```text
FE03043C 00000008 10030000 CC000000
40001010 40001410 40001510 40001550
40001560 40001564 40001565 40001566
```

The H3 packet contains eight explicit successors.  The executed proprietary
Linux `sfmCalcMipOffset_inv` layout places L9 immediately after L8, while the
Linux and Windows state builders expose no ninth address word and A30 selects
through level 11.  This supports an implicit `L9 = L8 + 0x100` rule, but that
rule remains the final silicon uncertainty.

## glmark2 state and insertion point

The full 512-to-1 glmark sampler state is:

```text
A30 30000240    bias=0, min level=0, max level=9
A31 31102492    linear S/T, linear mip filter
A32 32000049    clamp-to-edge
```

Insert H3 after the six-word image payload and before the sampler packet.  In
the current D16 MODULATE builder, image payload remains body words 20..25; H3
header/count/selector/pad occupies 28..31, entries occupy 32..39, sampler
header moves to 40, and A30/A31/A32 move to 44..46.  This adds 12 dwords.  A
three-vertex D16 MODULATE body becomes 244 dwords without blend and 248 with
blend; each additional vertex remains eight dwords.

## Fail-closed live ladder

1. Implement `pipe_context.generate_mipmap` before exposing the sampler path.
   Chrome9 currently provides neither `generate_mipmap` nor `blit`, so Mesa's
   `util_gen_mipmap()` fallback can call a NULL `pipe->blit`.  The safest first
   implementation is CPU box filtering through the existing level-aware
   texture map/unmap path for BGRX/BGRA.  A smoke test must initialize an
   asymmetric L0, generate the complete chain, and byte-check every level,
   padding interval, and guard.  Do not use render-to-mip until Chrome9 render
   targets and clears track surface levels rather than level-zero resource
   fields.
2. Extend the structured texture state with width, height, and last-level
   metadata.  The kernel must reproduce the exact capability-defined Mesa
   layout and require the exact complete BO size; generic GEM metadata does
   not contain level offsets, and userspace must never supply native H3 words
   or unchecked offsets.  Preserve both guards by treating the existing L0
   end as the internal guarded boundary and the full allocation size as the
   final guarded boundary.
3. Prove an explicit L1 sample first using forced LOD/bias and distinct L0/L1
   colors.  Verify guards and engine error `0x040c`.
4. Fill L8 red and L9 green, then test forced L8, forced L9, and a trilinear
   L8-to-L9 transition.  This is the required implicit-tail oracle.
5. Only after those readbacks pass may Mesa stop rejecting mip filters and
   expose the glmark2 `texture-filter=mipmap` path.

The existing allocation layout is suitable; no resource repacking is needed.
The `linear-shader` glmark mode is unrelated: it performs four explicit TEX
operations and mix arithmetic with the ordinary level-zero sampler.
