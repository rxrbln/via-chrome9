# Guarded H6 depth and alpha-blend oracles

Status: the packet-exact D16, source-alpha, and combined alpha-before-D16
derivatives have executed successfully on the live VX900.  The D16 LESS/write
case retained the first red triangle, the source-alpha case produced the exact
two-pass `00408000` result, and the combined successor killed the near
alpha-`c0` fragment before its depth write then stored the farther alpha-`40`
green fragment as `0000ff00/8000`.  They use CPU clears and linear translated-
SF resources so depth/blend behavior is not confounded with an unverified GPU
clear or tiling path.

The common 188-dword source body and its measured result are in
[vp-stall-analysis.md](vp-stall-analysis.md).  Topology, shading, automatic
PLEND, scissor, and the 184-dword three-vertex reduction are in
[windows-draw-path.md](windows-draw-path.md).

## Resource and relocation contract

Use disjoint guarded allocations for color and depth.  Each resource has a
mapped middle page and one mapped poison page on either side:

```text
color: [4 KiB guard] [4 KiB X8R8G8B8 target] [4 KiB guard]
depth: [4 KiB guard] [4 KiB D16 target]       [4 KiB guard]
```

Allocate page-aligned SF virtual ranges after the persistent command-ring
pages and populate every mapped PTE with the backing PFN.  For ordinary WB
GEM shmem use the Windows-proven cached/snooped attribute:

```text
RTSF PTE = (page_pfn & 0x3fffffff) | 0x02000000
```

NC/WC vendor-style backing uses the plain PFN instead.  Complete the bounded
`SR6F.bit7` GTI flush after publishing PTEs and before publishing either GPU
address.  Perform the already-proven CPU cache ownership transition before
GPU access and again after retirement.  Retain BOs, PTEs, and SF nodes until
the CR has reached PAUSE and every relevant engine busy bit is clear.

The packet contains typed relocations, never physical addresses:

```text
A50 = 0x50000000 | (color_target_sf_va >> 8)
A10 = 0x10000000 | (depth_target_sf_va >> 8)
```

Both fields have 256-byte units; the middle-page addresses satisfy that
alignment.  Select translated SF independently for each resource:

```text
A51[21:20] = 01  -> 51100000 for the linear color target
A11[1:0]   = 01  -> SF depth location
```

Do not infer one resource's domain from the other.  Validate both relocations
against the complete 4 KiB middle page, reject overlap with the ring and with
each other, and keep the guard pages mapped and stable for post-retirement
verification.

## Depth-tested overlapping triangle

### Exact state

Use a linear 32 by 32 D16 buffer.  Its byte pitch is 64, or two 32-byte depth
pitch units.  The four depth words are therefore:

```text
10dddddd  # dddddd = depth_target_sf_va >> 8
11004001  # pitch 2 << 13, reading-Z cache disabled, SF location 01
12380000  # D16 format 000, linear; force/clamp fixed Z in bits 21:19
13010000  # LESS, normalization factor zero
```

Enable ordinary Z test and Z writes by changing the verified A01 word only in
bits 3 and 2:

```text
01C01540 -> 01C0154C
```

This preserves the live color-write, vertex-cache, clipping, texture-cache,
line-direction, and diamond-rule state.  It leaves stencil, coarse Z, and the
reading-Z cache disabled.  Windows builder `VTGUModeDX32A:0x101f9f90` creates
A10--A12 from resource base `+f8`, byte pitch `+150`, private format, and
layout; `0x101fa910` independently sets A01 bits 3/2 from Z enable/write; and
`0x101faa10` packs `D3DCMP_LESS-1` as A13 value 1.  The corresponding OpenGL
builders are byte-identical in the other UMD.

### Complete D16 compare matrix

The manual and the D3D/OpenGL builders make A13 `[18:16]` a direct
`NEVER..ALWAYS = 0..7` selector, identical to Gallium `PIPE_FUNC_*`.  With
the D16 normalization byte kept at zero, the complete words are:

