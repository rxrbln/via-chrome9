# Bounded native clip-space VS UAPI and live oracle

## Scope

This is a proposal for the first kernel ABI which executes the proprietary
compiler's generic vertex finalizer.  It is intentionally narrower than a
general shader ABI:

- userspace selects an immutable VS and PS template ID; it never supplies
  native words, counts, register numbers, or packet controls;
- the matrix is fixed to identity, so the input position is already clip
  space even though the selected native program executes an M4x4;
- only one immediate triangle, COPY color output, no depth, no blend, no
  mipmaps, and at most two guarded level-zero textures are accepted;
- two address-free no-vertex probes must pass before a template can draw;
- success does not advertise GLSL, ES, or any new Gallium shader cap.

The identity-only ABI is deliberate.  A later UAPI can add a 16-float matrix
without reinterpreting reserved fields after the clip-space path is proven.

## Proposed UAPI

The canonical [UAPI allocation ledger](chrome9-uapi-allocation.md) reserves
command `0x10` and capability bit 24 for this proposal.  Earlier values
`0x0b`/15 are now occupied by textured D16 and depth-function support.  Do
not advertise the clip capability until the guarded draw gates below have
passed on the device.

```c
#define DRM_CHROME9_DRAW_CLIP_TRIANGLE 0x10
#define DRM_CHROME9_CAP_DRAW_CLIP_TRIANGLE (1ULL << 24)

#define DRM_CHROME9_CLIP_OP_VALIDATE          0
#define DRM_CHROME9_CLIP_OP_PROBE_PROGRAM     1
#define DRM_CHROME9_CLIP_OP_PROBE_ACTIVATE    2
#define DRM_CHROME9_CLIP_OP_DRAW              3

#define DRM_CHROME9_CLIP_VS_MVP_POSITION              0
#define DRM_CHROME9_CLIP_VS_MVP_POSITION_T0           1
#define DRM_CHROME9_CLIP_VS_MVP_POSITION_T0_T1        2
#define DRM_CHROME9_CLIP_VS_MVP_COL0_T0               3
#define DRM_CHROME9_CLIP_VS_MVP_COL0_T0_T1            4

#define DRM_CHROME9_CLIP_FS_CONSTANT_RED       0
#define DRM_CHROME9_CLIP_FS_COL0               1
#define DRM_CHROME9_CLIP_FS_TEX0_REPLACE       2
#define DRM_CHROME9_CLIP_FS_TEX0_MODULATE_COL0 3
#define DRM_CHROME9_CLIP_FS_TEX0_MUL_TEX1      4

struct drm_chrome9_clip_vertex {
        __u32 x_float;
        __u32 y_float;
        __u32 z_float;
        __u32 w_float;
        __u32 color;          /* normalized A8R8G8B8 / FVF format d1 */
        __u32 s0_float;
        __u32 t0_float;
        __u32 s1_float;
        __u32 t1_float;
};

struct drm_chrome9_draw_clip_triangle {
        __u32 op;
        __u32 vs_template;
        __u32 fs_template;
        __u32 flags;          /* must be zero */

        __u32 destination_handle;
        __u32 width;
        __u32 height;
        __u32 destination_pitch;

        struct drm_chrome9_texture_state textures[2];
        struct drm_chrome9_clip_vertex vertices[3];
        struct drm_chrome9_scissor_state scissor;
        __u32 reserved[8];    /* must be zero; total request size 256 */
};
```

`VALIDATE` performs all table and scalar checks but submits nothing.
`PROBE_PROGRAM` emits one address-free state/program transaction without A60.
`PROBE_ACTIVATE` repeats that first transaction, waits for it, then submits a
second address-free transaction containing only the exact A60 word.  Doing
both steps inside one ioctl while holding the submission lock prevents another
client from replacing the program between phases.  It must not depend on a
previous ioctl.  For all three non-draw operations every byte beginning at
`destination_handle` must be zero; no GEM object is looked up or pinned.

