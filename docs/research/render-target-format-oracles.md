# Chrome9 HD render-target formats and layout

Status: A52 formats `98` and `99` are live-proven on VX900 for linear SF
targets.  Formats `b8` and `b9`, tiled targets, and framebuffer sRGB retain
independent manual and executable-driver evidence but are not yet measured.
This note keeps storage format, address domain, tiling, pitch, and color-space
conversion separate so that adding EGL/FBO formats does not silently change
more than one hardware variable.

## The render-target tuple

The VX900 manual (Part II, internal pp. 106--110) defines RT0 as:

| word | field | meaning |
|---:|---:|---|
| A50 | `[23:0]` | base address in 256-byte units |
| A51 | `23` | `0` linear, `1` tiled |
| A51 | `22` | tiled height: `0` eight pixels, `1` sixteen pixels |
| A51 | `[21:20]` | `00` SL, `01` SF, `10` reserved/system memory, `11` reserved/LL |
| A52 | `[23:16]` | packed color format |
| A52 | `15` | extend non-alpha channels with zero rather than the high color bit |
| A52 | `12` | do not clamp pixel-shader output to the destination range |
| A52 | `[9:0]` | pitch: 32-byte units when linear; 256/512-byte tiles when tiled |
| A53 | `6` | de-gamma destination reads before blending |
| A53 | `[5:4]` | write gamma: `00` linear, `01` fixed gamma 2.2, `10` table |
| A53 | `[3:0]` | A, R, G, B write masks in bits 3, 2, 1, 0 |

For a linear SF target with byte pitch `P`, the bounded tuple is therefore:

```text
A50 = 50000000 | (sf_gpu_address >> 8)
A51 = 51100000
A52 = 52000000 | (format << 16) | (P >> 5)
A53 = 53010c0f                         # linear color, COPY, all channels
```

Require a 256-byte-aligned base, a 32-byte-aligned pitch, and initially
`P <= 16352`.  A50 carries no sub-256-byte offset and A50--A52 carry no width
or height.  Validate the resource extent and A80--A83 color/scissor bounds
against the BO independently.

## Exact 32-bit storage matrix

The format names below describe the packed dword, not GL's logical RGBA
component order.  The Gallium name describes increasing byte addresses on the
little-endian host.  This distinction prevents the common reversal between
`ARGB8888` register prose and `B8G8R8A8_UNORM` in Mesa.

| A52 byte | hardware name | packed dword | little-endian bytes | Gallium format | DRM fourcc | status |
|---:|---|---|---|---|---|---|
| `98` | X8R8G8B8 / ARGB0888 | `xxRRGGBB` | B,G,R,X | `B8G8R8X8_UNORM` | `DRM_FORMAT_XRGB8888` | live |
| `99` | A8R8G8B8 / ARGB8888 | `AARRGGBB` | B,G,R,A | `B8G8R8A8_UNORM` | `DRM_FORMAT_ARGB8888` | live |
| `b8` | X8B8G8R8 / ABGR0888 | `xxBBGGRR` | R,G,B,X | `R8G8B8X8_UNORM` | `DRM_FORMAT_XBGR8888` | static |
| `b9` | A8B8G8R8 / ABGR8888 | `AABBGGRR` | R,G,B,A | `R8G8B8A8_UNORM` | `DRM_FORMAT_ABGR8888` | static |

An X format supplies 1.0 when the destination alpha value is read by blending;
it is not a stored-alpha target.  Use `99` or `b9` when alpha writes,
destination-alpha factors, or later sampling of stored alpha must be real.

The manual also describes `d8/d9` (RGBA8880/8888) and `e8/e9`
(BGRA8880/8888).  Neither proprietary RT lookup below exposes them, and the
Xorg table does not contain them.  They must not be inferred from the manual
alone or advertised as alternative 32-bit FBO layouts.

### Executable provenance

Four independently useful paths agree:

1. Windows D3D `VTGUModeDX32A.dll` converts public D3D9 formats at
   `0x100c7590`, then its RT-only lookup at `0x1020a4c0` maps private
   `2030,2031,2038,2039` to `99,98,b9,b8`.  The complete lookup is bounded at
   `0x1020a4c0..0x1020a57d`.
