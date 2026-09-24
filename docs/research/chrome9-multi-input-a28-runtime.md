# Chrome9 three-input A28 runtime differential

Status: read-only Windows D3D draw-writer disassembly, Linux 5.75 compiler
replay, and current kernel-source audit, 2026-09-24. No GPU probe or product
change was made for this note. This is a causal candidate, not a proven fix.

## Observed boundary

On VX900, the guarded one-record `MAD oC0,T0,T1,T2` draw timed out at its
post-vertex cookie wait even after the A00 scheduled-resource correction and
ordered FE A01 fields. The 2026-09-24 log is
`/root/tmp/chrome9-live/bringup-20260924-0725/t2-float4-timeout-dmesg.txt`:
`seq=2`, `kind=22`, `current=0x1080`, `pause=0x1070`,
`cr=80000000`, `engine=00000711`. The ring window begins in the vertex
payload and shows the intended T0/T1/T2 float4 values; it does **not** show
the earlier FE/FVF state words. A known-good CMP/TEMP draw passed first.

The Linux 5.75 production compiler, replayed with configuration-two
postprocessing, emits the one-MAD ALU
`02c20530 302c1053 00f2c005 00000820`, controls
`90058007 91000000 92000001`, logical T-use mask `7`, and active-resource
tuple `(1,0,1,0)`. Public instance `+0x34` is one. These are the values
already reproduced by the current semantic compiler. The run-time conflict
below concerns H6 state, not these PS words.

## Windows D3D writer: A28/A29 follow active resources

The exact binary is
`binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll`, SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`.
Its hardware-image initializer at `0x101549c2` starts the self-addressed H6
word array at image offset `+0x20e0`; therefore image `+0x2180` and `+0x2184`
are H6 subaddresses `28` and `29`, not FE words. The packet copy at
`0x10154311..0x10154322` confirms that these two image dwords are emitted
consecutively.

In the programmable draw update, `0x1015b259..0x1015b26b` copies compiled
object `+0x24` (public instance `+0x34`) into FE A00 bits 7:4. Then:

1. `0x1015b37b..0x1015b3ae` clears the low 16 bits of H6 A28 and A29;
2. `0x1015b3b4..0x1015b42e` loops exactly `A00[7:4]` times over effective
   coordinate widths at context `+0x2d80[]`, mapping widths one through four
   to masks `8,c,e,f` in descending four-bit slots; and
3. `0x1015b50c..0x1015b529` ORs the assembled upper/lower halves into H6
   A28/A29. The separate FE linkage writer at `0x10157a90..0x10157ac5`
   fills `+0x2d80[0]` with the effective width used for A01 field zero.

The width table is not truncated to the active-resource count.  The linkage
builder at `0x101574a0` reads compiled-object `+0x30` at
`0x101574b4..0x101574ba`; this is the logical T-use mask.  It compacts every
set T bit and its producer width from context `+0x2d40[]` at
`0x1015751c..0x101575bb`, then writes the per-ordinal effective widths into
`+0x2d80..+0x2d9c` through the branches at
`0x101576ae..0x10157ac5`.  Consequently the one-MAD object with mask `7`
constructs three width-four entries for T0/T1/T2.  The later H6 writer still
iterates only compiled-object `+0x24` / public-instance `+0x34`, which is one.
The `f000` result is therefore a deliberate split between complete linkage
metadata and active-resource-limited H6 masking, not an absent T1/T2 width in
the context table.

For one active resource of width four, this writer sets only A28's T0 nibble
`[15:12]` to `f`; the T1 and T2 nibbles `[11:8]` and `[7:4]` remain zero.
The upper 16 bits of A28 depend on other runtime state and are not claimed
here. By contrast, `chrome9_fragment_raster_state()` in the clean-room kernel
uses every `program->varying_width[]` to build *both* H6 A25/A26 and A28/A29.
For three float4 inputs, it emits A28=`28fffff0`, with low 16 bits `fff0`.
Thus A28 differs from the Windows programmable writer by T1/T2 mask bits
`0x0ff0` in this precise one-MAD/resource-one shape. A25 may legitimately
retain all three declared FVF attributes: the manual describes A25 as the
FVF attribute mask and A28 separately as the primitive/default parameter
mask (HCM Part II, printed pp. 156 and 160--161). Do not conflate them.

This divergence is *not* proof that clearing A28's T1/T2 bits makes the
one-MAD shader functional. Defaults could merely replace those sources with
zeros, and the exact behavior of a compiled PS that uses three physical T
registers but reports one active resource remains unqualified. A complete
vendor draw-time image is still desirable before changing the kernel mask
rule, although the context width-table population and resource-limited mask
loop are now independently source-proven.

## Safest discriminator

The already-prepared `chrome9_t2_route_guard_smoke --chain` has three native
records: `MOV TEMP0,T0; ADD TEMP1,TEMP0,T1; ADD oC0,TEMP1,T2`. The Linux
compiler reports active-resource tuple `(3,2,3,0)`, so the Windows A28 loop
would populate all three width-four nibbles, agreeing with the kernel's
`fff0` lower mask. It also avoids the one-MAD simultaneous A/C/B class-3
read. After a cold boot and a known-good baseline, **one** bounded guarded
chain draw is the next useful silicon discriminator, then stop on timeout:

- If it retires with the expected T2 color, generic three-output FVF/VS/FE
  linkage works; investigate the one-record resource/A28 or simultaneous
  three-source interaction, without globally changing A28.
- If it times out, the shared three-output producer/consumer envelope remains
  suspect; do not retry the one-MAD or split-output variant until a complete
  packet differential exists.

The FE A01 tuple for three *float4* inputs is `012b2723` (field bytes
`23,27,2b`), not `012a2523`, which belongs to the kernel self-test's mixed
widths `{4,2,3}`. This documentation correction does not identify an emitted
kernel error: the FE builder computes the former for the actual one-MAD
smoke.
