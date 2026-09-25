<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 (VX900) FE43 index fetch

Status: working on silicon, 2026-09-25.  Evidence labels as in the other
notes: **S** static (vendor binaries), **D** dynamic (target run).

## Encoding

The vendor D3D UMD draws an indexed primitive from an index buffer
(`VTGUModeDX32A.dll` `0x101b1430..0x101b15ce`, **S**):

```text
fe020440 0000043c 00040000 cc000000     H2 to the TCL register block
24000000 | count                        A24 index count
cc000000 ...                            pad to 16 bytes
fe430020 | (aperture ? 1 : 0)            FE43, hierarchy bits 5:4 = 10
count * index_size                      byte length
0                                       address (patched by the KMD)
0
```

The KMD relocation type `0x3c` (`VTGKModeDX32A.sys` `0xb5e45`, **S**) sets
`d2 = A & ~1` and `d1 = ((A + len) - (A & ~0xf) + 3) & ~3`: the length is
counted from the 16-byte aligned start.  Bit 0 of `d0` selects SF (1) or
SL (0), as in the FE40 branch.

The FE43 packet replaces the inline index payload that follows A24 (our
counted H3 `fe03043c, count, 0`): the command reader fetches the payload
from memory and returns to the ring.

## Kernel use

Module parameter `index_fetch` (default on): stage B draws
copy their packed indices into an index arena instead of the ring and emit
FE43 with a 16-byte aligned address.  The arena (the ring's size, coherent
pages) is GART-mapped directly after the ring; queued jobs read it, so a
wrap drains the queue like a ring wrap, and each segment starts with at
least half the arena free (a segment's indices are at most its own size).

## Results (D, kernel k121, Mesa p65)

* `~/tmp/vtf/vtf0` (stage B, compacted indexed grid) renders byte-identical
  with and without FE43; with FE43 the stream carries no index data, so the
  fetch is proven.
* glmark2 refract 64.1 -> 62.3 ms, terrain unchanged, score 113 -> 114,
  no timeouts.

## Next

Fetching directly from the application's index buffer (no copy) needs
A2D = minimum index with raw indices and a bound for out-of-range indices
(rung B8, untested); the kernel would still have to scan the indices unless
the hardware clamps.

Occlusion queries: the snapshot after a draw read partial counts (67 of
128 samples in piglit occlusion_query_meta_save) when the draw's indices
came through FE43, so draws with a query keep inline indices.  Full piglit
list (2101) otherwise unchanged.