2. Windows OpenGL `VTGOGL32A.dll` contains the same lookup at
   `0x1025d3f0..0x1025d4ad`, including the four same return immediates.
3. Proprietary Linux GL `via_chrome9_dri.so` symbol
   `stmLookupRenderTargetFormat_invi`,
   `0x001a6c20..0x001a6d53`, maps the same four private values to the same
   bytes.
4. Proprietary Xorg `via_drv.so` has source table `viaFormats` at ELF
   `0x00086640`.  Its four 32-bit XRender rows at `0x866e0..0x8672f` contain
   format bytes `00980000,00990000,00b80000,00b90000`.  During
   `viaInit3DState_H5:0x2fecd..0x2ff3e` the row's hardware word is copied into
   both dynamic destination and texture slots.  `viaSet3DDestination_H5`
   loads the destination slot at `0x2e04b..0x2e063`, and
   `via3DEmitState_H5:0x2ec19..0x2ec45` emits A50, A51, A52 with the stored
   format ORed with `pitch >> 5`.

Binary identities used for these offsets:

```text
VTGUModeDX32A.dll   sha256 8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll       sha256 a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
via_chrome9_dri.so  sha256 fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
via_drv.so          sha256 5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787
```

## Exact builder behavior for base, domain, and pitch

Windows `VTGUModeDX32A:0x101f9f90` constructs all four RT slots.  For RT0,
`0x101fa123..0x101fa1d6`:

- reads the base from `surface+f8`, shifts by eight, and preserves the A50
  subaddress byte;
- reads `surface+160`, masks it with `3fff`, shifts by five, masks the result
  with `3ff`, and merges it into A52 `[9:0]`;
- writes the RT-only lookup byte into A52 `[23:16]`;
- derives A51 bit 23 from `surface+2c` bit 1;
- calls the location helper at `0x101c9bb0` and merges its result into A51.

The helper is exact: surface memory type at `+20` returns `0` normally, `1`
for type 4, and for type 2 returns `3` only when `surface+28` bit 6 is set.
On the VX900-class device branch (`device flags+68 & 4000`), the builder puts
the two-bit result directly in A51 `[21:20]`; the older branch retains only
the helper low bit and places it in A51 bit 21.  The RT1/2/3 cases repeat this
algorithm at image offsets `160/180/1a0`.  This executable repetition is
stronger than the manual's RT1 page, which incorrectly labels its location
field reserved.

Linux `STM_RenderTarget_INV:0x195e90` independently constructs RT0 at hardware
image offsets `140/144/148`.  At `0x195f7e..0x19604d` it loads base
`surface+140`, pitch `surface+194`, calls the RT-only lookup, emits base/256,
sets A51 bit 23 from `surface+20` bit 1, emits `pitch >> 5`, and writes the
format byte.  Its VX900-capable branch at `0x196802..0x196814` clears both
location bits and selects bit 20 for memory type 4, exactly producing the
live SF word `51100000`.  The older branch at `0x196066..0x196078` selects
bit 21 instead.

The unambiguous linear interpretation is byte pitch divided by 32.  The same
`byte_pitch >> 5` expression is also the exact tiled pitch; it does **not**
require a second pre-scaling step.  Proprietary Linux helper
`sfmCalculateTileOffset_invi:0xdee30..0xdee96` selects a tile width of 32, 16,
or 8 pixels for 8, 16, or 32 bpp respectively, and computes:

```text
tile_width_pixels = 256 / bytes_per_pixel
tile_columns = ceil(pixel_pitch / tile_width_pixels)
offset = 256 * (tile_columns * floor(y / 8) + floor(x / tile_width_pixels))
       + bytes_per_pixel *
         (tile_width_pixels * (y & 7) + (x % tile_width_pixels))
```

For the vendor's normal 32-byte-aligned byte pitch,

```text
byte_pitch / 32
    = ceil(pixel_pitch * bytes_per_pixel / 32)
    = ceil(pixel_pitch / tile_width_pixels)
    = tile_columns.
```

