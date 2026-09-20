# SF 2D copy and presentation oracle

This note specifies the smallest known-safe H6/VX900 2D copy from an internal
linear render target to a linear drawable mapped through the translated SF
aperture.  It separates engine ordering, command completion, CPU cache
ownership, and display presentation: the four are not interchangeable.

The bit fields come from the VX900 Open Graphics Programming Manual, Part I,
2D-register pages 97--101.  Packet construction and the 7122 call sequence are
recovered from the symbol-rich proprietary Xorg binary
`binary/5.75.32.87a-u910-55689/bin/via_drv.so`.

## H0 BitBLT register image

`viaAccelCopyHelper_H6` at `0x2ae20` emits an aligned header-0 packet with eight
register/value pairs (`0x2aeb8..0x2af77`):

```text
FE000000 00000008 00000000 CC000000
00000004 mode
0000001C src_base >> 3
00000014 dst_base >> 3
00000008 ((dst_pitch >> 3) << 16) | (src_pitch >> 3) | domains
00000018 (src_y << 16) | src_x
00000010 (dst_y << 16) | dst_x
0000000C ((height - 1) << 16) | (width - 1)
00000000 command
```

The manual and executable agree on the following decode:

| register | bits | meaning | safe copy value |
|---:|---:|---|---:|
| `00` | `31:24` | raster operation | `CC`, source copy |
| `00` | `15` | decrement X | only for rightward overlap |
| `00` | `14` | decrement Y | only for downward overlap |
| `00` | `6` | host source | `0`, framebuffer source |
| `00` | `3:0` | operation | `1`, BitBLT |
| `04` | `9:8` | destination depth | `00` 8 bpp, `01` 16 bpp, `11` 32 bpp |
| `08` | `31:30` | destination location | `01`, SF |
| `08` | `26:16` | destination pitch / 8 bytes | caller supplied |
| `08` | `15:14` | pattern location | `00`, unused |
| `08` | `13:12` | source location | `01`, SF |
| `08` | `10:0` | source pitch / 8 bytes | caller supplied |
| `0c` | `27:16` | height minus one | `0..4095` |
| `0c` | `11:0` | width minus one | `0..4095` |
| `10` | `31` | destination tiled addressing | `0`, linear |
| `10` | `27:16`,`11:0` | destination Y/X | caller supplied |
| `14` | `25:0` | destination base / 8 bytes | SF virtual address |
| `18` | `31` | source tiled addressing | `0`, linear |
| `18` | `27:16`,`11:0` | source Y/X for color BitBLT | caller supplied |
| `1c` | `25:0` | source base / 8 bytes | SF virtual address |

The location bits are part of register `08`, not the base words.  Hence a
linear SF-to-SF copy uses

```text
pitch_word = 40001000 | ((dst_pitch / 8) << 16) | (src_pitch / 8)
```

and both bases are offsets in the same internal RTSF virtual aperture.  They
are not PFNs, bus addresses, or CPU virtual addresses.  Imported drawable
pages must first be pinned and installed in that page table; ordinary cached
pages require the measured `02000000` cached/snooped PTE attribute, while the
vendor's noncached and WC mappings use a plain PFN.  The mapping must remain
pinned through the completion fence.  Publishing or changing those PTEs also
requires the bounded GTI invalidation protocol before any SF address reaches
the ring: wait for `SR6F.bit7` clear, disable table write protection through
`SR6C.bit7` without disturbing the table-base bit, write complete PTEs, issue
a write/readback barrier, set `SR6F.bit7`, wait for hardware to clear it, and
restore `SR6C.bit7`.  An H0 `006c` packet orders engines; it does not flush the
SF translation cache.

Although the pitch fields count qwords, the manual says they have 128-bit
alignment.  Require both base and pitch to be 16-byte aligned initially.  This
also avoids a vendor-Xorg bug: `viaExaPrepareCopy_H6:0x2bb79..0x2bb95` checks
the source pitch for only four-byte alignment, even though the emitter later
does `src_pitch >> 3`.  Destination pitch is checked for eight-byte alignment.
Neither weak check should be copied.