`DRAW` emits complete state in one ordered submission.  It is accepted only
after `PROBE_PROGRAM` and `PROBE_ACTIVATE` passed for the exact `(VS,FS)` key
on this device.  Probe success is kernel-private state, not a userspace flag.

### Closed template compatibility

The PS may consume only outputs actually written by the selected VS.  This
table is the complete initial allow-list:

| VS | accepted PS |
|---|---|
| position | constant red |
| position + T0 | constant red, TEX0 REPLACE |
| position + T0/T1 | constant red, TEX0 REPLACE, TEX0*TEX1 |
| position + COL0 + T0 | constant red, COL0, TEX0 REPLACE, TEX0*COL0 |
| position + COL0 + T0/T1 | all five initial PS templates |

The last row can be tested twice with the same VS: `COL0` proves the color
link while `TEX0*TEX1` proves both coordinates.  There is not yet an exact
three-input `TEX0*TEX1*COL0` PS template, so that expression is rejected.

## Exact kernel-owned vertex layouts

The UAPI record is not copied verbatim.  The kernel emits only fields consumed
by the selected template, producing these compact command-mode declarations:

| VS | H6 A00 and element words | immediate stride |
|---|---|---:|
| position | `00000004 010000c2` | 4 |
| position + T0 | `00000106 010000c2 02004142` | 6 |
| position + T0/T1 | `00000208 010000c2 02004142 03006242` | 8 |
| position + COL0 + T0 | `00000207 010000c2 020041d1 03005242` | 7 |
| position + COL0 + T0/T1 | `00000309 010000c2 020041d1 03005242 04007342` | 9 |

These words follow the recovered H6 declaration fields exactly: A00 `[11:8]`
is the last element, `[6:0]` is total dwords, element `[19:12]` is its dword
offset, `[11:8]` is the dense VS input, float2 is format `42`, float4 is `c2`,
and normalized packed color is `d1`.  The normalized bit is mandatory; `d0`
was live-proven to turn ordinary byte alpha into values above one which clamp
during blending.

Immediate payloads are correspondingly:

```text
position:             x y z w
position + T0:        x y z w s0 t0
position + T0/T1:     x y z w s0 t0 s1 t1
position + COL0 + T0: x y z w color s0 t0
all outputs:          x y z w color s0 t0 s1 t1
```

All keys use independent triangle list A20=`20000000`, Gouraud A23=`23004f02`,
and A24=`24000003`.  The Type-00 payload is exactly three times the selected
stride.  H2 alignment padding is outside that count and must be generated by
the common emitter.

## Exact native template table

Every template begins with the same four M4x4 records and full position MOV:

```text
0008014c 0040014c 00000000 00060801
0008014c 0040414c 00000000 00060802
0008014c 0040814c 00000000 00060804
0008014c 0040c14c 0000014c 0034010f
0010014c 00000000 00000000 00080a0f
```

Append the selected dense outputs in the order COL0, T0, T1:

```text
COL0 from v1: 0008414c 00000000 00000000 0008422f

# templates without COL0
T0 from v1:   0008414c 00000000 00000000 00080283
T1 from v2:   0008814c 00000000 00000000 00080293

# templates with COL0
T0 from v2:   0008814c 00000000 00000000 00080283
T1 from v3:   0008c14c 00000000 00000000 00080293
```

Then append the compiler's indivisible five-record finalizer/tail:

```text
00201b6c 00000000 00000000 00280108
00200150 00101b6c 00201b6c 001c0107
0010014c 00380150 00384154 0004021f
0020014c 00000000 00000000 0008020f
0020014c 00000000 00000000 0008020f
```

The last two records are uploaded but inactive.  The final table, including
the newly recovered all-output form, is:

| VS | upload | active | metadata | A25 / A26 | A28 | A62 | A60 |
|---|---:|---:|---:|---|---|---|---|
| position | 10 | 8 | `00000004` | `25f00000 / 26000000` | `28f9cc00` | `62008003` | `60800007` |
| position + T0 | 11 | 9 | `00000104` | `25f0c000 / 26000000` | `28f9cc00` | `62108005` | `60800008` |
| position + T0/T1 | 12 | 10 | `00000204` | `25f0cc00 / 26000000` | `28f9cc00` | `62208006` | `60800009` |
| position + COL0 + T0 | 12 | 10 | `00000144` | `25f4c000 / 26000000` | `28fdcc00` | `62208006` | `60800009` |
| position + COL0 + T0/T1 | 13 | 11 | `00000244` | `25f4cc00 / 26000000` | `28fdcc00` | `62308007` | `6080000a` |

`A62[23:20]` matches the last dense H6 input element and only its low nine
bits come from the compiler object.  The table therefore does not copy the
fixed Xorg `...81ff` value.  A61 is the initialized `61800000` for every
initial key.  A28 starts from the proven `28ffcc00` image and replaces only
bits 18:17 from A25; no other A28 field is synthesized.

The all-output form was recovered hardware-free from the local Linux 5.75
compiler with postprocessing enabled.  It reports 13 uploads, 11 active,
metadata `00000244`, and compiler A62 low value 7.  Its D3D source was M4x4,
full oPos, full oD0, T0.xy, T1.xy.  The compact form above uses v1/v2/v3; the
native words and metadata are now covered by the offline Mesa corpus.

### Constants and PS packets

The identity matrix is a kernel-owned c0..c3 H3 upload at selector
`32030000`:

```text
3f800000 00000000 00000000 00000000
00000000 3f800000 00000000 00000000
00000000 00000000 3f800000 00000000
00000000 00000000 00000000 3f800000
```

Retain the live-proven internal c256/c257 upload at selector `32030400`:

```text
3f800000 3f800000 3f800000 3f800000
49400000 49400000 00000000 00000000
```

The generic native tail reads raw position 128 and viewport internals 224/225,
not these user constants.  Do not expose either constant store to userspace.
The PS blocks must be byte-for-byte one of the finalized programs already in
`shader-upload-oracles.md`: constant red, COL0, TEX0 REPLACE, TEX0*COL0, or
TEX0*TEX1.  Controls, TAUs, ALUs, dependency bits, and CC/DD terminators form
one immutable table entry; mixing pieces from different entries is rejected.

## Two address-free live gates

Both probes begin only after the normal ring prime and proven 2D-to-3D
transition.  They contain no target, depth, texture-image, sampler, vertex
buffer, or Type-00 packet and therefore contain no GPU address.

### Phase 1: program without activation

Emit, in normal state order:

1. the compact FVF declaration and matching A25/A26/A28;
2. identity c0..c3, c256/c257, and the existing zero integer store;
3. A61 and A62, deliberately omitting A60;
4. the complete VS store including both inactive tail records;
5. the selected complete PS control and program stores;
6. the standard status/cookie tail and PID00 PAUSE.

Before kickoff assert exact equality with the selected table: program byte
count is `upload * 16`, `active <= upload`, `A60.low9 == active-1` even though
A60 is omitted, all headers are 16-byte aligned, every unused/pad dword is the
normal `cc`/`dd` sentinel, and no emitted payload matches an address-bearing
destination/image/stream selector.

Expected completion is PID00 PAUSE with MMIO 0444 equal to `c0000000` or the
legal quiescent `80000000`, MMIO 0400 non-CR busy mask zero, error register
zero, and the normal cookie observed.  Snapshot a separate guarded BO before
the call and require it byte-identical afterwards even though it was never
looked up by the ioctl.

### Phase 2: A60 only

`PROBE_ACTIVATE` first repeats and waits for phase 1.  It then emits one H2
type-04 transaction containing exactly the table's A60 plus normal alignment
padding, followed by status/cookie/PAUSE.  It emits no program store and no
primitive or address.  Require the same 0444/0400/error/cookie result and the
same untouched external guard BO.

Failure of either phase quarantines that exact `(VS,FS)` key before any draw.
A timeout additionally quarantines every native-clip key for the device until
module reload or reboot; it must not try a nearby A62/A60 value.

