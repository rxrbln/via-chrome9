# Chrome9 direct-T0 front-end count

Status: exact Linux production-compiler differential and exact Windows D3D
runtime writer.  The two negative VX900 A08 tests are recorded separately in
`raster-front-end-oracles.md`.

## Result

A pixel program that reads an interpolated texture-coordinate register
directly in ALU requires that coordinate in the type-FE A00 count even when
the program has no TAU instruction and no sampler.  A00 bits 7:4 are the
physical coordinate high-water, not merely the number of bound samplers.

The generic command builder's former rule

```text
A00[7:4] = fragment_texture_count
```

is therefore incomplete.  The production rule for the currently dense
coordinate allocation is

```text
A00[7:4] = max(fragment_texture_count, compact_coordinate_count)
```

while preserving A00 bit zero's independent cache-clear pulse.  This does
not require a dummy image, sampler, TAU instruction, or texture BO.

## Windows runtime writer

In `VTGUModeDX32A.dll` (SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`),
the programmable pixel-shader draw update selects the compiled-object view in
`esi`.  At `0x1015b255..0x1015b26b` it performs exactly:

```text
mov ecx,[esi+24h]
shl ecx,4
xor ecx,[edi+1c90h]
and ecx,000000f0h
xor [edi+1c90h],ecx
```

The hardware image at `edi+0x1c90` is type-FE subaddress A00.  The image
initializer at `0x101549a8..0x101549c0` writes the consecutive subaddress
headers 00 through 13 starting there.  Thus the writer copies compiled
instance metadata `+0x34` into A00 bits 7:4.

The same runtime consumes the updated count at
`0x1015b3b4..0x1015b42e`: it extracts A00 bits 7:4, loops over that many
coordinate dimensions, maps widths one through four to masks `8,c,e,f`, and
accumulates the vertex-output mask.  This is producer/consumer linkage, not a
sampler-only enable.

## Production compiler differential

The exact Linux binary is
`binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, build ID
`f140f76cfb18e3f368245054629d1ef2e67be129`.  Running its PS 2.0 compiler,
production register manager, and configuration-two finalizer gives:

| program | ALU/TAU | instance `+0x34` | logical-use metadata `+0x40` |
|---|---:|---:|---:|
| `MOV oC0,c0` | 1/0 | `00000000` | `00000000` |
| `MOV oC0,v0` | 1/0 | `00000000` | `00000000` |
| `MOV oC0,T0` | 1/0 | `00000001` | `00000001` |
| `MOV oC0,T5` | 1/0 | `00000001` | `00000020` |
| `TEX(s0,T0); MOV oC0,r0` | 1/1 | `00000001` | `00000001` |

The `T5` row is the discriminator: logical T5 compacts to physical coordinate
zero, so `+0x40` retains logical bit five while `+0x34` remains physical
high-water one.  The Windows writer uses `+0x34`, not the logical mask and not
TAU count.

The finalized direct-T0 object remains:

```text
ALU 01c00530 301c0053 00f2c005 000d0820
TAU count 0
A90..A92 90058001 91000000 92000001
```

A valid PS 2.0 `dcl T0` changes declaration metadata but produces the same
native instruction, resource controls, and `+0x34=1`.  The Linux DCL parser
increments instance `+0x3c` at `stmPPSTokenParser20_invi:0x1b7310..0x1b7344`;
the H5 DCL emitter itself is a no-op at
`stmPPS_DCL_PS20_invi:0x162c60..0x162c66`.  Consequently a missing DCL token
does not explain the live hang and there is no hidden DCL program-store word.

## Relation to the live failure

The guarded constant-only H3 draw succeeded with A00=`00000001`.  The direct
T0 draw used the byte-exact finalized ALU and A90 controls but timed out with
the same A00 value.  Changing A08's output count alone, then its count and
dimension together, did not retire.  The Windows writer proves why: neither
A08 change supplies the missing A00 coordinate high-water.  For one compact
direct T0 input the production state is A00=`00000011`, even with TAU count
zero.

