<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 hardware TCL: vertex element formats and stage B fetch

Status: research, 2026-09-24; silicon results for B8 and B13 in section 7
(2026-09-26), the other rungs in chrome9-hw-tcl-re.md.  This extends
[chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md) ("Vertex
fetch: streams and declarations", "Draw packets and command order") and
[chrome9-hw-tcl-implementation-plan.md](chrome9-hw-tcl-implementation-plan.md)
sections 2.1/2.2 with the complete format table, the fill rules, the index
bound semantics and a concrete stage B recipe.

Paths: `K/` = `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2`,
`M/` = `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/chrome9`,
`DRI` = `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (i386, not
stripped), `UMD` = `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll`,
`OGPM` = `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf` (VX900 manual,
page numbers as printed).

Evidence labels: **manual** (OGPM text), **static** (disassembly/data of a
vendor binary), **dynamic** (vendor code executed offline by
`tools/re/chrome9_tcl_stream_probe.c`), **silicon** (measured on the VX900),
**inference** (reasoned, not observed).

## Summary

* The low byte of A(1+i) is `components-1 << 6 | type << 1 | normalize`
  (manual, OGPM p.154-155).  Missing components fill as `(0,0,0,1)`; 10-bit
  types force `w = 1.0`.  This matches the GL default, so FLOAT3 positions
  and FLOAT2 texture coordinates are fetched natively.
* Supported natively: float32 1-4, half float 2/4, unsigned byte 1-4 (both
  plain and normalized), unsigned short 1-4 (both), short 1-4 (both), BGRA8
  color (both), 10:10:10 (both signs, `w = 1`).  Not supported: signed byte,
  32-bit integers, doubles, fixed point.  The vendor GL driver converts
  exactly these on the CPU (static, `DRI:conversion_table_inv`).
* Every format ioquake3 GL1/GL2, glxgears and glmark2 use today is
  native: float2/3/4, `R8G8B8A8_UNORM` (`c7`), `R16G16B16A16_SNORM` (`cb`),
  `R16G16B16A16_UNORM` (`c9`).  The CPU conversion in stage A is only needed
  for alignment, strides above 2044 bytes, the unsupported types and user
  (client) arrays.
* A2C/A2D are **inclusive** max/min index bounds; an index outside them
  makes the hardware "ignore this and the followed indices" (manual,
  OGPM p.169).  Linux vendor GL writes `min = 0`, `max = vertices-1`.
* Windows ORs `0x540000` into A00 on 7122.  This value has now been
  explained: bits 23:18 are the oPos NaN tests (X/Y, Z, W), and `01`
  means "ignore the primitive" (manual, OGPM p.154).  This is a hardware
  safety net for VS output that the kernel cannot check (see step B14).
* A00[6:0] in fetch mode is the **sum of element fetch dwords**, not the
  stride (manual + dynamic).
* Element bits 23:20 are documented as "Vertex Buffer ID" (manual), but the
  vendor always writes 0 and uses one stream slot per element (static +
  dynamic).  The shipping vendor driver draws correctly from separate
  buffers with ID 0 only if the hardware uses slot n for element n (or
  ignores the ID) (inference).  Step B2 tests this.

## 1. Element format byte

### 1.1 Encoding (manual)

OGPM p.154-155, `HParaType 04h, Sub-Address 01h` (A01; A02..A10 identical
for D1..DF):

```text
A(1+n)[23:20] Vertex Buffer ID of vertex data n
A(1+n)[19:12] offset to the related base, dwords, max 255*4 bytes
A(1+n)[11:8]  input register IR0..IRf
A(1+n)[7:6]   dimension: 00 1D (v0,0,0,1)  01 2D (v0,v1,0,1)
                         10 3D (v0,v1,v2,1) 11 4D (v0,v1,v2,v3)
A(1+n)[5:1]   type: 00000 unused, fill (0,0,0,1)
                    00001 float32         00010 float16 (s5.10)
                    00011 unsigned byte   00100 unsigned short
                    00101 short           00110 unsigned 10-bit (w <= 1.0)
                    00111 signed 10-bit (w <= 1.0)
                    01000 color: x=[23:16] y=[15:8] z=[7:0] w=[31:24]
                    others reserved
A(1+n)[0]     normalize
```

Normalization (manual, same page): unsigned byte/color `v/255`, unsigned
short `v/65535`, short `v/32767` with both `8000h` and `8001h` giving -1.0,
unsigned 10-bit `v/1023`, signed 10-bit `v/511` with `200h`/`201h` giving
-1.0.  The implementation replicates bits, so 00h/FFh map exactly to 0.0/1.0.
The signed rule is the D3D10/GL 4.2 rule, the same one Mesa's
`util_format` SNORM unpack uses.

Related fill rules (manual):

* A12[15:0] (OGPM p.159): bit n clear means "IRn should be default value
  (0,0,0,1)"; set means "filled from VB".
* Components beyond the dimension fill from `(0,0,0,1)`.
* Types 00110/00111 always set `w = 1.0`, so the 2-bit alpha of GL
  `*_2_10_10_10_REV` is lost.

### 1.2 Complete code table

Vendor mapping chains:

* D3D: `D3DDECLTYPE -> internal` via `DRI:D3DDeclTypeMappingTable` 0x427d20
  (static).  This table is byte-identical to `UMD` 0x103334b8, and the
  element-size table at `DRI` 0x427d80 matches `UMD` 0x10333504 (static, cross).
* GL: `internal = stmtype_table_inv[normalized*44 + (size-1)*11 + (type-0x1400)]`
  (`DRI` 0x3f8fc0, index arithmetic at `__glS3InvConfigVertexStream`
  0x231e73..0x231eb1, static).
* `internal -> native byte` and A00 dword contribution:
  `stmMapDeclFormat_invi_H5` (`DRI` 0x1760e0, jump table 0x3f1920, static).
  It was executed for every internal code by the offline probe (dynamic, see
  1.4).

| native | type | D3DDECLTYPE (vendor) | GL (vendor table) | Gallium `pipe_format` | fetch dwords | evidence |
|---:|---|---|---|---|---:|---|
| `02 42 82 c2` | float32 x1..4 | FLOAT1..4 | FLOAT size 1..4 | `R32[G32[B32[A32]]]_FLOAT` | 1,2,3,4 | manual, static, dynamic, **silicon** (all four codes in stage A) |
| `44 c4` | float16 x2, x4 | FLOAT16_2/4 | none (GL half is converted) | `R16G16_FLOAT`, `R16G16B16A16_FLOAT` | 1, 2 | manual, static, dynamic |
| `04 84` | float16 x1, x3 | none | none | `R16_FLOAT`, `R16G16B16_FLOAT` | 1, 2 (inference) | manual encoding only |
| `06 46 86 c6` | ubyte x1..4 | UBYTE4 = `c6` | UNSIGNED_BYTE size 1..4, not normalized | `R8[G8[B8[A8]]]_USCALED` | 1 each | manual, static, dynamic |
| `07 47 87 c7` | ubyte norm x1..4 | UBYTE4N = `c7` | UNSIGNED_BYTE size 1..4, normalized | `R8[G8[B8[A8]]]_UNORM` | 1 each | manual, static, dynamic |
| `08 48 88 c8` | ushort x1..4 | none | UNSIGNED_SHORT size 1..4 | `R16[..]_USCALED` | 1,1,2,2 | manual, static, dynamic |
| `09 49 89 c9` | ushort norm x1..4 | USHORT2N `49`, USHORT4N `c9` | UNSIGNED_SHORT normalized | `R16[..]_UNORM` | 1,1,2,2 | manual, static, dynamic |
| `0a 4a 8a ca` | short x1..4 | SHORT2 `4a`, SHORT4 `ca` | SHORT size 1..4 | `R16[..]_SSCALED` | 1,1,2,2 | manual, static, dynamic |
| `0b 4b 8b cb` | short norm x1..4 | SHORT2N `4b`, SHORT4N `cb` | SHORT normalized | `R16[..]_SNORM` | 1,1,2,2 | manual, static, dynamic |
| `d1` | color, normalized | D3DCOLOR | (BGRA arrays) | `B8G8R8A8_UNORM` | 1 | manual, static, dynamic, **silicon** (stage A, Xorg alpha sweep) |
| `d0` | color, not normalized | none | none | `B8G8R8A8_USCALED` | 1 | manual; silicon in the Xorg quad `020041d0` (see 1.3) |
| `8c` | unsigned 10:10:10, w=1 | UDEC3 | none | `R10G10B10X2_USCALED` | 1 | manual, static, dynamic |
| `8f` | signed 10:10:10 norm, w=1 | DEC3N | none | `R10G10B10X2_SNORM` | 1 | manual, static, dynamic |
| `8d 8e` | unsigned norm / signed plain 10:10:10 | none | none | `R10G10B10X2_UNORM`, `_SSCALED` | 1 | manual encoding only |
| `00 40 80 c0` | unused, (0,0,0,1) | none | none | none | 0 (inference) | manual only |

No native type, so CPU conversion is required (`stmMapDeclFormat_invi_H5`
returns -1 for them and `conversion_table_inv` at `DRI` 0x3f8f60 marks them
for conversion, static + dynamic): GL BYTE (internal `10..13`, normalized
`28..2b`), INT/UINT (`20..23`, `38..3b`), DOUBLE (`3c..3f`), and D3D internal
`5..7`.  The same applies to fixed point, `R11G11B10_FLOAT`, pure-integer
formats, other channel orders (for example ARGB or ABGR bytes) and any
`*_2_10_10_10_REV` whose `.w` the shader reads.

D3D formats named in the task, direct lookup (all static-proven):

| D3DDECLTYPE | internal | native |
|---|---:|---:|
| FLOAT1..4 (0..3) | 0..3 | `02 42 82 c2` |
| D3DCOLOR (4) | 4 | `d1` |
| UBYTE4 (5) | 0x0f | `c6` |
| SHORT2 (6) / SHORT4 (7) | 0x19 / 0x1b | `4a` / `ca` |
| UBYTE4N (8) | 0x27 | `c7` |
| SHORT2N (9) / SHORT4N (10) | 0x31 / 0x33 | `4b` / `cb` |
| USHORT2N (11) / USHORT4N (12) | 0x2d / 0x2f | `49` / `c9` |
| UDEC3 (13) / DEC3N (14) | 8 / 9 | `8c` / `8f` |
| FLOAT16_2 (15) / FLOAT16_4 (16) | 0x0a / 0x0b | `44` / `c4` |

The OGPM table itself (p.155) lists the same 17 D3D conversions (manual).

### 1.3 `d0` versus `d1`

`d0` is "color, not normalized".  It produces 0..255 per channel.  The live
Xorg declaration `020041d0 030052d0` was accepted by the Xorg PS only
because its colors were 00/FFh and the color path saturates.  The alpha
sweep in [raster-front-end-oracles.md](raster-front-end-oracles.md) (40h, 7Fh
and 80h all read as 1.0) agrees with this: the values were 64.0, 127.0 and
128.0, each clamped to 1.0 (inference from manual + silicon).  Use `d1` for
every color.  `d0` has no GL use.

### 1.4 Dynamic confirmation

The offline probe runs the vendor builder (32-bit, stub `libexpat`/`libdrm`
as in chrome9-hw-tcl-re.md).  Reproduce with:

```sh
cc -m32 -O0 -o probe tools/re/chrome9_tcl_stream_probe.c -ldl
LD_LIBRARY_PATH=<lib32 stubs> VIA_DRI=$DRI ./probe domain=4 flag69=0x40 \
    s:0:00100000:40 e:0:0:<internal>:0:0:0
```

Every internal code in the table produced the listed `A01` low byte and A00
dword count.  Unsupported codes produced `A01=010000ff` with A00 unchanged.
The dword counts are `ceil(bytes/4)`, except that 3-component short and
ushort count 2.  Given the sizes, the hardware reads 8 bytes for them
(inference).

### 1.5 Alignment and size constraints

* Address bits 1:0 are dropped without a check.  With address `0x100006`
  the probe gave `A01=01001082 stream=001001ff`, which fetches from
  `0x100004` (dynamic).  The kernel must therefore require every element
  address (buffer offset + element offset + base-vertex term) and every
  stride to be a multiple of 4.  GL allows byte-aligned layouts, so those
  need CPU repacking.
* Stride field: 9 bits of dwords, so 0..2044 bytes (manual p.153, dynamic:
  `0x7fc -> 1ff`).  The vendor accepts up to `0x800` (`__glS3InvConfigVertexStream`
  0x231d28 `cmpl $0x800`), and 2048 then encodes as pitch 0 (dynamic:
  `0x800 -> 00100000`).  This is a vendor bug; reject 2048.
* A00[6:0] is 7 bits wide: `sum(fetch dwords) <= 127`.  The maximum is 16 x 4 = 64.
* A00[11:8] + 1 elements, at most 16 (manual).
* A small type such as `06` (1 byte) still fetches a whole dword (inference
  from the dword accounting).  Kernel bounds checks must use
  `4 * fetch_dwords`, not the logical size.

## 2. Mapping of the target applications

| application | attribute | GL spec | native | CPU conversion needed |
|---|---|---|---|---|
| ioquake3 GL1 | position | `glVertexPointer(3, GL_FLOAT, 16, xyz)` (`code/renderergl1/tr_shade.c:266,1046`) | `82`, stride 16, w filled with 1 | no (client array needs an upload, see below) |
| ioquake3 GL1 | texcoord 0/1 | `glTexCoordPointer(2, GL_FLOAT, 0 or 16, ...)` (`tr_shade.c:367,383,1139`) | `42` | no |
| ioquake3 GL1 | color | `glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors)` (`tr_shade.c:583,1138,1214`) | `c7` (`R8G8B8A8_UNORM`, x = byte 0) | no; do **not** swizzle to `d1` |
| ioquake3 GL1 | indices | `GL_UNSIGNED_INT` (`renderergl1/tr_local.h:43`) | A20 `..3` | optional narrowing to 16 bit |
| ioquake3 GL2 | position | float3 in VAO, stride 60 (`renderergl2/tr_vbo.c:217-247`) | `82` | no |
| ioquake3 GL2 | normal, tangent, lightdir | `GL_SHORT` x4 normalized | `cb` | no |
| ioquake3 GL2 | st, lightmap | float2 | `42` | no |
| ioquake3 GL2 | color | `GL_UNSIGNED_SHORT` x4 normalized | `c9` | no |
| ioquake3 GL2 | tess VAO | same types, one buffer with large per-attribute offsets (`tr_vbo.c:458-480`) | as above | no (offsets fold into per-element slots) |
| glxgears | position, normal | immediate mode or display lists, which Mesa vbo stores as float3 | `82` | no |
| glmark2 | position, normal, texcoord | float3/float3/float2 user arrays or VBOs, `glDrawArrays` in `build` ([indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md)) | `82 82 42` | no |
| any | current value (no array) | Gallium delivers `src_stride = 0` | see 3.6 | constant folding preferred |

The rend2 static VAO layout is xyz 0, normal 12, tangent 20, st 28,
lightmap 36, color 44, lightdir 52, stride 60 (`tr_vbo.c:241-247`,
`srfVert_t` in `renderergl2/tr_local.h`).  Every offset and the stride are
multiples of 4, and there are 7 elements (at most 16).

GL1 uses client arrays and client indices.  The kernel cannot address
client memory, so stage B needs either a Mesa upload into an SF GEM
streaming buffer (u_upload-style) or stage A.  With stage A the native
formats still cut the payload: for example the color becomes one `c7` dword
instead of a FLOAT4, which needs only a new UAPI format enum.

## 3. Stage B details

### 3.1 Stream slot word (`fe03043c <n> 30030000 cc000000 <slot[0..n-1]>`)

```text
slot[n] = (addr_n & 0xfffffc00) | (stride_bytes >> 2 & 0x1ff)   # bit 9 reserved
addr_n  = buffer_gpu + stream_offset + element_offset + first_vertex * stride
A(1+n)[19:12] = addr_n[9:2]
```

* Manual (OGPM p.153): base in 1-KiB units in [31:10], [9] reserved, pitch
  [8:0] in dwords.  There are 16 entries.
* Static: `stmUpdateVertexInput_H5` 0x17cd62..0x17cdde (VS path) and
  0x17cf93..0x17d027 (FF path).  They form `addr`, then `(addr << 10) & 0xff000`
  into the element and `addr & ~0x3ff` / `(stride >> 2) & 0x1ff` into slot n,
  where n is the **element index** (loop counter `-0x28(%ebp)`).
* Dynamic: three streams at `00100000/c`, `00200000/8`, `00300010/4` gave
  `A01=01000082 A02=02000242 A03=030041c7`, slots `00100003 00200002
  00300001` and `A00[6:0]=6`, not the stride.  Stream 2's address low bits
  `0x10` appear as `A03[19:12] = 04 = 0x10 >> 2`.
* Offsets that cross 1 KiB fold into the base: element offset `0x404` with
  first-vertex term `0x30` gives slot `00100408`, `A01=0100d082` (dynamic).

**Per-element offsets inside one stream** work by construction: each element
gets its own slot with the same 1-KiB base, and bits 19:12 carry the
in-window offset (at most 1020 bytes).  The VS_FETCH oracle uses exactly
this: three identical slots `vertex_address | 6` and element offsets 0/4/5
(`K/drivers/gpu/drm/chrome9/chrome9_drv.c:768-770,942-946`).  The manual's
reading (a shared buffer ID in [23:20] plus offset) would also allow slot
sharing.  The vendor never uses it, so neither should we until B2b.

### 3.2 Stride limits and stride 0

* Stride: multiple of 4, 0..2044 bytes (3.1, 1.5).
* Stride 0 is used by the vendor for constant attributes.
  `__glS3InvConfigVertexStream` 0x2326cb..0x2327d3 creates one element per
  missing attribute: internal type 3 (FLOAT4, `c2`), offsets `16*k` in one
  shared stream, and stream stride field `+0x4390 = 0` (0x232768).
  `__glS3InvCopyMissingAttribute` 0x231560 writes the 16-byte current
  values (static).  The probe confirms `stride 0 -> slot = base | 0` (dynamic).
  Silicon: unproven (plan rung L9).
* Alternative that needs no memory: clear the A12 bit of an input register
  to get `(0,0,0,1)` (manual p.159).  This covers only the GL default value.

### 3.3 Multiple streams: vendor behavior

* Linux GL (`__glS3InvConfigVertexStream` 0x231bb0): one declaration element
  per enabled array, whose layout matches `D3DVERTEXELEMENT9` (stream u16,
  offset u16, type u8, method, usage, usage index).  Arrays of unsupported
  types, or with stride > 0x800, are converted into a driver VB
  (`__glDpVBGetSpace` 0x23290f, memcpy, `__glS3InvFlushVertexBufferPcieCache`),
  and missing attributes become the stride-0 stream above (static).
* `stmUpdateVertexInput_H5` then emits **one slot per element**, always with
  bits 23:20 = 0 (static, dynamic).
* A00[16:15] selects one memory domain for every stream.  Mixed memory
  types go through the CPU-gather `_VM` draw functions (see chrome9-hw-tcl-re.md).
  For our kernel every stage B buffer is an SF GEM object, so the domain is
  always `01`.
* Windows D3D: `SetStreamSource` per stream plus a declaration, and the same
  folding per element (`UMD` 0x10159670, chrome9-hw-tcl-re.md, static).

### 3.4 Index mode and packet framing

A20 (manual p.160; static `STM_SetIndices_INV` 0x1bc250):

```text
A20[0] = 1 index mode, A20[1] = 1 32-bit indices (0 = 16-bit)
triangle list 20000001 / 20000003, fan 201c5c01, strip 201c7801
index size 1 -> no index mode (vendor converts, or uses the _VM path)
```

Type-00 payload (manual p.73/89: "Sent ... Vertex Index (according to
HVertexMode) through ParaType 00h ... PLEND whenever the entire listed
vertexes are finished (according to HVTXnum)"):

```text
per chunk (vendor __glS3InvDrawIndexedPrimitiveTriangleList_H6 0x21b2b1..0x21b32a):
  fe020440 0000043c 00040000 cc000000
  24<count>  <pad to 16 bytes>
  fe03043c <payload dwords> 00000000 cc000000
  <index bytes copied verbatim>  <pad>
