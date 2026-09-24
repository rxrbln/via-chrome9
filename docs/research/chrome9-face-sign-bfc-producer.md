# Chrome9 back-face-color face-sign producer

Status: manual-backed architecture and proprietary-driver boundary, offline
only, 2026-09-24.  This is the first distinct raster producer found for a
GLSL `gl_FrontFacing`-equivalent sign.  It is not yet a live VX900 capability:
the back-face FVF route and programmable VS output still need a guarded
silicon oracle.

## The raster front end already performs the required selection

The VX855/VX875 Chrome9 HCM manual defines two extra primitive-vertex
attributes in HParaType 04 flexible-vertex-format state:

```text
31h  Back Face Color0 (BFCdiff)
32h  Back Face Color1 (BFCspec)
```

HParaType 04 sub-address 10 bits 23 and 22 independently say that the
primitive vertex contains BFCdiff and BFCspec.  The earlier HC3 manual gives
the same attribute numbers and mask bits.  The VX900/Chrome9 HD manual retains
the consuming raster state: HParaType 04 A23 bit 7 chooses which physical
winding receives the VS `oBFD#` back-face colors.

The HCM manual also specifies the selection algorithm, rather than only a
field name.  With back-face culling disabled, a triangle matching A23 bit 7
uses Back Face Color0/1 when the corresponding FVF mask is present; the other
winding uses ordinary Color0/1.  Cull rejection happens first.  Selection is
therefore per primitive in the raster front end, after vertex shading, not an
interpolated guess made by an ordinary VS output.

This is the semantic property needed for facing.  Emit ordinary Color0 as
packed white and BFCdiff as packed black.  After the hardware winding select,
the fragment primary color supplies an exact scalar:

```text
p = primary.r
front-selected value = 1
back-selected value  = 0
normal factor         = 1 - 2*p
```

Thus `normal * (1 - 2*p)` produces `-normal` on the selected front winding
and `+normal` on the back winding.  Zero and one are exactly representable in
the normalized packed-color path.  The expression can use one MAD if its
constant-port form is legal, or a MUL plus ADD; it needs no boolean NIR value
and no MISC/vPos alias.

For default ioquake3 lightall, `var_Color` can remain one of the existing six
generic varyings.  The face scalar uses native primary color, which the
semantic UAPI already represents as `DRM_CHROME9_FRAGMENT_FILE_PRIMARY`,
index zero.  It therefore need not create a seventh generic texture-class
coordinate.  The current kernel resource analyzer maps PRIMARY to native
file one, id zero, separately from its texture-coordinate `active0` count.

## What is still missing

The manuals close the producer concept but not the live encoding.  Current
Mesa and DRM code emit ordinary position/color/texture layouts only.  They do
not append FVF attribute 31h, set the BFCdiff-present mask, or validate a
programmable VS result feeding that slot.  The proprietary Linux fixed-
function routine `stmFFVSLightCopy_H5i` at `0x1b07d0` only emits the ordinary
oD0/oD1 copies and metadata bits `0x40/0x80`; it is not evidence for the
back-color output encoding.

The HCM/HC3 attribute numbers must also not be promoted on VX900 solely
because its A23 consumer remains documented.  The exact VX900 producer layout
needs either a Windows object/draw capture containing BFCdiff or a guarded
structured draw.  Until then the fragment compiler must continue to reject
FACE and must not alias it to MISC position.

## Bounded proof ladder

1. Extend only an offline VS/FVF packet builder with one ordinary Color0 and
   one BFCdiff attribute.  Assert attribute 31h is present exactly once and
   HParaType 04 sub-address 10 bit 23 is set; leave BFCspec absent.
2. With culling disabled, draw adjacent opposite-winding triangles and use a
   one-record fragment MOV from PRIMARY0.  One must be white and one black.
3. Toggle only A23 bit 7.  The colors must exchange; neither triangle may
   disappear.  Then enable culling separately to prove A23 bit 7 is not the
   cull selector at bit 8.
4. Repeat with a triangle clipped into multiple primitives and with both
   viewport-Y orientations.  The result must follow final physical winding,
   not original vertex order or screen Y.
5. Compile `normal * (1 - 2*primary.r)` and require the expected opposite
   signed normals while the existing generic `var_Color` remains unchanged.
6. Only after the live oracle passes may Mesa lower the bounded FACE normal-
   sign idiom to PRIMARY0 and advertise the route in a shader variant.

## Sources

- `OGPM_Chrome9 HCM_R100_Part2_3D_Video.pdf`, FVF attribute table on printed
  page 149, FVF17 mask on page 152, and A23 selection algorithm on page 155.
- `OGPM_Chrome9 HC3_R100a_Part2_3D_Video.pdf`, matching FVF attributes/masks
  and A23 algorithm on printed pages 129, 132, and 135.
- `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, A23 bit 7 on printed page
  161, proving the VX900 raster consumer remains present.
- `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
