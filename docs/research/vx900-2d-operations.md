# VX900 bounded 2D operation oracles

This note defines the 2D subset that can be implemented without importing the
old EXA driver's implicit state.  It covers solid fill, same-format copy,
overlap, byte masks, clipping, color-format conversion, AlphaBLT, scaling,
and the synchronization/cache rules needed by Mesa or Xorg presentation.

The register fields are from the VX900 Open Graphics Programming Manual,
Part I, internal pages 97--102.  Packet order comes from the symbol-rich
proprietary Xorg binary
`binary/5.75.32.87a-u910-55689/bin/via_drv.so` (SHA-256
`5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787`).
The independent Windows cross-check is
`binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGKModeDX32A.sys`
(SHA-256
`618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`).

Evidence is deliberately classified:

* the local-VRAM 32-bpp solid recipe is live-proven on the target VX900;
* H6 solid/copy order, ROPs, directions, and state clearing are executable
  Xorg evidence;
* the Windows KMD independently executes a source-SF to destination-SL copy;
* destination-SF, clipping, byte masks, CFC, and AlphaBLT are exact manual
  derivatives but still require the guarded tests below;
* no complete nonzero VX900 stretch-state oracle was found.  It must not be
  exposed merely because two opaque proprietary helper words were recovered.

## Common validation and state closure

Header-0 packets begin on a 16-byte boundary.  They contain four header dwords
followed by register/value pairs.  A seven-pair solid packet is 18 dwords, so
two `CCCCCCCC` fillers are required before the next header.  An eight-pair
copy is already 20 dwords and remains aligned.

For the initial linear subset require:

* nonzero width and height, each at most 4096;
* X/Y and the last addressed pixel in `0..4095` for both source and
  destination;
* base and pitch aligned to 16 bytes, matching the manual's 128-bit alignment;
* pitch no greater than 16368 bytes.  The 11-bit field can spell 2047 qwords,
  or 16376 bytes, but the largest value satisfying 16-byte alignment is 2046
  qwords;
* every first/last row byte interval checked against the pinned BO with
  overflow-safe arithmetic, using the source and destination cpp separately;
* only linear addressing (`10.bit31=0`, `18.bit31=0`); and
* only SL offsets known to lie in BAR2 or SF virtual addresses backed by pinned
  PTEs.  The currently recovered SF table covers 64 MiB even though the base
  register itself has a wider field.

The command must be built from zero for every operation.  Do not retain clip,
direction, host-source, mono, or operation bits from a preceding command.
Likewise, plain solid/copy must explicitly close all sticky auxiliary state:

```text
FE000000 00000004 00000000 CC000000
00000030 00000000                 CFC off
00000034 00000000                 proprietary scaling state off
00000038 00000000                 proprietary scaling state off
00000048 00000000                 byte mask and color keys off
```