payload dwords = ceil(count/2) (16-bit, first index in bits 15:0), count (32-bit)
```

The vendor pads with words from a table indexed by `addr & 3` (GOT slot
`-0x624`).  The kernel's `cc000000` padding (`chrome9_emit_header2/3`) is
already proven by every cmd22 job.  The fetch oracle writes A2C, A2D, A2E
and A24 in one type-04 packet, then the type-00 packet
(`chrome9_drv.c:947-958`).  Both forms are plain type-04 register writes
followed by type-00 (inference: equivalent).  Write the unused upper lane of
an odd 16-bit count as zero (kernel choice; the vendor copies the next
application index).

**Maximum indices per chunk.** The only limit is command space: the
vendor takes `(cmGetDMAInfo(ctx,6) - 0x600) / index_size` rounded down to a
multiple of 3 (0x21b087..0x21b0da, 0x21b1b1..0x21b1da) and reserves
`dwords + 0x100` (0x21b22e).  A24 is 24 bits (manual p.162), so the
hardware allows up to 16M-1 indices per list.  For us the bound is the
64-KiB run staging (`K/.../chrome9_engine.c:42`) minus state, tail,
read-ahead (0x220) and the PAUSE/STOP words, measured on the final cursor.
`DRM_CHROME9_VERTEX_MAX_INDICES = 12288` (24 KiB of 16-bit or 48 KiB of
32-bit payload) is the UAPI limit.  Split into chunks of multiples of 3.

### 3.5 A2C/A2D/A2E

Manual (OGPM p.169): A2C = lower 24 bits of the **maximum**, A2D = lower 24
bits of the **minimum**, A2E[15:8]/[7:0] = high bytes of max/min, and
"If (index of vertex > HVBIndexMax | index of vertex < HVBIndexMin) Ignore
this and the followed indices".  The bounds are therefore **inclusive**, and a
violation ends the list without a fault (manual; silicon unproven).

* Linux vendor: `stmUpdateChipLayerPrivateData_Inv` 0x176780 stores min=0
  and max = argument, and the H6 indexed list passes `vertices - 1`
  (0x21ba1c..0x21ba32).  `STM_SetIndices_INV` 0x1bc2f8..0x1bc345 copies the
  values into the A2C/A2D/A2E image (static).
* Windows passes `MinIndex + NumVertices` (one past, chrome9-hw-tcl-re.md).
  Under inclusive semantics this is harmless slack.
* The fetch oracle sets `A2C = count - 1 = 3` and actually uses index 3
  (`chrome9_drv.c:947,952-956`).  A pixel-identical fourth corner in L8a
  therefore also proves that index = A2C is accepted, i.e. inclusive.
* Recommendation: the kernel writes the true min/max of the indices it
  sends.  If the kernel rebases (3.7), that is `A2D = 0` and
  `A2C = max - min`.

### 3.6 Constant attributes, recommended handling

In order of preference:

1. Mesa folds stride-0 elements into VS constants.  The semantic IR already
   has `DRM_CHROME9_VERTEX_FILE_CONSTANT`.  The program key gains a
   "constant input" mask, and the values go into the constant upload.  This
   needs no fetch and no silicon unknown.
2. Stride-0 slot (vendor recipe) after rung B12.
3. A12 default `(0,0,0,1)` only when the value is exactly that (B13).

### 3.7 Base vertex and rebasing

The vendor folds first-vertex offsets per element into the slot and the
bits 19:12 (`+0x3790[slot]`, static at 0x17cd72/0x17cfa8).  A negative
`base_vertex` can then put a slot base below the object even though every
fetched vertex lies inside it.  Instead, the kernel should rebase:
`idx' = idx - min`, and fold `(min + base_vertex) * stride` into each element
address.  Then all slot bases lie inside the object, `A2D = 0`, and
32-bit input with `max - min <= 0xffff` can be sent as 16-bit indices,
halving the payload (inference, pure arithmetic).