Thus A52 `[9:0]` is a 32-byte unit count for linear addressing and the
numerically identical microtile-column count for an eight-row tiled surface.
The manual's "256-byte" tiled unit describes the storage consumed by one
column entry over eight rows, not a request to divide the byte pitch by 256.
For example, a 32-bpp 256-pixel surface has byte pitch 1024 and A52 pitch 32;
that is also 32 tiles of 8x8x4 bytes.  The tiled allocation extent is
`byte_pitch * align(height, 8)` and every tile base is 256-byte aligned.

This closes the A52 producer convention and the eight-row microtile swizzle,
but it does not by itself make tiled modifiers safe to expose.  The Windows
RT builder changes only A51 bit 23 from `surface+2c` bit 1 and leaves A51 bit
22 clear, so its recovered path is specifically the 256-byte/eight-row tile.
The 512-byte/sixteen-row layout, external modifier identity, CPU detiling,
2D-copy transitions, and live guarded extent test remain outstanding.  Until
those pieces land, accept only linear targets in the public resource path.

## sRGB is A53 state, not an A52 format

The packed storage remains `98`, `99`, `b8`, or `b9`.  Enabling framebuffer
sRGB changes destination read/write conversion in A53:

```text
linear COPY/all channels:  A53 = 53010c0f
sRGB  COPY/all channels:   A53 = 53010c5f
                                      ^^ bit6 read de-gamma + mode01 write gamma
```

Windows `VTGUModeDX32A:0x101f9d44..0x101f9d85` reads the RT0 sRGB Boolean at
context `+1dc0`; when set it selects A53 `[5:4]=01` and bit 6 together.  This
update is conditional on adapter workaround bit `[adapter+390] & 40` being
clear; if that bit is set the builder skips both conversions.  The same gate
and conversion appear for RT1--RT3 at `0x101f9db9..0x101f9edd`.  The manual
independently identifies mode 01 as fixed linear-to-gamma-2.2 on writes and
bit 6 as gamma-2.2-to-linear on destination reads before blending.  Alpha is
not gamma converted.  Consequently the tentative Gallium mappings are:

| Gallium sRGB storage | A52 | A53 when framebuffer-sRGB conversion is active |
|---|---:|---:|
| `B8G8R8X8_SRGB` | `98` | `53010c5f` |
| `B8G8R8A8_SRGB` | `99` | `53010c5f` |
| `R8G8B8X8_SRGB` | `b8` | `53010c5f` |
| `R8G8B8A8_SRGB` | `b9` | `53010c5f` |

Do not make A53 permanently `5f` merely because the resource's storage format
is sRGB.  GL controls encoding with `GL_FRAMEBUFFER_SRGB`; when conversion is
disabled the same storage bytes use the linear A53 state.  Sampling sRGB is a
separate texture-state mechanism and does not justify this destination bit.

Two bounded live discriminators remain required before advertising sRGB:

- write linear gray `0x80` through `53010c5f`; stored RGB should be about
  `0xbc`, while `53010c0f` stores `0x80`;
- blend half-alpha white over encoded sRGB gray `0x80`; correct read de-gamma
  plus write gamma gives about `0xcd`, while write gamma without read
  de-gamma gives about `0xe1`.

## Minimal EGL/FBO enablement order

The existing successor ABI can grow without changing any ioctl structure
size.  Preserve the current numeric values and append:

```text
DRM_CHROME9_DESTINATION_FORMAT_BGRX8888 = 0 -> A52 98
DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 = 1 -> A52 99
DRM_CHROME9_DESTINATION_FORMAT_RGBX8888 = 2 -> A52 b8
DRM_CHROME9_DESTINATION_FORMAT_RGBA8888 = 3 -> A52 b9

DRM_CHROME9_BLEND_FRAMEBUFFER_SRGB_ENABLE = 1 << 2
```

The kernel should lower the destination enum through a closed switch rather
than another two-way ternary and reject values above 3.  The sRGB flag changes
only A53 bits `[6:4]` from `000` to `101`; it must preserve raster-op and
write-mask fields.  A separate capability bit should gate the new A52 pair,
and a different capability bit should gate A53 sRGB conversion.  Requiring
the blend-aware successor for these formats is appropriate even when blending
is disabled, because the legacy ioctls cannot carry either selection.

