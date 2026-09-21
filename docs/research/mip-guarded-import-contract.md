# Chrome9 mip-chain and guarded-import contract

This note reconciles three layouts which had been easy to conflate: the
vendor's packed mip chain, the current Mesa allocator's compatibility-guarded
mip chain, and an arbitrary linear dma-buf import.  The hardware level table
can represent the first two.  It cannot make an arbitrary pitch or offset
equivalent to a canonical Chrome9 sampler image.

Unless explicitly marked **measured**, the packet facts are static
manual/binary reconstruction and still require the bounded silicon sequence
in [two-level-mip-abi-oracle.md](two-level-mip-abi-oracle.md).

## Current Mesa layout is intentionally not vendor-packed

`chrome9_resource_layout()` in the Mesa Chrome9 driver computes each logical
level with:

```text
stride[l] = ALIGN(max(width0 >> l, 1) * cpp, 32)
size[l]   = stride[l] * max(height0 >> l, 1)
```

It then retains the pre-mip level-zero object byte-for-byte:

```text
P         = system page size
offset[0] = P
cursor    = P + ALIGN(size[0], P) + P

for l = 1..last:
    offset[l] = ALIGN(cursor, 256)
    cursor    = offset[l] + size[l]

allocation_size = ALIGN(cursor, P) + P       when last > 0
```

The page immediately after the old L0 allocation remains an internal guard;
the complete chain also gains a final guard.  This is code evidence from
`chrome9_resource.c:97..137`, and the smoke test independently asserts the
same relationship at `chrome9_screen_smoke.c:1807..1819`.

For 32-bit 32x32 plus 16x16 levels on a 4 KiB-page host:

| range | object offset | size |
|---|---:|---:|
| leading guard | `0000` | `1000` |
| L0 | `1000` | `1000` |
| retained L0 guard | `2000` | `1000` |
| L1 | `3000` | `0400` |
| final alignment/guard region ends | `5000` | |

The stage-0 image address points at L0, so the H3 L1 offset is relative to
`0x1000`, not to the BO origin.  The correct entry for this current layout is
therefore:

```text
40000020                  # SF-relative, +0x2000 bytes in 256-byte units
```

The vendor-packed layout puts L1 immediately after L0 and uses `40000010`.
Both are representable.  Mixing `40000010` with the current BO would sample
the retained poison guard, while mixing `40000020` with a packed BO would
skip L1.  The kernel must derive entries from the one selected layout rather
than accepting userspace dwords or assuming that every guarded BO is packed.

The same distinction is visible in the existing tiny smoke resource.  A 2x2
L0 has stride 32 and size 64; its 1x1 L1 begins at object offset `0x3000`, not
`0x1100`, and the complete BO is `0x5000` bytes.

## Exact two-level state for the current allocation

For the guarded 32x32/16x16 example, the complete successor-address table is:

```text
FE03043C 00000008 10030000 CC000000
40000020 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

Image and sampler state remain independent of that physical spacing:

```text
image A18 = 18001101      # 2D, SF, relative level-address mode
image A20 = 20020020      # exact logical 32x32 extent
image A22 = 22008055      # POT, log2 width=height=5

A30 = 30000000 | ((bias_s5.5 & 03ff) << 12) | (max << 6) | min
A31 = 31100000 | (mag << 13) | (min_img << 10)
                | (mag << 7) | (min_img << 4) | mip