## 4. Implementation recipe

### 4.1 UAPI (append-only, ABI version unchanged or bumped once)

* New format enums after `BGRA8_UNORM = 4`, each with an explicit native
  byte and fetch dwords:
  `RGBA8_UNORM` (`c7`), `RGBA16_SNORM` (`cb`), `RGBA16_UNORM` (`c9`); later
  `RG16_FLOAT`/`RGBA16_FLOAT` (`44`/`c4`), the SSCALED/USCALED and 1-3
  component variants.  Add each to the whitelist only after its step in 5
  passes.  The kernel keeps `native[]`/`dwords[]` as tables indexed by the
  enum, so no raw code crosses the ABI.
* `struct drm_chrome9_vertex_buffer {handle, stride, offset}` already exists
  (`K/include/uapi/drm/chrome9_drm.h:1286`).  One buffer entry per element is
  the simplest for Mesa: Gallium carries the stride per element (`src_stride`),
  and the hardware has one slot per element anyway.
* Stage B is selected by `buffer_count != 0`; `vertex_count`/`vertices_ptr`
  must then be zero.

### 4.2 Kernel (`chrome9_draw_vertex_program_ioctl`, `chrome9_engine.c:8486`)

Validation, all before any reservation is published:

1. Gate on `DRM_CHROME9_CAP_VERTEX_BUFFERS` and the `hw_tcl` parameter.
   Require `index_size` 2 or 4, `index_count` in `1..MAX_INDICES` and
   `% 3 == 0`, `buffer_count` in `1..element_count`, and each element's
   `buffer < buffer_count`.
