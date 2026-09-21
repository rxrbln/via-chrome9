# glmark2 texture-cube and Gouraud-cat live gate (2026-09-21)

## Result

The priority texture cube and Gouraud cat are reference-image correct on the
VX900 target.  The one-texture D16 run path raises the 256x256 cube from 56 to
83 FPS while the LLVM Draw path keeps the cat at 25 FPS.  All measurements in
this note are **Measured** on PCI `1106:7122`, kernel `7.2.5-t2`.

The tested stack is:

- kernel Chrome9 `501c174`, module SHA-256
  `cd876ea9ab1c13fee85926f081795924113dd3c6d9609684e41818eda34a5b53`;
- kernel UAPI `0b94870`;
- Mesa Chrome9 `9f4732a`, parent `3543230` for the measured glmark runs;
- Gallivm legacy float-set fix `0ed5ce5`;
- smoke expectation follow-up Chrome9 `6704691`, parent `8db06cd`;
- test tools `6750d74`, post-warmup FPS gate `5318cb5`, and maximum-run
  oracle fix `5cb0ff1`.

## Correctness gates

The kernel was loaded with `textured_triangle_run=1`; capability bit 30 was
advertised.  `chrome9_textured_run` submitted six MODULATE+D16 triangles in
one command-`0x14` request.  Alternating cells passed/rejected LESS exactly,
all color/depth/texture guards matched, the source texel was unchanged, and a
25-triangle request was rejected.  This was run before Mesa or X.

The same oracle subsequently passed the exact 24-triangle UAPI maximum with
alternating D16 cells and all guards intact.  Its first maximum-size run found
an oracle-only overflow: the expected-color generator did not wrap a computed
red channel to eight bits after triangle 13.  `5cb0ff1` makes each generated
channel explicitly `uint8_t`; the unchanged hardware result then passed.

The full Gallium smoke then passed, including D16 LESS/LEQUAL/EQUAL and
read-only state, source-over and extended blending, one- and two-texture D16,
NPOT sampling, projected coordinates, compiled fragment programs, and guard
checks.  The first run exposed only a stale test-counter expectation: the
rendered sample, depth and coverage were exact, while primary color correctly
used the newer RHW-run path instead of the old compiled-program counter.  The
counter assertion was corrected in `6704691`; no rendering code changed.

With native KMS, Xorg glamor, and direct GLX active, all built-in glmark2
reference checks passed:

```text
texture:model=cube:texture=crate-base:texture-filter=nearest  Success
texture:model=cube:texture=crate-base:texture-filter=linear   Success
shading:model=cat:shading=gouraud                             Success
```

The exact fixed-frame captures are:

- `artifacts/glmark2-texture-fixed-frame-correct-20260921.png`
- `artifacts/glmark2-gouraud-fixed-frame-correct-20260921.png`

## Performance

| scene | prior path | current path | change |
|---|---:|---:|---:|
| texture cube, 1000 frames | 56 FPS, 17.865 ms | 83 FPS, 12.066 ms | +48.2% FPS |
| Gouraud cat, 100 frames | 25 FPS, 41.655 ms | 25 FPS, 41.414 ms | unchanged |

The cube comparison uses the same static-LLVM Draw build on both sides.  The
only material change is command `0x14`: up to 24 one-texture triangles share
one state-complete list, mapping/reservation, guard cycle, fence, and
retirement.

A 200-frame `strace -c -e ioctl` comparison fell from 1,222 ioctls and
2.368354 seconds in ioctl to 638 ioctls and 0.900808 seconds.  A detailed
five-frame trace contains exactly five `_IOC(_IOC_WRITE, 0x64, 0x54, 0xa0)`
requests: one command-`0x14` draw submit per cube frame.  Other ioctls are
loader, allocation, PRIME, close, and presentation traffic.

The cat does not use command `0x14`.  Its 20-frame trace remains 362 ioctls
and 0.336566 seconds in ioctl, with about 15 maximum-size D16 RHW runs per
frame.  The next safe performance rung is one structured userspace request
containing multiple internally segmented, state-identical 544-triangle lists;
it must retain the existing per-request mapping, guard, fence, and quarantine
rules.

The static LLVM path also fixed glxgears' former Gallivm assertion by adding
NIR `slt`, `sge`, `seq`, and `sne` float-set lowering.  Live samples were
55.303 FPS during first-use JIT warmup, then 83.323 and 83.335 FPS.  The
validation gate now excludes only that first warmup sample and still requires
every subsequent sample to exceed 60 FPS.

## Health and artifacts

After direct oracle, complete smoke, all glmark validation/performance runs,
and syscall traces:

```text
SWIOTLB io_tlb_used = 200
MMIO 040c           = 00000000
Xorg                = running on Chrome9 KMS/glamor
```

Target logs are retained in:

- `/root/tmp/chrome9-live/validate-20260921-131735/`
- `/root/tmp/chrome9-live/glmark-cmd14-20260921/`