Suggested first exact words for the existing canonical 32-by-32 pitch are:

```text
BGRX linear:  A52=52980004  A53=53010c0f
BGRA linear:  A52=52990004  A53=53010c0f
RGBX linear:  A52=52b80004  A53=53010c0f
RGBA linear:  A52=52b90004  A53=53010c0f
BGRA sRGB:    A52=52990004  A53=53010c5f
RGBA sRGB:    A52=52b90004  A53=53010c5f
```

The names above follow increasing host-memory byte order.  Avoid naming the
new enum `ARGB`/`ABGR`: those hardware packed-dword names are reversed from
the corresponding Gallium byte-order names on little-endian systems.

1. Keep the already live `B8G8R8X8_UNORM/A52=98` window and internal-target
   path.
2. Keep `B8G8R8A8_UNORM/A52=99` as an internal render target until its
   display/import/export contract is implemented.  Its COPY, alpha-only mask,
   and independent-alpha source-over results are already live.
3. Add `R8G8B8X8_UNORM/b8` and `R8G8B8A8_UNORM/b9` only after guarded channel
   order tests.  Each test must vary only A52, write logical red/green/blue,
   and compare exact dwords (`000000ff` red for `b8`, `ff0000ff` opaque red
   for `b9`).
4. Accept `PIPE_TEXTURE_RECT` for an internal DRI drawable destination using
   the same guarded linear layout.  Keep rectangle sampling rejected; enabling
   the full NPOT cap solely to force a 2D drawable target overclaims mip,
   filtering, and wrap behavior.
5. Expose sRGB target formats only after both A53 discriminators pass.  The
   resource/view format selects A52 storage; dynamic framebuffer-sRGB state
   selects A53 conversion.

### Mesa frontend traps relevant to EGL

Current Mesa does not communicate `GL_FRAMEBUFFER_SRGB` as a bit in
`pipe_blend_state`.  In `st_atom_framebuffer.c`, it chooses the bound
`pipe_surface.format` through `_mesa_renderbuffer_get_format()`: the surface
view becomes the sRGB twin when conversion is active and the linear twin when
it is not.  Chrome9 must therefore derive the A53 conversion bit from
`ctx->framebuffer.cbufs[0].format`, not only from
`surface->texture->format`.  The safe view check is:

```text
util_format_linear(surface->format) ==
util_format_linear(surface->texture->format)
```

plus the already required level/layer checks.  An arbitrary same-size format
reinterpretation remains invalid.  `chrome9_set_framebuffer_state()` and the
draw validator currently require exact view/resource format equality; leaving
that check unchanged would reject the state tracker's legitimate linear/sRGB
view switch even after the hardware format is enabled.

The DRI frontend presents another independent boundary:

- `dri_fill_in_modes()` queries window configs with
  `PIPE_TEXTURE_2D | RENDER_TARGET | DISPLAY_TARGET` and includes both linear
  and sRGB format candidates.
- `dri_init_screen()` selects `PIPE_TEXTURE_RECT` for actual internal drawable
  resources while `caps.npot_textures` is false.
- `dri_drawable_get_format()` intentionally requests a linear winsys resource;
  the state tracker later uses an sRGB surface view when appropriate.

Thus destination-only RECT support is required for an EGL/GLX drawable even
if FBO textures remain ordinary 2D.  Conversely, advertising an sRGB EGL
window config requires DISPLAY_TARGET support for the linear storage too; an
internal-only sRGB FBO implementation is not sufficient.  Keep those two
capabilities separate.

For every format, require level and layer to match the underlying resource and
permit only the exact linear/sRGB twin as a differing `pipe_surface` format.
A view must not reinterpret `98` as `99`, or `b8` as `b9`, because that changes
stored/destination alpha semantics without changing allocation size.  Continue
using guarded linear SF BOs, bounded fence waits, and exact resource-extent
checks for first silicon runs.