2. `vmemdup_array_user()` the indices, then compute `min`/`max` over them in
   the kernel.  Check `min + base_vertex >= 0` in s64.
3. For each buffer: `drm_gem_object_lookup`, SF (not imported),
   `offset % 4 == 0`, `stride % 4 == 0`, `4 <= stride <= 2044` (0 only after
   B12), and `offset < obj->size`.
4. For each element: `offset % 4 == 0`.  Check in u64 with overflow
   checks: `start = buf.offset + el.offset + (min + base_vertex) * stride`,
   `end = buf.offset + el.offset + (max + base_vertex) * stride +
   4 * fetch_dwords(fmt)`, and `end <= obj->size`.  `sum(fetch_dwords) <= 127`.
5. Rewrite the indices to `idx - min`.  Narrow to 16 bit when
   `max - min <= 0xffff`.
6. Keep the float-finite check for stage A only.  Stage B cannot inspect
   vertex data it does not copy (see B14 for the hardware NaN kill).

Build (extends `chrome9_tcl_raster_state()`, `chrome9_engine.c:2153`):

```text
A00 = 00008000 | (n-1)<<8 | sum(fetch_dwords)             # not stride_dw
A(1+i) = (1+i)<<24 | ((addr_i >> 2) & 0xff)<<12 | i<<8 | native[fmt_i]
  addr_i = sf_base(buf_i) + buf.offset + el.offset + (min + base_vertex) * stride_i
A12, A61, A62, A23, A25.. unchanged; A20 = 20000001 | (idx32 ? 2 : 0)
A2C = 2c000000 | ((max-min) & 00ffffff), A2D = 2d000000,
A2E = 2e000000 | ((max-min) >> 24) << 8
fe03043c <n> 30030000 cc000000  slot_i = (addr_i & fffffc00) | stride_i >> 2
per chunk: A24 type-04 packet, then fe03043c <dw> 00000000 cc000000 <indices>
```