The VX900 address-free 2D initializer documented in
[via-5.75-kmd.md](via-5.75-kmd.md#why-the-first-exact-h6-fill-latched-but-did-not-draw)
is a prerequisite.  The live solid latched but produced no pixels before that
initializer ran.  The initializer is engine bring-up, not per-job state.

## Solid fill

`viaAccelSolidHelper_H6:0x2ab60..0x2ac6e` emits exactly:

```text
FE000000 00000007 00000000 CC000000
00000004 mode
00000014 dst_base >> 3
00000008 ((dst_pitch >> 3) << 16) | dst_location
00000010 (dst_y << 16) | dst_x
0000000C ((height - 1) << 16) | (width - 1)
00000058 color
00000000 command
CCCCCCCC CCCCCCCC
```

`viaExaPrepareSolid_H6:0x2ba50..0x2bb1a` supplies 8/16/32-bpp modes
`00000000`, `00000100`, and `00000300`, and constructs
`(pattern_rop << 24) | 00002001`.  GXcopy is therefore `F0002001`:
fixed-color-pattern BitBLT, with the command register written last.  Register
`58`, not the otherwise plausible fill-color register `4c`, is the executable
H6 color oracle.

For a guarded 32-by-32 32-bpp SF fill at SF VA `00002000`, pitch 128, origin
zero, and color `ff336699`, the core packet is:

```text
FE000000 00000007 00000000 CC000000
00000004 00000300
00000014 00000400
00000008 40100000
00000010 00000000
0000000C 001F001F
00000058 FF336699
00000000 F0002001
CCCCCCCC CCCCCCCC
```

Only the otherwise identical SL form (`08=00100000`) is live-proven.  The SF
destination location `08[31:30]=01b` is explicit in the manual and consistent
with the independently proven SF source encoding, but this exact SF solid is
still a silicon discriminator.  Map one guarded page, require exactly 1024
changed pixels and unchanged guards, then repeat with a nonzero X/Y rectangle.

## Same-format copy and overlap

The exact eight-pair H6 copy and SF-to-SF domain word are in
[sf-2d-copy-present.md](sf-2d-copy-present.md#h0-bitblt-register-image).
For convenience, the core is:

```text
FE000000 00000008 00000000 CC000000
00000004 mode
0000001C src_base >> 3
00000014 dst_base >> 3
00000008 pitch_and_locations
00000018 (src_y << 16) | src_x
00000010 (dst_y << 16) | dst_x
0000000C ((height - 1) << 16) | (width - 1)
00000000 command
```

For two SF mappings:

```text
pitch_and_locations = 40001000 |
                      ((dst_pitch >> 3) << 16) |
                      (src_pitch >> 3)
```

`viaExaPrepareCopy_H6:0x2bb20..0x2bc49` rejects unlike bpp and constructs the
four GXcopy traversal commands below.  `viaAccelCopyHelper_H6` moves each X or
Y start to the last pixel when the matching decrement bit is set:

| traversal | command |
|---|---:|
| X forward, Y forward | `CC000001` |
| X forward, Y backward | `CC004001` |
| X backward, Y forward | `CC008001` |
| X backward, Y backward | `CC00C001` |

Select direction from the actual byte ranges after resolving aliases, bases,
pitches, cpp, and coordinates.  If a single X/Y traversal cannot preserve all
unread bytes, use a scratch BO or CPU memmove.  Different BOs always start with
forward/forward.  CFC conversion is never an overlapping operation.

There is independent Windows executable confirmation at
`VTGKModeDX32A.sys:0x2f8a2..0x2f95a`.  Its dummy-2D diagnostic writes, in
order, mode `80000300`, source base `(address+1000)>>3`, destination base
`address>>3`, pitch/location `00101010`, `48=0`, both positions zero,
dimension `00100010`, and command `CC000001`.  Ignoring the unexplained mode
bit 31, this is a 17-by-17 source-SF (`08.bit12`) to destination-SL GXcopy and
independently proves qword bases, qword pitches, source location, dimension,
and start-on-command order.

## 32-bpp byte mask

Register `48[31:28]` suppresses individual destination bytes for 32-bpp solid
fills: zero writes a byte and one preserves it.  The intended mapping in
`viaAccelPlaneMaskHelper_H6:0x2b950..0x2ba24` is:

| control bit | little-endian byte | ARGB8888 significance |
|---:|---:|---|
| `28` | 0 | B |
| `29` | 1 | G |
| `30` | 2 | R |
| `31` | 3 | A/X |

Thus `48=80000000` preserves A/X while updating RGB, and `48=70000000`
updates only A/X.  Always restore `48=0` after a masked operation because the
same register also enables source/destination color keys.

This mapping is manual-plus-code evidence, not executable proof for 32 bpp.
The shipped helper computes `(1 << 32) - 1` with an x86 variable shift; the
hardware masks the shift count and the compiled code obtains zero.  It then
mistakes every 32-bpp planemask for a full mask and leaves `48` unchanged.
Do not reproduce that bug or cite old EXA planemask success as validation.

The first direct test should prefill `44332211`, issue a 1-by-1 solid
`aabbccdd`, and use one mask bit at a time.  Exact expected results are:

| `48` | result |
|---:|---:|
| `80000000` | `44BBCCDD` |
| `40000000` | `AA33CCDD` |
| `20000000` | `AABB22DD` |
| `10000000` | `AABBCC11` |

Run each in a fresh guarded destination and reset `48` afterward.  Until all
four pass, expose only full writes.

## Clipping

The manual defines top/left in `40`, bottom/right in `44`, and command bit 12
as clipping enable.  It does not state whether bottom/right are inclusive.
Xorg decomposes EXA regions and supplies unclipped rectangles, so there is no
independent executable answer.

Use this exact bounded discriminator after plain solid passes:

```text
FE000000 00000002 00000000 CC000000
00000040 00020002                 top=2, left=2
00000044 00050005                 bottom=5, right=5

# ordinary 8-by-8 solid packet, except:
00000000 F0003001                 GXcopy solid + clip enable
```

Prefill an 8-by-8 target with a sentinel.  A 4-by-4 changed region at
coordinates 2 through 5 proves inclusive limits; 3-by-3 proves exclusive
limits.  No pixel outside that region or outside the BO may change.  A
subsequent ordinary command must use `F0002001`; clearing bit 12, rather than
the stored limit registers, disables clipping.  A production validator should
still constrain both limit registers to the destination bounds.

## Color-format conversion

Register `30` is sufficiently documented to construct bounded CFC probes.
Bit 31 enables conversion, bit 30 selects zero rather than high-bit extension
for RGB, bit 29 selects rounding rather than table dithering, bits `9:8` are
destination format, bits `7:6` source depth, and bits `1:0` source format.
`viaAccelCFCHelper_H6_410:0x2b190..0x2b1fe` independently confirms that exact
packing.  The known words are:

| conversion | register `30` | rounding variant |
|---|---:|---:|
| RGB555 -> ARGB8888 | `80000040` | `A0000040` |
| RGB565 -> ARGB8888 | `80000041` | `A0000041` |
| ARGB8888 -> RGB555 | `800000C0` | `A00000C0` |
| ARGB8888 -> RGB565 | `800001C0` | `A00001C0` |
| ARGB2101010 -> ARGB8888 | `800000C1` | `A00000C1` |
| ARGB8888 -> ARGB2101010 | `800001C0` | `A00001C0` |

The last two illustrate why destination mode is also required to interpret
the format fields.  For conversion, replace the common `30=0` reset with the
chosen CFC word while retaining `34=0`, `38=0`, and `48=0`; select destination
depth in register `04`, and use the ordinary GXcopy packet with source and
destination cpp-specific pitches and bounds.

For example, 32 RGB565 pixels at SF VA `00005000`, pitch 64, converted to a
32-bpp SF destination at `00002000`, pitch 128, use mode `00000300`,
`30=80000041`, and copy pitch/location word `40101008`.  Saturated source
primaries `f800`, `07e0`, and `001f` must produce destination RGB fields
`ff0000`, `00ff00`, and `0000ff`; characterize the destination high byte
separately because the manual does not unambiguously assign source alpha for
the 16-bpp formats.  The reverse saturated-primary probe uses mode `00000100`,
`30=800001c0`, and pitch/location `40081010`, and must produce exactly `f800`,
`07e0`, and `001f`.

These probes use separate guarded BOs, forward traversal, GXcopy, and a
completion fence.  Reset `30=0` before any same-format copy.  Do not expose
8-bpp CFC, channel swizzles, RGBA byte order, or sRGB conversion: the recovered
2D fields describe component widths in ARGB bit significance, not colorspace
or arbitrary swizzle.  Those operations belong in the validated 3D texture
path or a CPU fallback.

## AlphaBLT and composite boundary

The public command register defines operation `3` as AlphaBLT and requires ROP
`CC`.  Register `04.bit20` selects constant alpha and `04[19:12]` carries that
eight-bit value.  Consequently the exact smallest characterization stimuli
are:

```text
per-pixel alpha:  mode=00000300, command=CC000003
constant 0x80:    mode=00180300, command=CC000003
```

Both otherwise use the ordinary color-copy base, pitch, position, and
dimension registers.  A safe test uses distinct guarded 32-bpp SF BOs, one
pixel `80ff0000` over destination `ff00ff00`, and all auxiliary state cleared.
Record the exact result and repeat with source alpha `00`, `7f`, `80`, and
`ff`; then use opaque red with constant alpha and require the constant-`80`
result to equal the recorded per-pixel-`80` result.  This resolves equation,
rounding, alpha-output, and alpha-byte interpretation without assuming
source-over semantics.

No old VX900 EXA composite uses this operation.  `viaExaSetupFuncTable` sends
PCI `7122` solid/copy to H6 2D callbacks but keeps composite on the H5/H6 3D
state, texture, shader, and blend path documented in
[via-5.75-dri-state.md](via-5.75-dri-state.md#vx900h5-render-state).
Therefore AlphaBLT is a characterization-only gate.  Production XRender/Mesa
composite should use the already validated 3D path (or CPU) until these output
rules pass silicon tests.

Source color key is similarly defined but not yet executable-proven: set
`48.bit14`, put the key in `50`, and use `48.bit13` to invert match sense.
Destination key uses `48.bit15` and register `4c`.  Never combine keying with
the first CFC, overlap, or AlphaBLT tests, and always close with `48=0`.

## Stretch is not ready

The proprietary binary exposes two opaque words through
`viaAccelScalingHelper_H6_410:0x2b0e0..0x2b184` and emits them as registers
`34` and `38` through `viaAccelSet410Helper_H6`.  When enabled it constructs:

```text
reg34 = (80000000 or C0000000) |
        ((arg6 - 1) << 16) | (arg7 - 1) | arg5
reg38 = (arg2 << 20) | 0FF00FF0 | (arg3 << 4)   # if arg2 or arg3 != 0
```

The masks are respectively `8fffffff` or `cfffffff`.  This is exact code
recovery, but not an operation recipe: the public VX900 2D register table
marks the range between CFC `30` and clipping `40` undocumented, no argument
semantics are named, and the only PCI-7122 call sites found in solid, copy,
and marker paths pass all zeros.  There is no nonzero executable vector to
cross-check source/destination extents or filter behavior.

Keep `34=38=0` for every production 2D command.  Implement stretch as a 3D
textured quad or CPU fallback.  A nonzero 2D stretch test should wait for an
independent Windows call site or a captured vendor stream; inferring factors
from these two words alone is not a sufficient anti-wedge boundary.

## Ordering, fences, cache ownership, and failure

`viaExaSolid_H6` and `viaExaCopy_H6` bracket every operation with header-0
`006c=00400000`.  A same-stream 3D producer requires the independently
recovered both-engine form `006c=00c00000` before the 2D operation; the normal
2D-only postamble remains `00400000`.  These writes order engines but are not
externally visible completion fences.

The production sequence is:

1. wait on reservation dependencies and flush CPU-dirty WB aliases;
2. install/pin all SF PTEs, execute the bounded `SR6F.bit7` GTI invalidate,
   and keep every mapping pinned;
3. emit the appropriate `006c` preamble, complete state image, operation, and
   `006c=00400000` postamble;
4. append a kernel-owned FE3x fence and retain BOs/PTEs until retirement; and
5. only after retirement invalidate/read through a CPU WB alias or latch the
   destination for scanout.

The old polling paths are useful diagnostics, not completion primitives.
`viaAccelSync_H6:0x2aa80..0x2aad5` polls MMIO `0400` until
`status & 00001ff3` is zero, stops after `00ffffff` iterations, and silently
returns even on timeout.  Linux `viafb_wait_engine_idle()` independently
constructs the same H5/M1 mask from command-regulator `10`, 2D `02`, and 3D
`00001fe1`.  A modern driver must use a time-bounded fence wait, report the
status/current/pause snapshot, wedge the context/engine on timeout, quarantine
all possibly reachable mappings, and decline further jobs until recovery.  It
must not unpin merely because the legacy status loop ended.

An `006c` packet does not invalidate SF translations or solve CPU coherency.
The exact SF PTE/cache protocol and copy-present ownership sequence are in
[sf-2d-copy-present.md](sf-2d-copy-present.md#completion-ownership-and-present)
and [sf-render-target-coherency.md](sf-render-target-coherency.md).

## Staged enablement matrix

Enable only after each earlier stage survives repeated guarded tests and the
existing 3D regression suite:

1. live-proven SL solid, then the exact one-page SF solid;
2. separate-BO SF GXcopy, followed by all four same-BO overlap directions;
3. four one-byte-mask solid tests and mandatory `48=0` closure;
4. clip inclusivity discriminator, then clipped nonzero-origin rectangles;
5. saturated-primary 565-to-8888 and reverse CFC, then gradient
   rounding/dither characterization;
6. AlphaBLT characterization only, with no advertised composite capability;
7. presentation after the render/copy fence and KMS vblank-latch protocol.

Stretch, tiled 2D addressing, host uploads, arbitrary ROPs, 8-bpp CFC,
color-key combinations, and 2D composite remain disabled.  This subset is
large enough for safe clear/copy/presentation acceleration without claiming
semantics that neither the manual nor an executable vendor path establishes.