| function | A13 | incoming Z values which pass against destination `8000` |
|---|---:|---|
| NEVER | `13000000` | none |
| LESS | `13010000` | `4000` |
| EQUAL | `13020000` | `8000` |
| LEQUAL | `13030000` | `4000,8000` |
| GREATER | `13040000` | `c000` |
| NOTEQUAL | `13050000` | `4000,c000` |
| GEQUAL | `13060000` | `8000,c000` |
| ALWAYS | `13070000` | `4000,8000,c000` |

Only LESS is live.  The silicon-safe extension test should reset one guarded
D16 target to `8000` and color to zero before each row/value pair, submit one
opaque triangle, then check both color and depth.  A passing write-enabled
case must store the source color and exact incoming 16-bit Z; a failing case
must leave zero/`8000`.  Resetting between cases prevents an earlier compare
from changing the reference value and avoids relying on triangle overlap or
draw order.

A01 bit 3 enables the test and bit 2 enables its write:

```text
depth test + write:     01c0154c
depth test, read-only:  01c01548
depth disabled:         01c01540
```

After the compare matrix, validate read-only depth with a two-draw sequence.
Start at depth `8000`; draw red at `4000` with LESS and writes disabled, then
draw green at `8000` with EQUAL and writes disabled.  Correct output is green
with depth still `8000`.  An accidental first depth write leaves red/`4000`,
while a disabled depth test lets the second color through but is caught by
the per-function matrix.

### Combined successor reset invariants

Every D16+blend+alpha body must synthesize the following state rather than
inherit it:

- A52 destination format/pitch, A53 channel mask, A00 blend/alpha enables,
  closed A38--A3F constants, and A34--A37 equations;
- A01 depth-test/write bits, A10--A12 surface state, A13 compare mode, and the
  wide-open type-01 A20 `20007fff` clip tuple;
- A33 whenever alpha test is enabled.  When disabled, A00 bit 20 must be
  cleared; stale A33 is then inert and body index 31 may remain padding.

The reverse transition is equally important.  Every legacy color/textured
body writes A00=`00000011`: RT0 blending remains enabled, but the legacy
A34/A35 RGB equation is canonical COPY and alpha test is disabled.  It also
writes A01 with depth disabled, A52 format 98, and A53 full mask, so a combined
successor followed by a legacy COPY must not retain a prior equation, alpha
kill, depth rejection, format 99, or partial mask.  (Its partial A36/A37 image
is harmless only because format 98 discards stored alpha.)  A10--A13 and A20
may remain in the hardware image only while A01 disables depth; current A20
is the same wide-open value on every D16 path.  Before exposing configurable
depth ranges, make A1E/A1F/A20 a closed tuple on every transition rather than
depending on this harmless stale value.

The strongest transition regression uses a combined draw with BGRA format,
partial mask, enabled blending, enabled alpha test, and LESS/write depth,
then a legacy opaque BGRX COPY with no depth handle.  The second draw must
write every RGB channel regardless of alpha and must leave the guarded depth
BO byte-for-byte unchanged.  Then submit a read-only D16 draw followed by a
write-enabled D16 draw to prove that A01 bit 2 is restored in both directions.

A concrete all-state vector is BGRA prefill `0000ff00` and depth `ffff`, then
a combined z=`4000`, source=`40ff0000`, canonical source-over draw with alpha
LESS/ref `80`; its interior is approximately `4040bf00` with depth `4000`.
Follow it over the same pixels with legacy BGRX full-mask COPY source
`c00000ff`.  The exact final color/depth is `000000ff/4000`.  Stale alpha LESS
kills that second source, stale blend/mask/format changes its color, and stale
A01 depth write changes depth because the legacy vertex Z is zero.

The state-complete D16+blend successor's first type-01 group is exactly 32
dwords including its four-dword header.  Angle-bracketed words are kernel
relocations, not literal packet input.  This canonical BGRX COPY form keeps
blend and alpha test disabled while still closing every blend constant and
equation register:

```text
FE020440 0000043C 00010000 CC000000
<50000000|(color_sf_va>>8)> 51100000 52980004 38000000
39000000 3A000000 3B000000 3C000000
3D000000 3E000000 3F000000 34000150
35000114 36000590 37000224 86FF0000
870000FF 53010C0F 00000001 01C0154C
88001ED0 89000800 <10000000|(depth_sf_va>>8)> 11004001
12380000 13010000 20007FFF CC000000
```

For source-over plus alpha LESS/ref `80`, replace A34--A37 with
`34000020 35000148 36000590 37000244`, A00 with `00100011`, and the final
padding word with A33=`33001202`.  A format-99 destination changes only A52
from `52980004` to `52990004` at this pitch.  The next type-02 header is body
index 32; A23/A24 remain at indices 57/58 and the immediate header remains at
168.

All groups after it through the pixel program are copied byte-for-byte from
the verified body, except A24 is `24000003`.  Keep the proven fan word
`201c5c00` and solid word `23000f02`.  Replace the final Type-00 group with
three vertices plus the required two alignment dwords:

```text
FE020440 0000043C 00000000 CC000000
BF000000 BF000000 00000000 3F800000 FFFF0000 FFFF0000
41FC0000 BF000000 00000000 3F800000 FFFF0000 FFFF0000
41FC0000 41FC0000 00000000 3F800000 FFFF0000 FFFF0000
CC000000 CC000000
```

Relative to the original 188-dword three-vertex D16 body, closing A38--A3F
adds four dwords.  The state-complete successor is therefore exactly 192
dwords; its final two padding dwords are at indices 190/191.

### Two-list discriminator

Before GPU ownership, initialize:

```text
color target: 0x00000000 in all 1,024 dwords
depth target: 0xffff in all 1,024 D16 samples
guards:       fixed nonzero per-page poison pattern
```

The original live depth oracle predates the 192-dword state-complete
successor.  It submits two complete 188-dword predecessor bodies in one
guarded command job, with the normal proven 2D-to-3D synchronization before
the first and the normal CR tail/PAUSE after the second:

1. First body uses the four-word partial constant image from the original
   fixed packet: opaque solid red, Z=0.
2. Second body identical except `86FF0000 -> 8600FF00`: opaque solid green at
   the same geometry and Z=0.

The first list passes `0 < 0xffff`, writes red, and writes D16 zero.  The
second list fails `0 < 0` everywhere covered by the first.  Robust sample
expectations, away from all edges, are:

| pixel `(x,y)` | coverage | expected color | expected D16 |
|---|---|---:|---:|
| `(24,8)` | inside TL/TR/BR triangle | `00ff0000` | `0000` |
| `(8,24)` | outside triangle | `00000000` | `ffff` |

Also require at least one covered and one uncovered scanline span, unchanged
unused depth-page bytes, intact full guard pages, `040c=0`, CR at PAUSE, and
all 3D busy bits clear.  If the second draw becomes green, the address and
raster path worked but Z reject/write did not.  If color is red while depth
remains `ffff`, Z testing may be rejecting for another reason; the depth
readback distinguishes that from a correct write.

This oracle intentionally does not exercise GPU depth clear, D24S8, stencil,
coarse Z, tiling, or reading-Z cache.  A D24S8 linear follow-up at the same
128-byte pitch would use `11008001` and `123e0000`, with clear dword
`ffffff00`, but should follow the D16 result.

### Live result

The guarded six-page implementation passed on 2026-09-20.  Exactly 528
samples were covered; `(24,8)` read `00ff0000/0000`, `(8,24)` read
`00000000/ffff`, the unused half of the D16 page remained `ffff`, and all four
guard pages remained intact.  The command reader retired at PAUSE with no new
kernel error and the persistent engine accepted subsequent work.

### Structured-draw Z state and DMA ownership omissions

The fixed oracle above proves address, pitch, LESS, write, and readback only
at vertex Z zero.  The first structured D16 implementation copied that body
and substituted per-vertex Z, but did not program the type-01 pixel-Z clip
range.  The indexed engine initialization leaves A1E, A1F, and A20 at:

```text
1E000000 1F000000 20000000
```