The existing emit order in `chrome9_build_fragment_program` (the `31030000`
program, A60, PS, then the vertex payload at `chrome9_engine.c:2811`) stays
as it is.  The vendor places the `3003` slots after the type-04 block and
before the texture state (chrome9-hw-tcl-re.md, "Complete Windows per-draw
emission order").  The oracle emits them directly before the draw packet
(`chrome9_drv.c:945`), and both work in the vendor/oracle context
(inference).  Take the oracle position first, because it is the one that
passed.

Object handling:

* Acquire an SF mapping per distinct vertex BO through
  `chrome9_job_acquire_mapping_locked()`.  `job->sf_mappings` is sized
  `2 + DRM_CHROME9_FRAGMENT_MAX_TEXTURES` (`chrome9_engine.c:151`).  Grow it
  by `DRM_CHROME9_VERTEX_MAX_INPUTS` or allocate it per job.
* Do **not** run `chrome9_prepare_map_guards()` on vertex BOs
  (`chrome9_engine.c:5702`), because it would overwrite user data.  Only
  `chrome9_cache_flush` plus `dma_sync_sgtable_for_device` is needed, and the
  fence goes in as `DMA_RESV_USAGE_READ`.
* Every job still writes the full type-04 block (A00 domain back to 0, A20
  back to `20000000`), so a later cmd22/stage A job inherits no index or
  fetch state.  A2C..A2E are not rewritten by immediate jobs, but they are
  only consulted in index mode (inference).  Keep it that way.
* Self-test: `chrome9_tcl_raster_selftest()` gains a stage B vector that
  reproduces the dynamic probe words above byte for byte (A00/A01..A03/slots).

### 4.3 Mesa (`M/chrome9_tcl.c`)

1. Add a `pipe_format -> DRM enum` table (section 1.2).  The first release
   contains only the enums the kernel whitelists.
2. `chrome9_tcl_draw()` (`chrome9_tcl.c:1492`): choose stage B when every
   element (a) comes from a `pipe_resource` (not a user buffer), (b) has a
   whitelisted format, (c) has 4-aligned `buffer_offset + src_offset` and
   `src_stride` in `4..2044`, (d) has no instance divisor.  Stride-0
   elements become constants (3.6 option 1).  Otherwise use today's stage A
   (`tcl_bind_inputs`/`tcl_fetch_vertex`, `chrome9_tcl.c:1185,1223`).
3. Indices: resource or user index data, 8-bit widened to 16-bit, strips and
   fans converted to lists with the existing `tcl_triangle()`, primitive
   restart split.  Non-indexed draws generate `start..start+count-1`.  The
   kernel rebases, so Mesa sends raw values plus `base_vertex`
   (`range->index_bias`).
4. Chunking by `DRM_CHROME9_VERTEX_MAX_INDICES`.  A kernel `-E2BIG` on the
   first chunk falls back to stage A, as today.
5. Later (performance, not correctness): upload GL1 client arrays into a
   persistently mapped SF streaming BO so ioquake3 GL1 also reaches stage B.
6. Stage A optimization that needs no new fetch path: send `c7` instead of
   FLOAT4 for `R8G8B8A8_UNORM` arrays once step B10 passes.

## 5. Silicon ladder (one variable per step)

Base for B1 onward: the passing stage A draw through `DRAW_VERTEX_PROGRAM`
(`mov oPos,v0; mov oD0,v1; mov oD1,v2`, three elements `c2 d1 d1`,
32x32 guarded target).  Each step compares the full target against its
stage A twin and checks the cookie, `040c = 0`, non-CR idle, guards, and a
following cmd22 closure draw.  Keep each run under about 7 s.  Stop on the
first timeout, engine error or guard change.

| step | only change | pass criterion | hang risk |
|---|---|---|---|
| B0 | `VS_FETCH` fixed quad (L8a), unchanged | equals inline quad; corner 4 proves index = A2C accepted | low (reported passing) |
| B1 | new ioctl, one BO with the same 6-dword records, three slots with the same base, **fan** `201c5c01`, 4 indices | equals B0 | low |
| B2a | same, A20 fan -> triangle list `20000001`, 6 indices | equals stage A | low |
| B2 | position and colors in **two BOs** (distinct 1-KiB bases), bits 23:20 = 0 | correct colors: slot n serves element n | low (wrong data, no bad address) |
| B2b | only if B2 fails: bits 23:20 = i | correct colors: ID selects the slot | low |
| B3 | color element offset 0x404 (crosses 1 KiB, folded) | equals B2a | low |
| B4 | stride 24 -> 28 with padding (A00[6:0] stays 6 = sum) | equals B2a; proves A00 = sum, not stride | medium: if A00 means stride the CR loses sync |
| B5 | 32-bit indices, A20 `..3` | equals B2a | low |
| B6 | `base_vertex = 3` with shifted data (kernel rebase) | equals B2a | low |
| B7 | two A24 chunks in one job | equals B2a | medium (second PLEND) |
| B8 | A2C = true max - 1 (last triangle out of range) | last triangle missing, clean retire | medium: first use of the ignore rule; **silicon PASS**, the list ends at the first out-of-range index (section 7) |
| B9 | position `c2 -> 82` (stride 12 packed), shader writes `v0.w` to oT0 | w = 1.0 read back | low |
| B10 | color `d1 -> c7` with RGBA bytes | same image as BGRA+`d1`; proves byte order | low |
| B11 | `cb`, `c9` (rend2 normal/color), then `4a/ca`, `44/c4`, `c6`, `8c/8f` | per format within 1 LSB of CPU unpack | low |
| B12 | one element with stride 0 (float4 constant) | constant color across the draw | medium (pitch 0 read path) |
| B13 | A12 bit of one input cleared, element dropped | input reads (0,0,0,1) | low; **silicon PASS**, also with the element still fetched (section 7) |
| B14 | A00 `|= 0x540000`, one vertex position NaN in the VS | that primitive dropped, others drawn | medium |
| B15 | last vertex ends at the last byte of a BO (no tail slack) | clean retire | **high**: hardware read-ahead past the object could reach an unmapped SF page |
| B16 | stride 2044 and `sum = 64` dwords (16 x FLOAT4) | equals stage A | medium |

Hang risks and mitigations:

* **Unmapped or stale SF pages** (read-ahead past the last vertex, index
  beyond the validated range): the kernel checks exact element bounds.  The
  prefetch granularity of the vertex fetcher is unknown, so B15 runs last.
  Until it passes, require `end + 64 <= obj->size` (inference: conservative
  margin, one cache line pair).
* **A24 count vs payload mismatch** stalls the CR parser: the kernel derives
  both from one variable and asserts the exact packet end.
* **A00 domain**: a wrong [16:15] makes the engine read from local frame
  buffer addresses.  The kernel hard-codes `01` for stage B and `00` for all
  immediate paths.
* **Stride 2048 / address bits 1:0**: rejected, never emitted.
* **Out-of-range indices**: manual says "ignored"; the kernel still
  validates all of them before submission, and B8 is the only deliberate
  test.
* **Hardware TCL state leakage** into the next job: every job rewrites
  A00/A20, and stage B jobs always write A2C..A2E (see 4.2).

## 6. Open questions

* Element bits 23:20 (B2/B2b).
* Whether A00[6:0] may differ from the stride (B4); the manual and vendor say
  yes.
* Fetch read-ahead beyond the last element (B15).
* Stride-0 behaviour on silicon (B12).
* Whether the Windows `0x540000` NaN kill has side effects on clipping (B14).
* `04`/`84` half-float and `8d`/`8e` 10-bit variants: manual encodings the
  vendor never emits.  Test them only if a workload needs them.
* The VS_FETCH oracle is described as passing by the current lane, but
  [chrome9-hw-tcl-implementation-plan.md](chrome9-hw-tcl-implementation-plan.md)
  line 27 still says "working tree only".  Record the silicon result there.

## 7. Silicon: B8 and B13 (2026-09-26)

Kernel `980d647` (target tag `k144-scfix`), knobs in `tcl_re`: 1024 writes
A2C one below the rebased index maximum, 2048 leaves the last element out
of the fetch (A00 count and dword sum, A(1+n) word and 3003 slot) so its
A12 bit is clear, 4096 clears only the last input's A12 bit.  Tool:
`tools/re/chrome9_state_rungs.c` (`b8`, `b13`), one 20x20 quad per ioctl
(indices 0 1 2 0 2 3), 64x64 target, guards checked, a normal draw after
each knob run.

| rung | draw | result |
|---|---|---|
| B8 | first triangle only (reference) | 210 pixels |
| B8 | quad, A2C exact | 400 pixels |
| B8 | quad, A2C = max - 1 (index 3 out of range in the 2nd triangle) | 210 pixels, identical to the reference |
| B8 | swapped triangles (0 2 3 first), A2C = max - 1 | 0 pixels: nothing after the first out-of-range index is drawn |
| B8 | quad again, knob off | 400 pixels, identical: clean retire, no state leak |
| B13 | v4 = BGRA `ff000000` (0,0,0,1), knob off (reference) | 400 pixels |
| B13 | v4 = `ff2f5a13`, knob off | all 400 pixels differ: v4 reaches oD0 |
| B13 | v4 = `ff2f5a13`, element not fetched, A12 bit 4 clear | identical to the reference |
| B13 | v4 = `ff2f5a13`, element fetched, A12 bit 4 clear | identical to the reference |

B8 passes with FE43 index fetch (`index_fetch=1`) and with inline indices
(`index_fetch=0`).  The manual's wording ("ignore this and the followed
indices", OGPM p.169) is exact: the bound ends the list, it does not skip
single primitives.  B13: an input whose A12 bit is clear reads
(0, 0, 0, 1) regardless of any element that targets it.  The test input
index (4) was above the output count (4 registers with one varying), since
the kernel's A12 is the superset of the input and output masks; whether a
clear bit below the output count also clears an output was not tested.

Use in the driver, decided on these results:

* **A2C/A2D as a replacement for the CPU index scan: not done.**  The
  kernel already writes the exact inclusive bounds (A2D = 0, A2C = max - min
  after the rebase), so the hardware check is active as a second line of
  defence.  The scan cannot be dropped: `min` is needed for the rebase
  (element addresses stay inside the object, 16-bit narrowing of 32-bit
  input), and `max - min` bounds the stage B finite check and the object
  bounds check.  A clamp derived from the BO size would also turn an
  out-of-range index into a truncated draw instead of `-EINVAL`.  Cost of
  the scan/rebase/pack on the target's VIA Nano X2 L4350
  (`tools/re/chrome9_index_scan_bench.c`): 0.25 / 0.71 / 3.4 / 16.7 us for
  36 / 120 / 600 / 3000 16-bit indices, against 0.04-0.41 us for a plain
  copy and about 8-9 us kernel wall time per draw (R3); with ioquake3's
  typical 100-300 indices per draw it is under 10 % of the per-draw cost.
* **A12 default lanes for (0,0,0,1) inputs: not done.**  It would only
  save the fetch of an attribute that is exactly (0,0,0,1) for every
  vertex, only for input registers at or above the output count, and
  would need input renumbering plus a new element kind in the UAPI.
  Mesa already sends constant attributes as stride-0 elements (B12,
  one 16-byte record) or folds them into VS constants; no measured
  workload fetches such an attribute per vertex.