The pitch fields allow at most 2047 qwords and the width/height fields at most
4096 pixels.  Independently validate every row's byte interval against its BO
mapping and the 64-MiB SF aperture used by the recovered stack.  Field width is
not proof that a larger mapped aperture exists.

## Same-format requirement and sticky state

The baseline copy is bit preserving, not a format conversion.  Register `30`
bit 31 enables CFC.  With it clear, the manual says source depth and format are
forced to the destination depth and format.  The 32-bpp interpretation is
ARGB8888 in bit significance (`A[31:24], R[23:16], G[15:8], B[7:0]`), which is
the usual little-endian B,G,R,A byte order.  For the first path, accept only
identical source/destination DRM formats and cpp and use `30=0`.

Register `48` contains source/destination key enables and a 32-bpp byte mask.
`viaExaCopy_H6:0x2b55b..0x2b580` calls
`viaAccelTransparentHelper_H6` with transparency disabled; its exact H0 write
at `0x2ad19..0x2ad43` is `48=00000000`.  The special 7122 branch also clears
scaling and CFC state and emits registers `30`, `34`, and `38` through
`viaAccelSet410Helper_H6` (`0x2b648..0x2b6d3`).  A self-contained copy must not
inherit any of these sticky settings from an earlier composite operation:

```text
FE000000 00000004 00000000 CC000000
00000030 00000000
00000034 00000000
00000038 00000000
00000048 00000000
```

The four-pair grouping is equivalent to Xorg's separate aligned helpers and is
the preferred compact kernel template.  It needs silicon confirmation as a
group only because the vendor happened to split `48` into its own H0 packet;
all four are ordinary 2D register writes and no start register is included.

## Exact 32-by-32 SF copy

For a 32-bpp source at SF VA `00005000`, destination at `00002000`, both with
128-byte pitch, zero X/Y, and 32-by-32 extent, the complete core BitBLT is:

```text
FE000000 00000008 00000000 CC000000
00000004 00000300
0000001C 00000A00
00000014 00000400
00000008 40101010
00000018 00000000
00000010 00000000
0000000C 001F001F
00000000 CC000001
```

`40101010` decomposes as destination SF `40000000`, destination pitch
`00100000`, source SF `00001000`, and source pitch `00000010`.  `CC000001` is
GXcopy plus the BitBLT opcode.  This copies exactly 4096 bytes and therefore
fits one page at each shown base.  A guarded test should nevertheless map a
guard page on each side of both resources and compare all guards after the
fence.

The engine-order wrapper used by `viaExaCopy_H6` before and after every EXA
copy
(`0x2b53e..0x2b556`, `0x2b60b..0x2b623`) is:

```text
FE000000 00000001 00000000 CC000000
0000006C 00400000
CCCCCCCC CCCCCCCC
```

The two fillers align the following header.  `00400000` orders/waits the 2D
engine only; it is appropriate for EXA's 2D producer chain.  The independently
recovered helper `stmAddCRWaitChipIdle_invi` proves `00800000` is the 3D bit
and `00c00000` requests both.  Consequently the render-to-copy path requested
here must use this stronger preamble unless the 3D producer fence has already
retired:

```text
FE000000 00000001 00000000 CC000000
0000006C 00C00000
CCCCCCCC CCCCCCCC
```

The initial conservative render/copy job body is therefore: both-engine
pre-sync, clear `30/34/38/48`, the eight-pair BitBLT, 2D-only post-sync.  If
the render and copy are separate scheduled jobs and the copy waits on the
retired render fence before submission, the vendor's `00400000` preamble is
sufficient, but `00c00000` is the safer bring-up discriminator.  No `006c`
write is an externally visible completion fence.

## Overlap is a two-axis memmove

`viaExaPrepareCopy_H6:0x2bbb1..0x2bc49` constructs `(rop << 24) | 1`, adds bit
15 for negative X direction and bit 14 for negative Y direction.
`viaAccelCopyHelper_H6:0x2af88..0x2afbc` then adds `width-1` to both source and
destination X when X decrements, and `height-1` to both Y values when Y
decrements.  The exact source-copy command variants are:

