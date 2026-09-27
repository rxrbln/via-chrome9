# Chrome9 OpenGL feature, limit, and format matrix

This note reconstructs the userspace feature contract of the proprietary
Chrome9 OpenGL drivers.  It is an implementation ceiling and a source of test
vectors, not permission to expose a Gallium capability before the corresponding
packet, layout, and synchronization paths work on VX900.

## Provenance and method

The two binaries used here are:

| binary | SHA-256 | relevant properties |
|---|---|---|
| `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` | `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` | ELF32, symbols and partial DWARF, 2010 Linux DRI driver |
| `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGOGL32A.dll` | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` | PE32 Windows 7 OpenGL ICD, 2015, PDB path but no PDB |

The Linux `__glExtension` object is a 302-entry array of 12-byte records at
`0x495360`; the enable byte is record offset 8.  Consequently every store in
`__glS3InvInitExtensions` (`0x1d7980`) maps mechanically to a name pointer in
the same record.  There are exactly 69 such stores.

The Windows table is the newer version of the same SI-core table.  It starts
at VA `0x1038b5c8`, has 305 20-byte records, and again has its enable byte at
record offset 8.  The straight-line initializer at `0x10042420` performs 72
stores.  Record indices and names agree with Linux after accounting for the
three earlier WGL-only records.  This is stronger evidence than taking every
`GL_*` string in either binary as supported: both binaries contain hundreds of
generic core strings which their device initializers never enable.

Evidence labels below follow the repository convention.  Addresses are image
VAs, not file offsets.

## Advertised identity and API generation

| implementation | vendor | renderer | version |
|---|---|---|---|
| Linux 5.75 | `S3/VIA Graphics, Incorporated` | `S3/VIA Graphics Chrome9 HC IGP` | `1.4 15.13.18.01` |
| Windows 24.16.01j | `S3 Graphics, Incorporated` | selected by PCI ID, below | `1.4 24.16.01j` |

**Code:** Linux `__glS3InvGetDeviceConstants` at `0x1d7840`; Windows equivalent
at `0x10042290`.

The Windows renderer selection is the only PCI-ID-dependent branch in these
two initializer functions:

| PCI ID | renderer string |
|---:|---|
| `1122` | `VIA Chrome9 HC3 IGP` |
| `3225`, `7122`, `a122` | `VIA Chrome9 HD Pro IGP` |
| `3230`, `3371` | `VIA Chrome9 HC IGP` |
| `5122`, `8122` | `VIA Chrome9 HCM IGP` |
| other | `VIA Chrome9 HC IGP` |

**Code:** Windows `0x100422a5..0x1004231a`.  The Linux build hard-codes the HC
renderer string and its extension initializer is unconditional.  This does
not prove identical hardware behavior across those chips; it proves only that
this ICD did not use either the extension string or initialized limits to
distinguish them.  No per-chip GL capability gate was found here.

## Exact advertised extension set

The following 69 extension-table records are enabled by both drivers.  The
texture-rectangle record contains two whitespace-separated names, so the
resulting extension string has 70 names:

| family | enabled names |
|---|---|
| ARB texture | `GL_ARB_multitexture`, `GL_ARB_texture_compression`, `GL_ARB_texture_cube_map`, `GL_ARB_texture_env_add`, `GL_ARB_texture_env_combine`, `GL_ARB_texture_env_crossbar`, `GL_ARB_texture_env_dot3`, `GL_ARB_texture_border_clamp`, `GL_ARB_texture_mirrored_repeat`, `GL_ARB_texture_non_power_of_two`, `GL_ARB_depth_texture`, `GL_ARB_shadow`, `GL_ARB_shadow_ambient` |
| ARB program/object | `GL_ARB_vertex_program`, `GL_ARB_fragment_program`, `GL_ARB_vertex_buffer_object`, `GL_ARB_occlusion_query` |
| ARB miscellaneous | `GL_ARB_transpose_matrix`, `GL_ARB_point_parameters`, `GL_ARB_point_sprite`, `GL_ARB_window_pos` |
| EXT texture | `GL_EXT_texture3D`, `GL_EXT_texture_cube_map`, `GL_EXT_texture_edge_clamp`, `GL_EXT_texture_env_add`, `GL_EXT_texture_env_combine`, `GL_EXT_texture_filter_anisotropic`, `GL_EXT_texture_lod_bias`, `GL_EXT_texture_rectangle GL_ARB_texture_rectangle`, `GL_EXT_texture_compression_s3tc`, `GL_EXT_shadow_funcs` |
| EXT framebuffer/state | `GL_EXT_abgr`, `GL_EXT_bgra`, `GL_EXT_blend_color`, `GL_EXT_blend_func_separate`, `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract`, `GL_EXT_packed_pixels`, `GL_EXT_rescale_normal`, `GL_EXT_separate_specular_color`, `GL_EXT_stencil_two_side`, `GL_EXT_stencil_wrap`, `GL_EXT_fog_coord`, `GL_EXT_secondary_color` |
| EXT draw/vertex | `GL_EXT_compiled_vertex_array`, `GL_EXT_vertex_array`, `GL_EXT_draw_range_elements`, `GL_EXT_multi_draw_arrays`, `GL_EXT_point_parameters`, `GL_EXT_vertex_shader` |
| SGI/SGIS/S3 | `GL_S3_s3tc`, `GL_SGIS_texture_edge_clamp`, `GL_SGIS_texture_lod`, `GL_SGIS_texture_border_clamp`, `GL_SGIS_generate_mipmap`, `GL_SGI_color_matrix` |
| imaging subset | `GL_EXT_color_table`, `GL_EXT_convolution`, `GL_EXT_histogram` |
| NV/ATI | `GL_NV_texgen_reflection`, `GL_NV_blend_square`, `GL_NV_texture_rectangle`, `GL_ATI_texture_env_combine3`, `GL_ATI_element_array`, `GL_ATI_vertex_array_object`, `GL_ATI_vertex_attrib_array_object`, `GL_ATI_fragment_shader` |
| platform/legacy | `GL_KTX_buffer_region`, `GL_WIN_swap_hint` |

The Windows initializer enables exactly three additional records/names, for 72
enabled records and 73 names in its resulting extension string:

| Windows-only advertisement | evidence | implication |
|---|---|---|
| `GL_EXT_blend_equation_separate` | table index 263, store at `0x10042530` | separate RGB/alpha blend equation path exists in the newer ICD |
| `GL_ATI_separate_stencil` | index 253, `0x10042576` | newer ICD exposes the native two-sided stencil state under another API |
| `GL_EXT_framebuffer_object` | index 286, `0x10042585` | one-color-target FBO path is considered supported by Windows |

Important absences are as informative as the enabled list:

| capability not advertised | consequence |
|---|---|
| `GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`, `GL_ARB_shading_language_100` | neither ICD exposes GLSL, despite having native vertex and pixel shader compilers |
| `GL_ARB_draw_buffers`, `GL_ATI_draw_buffers`, `GL_EXT_draw_buffers2` | expose one color render target, not MRT |
| `GL_EXT_framebuffer_blit`, `GL_EXT_framebuffer_multisample` | no advertised FBO resolve or multisample renderbuffer API |
| `GL_ARB_texture_float`, `GL_ATI_texture_float`, `GL_NV_float_buffer` | float-format conversion cases do not establish filterable or renderable float textures |
| `GL_EXT_texture_sRGB` | do not expose sRGB sampling/rendering from string presence alone |
| geometry, array, integer, buffer-texture extensions | their strings and generic entry points are core-library residue, not device support |

`GL_ARB_multisample` is not appended to either extension string.  Multisample
state is core in OpenGL 1.4, but the actual Linux framebuffer configurations
still have zero sample buffers as described below.

## Device constants and limits

The initializer writes only the fields listed here.  Unknown fields are left
to SI-core defaults.  Names in the first part are high-confidence from identity
use and texture validation; the later program block is retained in raw form
until the exact private SI structure revision is recovered.

| semantic / raw field | Linux offset/value | Windows offset/value | confidence |
|---|---:|---:|---|
| vendor pointer | `00`, string | `00`, string | exact |
| renderer pointer | `04`, fixed HC | `04`, PCI-selected | exact |
| version pointer | `08`, string | `08`, string | exact |
| texture units | `10 = 8` | `10 = 8` | high |
| maximum 2D texture dimension | `28 = 2048` | `2c = 2048` | high |
| mip-level count | `2c = 12` | `30 = 12` (computed) | high; levels 0..11 cover 2048 |
| raw recommended vertex count | `8c = 262140` | `90 = 262140` | probable `GL_MAX_ELEMENTS_VERTICES` |
| raw recommended index count | `90 = 65535` | `94 = 65535` | probable `GL_MAX_ELEMENTS_INDICES` |
| raw limit | `94 = 16` | `98 = 16` | unresolved; not the anisotropy limit, which is the SI default `ctx+0x1f4 = 16` (chrome9-anisotropic-filtering-re.md) |
| raw limit | `98 = 256` | `9c = 256` | unresolved |
| raw float | `a4 = 64.0` | `a8 = 64.0` | unresolved point/line range member |
| raw float | `b0 = 128.0` | `b4 = 128.0` | unresolved point/line range member |
| raw program block | `b8=0, bc=64, c0=0, c4=12, c8=0, cc=32, d0=0, d8=1, dc=1, e0=96, e4=24, e8=96, ec=24, f0=32, f4=32, f8=32` | `bc=0, c0=64, c4=0, c8=12, cc=0, d0=32, d4=0, dc=1, e0=1, e4=96, e8=24, ec=256, f0=24, f4=32, f8=32, fc=32` | exact raw writes; private field names unresolved |
| terminal booleans | `fd=1, fe=0` | `101=1, 102=0` | exact raw writes, semantics unresolved |

**Code:** Linux `0x1d7840..0x1d7979`; Windows
`0x10042290..0x10042418`.

The Windows structure has an inserted dword before the texture-size fields and
again differs in the program block, so offsets must not be assigned names by
blindly overlaying the two revisions.  Independent compiler evidence is safer:
the Linux native pixel compiler rejects more than 96 ALU instructions and more
than 32 texture-address instructions.  See `via-5.75-shader-re.md`.  No vertex
instruction limit should be inferred from the raw constants alone.

For the first textured Gallium screen, use 2048 as the eventual
`PIPE_CAP_MAX_TEXTURE_2D_SIZE` ceiling, expose **one** fragment/fixed-function
sampler backed by the measured stage-zero path, and one render target.  Eight
is the proprietary API ceiling, not the initial truthful implementation count.
Do not advertise vertex texture sampling, GLSL, or unresolved program limits
until NIR lowering and silicon tests establish them.  The bounded one-unit
implementation gate is specified in
[mesa-texture-bringup.md](mesa-texture-bringup.md).

The native PS/TAU encoding does contain a four-bit sampler id: the Linux
offline compiler accepts one program using all `s0..s15`, emits 16 TAU records,
and schedules the final TAU-to-ALU transition at bit 15.  That compiler proof
does not override the two ICDs' advertised limit of eight or the current
stage-zero-only silicon result.  Treat 16 as a later hardware-test candidate,
not an initial Gallium capability.

## Texture/storage format conversion

`__glS3InvQueryDeviceFormat` (`0x1d7bd0`) converts an OpenGL internal format to
a private device-format index.  Its third byte argument selects between two
representations; its exact private name is not present, so the columns below
are deliberately called selector 0 and 1.  A return of `18` is the ordinary
32-bit fallback format, not an error.

| GL internal format(s) | selector 0 | selector 1 |
|---|---:|---:|
| `GL_ALPHA`, `GL_ALPHA4..16` | `0` | `18` |
| `GL_LUMINANCE`, `GL_LUMINANCE4`, `GL_LUMINANCE8` | `4` | `5` |
| `GL_LUMINANCE12`, `GL_LUMINANCE4_ALPHA4` | `12` | `13` |
| `GL_LUMINANCE16`, `GL_LUMINANCE6_ALPHA2..16_ALPHA16`, `GL_LUMINANCE_ALPHA` | `13` | `13` |
| `GL_INTENSITY4..16` | `8` | `18` |
| `GL_R3_G3_B2`, `GL_RGB4`, `GL_RGB5` | `15` | `15` |
| `GL_RGB`, `GL_RGBA`, `GL_RGB8`, `GL_RGB10`, `GL_RGB12`, `GL_RGB16` | `18` | `18` |
| `GL_RGBA2`, `GL_RGBA4` | `16` | `16` |
| `GL_RGB5_A1` | `17` | `17` |
| `GL_RGBA8`, `GL_RGBA12`, `GL_RGBA16` | `18` | `18` |
| `GL_RGB10_A2` | `20` | `20` |
| `GL_COMPRESSED_ALPHA` | `0` | `18` |
| `GL_COMPRESSED_LUMINANCE` | `4` | `5` |
| `GL_COMPRESSED_LUMINANCE_ALPHA` | `13` | `13` |
| `GL_COMPRESSED_INTENSITY` | `8` | `18` |
| `GL_COMPRESSED_RGB` | `25` | `15` |
| `GL_COMPRESSED_RGBA` | `28` | `18` |
| `GL_RGB_S3TC`, `GL_RGB4_S3TC` | `25` | `15` |
| `GL_RGBA_S3TC`, `GL_RGBA4_DXT5_S3TC` | `28` | `18` |
| `GL_RGBA4_S3TC` | `4` | `5` |
| `GL_RGBA_DXT5_S3TC` | `13` | `13` |
| private legacy S3 enum `83a6` | `27` | `16` |
| `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` | `25` | `15` |
| `GL_COMPRESSED_RGBA_S3TC_DXT1_EXT` | `26` | `17` |
| `GL_COMPRESSED_RGBA_S3TC_DXT3_EXT` | `27` | `16` |
| `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT` | `28` | `18` |
| `GL_DEPTH_COMPONENT`, `GL_DEPTH_COMPONENT16/24/32`, `GL_STENCIL_INDEX`, `GL_STENCIL_INDEX1/4/8/16` | `29` or `30` | same |
| `GL_RGBA32F_ARB` (`8814`) | `50` | `50` |
| `GL_RGBA16F_ARB` (`881a`) | `49` | `49` |
| private sentinels `bffff/dffff/effff/fffff` | `23/22/21/24` | same |

The depth choice is controlled by a context byte at `+0xf2`; Windows uses a
context dword at `+0x108` and returns the same pair `29/30`.  This is a drawable
depth-layout choice, not an extension or PCI-ID gate.

The `84e9..84ee` cases above are the six generic
`GL_COMPRESSED_{ALPHA,LUMINANCE,LUMINANCE_ALPHA,INTENSITY,RGB,RGBA}` tokens.
They must not be mistaken for packed depth/stencil or ATI floating-point
formats.  Conversely, the two explicit `8814/881a` conversion cases are not
enough to expose float textures because neither float-texture extension is
advertised and filtering/render-target support is not established.

`__glS3InvQuerySupportedCompressedTextureFormats` at `0x2336c0` returns exactly
four public compressed formats: DXT1 RGB, DXT1 RGBA, DXT3 RGBA, and DXT5 RGBA
(`83f0..83f3`).  The 1D and 3D compressed upload hooks at `0x2336f0` and
`0x233700` are empty, as are the corresponding subimage hooks.  Treat S3TC 2D
as established and compressed 1D/3D as unsupported until another path is
proven.

The private device-index-to-SFM table at `0x4298e0` provides another exact
cross-check for indices 0..30:

```text
0:0806  1:10a5  2:18a6  3:20a7  4:0805  5:1028  6:18a8  7:20a9
8:08aa  9:10ab 10:18a3 11:20a2 12:0813 13:1029 14:20a4 15:1018
16:101b 17:1019 18:2030 19:2031 20:203a 21:101d 22:2032 23:2033
24:2040 25:10400 26:10400 27:20807 28:20809 29:2032 30:2033
```

Indices 49 and 50 are handled specially and have no entry in this short table.
They are conversion evidence only: because float texture extensions are not
advertised, they must not become Gallium sampler/render capabilities without
direct state and filtering evidence.

## Render targets, depth/stencil, and framebuffer objects

The private render-target lookup is explicit at
`stmLookupRenderTargetFormat_invi` (`0x1a6c20`):

| SFM surface format | H5/H6 render-target code |
|---:|---:|
| `1017`, `101a` | `88` |
| `1018` | `89` |
| `1019` | `8a` |
| `101b` | `8b` |
| `1023` | `30` |
| `1028`, `1029`, `102c` | `16`, `15`, `f0` |
| `2030`, `2031` | `99`, `98` |
| `2038`, `2039` | `b9`, `b8` |
| `203a`, `203b`, `203c`, `203d`, `203e` | `9a`, `ba`, `bb`, `f4`, `f5` |
| any other value | `ffffffff` |

The color renderbuffer validator at `__glS3InvRenderbufferStorage+0x47`
accepts `GL_RGB`, `GL_RGBA`, `GL_R3_G3_B2`, and every sized color format from
`GL_RGB4` (`804f`) through `GL_RGBA16` (`805b`).  It accepts depth formats
`GL_DEPTH_COMPONENT` and `GL_DEPTH_COMPONENT16/24/32`, and stencil formats
`GL_STENCIL_INDEX` and `GL_STENCIL_INDEX1/4/8/16`.  It rejects all other
renderbuffer formats, including the packed depth/stencil enumerant in this
path.

The depth conversion at `INV_D3DFmt_DDPFToDepth` (`0x1a6d60`) is:

| surface format | depth code |
|---:|---:|
| `101d` | `0` |
| `2032`, `2033` | `6` |
| `203f` | `4` |
| `2040` | `5` |

Only `2033` enables stencil in `STM_Stencil_INV`.  This agrees with the
separate format work in `via-5.75-dri-state.md` and must drive distinct
`PIPE_BIND_DEPTH_STENCIL` checks rather than treating all texture formats as
renderable.

The Windows FBO implementation is consistent with one active color attachment
plus depth and stencil: `__glS3InvFBOMakeCurrent` scans three 0x14-byte
attachment records, and the ICD does not enable any draw-buffers extension.
The attachment-role assignment is therefore a strong inference rather than a
recovered type name.  The Linux binary contains the same FBO implementation
symbols, but does not enable `GL_EXT_framebuffer_object`; the code is valuable
for packet recovery but was not a supported Linux API contract.

## Window-system formats and multisampling

Linux `s3gFillInModes` (`0x2cf980`) constructs modes for the screen's native
color depth:

- 16-bit color uses RGB565 with `GL_UNSIGNED_SHORT_5_6_5` (`8363`);
- 32-bit color uses BGRA8888 with `GL_UNSIGNED_INT_8_8_8_8_REV` (`8367`);
- depth is 16 bits when the screen depth class is 2, otherwise 24 bits;
- two variants are produced: no stencil and 8-bit stencil;
- direct-color and true-color visual types are both generated;
- no multisample array is passed and the sample-buffer fields remain zero.

**Code:** `0x2cf9a6..0x2cfab8`; `driFillInModes` at `0x2cee40`.

The binaries do contain SFM multisample machinery and the Windows registry
keys `S3GDW_MultiSampleCount` and `S3GDW_MultiSampleCountOGL`.  The Windows
default stored for both keys is `80000000`; it behaves like an automatic/default
sentinel and is not a proven sample count.  Neither ICD enables
`GL_EXT_framebuffer_multisample`,
and Linux exposes no multisample visuals.  Initial Gallium support should set
sample counts above one to unsupported.  Windows WGL pixel-format experiments
may later establish a chip-specific window MSAA path, but the static evidence
does not justify exposing it now.

## Queries and synchronization

`GL_ARB_occlusion_query` is a real hardware path rather than a generic-core
artifact:

- begin flushes the 3D pipe and emits `hwmUpdateStatus` operations of types 3
  and 1 (`__glS3InvBeginQuery`, `0x250020`);
- end allocates an HWM event, emits a status update against it, submits, queries
  status, and emits type 2 (`0x2500a0`);
- result polling calls `hwmQueryStatus`, tests event completion, and copies the
  event's result dword (`__glS3InvGetQuery`, `0x24ffb0`);
- delete releases the HWM event (`0x24ff40`).

This should become a native Gallium occlusion-counter query only after the new
DRM event/fence allocation is isolated per context and range validated.  It is
not safe to emulate the legacy global event allocator in the kernel ABI.

## Concrete Gallium and NIR plan

The safest initial capability envelope is:

| Gallium area | initial value / policy | promotion evidence required |
|---|---|---|
| 2D texture size | maximum 2048, 12 levels | layout tests for every level and format |
| texture units | 1 fragment/fixed-function unit | promote toward the proprietary ceiling of 8 only after per-stage sampler/linkage and dependent-read tests |
| vertex textures | 0 | native VS texture instruction and cache/coherency evidence |
| color buffers | 1 | do not expose MRT; no proprietary GL path does |
| MSAA | sample count 1 only | exact sample layout, resolve, clears, and WGL/static cross-check |
| occlusion query | implement after event isolation | guarded counter tests and reset behavior |
| FBO | ordinary Mesa FBO once RT/depth resources work | Windows path is positive evidence, Linux advertisement is not |
| S3TC | DXT1/DXT3/DXT5 sampling, initially 2D only | upload/readback/filter tests; honor patent-free modern Mesa policy |
| NPOT/cube/3D/rectangle | candidate sampler features | per-target pitch, mip, wrap and filter validation |
| anisotropy | candidate, cap 16 (vendor GL value) | A31 contract closed in chrome9-anisotropic-filtering-re.md; silicon rungs R1..R3 |
| float/sRGB/integer textures | unsupported initially | extension strings explicitly withhold them |
| native shader ceiling | PS 96 ALU and 32 texture instructions | compiler corpus plus on-silicon execution at boundaries |

The proprietary GL 1.4 ceiling is not the hardware ceiling.  ARB vertex and
fragment programs plus the D3D shader compiler demonstrate programmable
hardware, so a NIR backend can ultimately expose a higher Mesa GL version than
the old ICD.  The sequence should be:

1. implement format-aware resources and one color/depth target;
2. bring up fixed-function draws through explicit NIR lowering rather than
   copying the SI state tracker;
3. implement scalar/vector ALU, varyings, constants, and texture instructions
   under the recovered native limits;
4. enable occlusion queries and FBOs;
5. promote NPOT, cube, 3D, S3TC, anisotropy, and assembly-program-derived GLSL
   features only as their conformance groups pass;
6. leave MRT, MSAA, float, sRGB, integer, geometry, and vertex-texture caps off
   until new evidence closes the corresponding hardware gaps.

This ordering preserves useful evidence from both proprietary drivers without
mistaking generic SI entry points, dormant code, or strings for silicon support.
