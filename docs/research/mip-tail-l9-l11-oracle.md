# Chrome9 L9--L11 mip-tail layout and bounded oracle

Status: the proprietary allocator's complete L0--L11 byte layout is now
executed and exact for canonical linear 32-bit textures.  The register model
and both Windows/Linux state builders prove that L9--L11 have no additional
H3 address words.  The conclusion that hardware advances from L8 in
256-byte blocks is therefore the only layout consistent with the vendor
allocator, but remains an **inference until the bounded silicon oracle below
passes**.

The machine-readable vectors are in
[mip-tail-layout-corpus.tsv](mip-tail-layout-corpus.tsv).

## Proven register boundary

The Chrome9 HD 3D manual gives three independent parts of the contract:

* A30 maximum and minimum level accept 0 through 11 (manual register page
  129).
* H3 subtype 10 contains exactly eight words, for L1 through L8 (manual
  register pages 144--145).  There is no second address packet.
* For L8, L9, L10, and L11, A18 selects the location from the L8 field
  (manual register pages 122--123).  This statement controls SL/SF location;
  it does not by itself specify a byte offset.

The Linux texture builder at `stmHWPrepareTextureRegs_invi:0x1b251d` accepts
up to twelve resource levels.  Both address-building branches cap their
explicit loop to nine total levels at `0x1b264a..0x1b2654` and
`0x1b2ab1..0x1b2abb`, hence emit only L1--L8.  Windows D3D
`VTGUModeDX32A:0x101d7c10` independently accepts twelve and applies the same
nine-total-level cap at `0x101d8112..0x101d811b` and
`0x101d8658..0x101d8665`.  The Windows OpenGL copy uses the same state model.

The corresponding A30 builders do not clamp the selector to L8:

* Linux `STM_MipMapLOD_INV:0x1b1a50`;
* Windows D3D `VTGUModeDX32A:0x101d9a40`;
* Windows OpenGL `VTGOGL32A:0x10242260`.

All encode

```text
A30.low24 = ((signed_lod_bias_s5_5 & 0x3ff) << 12)
           | ((maximum_level & 0x3f) << 6)
           |  (minimum_level & 0x3f)
```

and the Windows builders cap the resource count to twelve before deriving
the maximum selector.  Thus the absent L9--L11 address words are intentional,
not a software limit accidentally inconsistent with A30.

## Executed proprietary layout helper

The Linux UMD image used here is
`via_chrome9_dri.so`, SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
An isolated 32-bit harness mapped its executable page at file offset
`0xec000` and called `sfmCalcMipOffset_inv:0xec2e0` directly.  The synthetic
surface used format class 2, 32 bits per pixel, and the stated width/height;
the requested level was varied from zero through twelve.  No display, DRM,
or GPU access was involved.

The function returned:

```text
2048x2048 L0=00000000 L1=01000000 L2=01400000 L3=01500000
          L4=01540000 L5=01550000 L6=01554000 L7=01555000
          L8=01555400 L9=01555500 L10=01555600 L11=01555700
          L12=01555800

2048x1025 L0=00000000 L1=00810000 L2=00a10000 L3=00a90000
          L4=00ab0000 L5=00ab8000 L6=00aba000 L7=00aba800
          L8=00abaa00 L9=00abab00 L10=00abac00 L11=00abad00
          L12=00abae00
```

Static disassembly explains the result.  Each level uses

```text
pitch            = max(32, align(width * 4, 32))
allocated_height = max(8, align(height, 8))
level_bytes       = pitch * allocated_height
next_width        = max(1, width / 2)
next_height       = max(1, height / 2)
```

Consequently every 32-bit level at and below 8x8 occupies exactly 256 bytes.
For the vendor's canonical contiguous resource layout,

```text
offset(Lm) = offset(L8) + (m - 8) * 0x100,  m in [9,11]
```

This equation is **measured UMD allocation policy plus a hardware inference**.
It must not be described as a measured GPU address rule until the silicon
test succeeds.

## Exact POT L0--L11 oracle

Allocate one contiguous, canonical linear 2048x2048 XRGB8888 or BGRA8888
chain.  Its useful range through L11 ends at byte `0x01555800`.  Program:

```text
A18 = 18001101
A20 = 20800800
A22 = 220080bb

FE03043C 00000008 10030000 CC000000
40010000 40014000 40015000 40015400
40015500 40015540 40015550 40015554
```

The H3 entries are `(level_offset >> 8) | 0x40000000` for L1--L8.  Fill L8,
L9, L10, and L11 with four unmistakable solid colors and poison every other
level.  With nearest image and mip filtering, force one level at a time:

| selected level | A30 |
|---:|---:|
| L8 | `30000208` |
| L9 | `30000249` |
| L10 | `3000028a` |
| L11 | `300002cb` |

The four draws must return the four tail colors in order.  Run the transition
`L8 -> L9 -> L10 -> L11 -> L8` in one context without reinstalling unrelated
state; the final L8 draw catches retained implicit-tail selection.

All levels and their row/height padding must be initialized.  The command
validator must derive the eight H3 words, require the full
`0x01555800` range to remain pinned, and reject user-provided raw addresses.
This makes every candidate fetch land in mapped, recognizable data even if
the implicit rule differs.

## Exact asymmetric NPOT discriminator

After the POT chain passes, repeat with 2048x1025.  This separates an actual
L8-rooted tail rule from a rule accidentally dependent on equal dimensions.
The useful range through L11 ends at `0x00abae00`.  Program:

```text
A18 = 18001101
A20 = 20401800
A22 = 220000ab

FE03043C 00000008 10030000 CC000000
40008100 4000a100 4000a900 4000ab00
4000ab80 4000aba0 4000aba8 4000abaa
```

The allocator puts L8/L9/L10/L11 at `0x00abaa00`, `0x00abab00`,
`0x00abac00`, and `0x00abad00`.  Use the same four A30 selectors and color
sequence.  A22 bit 15 is clear; its height and width exponent nibbles are the
truncated `log2(1025)=10` and `log2(2048)=11` values `a` and `b`, as emitted
by both proprietary builders.

## Promotion and failure rules

This first oracle deliberately covers only canonical, linear, uncompressed
32-bit images.  Do not generalize the rule to tiled, compressed, imported
noncanonical-pitch, cube, or volume resources.  Although canonical 8-bit and
16-bit tail levels also reach the same 32-byte by 8-row minimum, they require
their own format fetch tests.

On any wrong color, engine error, or timeout:

1. keep the current `last_level <= 8` capability limit;
2. quarantine the context under the existing timeout policy;
3. retain all referenced mappings until the engine is known idle or reset;
4. save the A18/A20/A22/A30 words, eight H3 entries, fence state, and MMIO
   status before teardown.

Only successful POT and asymmetric NPOT execution closes the L9--L11 item in
the Gallium NPOT capability gate.  Transition back to a one-level resource
must rewrite A30 to select L0 and clear all eight stale H3 words.

## Confidence ledger

| fact | confidence | evidence |
|---|---|---|
| A30 selects L0--L11 | high static | manual plus three executable builders |
| only L1--L8 have explicit H3 addresses | high static | manual plus Linux and Windows builders |
| L8 location applies through L11 | high static | manual A18 procedure |
| canonical L9--L11 allocation offsets | high executable | direct call to proprietary Linux helper |
| hardware uses consecutive 256-byte blocks after L8 | high inference, unmeasured | only rule consistent with executed allocator and absent address fields |
| truthful 2048-wide NPOT mip capability | not yet | requires both bounded silicon tests |