The manual reconstructs the far float from A1E.low24 and A20[15:8], and the
near float from A1F.low24 and A20[7:0].  It explicitly drops a pixel before
depth testing when `Z > far || Z < near`.  The reset tuple therefore selects
near = far = zero.  Static evidence therefore requires opening this interval
before general nonzero-Z use.

This is also the byte-exact behavior of the proprietary driver.  The normal
`STM_ViewPort_INV` path at `via_chrome9_dri.so:0x1bb1e0..0x1bb292` writes:

```text
1E000000 1F000000 20007FFF
```

and dirties subaddresses 1E through 20.  That reconstructs a wide-open
far=`0x7f000000`, near=`0xff000000` interval.

The independent Windows D3D viewport builder agrees byte for byte.
`VTGUModeDX32A.dll:0x101f8bb4..0x101f8c0e` loads near `ff000000` and far
`7f000000`, stores their low 24 bits in image offsets `+7c/+78` (A1F/A1E),
and their high bytes in `+80/+81` (A20).  This occurs immediately before the
render-target/depth builder at `0x101f9f90`; it is normal draw state, not an
OpenGL-only workaround.

The smallest exact discriminator does not change the 188-dword body or any
later index: replace the first depth packet's padding at body index 26 with
`20007fff` and retain index 27 as `cc000000`.  A1E and A1F are already at
their required zero-low24 values.  Emit A20 in every depth body, including
the fixed helper, so each submission is independent of mutable prior context.

That A20-only discriminator was run live and retired safely, but the observed
failure was unchanged.  A stronger degenerate-triangle run then established
that the entire useful D16 target changed from userspace's `ffff` to `0000`
even when all three vertices were identical, while color stayed unchanged.
That is not raster coverage.  It identifies a pre-submit memory-ownership
problem which had been masked by the fixed oracle.

`chrome9_gem_create_ioctl` calls `drm_gem_shmem_get_pages_sgt` before exposing
the object.  This performs the persistent bidirectional DMA map while the
new shmem/bounce storage is still zero, before userspace writes `ffff` through
its mmap.  The structured submission path then incorrectly starts with:

```text
dma_sync_sgtable_for_cpu(depth_sgt, DMA_BIDIRECTIONAL)
```

On the 32-bit DMA/SWIOTLB path this copies the stale, create-time zero bounce
contents back over the CPU shmem pages.  The later cache flush and
sync-for-device faithfully publish those zeros.  The fixed single-BO oracle
passes because `chrome9_prepare_depth_oracle` fills `ffff` only *after* the
destructive sync-for-CPU.

The correct lifetime is explicit:

1. immediately after the eager DMA map in GEM create, sync for CPU before the
   object is returned to userspace;
2. while the object is CPU-owned, never sync for CPU again before submission;
   flush the CPU aliases and sync for device;
3. after proven GPU retirement, sync for CPU once before userspace readback.

Remove the pre-submit sync-for-CPU calls for both color and depth in the
structured path.  The same latent bug exists in the color-only submission and
would destroy nonzero CPU destination contents needed by blending, although
zero-cleared bring-up tests hide it.  Retain the post-retirement sync-for-CPU.

The combined ownership correction plus A20 `20007fff` passed live.  The
degenerate three-identical-vertex control changed zero depth samples and left
the target at `ffff`.  A normal first draw changed exactly 528 color/depth
samples and read back D16 `c000` for input Z=0.75.  The complete far/near/reject
suite then passed at both 64 by 64 and 257 by 129, followed by every fixed,
color-triangle, and Gallium regression.  This proves both independent defects:
the previous full-target zero was stale-bounce overwrite, while the wide-open
A20 is the vendor-required nonzero-Z clip state.

Persistent artifacts:

```text
/root/tmp/chrome9/chrome9-d16-ownership-v8.ko
  a66a5ee422bd2ac0cd942569c48fbeea91c82856c76db3d146f346c9f12a5f4a
/root/tmp/chrome9/chrome9_d16_triangle-probe
  f339d3c0045ae401ff3dc049980f1793f93e5c191217f76b9568f8a1e4a25f19
/root/tmp/chrome9/chrome9-gallium-d16-v1
  8dad51a59c6a7680acbf634d37d2a64c5ac515f362d6d211af2da226171281f5
```