## Guarded draw oracle

Use 32x32 X8R8G8B8 targets with pitch 128.  Each target is exactly three
pages: guard, one page of pixels, guard.  Fill guards with two different
nonzero patterns.  Texture BOs use the same guarded layout, are distinct from
each other and from both targets, and are snapshotted before submission.

The identity clip triangle is exact IEEE-754 data:

```text
v0 = (-0.5, -0.5, 0.5, 1.0) = bf000000 bf000000 3f000000 3f800000
v1 = (+0.5, -0.5, 0.5, 1.0) = 3f000000 bf000000 3f000000 3f800000
v2 = ( 0.0, +0.5, 0.5, 1.0) = 00000000 3f000000 3f000000 3f800000
```

Create a reference image first with the already live post-TNL path at screen
coordinates `(8,24)`, `(24,24)`, `(16,8)` in a second guarded target.  Clear
both pixel pages identically.  Exact success means the entire candidate pixel
page equals the reference page, not merely a few samples, and both pairs of
guards remain unchanged.

Run these rungs in order, never batching them:

1. `MVP_POSITION + CONSTANT_RED`; require exact reference coverage and
   `00ff0000` in every covered XRGB pixel.
2. `MVP_COL0_T0 + COL0`; use opaque red, green, and blue vertices and compare
   against the proven normalized-Gouraud reference.  This proves `d1` and
   COL0 linkage before sampling.
3. The same VS with `TEX0_REPLACE`; use a guarded 2x2 BGRA texture containing
   four distinct opaque colors, NEAREST/CLAMP, and set all three T0 values to
   `(0.25,0.25)`.  Every covered pixel must equal the selected texel.
4. The same VS with `TEX0_MODULATE_COL0`; first use white COL0 to reproduce
   rung 3, then `0x80808080` to distinguish normalized modulation.
5. `MVP_POSITION_T0_T1 + TEX0_MUL_TEX1`; give the second guarded 2x2 texture
   independent colors, use constant in-range coordinates, and require the
   exact componentwise RGBA product with the already accepted rounding.
6. `MVP_COL0_T0_T1` twice: once with COL0 and once with TEX0*TEX1.  Both full
   images must reproduce rungs 2 and 5 respectively.  This proves every
   output of the largest VS without requiring an unproven three-input PS.

After every rung require cookie, PAUSE, non-CR idle, error zero, immutable
texture bytes, and all guards before performing the already established 2D
fill regression.  A result mismatch with clean idle quarantines only that
key; timeout, engine error, source mutation, or any guard change quarantines
the whole clip path.

## Validation and quarantine rules

The initial draw validator accepts only finite IEEE values, exact
`w=3f800000`, `-1 < x,y < 1`, and `0 <= z <= 1`.  Required texture coordinates
must be finite and within `[0,1]`; unused color/coordinate fields must be zero.
Width and height are 1..2048, destination pitch is exactly
`ALIGN(width*4,32)`, texture pitch is exactly `ALIGN(texture_width*4,32)`, and
all size arithmetic is checked before GEM lookup.  Destination and sources
must be distinct native guarded shmem objects.  The first live gate further
uses only the exact 32x32 vector above.

Reject nonzero flags/reserved data, unsupported VS/PS pairs, aliases, depth,
blend, non-XRGB targets, non-BGRA/BGRX textures, mip levels, projection,
out-of-range coordinates, and any request whose rebuilt packet differs from
the immutable table.  No validation failure submits a prefix.

Maintain separate per-device `program_ok`, `activate_ok`, `draw_ok`, and
`quarantined` bitsets indexed by the closed VS/PS key.  They reset only at
module load.  Never persist them across reboot and never let userspace set or
clear them.  A failed experimental key falls back to the existing software
VS/post-TNL ioctl in Mesa; the kernel ioctl itself returns an error and does
not silently substitute another shader.  The public capability bit remains
clear until all intended keys pass and the non-experimental draw path uses
the same validators and immutable tables.