A32 = 32000040 | (wrap_t << 3) | wrap_s
```

The bounded L0--L1 values are:

| intent | A30 | A31 |
|---|---:|---:|
| nearest, L0 only | `30000000` | `31100000` |
| bilinear, L0 only | `30000000` | `31102490` |
| nearest mip, L0--L1 | `30000040` | `31100001` |
| bilinear plus nearest mip | `30000040` | `31102491` |
| trilinear | `30000040` | `31102492` |
| force L1 by min=max | `30000041` | `31100001` |

The Windows D3D/OpenGL image builders at `0x101d7c10/0x10240740` carry eight
explicit 256-byte-unit successor offsets; they do not require L1 to be at a
fixed implicit location.  Their LOD builders at
`0x101d9a40/0x10242260` independently produce the A30 formula above.  This is
why preserving the current internal guard is compatible with the recovered
hardware interface.

## What the current kernel already preserves

The structured texture paths distinguish the requested old level-zero object
size from the actual, possibly larger texture BO.  In
`chrome9_engine.c:2759..2768`, a texture source may be larger than the
derived L0 size.  Before publication, `:2984..3000` prepares the guard at the
requested L0 end and, when the BO is larger, the final object guard as well.
After retirement `:3106..3118` verifies both.  The full BO remains pinned and
cache-synchronized for the job.

This compatibility is necessary but not sufficient to enable mip sampling:
the current sampler-view validator still requires first/last level zero, and
the import/export paths reject `last_level != 0`.  A mip successor must bind
the complete BO, derive every reachable level and table entry, and retain all
pages through bounded retirement or timeout quarantine.

## Noncanonical same-device import diagnosis

The level-zero import contract is exact.  `chrome9_resource_from_handle()`
accepts a linear BGRX/BGRA dma-buf, but marks it directly guarded only when
all three values equal the locally derived tuple:

```text
external stride == canonical level_stride[0]
external offset == P
actual BO size  == canonical allocation_size
```

Any other in-bounds tuple remains `guarded=false`.  Creating a supported
sampler view for it allocates a canonical guarded shadow
(`chrome9_context.c:1269..1278`), and `chrome9_refresh_sampler_shadows()`
copies the source immediately before every draw.  Same-device PRIME still
provides the original GEM object and reservation; `guarded=false` describes
the byte layout, not device provenance.

The September 2026 full-smoke diagnostic measured exactly the intended path:

```text
shadow_source != NULL
shadow_view   != NULL
source_guarded = 0
shadow       != NULL
shadow_guarded = 1
```

The setup failure which initially appeared beside this test was not an import
or shadow failure.  A shared RGBX helper had made exact vec3 identity collide
with the older packed-gray form `vec3(VAR0.x, VAR0.y, VAR0.z)`, where `x` is
gray and `yz` are texture coordinates.  The packed-gray fragment enum then
failed its final setup assertion.  The required repair is separate exact
identity and repeated-X/packed classifiers; changing dma-buf guardedness would
mask the wrong problem.

### The current synthetic alias should not point at offset zero

The smoke test exports a canonical guarded texture and overwrites only the
returned handle's offset with zero to force the unguarded path.  The alias is
valid by bounds, but its logical pixels then overlap the original object's
leading poison guard.  The later two-pass shadow test writes through that
alias at `data_offset`, so it deliberately corrupts the exporting resource's
guard and does not check that original guard afterward.

A safer hardware-free discriminator preserves `offset=P` and changes the
external stride to another accepted 32-byte multiple while choosing a small
height whose widened rows remain in-bounds.  It still fails the exact guarded
tuple and therefore creates a shadow, but its logical writes remain inside
the original useful allocation.  Alternatively use an independent exported
BO created for this alias test.  Offset zero is not evidence of a production
layout and must never be used to justify submitting guard bytes as texels.

## Why arbitrary pitch still requires the shadow

There is no recovered independent linear texture-pitch register.  Xorg
`viaSet3DTexture_H5:0x2fc5a..0x2fc88` keeps A20 at the logical width when the
pitch is the canonical `ALIGN(width*cpp,32)`.  For a noncanonical pitch it
substitutes `pitch/cpp` into the effective width and correspondingly changes
coordinate normalization.  That transformation does not preserve general
GL repeat, mirror, clamp, derivative, or mip behavior for a logical image
whose row width differs from its storage pitch.

Therefore:

- an exact native guarded tuple may be sampled directly and zero-copy;
- a same-device but noncanonical tuple must be refreshed into its canonical
  sampler shadow;
- a mip-chain dma-buf must remain rejected until the external metadata can
  prove every level offset, stride, range, and complete guard layout; and
- a larger BO alone is not a mip-chain proof.

## Minimal implementation order

1. Keep the current strict level-zero import classifier and draw-time shadow
   refresh.  Fix shader classifiers independently.
2. Make the shadow smoke alias non-destructive and assert both the source
   useful bytes and the exporting object's original guards.
3. For a first mip successor, retain the current compatibility-guarded Mesa
   layout and synthesize each H3 entry as
   `(level_offset[l] - level_offset[0]) >> 8`; do not switch allocation and
   command layout in separate commits.
4. Require a locally allocated guarded resource, POT 32-bit 2D L0--L1, and
   canonical per-level strides for the first live test.  Keep mip import and
   export disabled.
5. Run force-L1, min=max, derivative, trilinear, and transition-closure
   oracles.  Only then advertise mip filters; NPOT mip chains, L9--L11 and
   imported chains remain separate gates.

The exact versioned UAPI structure, internal size split, packet indices, and
raw test order are fixed in
[two-level-mip-ioctl-design.md](two-level-mip-ioctl-design.md).
