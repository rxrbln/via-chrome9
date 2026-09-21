# Chrome9 PS pre-finalizer resource ordering

Status: exact Linux 5.75 production-compiler differential, cross-checked
against the equivalent Windows D3D and OpenGL finalizer call order.  This
note complements the resource-bucket table: it establishes **when** the
counter and updater run, and proves that the updater's `object+0x70`
predicate is not A91.

## Correct object base and call order

`stmPPS_PostTranslationSetting_invi` receives `instance+0x10`.  Calls made
from it therefore use the following two coordinate systems:

```text
compiled object +0x1c = instance +0x2c = A90 resource payload
compiled object +0x60 = instance +0x70 = A91 payload
compiled object +0x70 = instance +0x80 = auxiliary predicate
```

The last field is not A91.  In both Windows finalizers the production order
is:

1. compute four active-resource counts;
2. call the A90 resource-bucket updater with count 0, count 1, the compiled
   object, and the configuration;
3. scan and rewrite ALU/TAU records; and
4. write the completed sampler/source mask to `object+0x60` (A91).

Exact Windows sites are:

| binary | finalizer | count call | updater call | late A91 write |
|---|---:|---:|---:|---:|
| D3D `VTGUModeDX32A.dll` | `0x10202720` | `0x10202831 -> 0x10201e60` | `0x10202846 -> 0x102024e0` | `0x102031d6`, `[ecx+0x60]` |
| OGL `VTGOGL32A.dll` | `0x10255740` | `0x10255851 -> 0x10254ee0` | `0x10255866 -> 0x10255560` | `0x102561f6`, `[ecx+0x60]` |

Linux follows the same order: `stmGetActiveTexTmpNum@plt` at `0x0019b094`,
`stmUpdateConfig@plt` at `0x0019b0b4`, then the late A91 write
`[esi+0x60]` at `0x0019b625`.  The four count outputs are passed by pointer;
the updater receives the first output as argument 0 and the second as
argument 1.

## Production count differential

The production Linux compiler was fed four families containing 4, 5, 7, or
8 independent TEXLD tokens followed by one terminal MOV.  `DISTINCT_N`
writes sampled stages to r0 through r(N-1) and reads the last temporary.
`WAW_N` writes every sample to r0 and reads r0.  Coordinates and samplers are
still t0/s0 through t(N-1)/s(N-1), so the two forms differ only in texture
destination ids.

The pre-finalizer resource query and final configuration-two result are:

| stages | destination form | resource tuple | A90 payload | final A91 |
|---:|---|---|---:|---:|
| 4 | distinct | `(4,4,4,4)` | `0x00010000` | `0x0000000f` |
| 4 | WAW r0 | `(4,1,4,4)` | `0x00018000` | `0x0000000f` |
| 5 | distinct | `(5,5,5,5)` | `0x00008000` | `0x0000001f` |
| 5 | WAW r0 | `(5,1,5,5)` | `0x00010000` | `0x0000001f` |
| 7 | distinct | `(7,7,7,7)` | `0x00000000` | `0x0000007f` |
| 7 | WAW r0 | `(7,1,7,7)` | `0x00008000` | `0x0000007f` |
| 8 | distinct | `(8,8,8,8)` | `0x00000000` | `0x000000ff` |
| 8 | WAW r0 | `(8,1,8,8)` | `0x00008000` | `0x000000ff` |

The query runs before the explicit configuration-two pass in the probe, so
the resource tuples are genuinely pre-finalizer values.  Raw A90, raw A91,
and raw `instance+0x80` are all zero.  After configuration two, A91 has the
mask above while `instance+0x80` remains zero in every row.

The eight-stage WAW pair is decisive.  Both final objects have A91=`0xff`,
but only the destination topology changes the second resource count and
A90.  The updater sees `(8,1)` and the zero auxiliary predicate, selecting
`0x08000`.  If final A91 participated in the first-count increment, an
adjusted count of nine would select the overflow bucket instead.  Therefore
A90 cannot be reconstructed later from final A91.

## Exact eight-stage streams

The distinct form finalizes as:

```text
A90..A93 90000000 910000ff 92080001 93000080
TAU      b01001b0 b11111b1 b21221b2 b31331b3
         b41441b4 b51551b5 b61661b6 371771b7
ALU      01c00530 301c0053 00f20705 000d0820
```

The WAW-r0 form finalizes as:

```text
A90..A93 90008000 910000ff 92080001 93000080
TAU      b01001b0 b11011b1 b21021b2 b31031b3
         b41041b4 b51051b5 b61061b6 371071b7
ALU      01c00530 301c0053 00f20005 000d0820
```

The corresponding raw streams have TAU words `20..27` with no finalizer high
bits, A90=A91=0, and the same ALU except that d2.bit17 is clear.  Full rows
and the exact token construction are retained in
[`chrome9-ps-prefinal-resource-ordering-corpus.tsv`](chrome9-ps-prefinal-resource-ordering-corpus.tsv).

## Backend consequence

The safe implementation order is vendor order: compute resource high-water
counts from the scheduled physical program, update the closed A90 bucket
while the auxiliary predicate has its defined pre-finalizer value, then
build A91 and lifetime/phase fields.  Do not feed the completed A91 mask back
into the updater's auxiliary predicate.  Also do not treat the WAW objects as
permission to emit WAW schedules: they are discriminators for the proprietary
counter, while the initial Mesa multi-ALU backend should remain SSA-based and
reject WAW after allocation.

Evidence used `tools/re/chrome9_pps_probe.c`, SHA-256
`f73e238a22ec86ab342c1b27ce339cd2dcc1796ba5c7e09681d8cab601f103f3`.
The Linux DRI, D3D DLL, and OGL DLL SHA-256 values are respectively
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
and `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`.
This is compiler/static evidence; neither eight-stage object has executed on
Chrome9 silicon.
