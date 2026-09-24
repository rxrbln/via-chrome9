# Chrome9 draw twoside face-sign fallback

## Scope and result

This note records a software-pipeline fallback for the bounded fragment idiom
used by ioquake3's default `lightall` shader:

```glsl
vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
```

The exact recovered source is `/tmp/ioq_default_lightall.frag`, SHA-256
`7913798a3f3299f5df065c2523cb15f4ad01511241c516df70e4c7b2bc9cc2d6`.
Its required sign is `-1` on front faces and `+1` on back faces.

Gallium draw already has a face-aware stage that can materialize this sign in
the existing Chrome9 PRIMARY0 input.  This avoids inventing a native vFace or
back-face-color producer.  It is an exact fallback only while PRIMARY0 and the
vertex COLOR0/BCOLOR0 pair are otherwise unused.  A conflict must reject the
fallback; the present UAPI does not expose a second color input.

## Existing draw-pipeline evidence

Mesa's
`src/gallium/auxiliary/draw/draw_pipe_twoside.c` implements the relevant
operation directly:

1. `twoside_first_tri()` finds vertex outputs `TGSI_SEMANTIC_COLOR[0..1]`
   and `TGSI_SEMANTIC_BCOLOR[0..1]`.
2. It derives `sign = -1` for `front_ccw` and `+1` for clockwise-front state.
3. `twoside_tri()` classifies a back face when `header->det * sign < 0`.
4. For a back face, `copy_bfc()` duplicates all three post-transform vertices
   and copies BCOLOR0 to COLOR0.  Front faces retain COLOR0.

`src/gallium/auxiliary/draw/draw_pipe_validate.c:validate_pipeline()` builds
the pipeline end-to-start.  With `light_twoside`, its execution order is clip,
cull/determinant, twoside, then rasterize (with optional stages between
twoside and rasterize as selected by state).  Face selection therefore uses
the determinant of the final clipped triangles and the current `front_ccw`
state.  `draw_pipe_clip.c:find_interp()` also treats COLOR and BCOLOR as the
corresponding color pair while generating clipped vertices.

NIR/TGSI plumbing already preserves the semantics:
`tgsi_from_mesa.c:tgsi_get_gl_varying_semantic()` maps `VARYING_SLOT_COL0` to
`TGSI_SEMANTIC_COLOR,0` and `VARYING_SLOT_BFC0` to
`TGSI_SEMANTIC_BCOLOR,0`; the draw vertex interpreter accepts both outputs.

Chrome9 cannot accidentally bypass this path.  Its
`chrome9_draw.c:chrome9_vbuf_need_pipeline()` currently always returns true,
because the kernel ABI accepts triangles and draw performs clipping, culling,
flat-shade fixup and decomposition.

## Exact marker algebra

For a qualifying linked shader variant, clone the draw vertex shader and add
constant outputs on every exit:

```
COLOR0  = vec4(1.0)
BCOLOR0 = vec4(0.0)
```

Enable `light_twoside` only in the rasterizer-state copy used by that draw
variant.  Do not mutate the application's rasterizer object or its native
front/back policy.  After clipping and the twoside stage, define
`p = COLOR0.r`; then `p=1` on a front face and `p=0` on a back face.  Rewrite
only the recognized lightall form to:

```
surfNormal = var_Normal * (1.0 - 2.0 * p)
```

This yields `-var_Normal` for a front face and `+var_Normal` for a back face,
exactly matching the source.  Writing all four marker channels to identical
values makes the result independent of the packed BGRA byte chosen as `.r`.
The constants also survive vertex-color clamping unchanged.

This route does not consume another texture-class varying.  Chrome9's generic
vertex layout emits PRIMARY0 as one `EMIT_4UB_BGRA` attribute when
`semantic_input_mask == 1`, followed independently by every generic varying.
The generic draw queue zero-initializes each
`drm_chrome9_fragment_vertex`, copies the packed color to `vertex.color`, and
copies generic vectors to `vertex.varying[]`.  Thus default lightall retains
its six generic varyings while the marker travels through the already distinct
PRIMARY file.

## Required fail-closed boundary

The fallback is valid only when all of these link-time conditions hold:

- the fragment FACE use matches the proven normal-sign idiom (or an
  independently proven algebraically equivalent form);
- fragment PRIMARY0 is unused before the rewrite;
- vertex COLOR0 and BCOLOR0 are both unused before injection;
- the linked vertex shader can be cloned with COLOR0 and BCOLOR0 stores on all
  exits, including all return paths;
- all original generic output locations and masks remain unchanged; and
- the rewritten fragment program records PRIMARY0 while preserving the six
  existing generic inputs and their interpolation qualifiers.

If either shader already uses COLOR0/PRIMARY0 for application data, injecting
the marker would overwrite observable semantics.  If BCOLOR0 is already used,
the fallback would overwrite the application's back color.  Both cases must
fail closed and leave the shader unsupported until a native BFC route or a
separate proven input is available.  The same applies to any unrecognized or
arbitrary `gl_FrontFacing` use: this note proves the bounded sign selection,
not a general boolean FACE implementation.

PRIMARY0 is a single current interface, not a spare second color.  The
semantic compiler records it in `semantic_input_mask`; the vbuf path requires
the corresponding COLOR0 output and serializes it separately from generic
varyings.  The kernel-side fragment vertex ABI likewise has one `color` field.
Consequently a COLOR0 conflict cannot be resolved by silently aliasing COLOR1
or by shifting a generic slot.

## Guarded offline regression

A host-only regression should construct and link:

1. a vertex shader writing position, six distinguishable generic vectors,
   COLOR0=`vec4(1)`, and BCOLOR0=`vec4(0)`;
2. a fragment shader containing the exact FACE normal-sign idiom and the
   generic inputs; and
3. a conflicting pair that already consumes vertex COLOR0/BCOLOR0 or fragment
   PRIMARY0.

Run the first pair through draw's no-LLVM pipeline and the Chrome9 vbuf
packing path.  Use adjacent triangles with opposite winding and assert that
the rasterizer receives COLOR0 `1,1,1,1` for the front triangle and
`0,0,0,0` for the back triangle, while all six generic vectors are byte-for-
byte unchanged.  Repeat with one triangle crossing a clip plane to prove the
generated triangles receive the post-clip determinant classification.  Toggle
`front_ccw` and require the marker assignment to swap.  The conflicting pair
must be rejected before native fragment compilation, with no mutation of the
original linked shaders or application rasterizer state.

The live ladder is deliberately narrow:

1. untextured one-MOV PRIMARY output on two adjacent opposite-winding
   triangles;
2. the same oracle with a clipped triangle and both `front_ccw` settings;
3. the exact lightall normal-sign rewrite with its six generic inputs; and
4. default lightall only after the existing instruction/resource tests pass.

This establishes the software face producer independently of lighting,
textures, and the unresolved native BFC encoding.
