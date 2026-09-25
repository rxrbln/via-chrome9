# Chrome9 Fast-Z and coarse-Z metadata boundary

Status (2026-09-25): register fields, the coarse-Z buffer geometry, its
clear pattern, the reset/invalidate lifetime, and the Fast-Z tag scheme are
recovered statically from the vendor Linux UMD (see "Static recovery,
2026-09-25" below).  Fast-Z needs **no** auxiliary allocation: its scene
index lives in the depth LSBs.  Remaining unknowns are listed explicitly in
that section.  Both mechanisms stay disabled in the generic driver until the
guarded experiment at the end of this note has passed.

This note prevents the control-panel name "Z compression" from being lowered
to an invented single register bit.  On Chrome9 HD, ordinary depth storage,
Fast-Z clear indexing, and coarse-Z rejection are three different mechanisms.
The already proven linear D16/D24 paths need neither optimization.

## Manual register split

The VX900 Part II manual defines these independent controls:

| word | field | function |
|---:|---:|---|
| A01 | bit 13 | enable coarse-Z testing for triangles |
| A01 | bit 9 | write/update the coarse-Z buffer |
| A27 | bit 0 | enable Fast-Z clear |
| A27 | `[11:8]` | Fast-Z scene-index length, 0 disables indexing |
| A27 | `[15:12]` | current scene index |
| A28 | `[23:0]` | separately stored stencil base, not a coarse-Z base |
| A29 | `[8:0]` | coarse-Z pitch in 32-byte units |
| A29 | `[21:20]` | coarse-Z memory location |
| A29 | bit 15 | one-shot coarse-Z tag-cache reset |
| A2A | `[23:0]` | coarse-Z base in 256-byte units |

A29 also carries conservative merge, inverse-depth, writeback, and forced
retest policy.  Consequently setting A01 bit 13 or 9 without a validated A29,
A2A, and separately allocated coarse-Z object is an out-of-bounds memory
request, not a harmless performance experiment.

## Exact proprietary Fast-Z builder

The executable oracle is `via_chrome9_dri.so` SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
function `STM_FastZClear_INV:0x195480..0x1955dc`.  Its hardware-state base is
the object loaded from context `+0x9430`; byte offsets `+0x9c/+0x9d` are the
low two bytes of A27.

The builder recognizes only these depth classes:

| private depth format | index-length capability source |
|---:|---:|
| D16 `101d` | device state `+0x380c` |
| D24X8/D24S8 `2032/2033` | device state `+0x3810` |
| fixed D32 / D32F `203f/2040` | device state `+0x3814` |

Every other format selects length zero.  If no depth surface is bound, the
builder clears A27 bit 0 and A27 `[15:8]`.  If the selected capability length
is zero, it does the same even when Fast-Z was requested.  Otherwise it:

1. builds the low-bit mask `(1 << length) - 1` and stores it in device state
   `+0x3818`;
2. sets A27 bit 0 from the requested enable;
3. writes `length & 0xf` to A27 `[11:8]`; and
4. writes `((depth_surface[+0x2f0] - 1) & mask) & 0xf` to A27 `[15:12]`.

The fourth step proves that Fast-Z is versioned metadata associated with a
depth surface and scene/clear generation.  A constant A27 scene index can
make stale physical depth appear current after a later clear.  The builder
also dirties the type-01 state transaction after changing A27; it does not
emit an ordinary depth clear or allocate metadata.

The only safe baseline state is therefore `A27=27000000`.  A first Fast-Z
implementation must recover the allocation represented by the depth-surface
metadata, its initialization, wrap behavior for one- through four-bit scene
indices, and the resolve/materialization rule before CPU mapping or texture
sampling.  A27 alone is insufficient.

## Exact coarse-Z reset transaction

`stmResetCoarseZBuffer_invi:0x176800..0x176941` emits a bounded two-write
type-01 transaction:

```text
FE020440 0000043c 00010000 cc000000
29008000
29xxxxxx                         # current A29 image
```

The first A29 write pulses bit 15, which the manual defines as an
auto-clearing coarse-Z tag-cache reset; the second restores the current A29
image.  The function then marks that state dirty in the vendor context.  It
does not clear the coarse-Z allocation and does not program A2A.  The separate
`sfmClearCoarseZBuffer_invi` path is much larger and owns the actual buffer
operation, reinforcing that the tag pulse is not a substitute for metadata
initialization.

The policy builder `STM_CoarseZ_INV:0x1938b0..0x19432f` modifies A01 and A29
through many render-state and primitive-class conditions.  It is not evidence
for an unconditional enable.  In particular the manual excludes lines, and
the executable selectively forces retest or disables rejection for state that
cannot safely use the optimized merge rules.

## Static recovery, 2026-09-25

All addresses are in `via_chrome9_dri.so` SHA-256 `fddb4dea...` (ELF VAs),
disassembled with `objdump -M intel` (`~/tmp/c9dri.dis`).  The type-01
hardware image at `ctx+0x9430` stores register `Axx` at byte offset
`xx*4`, so `+0x04/+0x05` are A01 bits `[7:0]/[15:8]`, `+0xa4..+0xa7` is
A29 and `+0xa8` is A2A.  Note: the point-size A2A/A2B in
[chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md) are
**type 04** registers; the coarse-Z base A2A is **type 01**.  They do not
alias.  Evidence labels: **S** static/exact, **M** manual, **I** inference.

### Driver switches and object model (S)

| field | meaning | default / source |
|---|---|---|
| `cfg+0x23c` | coarse-Z enable | `1` (`osGetRegistryOptions:0xa4989`) |
| `cfg+0x25c` | Fast-Z enable | `0` (`0xa4482`) |
| `cfg+0x260/264/268` | Fast-Z index length D16 / D24 / D32 | `2 / 3 / 4` |
| `dev+0x3824` | coarse-Z active (copy of `cfg+0x23c`) | `stmContextInit_inv:0x1795f7` |
| `dev+0x3820` | Fast-Z active (copy of `cfg+0x25c`) | `0x179520`; an alternate path at `0x17958f` forces it on with lengths 1/2/4 when `ctx+0x278` bit 0 is set (source of that bit not traced) |
| `dev+0x380c/10/14` | per-class index length used by `STM_FastZClear_INV` | as above |
| `dev+0x3818` | current index mask `(1<<len)-1` | written by `STM_FastZClear_INV` |
| `dev+0x3828` | coarse-Z inverse mode (GREATER/GEQUAL) | `STM_ZFunc_INV:0x192d70` |
| `dev+0x3c58` | coarse-Z enabled on the previous draw | `STM_CoarseZ_INV:0x193ef1` |
| `dev+0x3c60` | depth base last bound with coarse-Z | `STM_RenderTarget_INV:0x196532` |
| `zsurf+0x308` | coarse-Z buffer object | `sfmSurface_CreateZBuffer:0xbf05c` |
| `zsurf+0x12b` bit 7 | coarse-Z contents invalid | set by `STM_CoarseZ_INV`, `stmGlobalHardwareAdjust_inv`; cleared after a coarse-Z clear |
| `zsurf+0x12c` bit 2 | surface has been cleared by `sfmClear_inv` | `0xe9a11` |
| `zsurf+0x1c` bit 2 | contents changed outside 3D (blit, CPU lock, rename, relocate) or by a partial clear | `sfmSetSurfaceContentChange:0xdca7f`, `sfmClear_inv` |
| `zsurf+0x2f0` | Fast-Z next scene index | `sfmClear_inv:0xe99d3` |
| `zsurf+0x2f4` | ZW clear data (A14/A15 image) | `stmUpdateZWClearData_inv` |

Whether the VX900 capability bit consulted at depth creation
(`[ctx+0x254]+0x64` bit 1) is set is **not** recovered statically; the chip
capability table is runtime-initialized.

### 1. Coarse-Z buffer allocation (S, except where marked)

The **UMD** allocates it, at depth-surface creation, as a second surface
(`sfmSurface_CreateZBuffer:0xbefd8..0xbf06d`), when the create descriptor
flag `desc+0x28` bit 9 is set, `cfg+0x23c != 0` and the capability bit
above is set.  The depth descriptor is copied, the format is replaced by
`0x2032` (32 bpp), bit 9 is cleared and flag `0x200000` ("coarse-Z
surface") is added.  The result is stored in `zsurf+0x308`, and
`zsurf+0x12c |= 3`.  No KMD involvement beyond the ordinary allocation.

Geometry for a W x H depth surface (any depth format; the depth bpp is not
used):

```text
cz_pitch_bytes = align(((W + 7) >> 3) * 16, 32)   # sfmGetSurfacePitch_inv:0xebcc0
cz_rows        = (H + 7) >> 3                      # sfmClearCoarseZBuffer_invi:0xe3d99, 0xe448a
cz_bytes      >= cz_pitch_bytes * cz_rows          # extent the vendor clear touches
A29[8:0]       = cz_pitch_bytes >> 5               # STM_RenderTarget_INV:0x196472
A2A[23:0]      = cz_gpu_offset >> 8                # 0x19649d
```

So one 16-byte entry covers an 8x8 pixel block.  The manual address formula
`HCZBas*256 + Y*HCZPit*32 + (2*X)*16` addresses 32-byte pairs of entries
(**I**: X indexes 16-pixel columns).  The 9-bit pitch limits W to 8176.
For 1600x1200: pitch 3200 B (A29 pitch 100), 150 rows, 480000 B.

Not recovered: the vendor's exact allocation size and alignment.  Flag
`0x200000` routes sizing through a chip table
(`sfmSurface_CreateInternal:0xbdbdf`: `ceil(W/(4t)) * ceil(H/(4t)) * bpe`
with `t = tbl+0x68`, `bpe = tbl+0x10`) and alignment through
`tbl+0x5c` (`0xbdb10`/`0xbe0aa`); the table is runtime data.  The hardware
requirement is 256 B (A2A units).  Recommended: allocate
`cz_pitch_bytes * align(cz_rows, 2)` rounded to 4 KiB, 4 KiB aligned; the
extra row is a guard margin, not a recovered requirement.

Location `A29[21:20]` (`0x1964bc..0x1964e9`, Windows D3D UMD
`0x101fa7ca..0x101fa7f6` identical): written only when the device flag
`[+0x254]+0x68` bit 14 (the aperture/PCIe path) is set; then `01` (SF) if
the buffer is in system memory (`cfg+0x548`), else `00` (SL).  With the flag
clear the field keeps its initial `00`.  Our driver: `00` for a local (SL)
buffer, `01` for GART/SF, i.e. the same rule as A11 `[1:0]` in
[chrome9-local-fb-placement.md](chrome9-local-fb-placement.md).

### 2. Initialization, clear and reset (S)

**Contents.**  `sfmClearCoarseZBuffer_invi:0xe3cf0` (called from
`sfmClear_inv:0xe9d9d` with the *hardware* depth clear word) fills the whole
buffer (`cz_rows * cz_pitch_bytes`) with a repeating 16-byte entry:

```text
v    = (bpp == 16) ? ((clear & 0xffff) >> 2) : (clear >> 18)   # top 14 depth bits
pack = v | (v << 16)
entry = { 0xffffffff, 0xffffffff, pack, pack }
```

For a clear to 1.0 (`pack == 0x3fff3fff`) the entry is all ones and the
function uses a plain 2D fill (`sfmSurface_Fill`, rect width
`cz_pitch_bytes/4` dwords, height `cz_rows`).  Otherwise it streams the
pattern through a command-buffer upload, splitting at 4 KiB per chunk and
inserting `stmAddCRWaitChipIdle_invi`.  A CPU-write fallback
(`0xe4140..0xe41ad`, taken when `cfg+0x244` is set) writes the same pattern
after `hwmWaitChipIdle`.  The meaning of the two all-ones dwords is not
decoded (**I**: per-sub-block masks or maxima); reproducing the pattern
bit-exactly is sufficient.

**When.**  Only in `sfmClear_inv` for a depth clear that covers the whole
surface (rect `(0,0)-(W,H)`, `0xe98c6..0xe9b98`) and only if the surface has
a coarse-Z object.  After filling, `zsurf+0x12b` bit 7 (invalid) is cleared
and `stmResetCoarseZBuffer_invi` pulses A29 bit 15.  A partial depth clear
does not touch the coarse-Z buffer; it sets `zsurf+0x1c` bit 2 instead, which
disables coarse-Z (below) until the next full clear.

**Tag-cache reset (A29 bit 15)** is emitted:

1. after every coarse-Z buffer clear (`0xe9dc4`);
2. on depth bind when the depth base differs from `dev+0x3c60`
   (`STM_RenderTarget_INV:0x196524..0x19653e`);
3. inside the per-draw policy when coarse-Z goes from disabled on the
   previous draw (`dev+0x3c58 == 0`) to enabled (`STM_CoarseZ_INV`, OR of
   `0x80` into image `+0xa5`).

**Invalidation / forced disable** (`stmGlobalHardwareAdjust_inv:0x17d406`,
run at state validation with a bound depth surface):

* if the surface was never cleared (`+0x12c` bit 2 clear), or its contents
  changed outside the 3D pipe (`+0x1c` bit 2: blit, `sfmLock` CPU map,
  rename, relocate, partial clear), then: A01 bits 13 and 9 are cleared,
  `zsurf+0x12b` bit 7 is set (invalid), and Fast-Z is turned off
  (A27 bit 0, `[11:8]`, `[15:12]` cleared, `zsurf+0x2f0 = 0`).
* `STM_CoarseZ_INV` additionally marks the buffer invalid when a draw with
  depth test/write uses lines (internal primitive types 2, 3, 7), wireframe
  fill (`ctx+0x1dfc == 2`), `ctx+0x1ce0` (not identified), a pixel shader with
  `shader+0x78` set (**I**: depth output or discard), or when the depth
  compare direction flips relative to A29 bit 17.

Only a full-surface clear brings an invalid buffer back.

### 3. Per-draw register words (S for the fields, partial for the policy)

With depth bound, a coarse-Z object present and `dev+0x3824` set:

| word | value | when |
|---|---|---|
| A29 `[8:0]`, A2A | pitch/32, base/256 | depth bind |
| A29 `[21:20]` | location | depth bind |
| A01 bit 13 | 1 | coarse-Z enabled for this draw |
| A01 bit 9 | = A01 bit 2 (Z write) | same |
| A29 bit 16 | 1 (write-back when selected cell changes) | always when enabled |
| A29 bit 17 | 1 for GREATER/GEQUAL (`dev+0x3828`) | always when enabled |
| A29 `[13:12]` | `10` (conservative, update MIN only) when `ctx+0x1ca4` and `ctx+0x1dac != 8` (**I**: alpha test); otherwise `01` or `00` by chip family | per draw |
| A29 bits 22/23, 10/11 | forced "retest" for CW/CCW pass/reject, set from the stencil state (`ctx+0x1e18..0x1e40`) | stencil enabled |
| A29 bit 9 | force retest when `ctx+0x1e10 == 6` | per draw |
| A29 bit 15 | 1 (one-shot) | see reset list above |

Z test modes ALWAYS (7) and 5, or Z write without Z test, take a separate
branch (`0x194030`) that looks at the shader/state object; NEVER (0) and
EQUAL (2) skip the valid-buffer policy.  The full decision tree
(`0x1938b0..0x19432f`) is not completely decoded.  For our implementation a
whitelist is sufficient (see plan).  Both A01 and A29 are re-emitted through
the type-01 dirty mechanism, i.e. inside the ordinary FE020440 state
transaction before the draw; no separate flush or wait is used between
draws.  No 3D-cache flush other than the A29 bit-15 pulse was found on depth
surface change.

### 4. Fast-Z clear: no metadata allocation (S)

`sfmClear_inv:0xe9979..0xe9fde` shows that Fast-Z stores the scene index in
the **least significant bits of each depth word**:

1. The untagged clear value is stored as the ZW clear data
   (`stmUpdateZWClearData_inv` → `zsurf+0x2f4`, D24 low byte zeroed) and
   emitted as A14 `[23:0]` / A15 `[7:0]` by `stmGlobalHardwareAdjust_inv`.
2. On a full-surface depth clear with Fast-Z active: `idx = zsurf+0x2f0`.
   The clear word gets `idx` inserted into its low bits (`<< 8` for D24X8 /
   D24S8, `<< 0` for D16/D32).  The physical clear (blit) is performed
   **only when `idx == 0`**; otherwise memory is not written.
3. `zsurf+0x2f0 = (idx + 1) & mask`.  `STM_FastZClear_INV` then programs
   A27 `[15:12] = (zsurf+0x2f0 - 1) & mask = idx`.

(**I**, consistent with the manual A27 text and A14/A15 "ZW clear data"):
the hardware treats a stored depth whose tag bits differ from A27 `[15:12]`
as the clear value and writes new depths tagged with the current index.
Wrap handling is therefore a real clear every `2^len` clears, which also
removes any stale word that happens to carry index 0.  A partial clear,
blit, CPU map or relocation resets the index to 0 and disables Fast-Z until
the next full clear (item 2 above).

Consequences:

* No allocation, but `len` depth LSBs are lost (D24 default 3 bits).
* Stale-tag words hold old depths, so the buffer is **not** valid for CPU
  mapping, depth-texture sampling, or blits until a materializing full
  clear.  No resolve function exists in the vendor binary; the vendor
  default is Fast-Z **off** (`cfg+0x25c = 0`), which is the likely reason.
* Fast-Z saves only the clear write (7.7 MB for 1600x1200 D24) in
  `2^len - 1` of `2^len` frames; the depth reads during drawing remain.
  Coarse-Z is the mechanism that reduces per-draw depth traffic.

### Remaining unknowns

* VX900 value of the coarse-Z capability bit and chip-table allocation
  size/alignment (bounded by the recommended allocation above).
* Meaning of the two all-ones dwords in the coarse-Z entry and of the
  exact 14-bit value encoding for D32F.
* The complete `STM_CoarseZ_INV` decision tree (stencil and alpha branches).
* Fast-Z tag comparison semantics are inferred, not measured.

## Proposed guarded implementation

Current state: the kernel command builder
(`drivers/gpu/drm/chrome9/chrome9_engine.c`) never emits type-01 A27, A29
or A2A.  The `0x29000000` and `0x2a3f8000` words it does emit are the
**type-04** FVF/link and point-size registers (manual p.~150 onward), not
the coarse-Z registers.  The first coarse-Z patch must emit explicit type-01
`A27=27000000` / `A29` / `A2A` words rather than rely on power-on defaults.

Coarse-Z first (it reduces per-draw depth traffic); Fast-Z second and
optional.

**Kernel (owns addresses, because it validates every surface extent):**

1. Capability bit `DRM_CHROME9_CAP_COARSE_Z`, off unless the module
   parameter `coarse_z=1` is given.
2. The coarse-Z buffer is a kernel-private BO attached to the depth BO on
   first use with coarse-Z: SL placement, 4 KiB aligned, size
   `align(((W+7)>>3)*16, 32) * align((H+7)>>3, 2)` rounded to 4 KiB.  W/H
   are the depth surface's draw dimensions from the submit; reject
   `W > 8176`.  Freed with the depth BO.
3. Per-depth-BO state: `cz_valid`, `cz_dir` (LESS vs GREATER), last bound
   base (for the tag reset), plus a flag set on every CPU mmap/fault,
   blit, copy, or partial clear of the depth BO, which clears `cz_valid`.
4. Full-surface depth clear with a coarse-Z buffer: after the normal depth
   clear, fill the coarse-Z buffer with the recovered entry pattern (2D
   solid fill for 1.0, otherwise a 16-byte pattern fill), then emit
   `A29 | 0x8000`, `A29`; set `cz_valid`.
5. Draw: enable only when all hold: `cz_valid`, triangles, fill mode solid,
   depth test on with LESS/LEQUAL (or GREATER/GEQUAL, A29 bit 17), no
   stencil test, no alpha test, fragment program without depth output or
   discard, and the direction equals `cz_dir`.  Then
   `A01 |= 0x2000 | (zwrite ? 0x200 : 0)`, `A29 = 0x29010000 | dir<<17 |
   pitch>>5` (`[13:12]=01` conservative merge for the first bring-up),
   `A2A = 0x2a000000 | base>>8`, with the bit-15 pulse on the first enabled
   draw after a disabled one or after a depth-base change.  Otherwise emit
   A01 bits 13/9 clear and, if the draw writes depth, clear `cz_valid`.

**Mesa:** no new state beyond passing a "full clear" bit that the kernel
can verify from the clear rectangle, and advertising nothing to GL.  All
policy lives in the kernel, so userspace cannot enable a stale buffer.

**Fast-Z (after coarse-Z is proven):** per-depth-BO `fz_index`, index length
from the recovered defaults (D24: 3).  Only for depth BOs that are never
sampled or mapped (the kernel disables it and does a real clear before the
first map/texture use), because no resolve exists.  Emit A14/A15 untagged
clear data and A27 `= 0x27000001 | len<<8 | idx<<12`.

## Smallest hardware experiment

One run, < 7 s, `coarse_z=1`, 64x64 D24 target, under the
existing target-safety rules (GPU lock, hash-gated module handoff):

1. Clear depth to 1.0 and color to black (coarse-Z fill + reset).
2. Draw a full-screen quad at z=0.25 (LESS, depth write on), red.
3. Draw a full-screen quad at z=0.75, green.  Expected: all red; the PPC
   counter `0x0A` (coarse-Z reads) must be non-zero and `0x0C` (Z reads)
   lower than in the same run with `coarse_z=0`.
4. Draw a quad at z=0.10, blue, over the left half.  Expected: left blue,
   right red (no false rejection).
5. Read back color and depth (depth read invalidates coarse-Z), repeat
   step 3 and expect coarse-Z to be off for that draw.

Pass criteria: images equal the `coarse_z=0` reference byte for byte, no
engine hang (FE/3D idle within the existing timeout), and a GART/SL guard
page after the coarse-Z BO unchanged.  Any mismatch or timeout disables
the capability again.  Only then measure glmark2 refract.

## Implementation and proof order

1. Keep A01 bits 13/9 clear and A27 zero for all ordinary depth, shadow, and
   depth-texture work.
2. Done statically (2026-09-25): coarse-Z geometry, A29/A2A producer,
   clear pattern and reset/invalidate rules.
3. Done statically (2026-09-25): Fast-Z has no separate metadata; the
   scene index is stored in the depth LSBs.
4. Prove each optimization against a linear non-optimized reference with
   alternating full and partial clears, depth-write-disabled draws, CPU
   readback, and depth-as-texture transitions.
5. Only then treat the control-panel "Z compression" option as a capability;
   do not expose it as a public render feature or correctness prerequisite.

This is a performance ladder.  It must not block the full GL feature set:
ordinary uncompressed D16/D24 storage remains the conformance path.