| traversal | command |
|---|---:|
| X forward, Y forward | `CC000001` |
| X forward, Y backward | `CC004001` |
| X backward, Y forward | `CC008001` |
| X backward, Y backward | `CC00C001` |

For distinct BOs choose forward/forward.  For aliases of one BO, compare the
actual byte rectangles after base and pitch are applied: reverse Y when the
destination begins in later overlapping rows, and reverse X when the
destination begins to the right within overlapping rows.  If pitches or
aliasing make a single pair of traversal directions insufficient to preserve
all unread source bytes, reject the hardware copy and use a scratch BO or CPU
fallback.  Do not infer safety from X/Y rectangles alone when two handles
alias the same dma-buf.

## Completion, ownership, and present

The safe transition is:

1. Wait on both BO reservation dependencies.  Flush CPU dirty lines before
   GPU ownership where a WB CPU alias exists; keep the proven RTSF cache bit
   paired with the mapping type.  If either SF mapping is new or changed,
   finish the bounded `SR6F.bit7` translation-cache flush described above
   before publishing the command.
2. If 3D produced the source in the same command stream, use the
   `006c=00c00000` both-engine pre-copy wait.  If it was a separate job, first
   wait for its returned render fence and then use at least the vendor's
   `006c=00400000` 2D preamble.  Initially serialize at the scheduler/fence
   level as well; the H0 write alone has no timeout or completion record.
3. Emit the state clear, BitBLT, and post-copy `006c=00400000` in one validated
   branch buffer.
4. Append a kernel-owned FE3x completion fence and hold all pins until it
   retires.  The Windows scheduler's workload table uses a 2D-class header and
   the composite 2D+3D wait contribution: `d0=FE300036`, `d1` low 24 bits
   `00300000`, with fence ID/address split as documented in
   [via-5.75-kmd.md](via-5.75-kmd.md#vx900-new-fence-packet-header-3x).  The old
   Linux KMD used the same `FE300036` flags with wait mask zero; that proves
   packet encoding, not adequate mixed-engine ordering.  Bring-up may instead
   use a non-pausing polled system-write fence until interrupt acknowledgement
   is known reliable.
5. Only after fence retirement may the CPU invalidate/read a WB destination or
   KMS latch it for scanout.  A display page-flip/vblank event proves display
   latching, not completion of the preceding render/copy.

The fence packet itself is four dwords:

```text
d0 = FE300000 | mode_flags
d1 = (fence_id[7:0] << 24) | wait_mask
d2 = fence_id[31:8] | ((fence_addr[11:4]) << 24)
d3 = fence_addr[31:12]
```

For the production Windows policy above, `mode_flags=36` means interrupt,
system write, interrupt wait, and queue skip.  Do not enable interrupt-wait
until IRQ installation, unmasking, and RW1C acknowledgement are proven: a
lost interrupt can pause the command reader.  Queue skip also deserves a
single-job characterization before concurrent scheduling.

For a copy-present, the imported drawable need not be a raw display physical
address.  It is an ordinary pinned dma-buf mapped into SF for the 2D job.  KMS
may scan out that BO only if the display engine's own address/stride/modifier
constraints are satisfied.  If the imported BO is not scanout-capable, copy to
a scanout BO instead.  Never put display-base programming in the render
command allowlist.  The exact kernel-only automatic-flip packets and CRTC
vblank bits are in [display-flip-vblank.md](display-flip-vblank.md).

## First silicon discriminator

The least ambiguous test uses two separate guarded, page-aligned SF mappings,
pre-fills the source with a per-pixel coordinate pattern and the destination
with a sentinel, submits the exact 32-by-32 stream above, and waits on a
kernel-owned fence with a bounded timeout.  Verify:

* all 1024 destination pixels equal source pixels;
* source and all guard pages are unchanged;
* the fence word/ID retires monotonically;
* the existing local-VRAM solid-fill and H6 3D regression still pass.

Then repeat with four same-BO overlaps to exercise each direction command.
Only after those pass should the destination be replaced by an imported
linear drawable and followed by an atomic KMS commit.  This staging isolates
SF mapping, copy direction, completion, and presentation failures instead of
collapsing them into a single black-screen result.