The proprietary chip-image initializer separately changes the reset values
of the adjacent Z-bias clamp/precision state to:

```text
19800000 1A800000 1B803FBF
```

`stmInitChipImage_inv:0x17880f..0x178847` is the exact source.  A19/A1A and
A1B's two bytes reconstruct `-1.0`/`+1.0` bias clamp bounds, while A1B bit 23
enables enhanced Z precision.  Those are valid follow-up defaults and should
eventually be restored at initialization or in complete context state, but
they are not the minimal explanation for the all-or-nothing Z=0 boundary.

## Alpha-blended overlapping triangles

### Exact source-over equation

The verified body already has A00 `00000011`, which enables RT0 alpha
blending (bit 4) and RT0 dithering (bit 0).  Its original blend equation is a
copy operation, source times one plus destination times zero.  For ordinary
`SRC_ALPHA, INV_SRC_ALPHA, ADD`, replace only A34--A37:

```text
34000150 -> 34000020  # RGB: source operand * Asrc
35000114 -> 35000148  # RGB: + destination operand * (1-Asrc)
36000500 -> 36000410  # A:   Asrc * Asrc
37000200 -> 37000244  # A:   + Adst * (1-Asrc)
```

The RGB words decode exactly as:

```text
Cout = Csrc * Asrc + Cdst * (1 - Asrc)
```

The values come directly from the proprietary D3D factor tables at
`0x10336480`, `0x103364d0`, `0x10336520`, and `0x10336570`, consumed by
builder `0x101fd9e0`; the OpenGL tables and builder are byte-identical.  A34
uses source operand 0 and factor code `02`; A35 uses destination operand 1,
ADD 0, and inverse-source-alpha factor `12`.  With separate-alpha blending
disabled, A36/A37 analogously use alpha operands 1/2 and factor codes `01/11`.

### Packet and expected samples

Use the 184-dword three-vertex fan body from
[windows-draw-path.md](windows-draw-path.md), the already-proven SF color
relocations (`A50=color_sf_va>>8`, A51 `51100000`), solid shading
`23000f02`, A24 `24000003`, and the same TL/TR/BR three-vertex Type-00 group.
No depth resource or depth words are present.  Initialize the entire color
target to `00000000` and poison both full guard pages.

Submit two complete bodies in one job:

1. 50%-alpha red: A86/A87 are `86FF0000 87000080`.
2. 50%-alpha green: A86/A87 are `8600FF00 87000080`.

Every other dword is identical, including the four equation words above.
The format-98 target discards alpha and stores X8R8G8B8.  A53's live rounding
mode rounds after multiplication by the destination range.  With
`Asrc=128/255`, the expected inside value is:

```text
after red:    round(255*128/255)                         = 00800000
after green:  round(128*127/255), round(255*128/255), 0 = 00408000
```

Use the same robust samples:

| pixel `(x,y)` | expected final color |
|---|---:|
| `(24,8)` | `00408000` |
| `(8,24)` | `00000000` |

If the second list simply replaces the first, the inside value is
`0000ff00`; that cleanly distinguishes disabled/copy blending from the
expected source-over path.  Require all target pixels to belong to the exact
inside/outside classes, intact guards, clean engine/error status, and the same
post-retirement CPU cache transition as the proven SF draw.

After this source-over test passes, the next isolated cases should be ONE/ZERO
add, destination alpha, separate alpha, reverse subtract, and only then blend
constants or MIN/MAX.  They change equation semantics but not the relocation
or primitive protocol established here.

### Live result

The guarded three-page implementation passed on 2026-09-20.  Exactly 528
samples were `00408000`; `(24,8)` was inside, `(8,24)` remained zero, and both
guard pages remained intact.  The same module then repeated both variable
triangle regression sizes successfully, establishing that neither state test
left the persistent engine wedged.
