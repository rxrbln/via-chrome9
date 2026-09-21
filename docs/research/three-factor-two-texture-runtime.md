# Chrome9 full `tex0 * tex1 * primary` runtime closure

Status: packet- and patch-ready design, 2026-09-21. The Windows compiler
sequence has been executed under Wine and agrees with the independently
scheduled Xorg/Linux physical object. The program itself has not yet been run
on VX900 silicon. This note specifies the full-RGBA path; the different
`RGB=tex0*tex1*primary, A=primary.a` key remains a separate four-ALU object.

## Closed semantic operation

The one operation selected by this design is:

```text
sample0 = ordinary normalized 2D TEXLD(unit 0, TEX0.xy)
sample1 = ordinary normalized 2D TEXLD(unit 1, TEX1.xy)
oC0     = sample0.rgba * sample1.rgba * primary.rgba
```

Both projective Q lanes must be bitwise `0x3f800000` before Mesa discards
them. There is no LOD, bias, offset, shadow compare, array layer, native
projective divide, fragment constant, saturation, or third sampler in this
key. D1 is the packed primary color and is normalized by FVF A02
`020041d1`; all four D1 bytes are semantic input.

Format translation makes this one vector program implement all four source
alpha cases:

| logical view | sampler image A30 | sampled alpha |
|---|---:|---:|
| XRGB/RGB | `30980000` | one |
| BGRA/RGBA | `30990000` | stored alpha |

If `alpha_mask` bit 0 denotes stored alpha in texture 0 and bit 1 denotes it
in texture 1, the resulting alpha is:

| alpha mask | output alpha |
|---:|---|
| 0 | `primary.a` |
| 1 | `tex0.a * primary.a` |
| 2 | `tex1.a * primary.a` |
| 3 | `tex0.a * tex1.a * primary.a` |

`alpha_mask` is a Mesa classifier fact, not a new UAPI field. The two
existing `textures[i].format` members are authoritative. Destination A52 is
independent: 98 selects XRGB storage semantics and 99 stores alpha. Never
derive either source format from the destination.

Legacy Mesa independently establishes the fixed-function semantics in
`via_texcombine.c`: RGB `GL_MODULATE` multiplies its two selected arguments;
alpha independently selects `GL_TEXTURE`, `GL_PRIMARY_COLOR`, or
`GL_PREVIOUS` at lines 293--323 and multiplies both arguments at lines
338--350. At unit zero PREVIOUS is diffuse; at a later unit it is the current
texture result. Thus a two-stage MODULATE is exactly the three-leaf product
above, including alpha.

## Reproducible Windows compiler differential

The checked-in probe source was rebuilt as 32-bit PE with:

```text
clang --target=i686-w64-windows-gnu -fuse-ld=lld -nostdlib \
  -Wall -Wextra -Werror -Wl,--no-insert-timestamp \
  -Wl,-entry,mainCRTStartup -Wl,-subsystem,console \
  -L/usr/lib64/wine/i386-windows tools/wine_shader_probe.c \
  -lkernel32 -o /tmp/wine_shader_probe-three-factor.exe
```

The source SHA-256 is
`8e8bd41d9a296e79b5e75b17f1b46c641823ffadc98e258122c88621d86dea57`;
the reproducible executable SHA-256 is
`a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c`.
The exact `ps_2_0` token stream was:

```text
ffff0200
04000042 000f0000 30e40000 20e40800   # texld r0,t0,s0
04000042 000f0001 30e40001 20e40801   # texld r1,t1,s1
04000005 000f0002 00e40000 00e40001   # mul r2,r0,r1
04000005 000f0003 00e40002 10e40000   # mul r3,r2,v0
03000001 000f0800 00e40003            # mov oC0,r3
0000ffff
```

The input SHA-256 was
`65f6c152a60607044e07d3fb3276eb67a63026d1e3b7cfc265ff0c2f6749bbb8`.
The UMD consumed those exact bytes. Raw and finalized output were identical:

```text
TAU 201001b0 211111b1
ALU 0140b6d0 00001000 00f00000 000000a0
    0140b6d0 00040053 00f00200 000000e0
    01c00530 001c0053 00f00300 000d0820
```

