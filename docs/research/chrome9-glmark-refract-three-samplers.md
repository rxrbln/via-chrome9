# Refract: three fragment samplers are live; rendering remains open

The installed glmark2 2023.01 `light-refract.frag` declares three `sampler2D`
uniforms. `scene-refract.cpp` binds the depth, color, and image textures to
GL texture units 0, 1, and 2. Mesa formerly advertised two Chrome9 fragment
samplers, so even a link-only test of that shader failed at the GL limit.

Before raising the production cap, `tools/re/chrome9_three_texture_guard_smoke.c`
submitted one bounded 64x64 structured fragment draw to the VX900. Its three
distinct 4x4 BGRA sources were sampled through descriptors 0, 1, and 2, then
combined as `s0 + 0.5*s1 + 0.25*s2`. The result was the expected BGRA word
`0x00404030` (RGB 40/40/30); destination exterior and guard pages, and all
three complete source allocations, remained intact. The command returned
success with no new kernel fault. This validates three nearest/clamp, L0,
distinct-BO descriptors together, not arbitrary mixed filtering or mipmaps.

Chrome9 Mesa now advertises three fragment sampler/view/TEX slots only when
the kernel reports the structured fragment-program capability. The legacy
immediate-triangle path retains its two-texture ceiling. The targeted Gallium
capability smoke reported `3/3/3` on the target.

`tools/re/chrome9_three_sampler_gl_link.c` creates a GL context and compiles
and links shaders without drawing. With the previous two-slot Mesa library,
the test reported `GL_MAX_TEXTURE_IMAGE_UNITS=2 < 3`. With the new library it
linked both a minimal three-sampler program and the installed
`light-refract.vert`/`.frag` sources (the latter with the scene's constant
definitions supplied at the start of the fragment source). No GPU draw was
submitted by this test. This proves the GL capability/link boundary, not
refract's visual correctness.

The broader Gallium smoke initially reported a D24 render-to-texture mismatch,
but the pixels, source preservation, and guards were correct; only its expected
batch count was stale after two triangles began sharing one program submission.
After correcting that assertion, the focused `--d24-only` test passed both
D24 render-to-texture and D24-to-Z16 sampling. The non-D24 full smoke still
fails a later D16 triangle check, separately under investigation. Refract
still needs a conforming BGRA mipmapped render destination,
producer-to-CPU-mipmap synchronization, mixed depth/color/mip descriptor
validation, and its fragment shader ISA path before a scene validation claim.
