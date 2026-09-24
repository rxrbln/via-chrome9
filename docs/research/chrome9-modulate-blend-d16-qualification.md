# Chrome9 TEX × primary-color blend qualification boundary

Status: static source and incident audit, 2026-09-24. No new GPU submission was
made for this note. The current fixed one-texture MODULATE plus blend gate must
remain in both Mesa and DRM until a guarded silicon oracle retires reliably.

## What is known, and what is not

The existing one-texture packet builder in `chrome9_engine.c` composes the
destination/blend H2, texture image and sampler H2s, common RT/depth H2, FE and
raster state, then TAU/ALU and immediate vertices. This is the same broad
ordering as Linux 5.75's `stmFullUpdateHardware_invi:0x177490`, which serializes
global destination state separately from image/sampler/PS state. The Windows
D3D blend builder `VTGUModeDX32A:0x101fd9e0` and OpenGL counterpart
`VTGOGL32A:0x10251220` derive A34--A37 without consulting the active TEX/ALU
count. Legacy UniChrome `viaChooseColorState` is a semantic factor cross-check,
not a Chrome9 packet grammar source. None of these establish that the *exact*
MODULATE + blend + D16 tuple retires safely on VX900.

For `DST_COLOR/SRC_COLOR` applied to RGB and alpha, full RGBA mask, no alpha
test, and D16 EQUAL/read-only, the static state discriminator is:

```text
A38..A3F  38000000 .. 3f000000       zero blend constants
A34..A37  34000010 35000100 36000420 37000204
A53       53010c0f                    full RGBA write mask
A00       00000011                    RT0 blend enable
A33       33007000                    disabled alpha test
A01       01ce1548                    MODULATE, D16 test/read-only
A12       12380000                    D16
A13       13020000                    EQUAL
A20       20007fff                    full pre-depth Z interval
```

The RGB factor selectors are `DST_COLOR=01`, `SRC_COLOR=00`; alpha aliases
`DST_ALPHA=02`, `SRC_ALPHA=01`. In the current one-texture+D16+blend builder,
the 16-word destination group closes A38--A3F and A33 before A00 enables
blending, and the 12-word common group then writes A01/A10--A13/A20. The
current one-triangle fixed MODULATE builder asserts a **232-dword** pre-tail
body (`224 + 4 blend + 4 depth`): its immediate H2 starts at body[204] and
three eight-dword vertices end at body[232]. Older design notes describe a
236-dword derivative and should not be used as an offset oracle without
reconciling that historical packet variant against the current builder.

The observed timeout is narrower than this tuple: ioquake3 GL1 reached a
**no-depth**, 16-triangle, source-over MODULATE list. Its fetch cursor reached
the post-draw H2 boundary but the final DST_POS cookie and owned PAUSE were
absent. A separate immediate MODULATE+source-over run completed 11,158 draws
before a shutdown-only failure. In the list timeout, non-CR work bits were
idle at the sample instant, while CR was busy. These facts make a missing
completion/tail or list-end handshake plausible, but do not prove it; the
complete first 32 vertices and MMIO 040c error register were not captured.
Neither the one-triangle history nor the vendor list-count implementation
justifies a count-based production whitelist.

## Why cmd22 cannot test TEX × COL0 exactly

`DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM` currently exposes only TEMP,
VARYING, CONSTANT and POSITION fragment source files. The kernel validates
that enum in `chrome9_fragment.c:chrome9_fragment_validate_source` and maps a
VARYING source to a dense texture-coordinate T input in
`chrome9_fragment_source_id`. Its raster setup still links the packed
`fragment_vertex.color` to D1 (`A02=020041d1`), but no semantic source file
can name D1. `fragment_vertex.color` therefore cannot become the ALU operand
for raw cmd22 TEX × primary color. Encoding COL0 as VARYING instead reads a
separate T input, not D1. Mesa's converter maps its internal `PS_SOURCE_INPUT`
to UAPI VARYING, so `chrome9_use_generic_fragment_program()` correctly
requires `semantic_input_mask == 0`; removing that guard would silently
miscompile primary-color shaders. The generic enqueue path also does not copy
primary color into `fragment_vertex.color` for such a shader.

Thus a guarded cmd22 `TEX × c0` or `TEX × T1` oracle can isolate texture,
one-ALU fragment output, destination blend, EQUAL/read-only D16, and command
retirement, but **cannot** qualify the fixed D1/COL0 MODULATE program or its
packet form. Adding a PRIMARY semantic source to the ABI would need its own
native-source encoding and offline validation first; this is a product-design
step, not an oracle shortcut.

## Bounded qualification ladder

1. Keep the current Mesa gate (`textured-modulate-blend-unproven`) and DRM
   preflight `-EOPNOTSUPP` gate. Confirm they reject the exact fixed tuple
   before GEM lookup or publication. Do not use a failed Gallium smoke draw as
   evidence that hardware blend is broken.
2. On a cold target with a known-good ring, run **one** generic cmd22 triangle
   with TEX × c0, then one without blend, then one with `DST_COLOR/SRC_COLOR`.
   Use distinct canonical guarded 32×32 BGRA target, 2×2 BGRA texture and
   32×32 D16 objects, nearest/clamp L0, finite in-bounds vertices, and a
   prefilled equal-depth `0x8000` plane. Texture `80808080` and c0=`0.5`
   produce approximately `40404040`; over `44332211`, clamped two-sided
   multiplication should store `221a1109` (one RGB/alpha LSB tolerance),
   leaving depth `0x8000`. Also draw a deliberately unequal-Z negative, which
   must preserve color/depth, and then an opaque COPY control on a disjoint
   rectangle. Verify exact retirement cookie, owned PAUSE, zero error/work
   bits, center/exterior pixels, all guards, source bytes and depth bytes.
   A timeout is a failure, never a reason to accept pixels without retirement.
3. Before reopening fixed MODULATE, add the vendor `FE330004` coherent 3D
   completion record and independently prove its exact ID, in addition to the
   legacy cookie, owned PAUSE and non-CR idle. The no-work and known-good
   texture/COPY cases precede the new combination; see
   `fe33-persistent-ring-production.md`. Retain all BOs and mappings in
   quarantine on a published timeout.
4. Only in a deliberately opt-in, one-shot diagnostic kernel build, admit
   the fixed single-triangle MODULATE+blend request with otherwise closed
   validation. Repeat the same BGRA/D16 EQUAL/read-only vector, first with
   source-over and then `DST_COLOR/SRC_COLOR`, each followed by a known-good
   COPY draw. Log the complete packet, full three vertices, A24 count,
   resource mappings/PTEs, FE33 ID, cookie, PAUSE, 0400/040c/043c/0444 and
   delayed status samples. Stop on the first timeout; cold power cycle only
   after retaining diagnostics. This diagnostic admission is not a production
   hardware workaround or benchmark special case.
5. If one triangle repeatedly retires, test 2/4/8/12/15/16 captured
   triangles, one new rung per cold boot. Compare REPLACE+blend and
   MODULATE+no-blend at the first failing count. Promote only the exact
   independently proven general state class; leave the broad gate otherwise.

The first generic oracle answers whether blend/depth coexist with a TEX+ALU
program. The fixed oracle separately answers whether the native primary-color
linkage and legacy immediate packet retire. Neither result may be substituted
for the other.