The ALU SHA-256 is
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`;
the TAU SHA-256 is
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
This proves two texture operations, three ALUs, their order and unchanged
T2A switch/count. These are virtual allocation words and must not be copied
into the kernel packet.

The OpenGL ICD byte-matches the same tokenizer, dispatcher, and finalizer at
RVAs `0x244530`, `0x245280`, and `0x256550`. The uploadable physical object is
the independently recovered Xorg schedule:

```text
A90..A9a 90018000 91000003 92020003 93000002
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       301001b0 311211b1 cccccccc dddddddd
ALU       0140b6d0 30202053 00f20005 0000007f
          0140b6d0 30040053 00f20105 0000007f
          01c00000 301c0000 00f20105 000d083f
          cccccccc dddddddd
```

The second ALU's identity swizzle/control is exactly `30040053`. The stale
`00040053` value belongs to the compiler's virtual object, not this physical
allocation. Treat controls, TAUs, all ALUs, and terminators as one indivisible
program.

## Minimal versioned ABI

Reserve the already-ledgered capability and a command-local program flag:

```c
#define DRM_CHROME9_CAP_TWO_TEXTURE_PRIMARY_MODULATE (1ULL << 25)
#define DRM_CHROME9_TWO_TEXTURE_PRIMARY_MODULATE     (1U << 0)
```

Capability 25 means all three existing two-texture ioctls accept the bit:

* command 0a, 240 bytes, no depth;
* command 0d, 256 bytes, D16; and
* command 13, 296 bytes, D16 plus blend/alpha/destination state.

No structure changes. In commands 0d/13 bit 0 composes with depth function
bits `[10:8]` and read-only bit 11. In command 13 it remains distinct from
the nested `blend.flags`. Advertise cap 25 only after the shared builder and
all three validators implement the same meaning. An old kernel rejects bit
0, so capability negotiation is fail-closed.

Patch points in the current kernel are deliberately small:

1. Add the two defines to every synchronized UAPI copy and bit 25 to the
   advertised capability mask.
2. Copy `args->flags` into the internal request in command 0a; commands 0d/13
   already do this.
3. Command 0a accepts only bit 0. Commands 0d/13 pass bit 0 as the `allowed`
   argument to `chrome9_validate_depth_flags()`. Reserved fields stay zero.
4. In `chrome9_build_two_textured_triangle()`, select a complete two- or
   three-factor program object. Do not patch a live array in place.
5. For three-factor only, set common A01 to `01ce1540` without depth,
   `01ce154c` with D16 writes, or `01ce1548` with read-only D16. Retain FVF
   A02 `020041d1`.
6. Select A92 `92020003`, emit the added primary MUL, and raise the exact body
   length by four dwords. Every legacy flag-clear word and length remains
   byte-identical.

The existing size/pitch/format/filter/wrap/scissor/finite-coordinate,
nondegenerate triangle, distinct-object, guarded mapping, fence, retirement,
and quarantine checks remain mandatory. The new flag must not impose
`D1.a==ff` or BGRX-only sources: doing so would erase valid full-RGBA
semantics. The kernel cannot validate discarded Q; Mesa owns the Q=1 proof.

## Exact packet indices

Let `B=0` without the command-13 destination extension and `B=4` with it.
Let `D=0` without D16 and `D=4` with it. The full three-factor body length is:

```text
266 + B + D dwords
```

The relevant indices are:

| state | body index | required value |
|---|---:|---:|
| common A01 | `64+B` | `01ce1540`, `01ce154c`, or `01ce1548` |
| FVF A02 | `86+B+D` | `020041d1` |
| PS A92 | `190+B+D` | `92020003` |
| TAU payload | `208+B+D .. 211+B+D` | physical two-TAU row |
| texture product | `216+B+D .. 219+B+D` | first MUL |
| primary product | `220+B+D .. 223+B+D` | second MUL |
| output MOV | `224+B+D .. 227+B+D` | full-vector MOV |
| program terminators | `228+B+D .. 229+B+D` | CC/DD |
| immediate H2 | `232+B+D .. 235+B+D` | unchanged header |
| vertex payload | starts `236+B+D` | 30 dwords |

Therefore command 13 is exactly 274 dwords:

```text
A01 body[68] = 01ce154c or 01ce1548
A02 body[94] = 020041d1
A92 body[198] = 92020003
TAU            body[216..219]
tex0*tex1      body[224..227]
* primary      body[228..231]
output MOV     body[232..235]
CC/DD          body[236..237]
padding        body[238..239]
immediate H2   body[240..243]
vertices       body[244..273]
```

All earlier command-13 state remains fixed: destination A52 is body 6,
alpha-test A33 body 19, texture image A30 words body 29/49, common
A53/A00/A01 body 66/67/68, D16 A10--A20 body 71--75, and clip A80--A83 body
152--155. A33 adds no length. The target/image/depth/blend object roles and
four-BO fence lifetime do not change.

Builder assertions should cover the total length, A01, A02, A92/A93, both
TAUs, every dword of all three ALUs, both terminators, immediate header, and
first/last vertex positions. A control-count-only edit is insufficient.

## Mesa classifier and draw-time validation

Use a distinct full-vector key, for example:

```text
CHROME9_FS_TWO_TEXTURE_PRIMARY_RGBA
```

Flatten RGB multiply trees only when reassociation is legal and require the
multiset `{tex0.rgb, tex1.rgb, COL0.rgb}`. Normalize alpha separately and
require `COL0.a` times exactly the stored-alpha texture leaves indicated by
the two translated view formats; format-98 leaves are implicit one. Accept
identity MOVs and only an independently justified final saturation. Reject
missing/repeated leaves, constants, nonidentity swizzles, partial output,
discard, derivatives, coordinate arithmetic, extra ALU, extra samples, and
the alpha-bypass expression.

At draw time require exactly two ordinary non-array, non-shadow level-zero
2D views bound to matching units 0/1; existing canonical 32-bit layouts;
finite normalized S/T; and matching T0/T1 Q raw bits equal to `3f800000` for
all three post-TNL vertices. D1 may be any packed UNORM8 RGBA. Require cap 25
plus the selected base command capability and every independently required
depth/blend/alpha/scissor/format capability.

The two coordinate expressions need not originate in two distinct NIR input
variables. A linked GL2 shader may pack unit-0 XY and unit-1 XY into the XY/ZW
halves of one vec4 varying. Mesa may split that form into the same two native
S/T pairs only after proving the lane mapping and finding no cross-lane
arithmetic. Its alpha expression may omit unit-1 alpha; the full-vector
native object is still exact when the translated unit-1 view is XRGB/format
98, because that hardware sample supplies one. A BGRA unit-1 view must fail
that key unless its alpha leaf is present.

The existing all-white-primary optimization may continue to select the live
two-factor program because multiplication by `ffffffff` is exact. It must be
an optimization, not the only path. A nonwhite primary selects bit 0 and the
three-factor object.

## Bounded silicon sequence

Run each stage with full guard verification, exact source preservation,
retirement cookie, CR PAUSE/work-mask checks, and immediate quarantine on a
timeout:

1. Offline-build all six bodies: commands 0a/0d/13 with bit clear/set. Assert
   legacy bodies 262/266/270 and new bodies 266/270/274 dwords.
2. Command 0a, BGRX/BGRX, D1 `ff808080`: use the proven red/black/blue/yellow
   two-factor quadrants. Expected output is approximately half intensity.
3. Submit `three-factor -> legacy two-factor -> three-factor` without an
   intervening engine reset. The middle result must return to full intensity.
4. Command 0d with guarded D16: LEQUAL at equal depth passes, LESS at equal
   depth fails, then LESS at nearer depth passes. Check exact D16 writes.
5. Command 13 on BGRA destination: texture0 `80ff8040`, texture1 `40c0ffff`,
   D1 `80ff40c0`. Full RGBA must store approximately `10c02030`. A program
   accidentally bypassing sampled alpha stores `80c02030`.
6. Exercise all alpha masks with RGB `(0.8,.5,.25)`, `(.5,.25,.8)`, and
   primary `(.5,.8,.25,.5)`. Stored alphas `.25/.6` produce common RGB
   `(.2,.1,.05)` and alpha `.5`, `.125`, `.3`, `.075` for masks 0--3.
7. Repeat stage 6 under alpha test and source-alpha blend, then transition
   BGRA/BGRA -> XRGB/XRGB -> BGRA/BGRA to expose stale A30 state.
8. Gallium positive draws must increment submitted by two triangles and
   rejected by zero. Q!=1, format/key alpha mismatch, bypass alpha, and an
   unadvertised cap must each submit no ioctl.

Passing only the opaque white-primary case does not prove this feature; that
case is deliberately byte-equivalent to the old two-factor path. The nonwhite
RGB discriminator and stored-alpha command-13 vectors are the minimum proof
that the added ALU and all four lanes execute.
