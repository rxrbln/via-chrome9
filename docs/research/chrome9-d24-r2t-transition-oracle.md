# Chrome9 D24 render-to-texture transition and oracle

This note defines the smallest generic writer-to-reader contract needed by
glmark2 `shadow` and `refract`.  It deliberately separates depth storage,
producer retirement, consumer texture invalidation, and CPU/DMA ownership.
Those are four different mechanisms.  The state words are statically exact;
the complete D24 writer-to-sampler sequence is live-qualified for a single
guarded transition, an independent active Z16 destination, and 64 alternating
synchronous producer/consumer transitions.  A no-clear versus forced-miss
cache discriminator remains a separate promotion gate.

## 2026-09-24 alternating transition test

`chrome9-d24-r2t-guard /dev/dri/renderD128 --alternate=64` passed on VX900
at `192.168.2.60` with the current 7.2.5-t2 Chrome9 module.  The oracle
first established the original 1,176-pixel D24 producer and six raw samples.
It then alternated depth 0.25 and 0.75 using 64 `ALWAYS` producer draws on the
same guarded depth BO, each immediately followed by a raw X8Z24 consumer
draw.  The interior sample alternated between the expected 0x40/0xc0 bytes
within one LSB; final source depth, exterior depth, unchanged producer color,
and all three resources' guard pages passed.  A subsequent glmark2 cube draw
passed and dmesg showed no new GPU fault.

This proves the production *synchronous per-ioctl* transition repeatedly.
The kernel vmaps, cache-flushes, and DMA-syncs BOs on each ioctl, so it does
not isolate a CPU-independent GPU texture-cache effect.  Do not infer that
the no-clear/forced-miss diagnostic or mixed multi-sampler closure passed.

## Live Gallium closure

On 2026-09-23 the bounded Gallium sequence passed on VX900 with Mesa Chrome9
nested commit `1d42d47` (parent Mesa commit `68bf5c2`):

```text
combined color + D24 draw
depth-only D24 draw through private scratch color
D24 render-to-texture producer/consumer
D24 sampled while an unrelated Z16 target is active
```

The last draw returned center color `ff800000` and Z16=`8000`.  Source,
destination, and scratch guard regions were intact; submission/rejection/batch
counters matched; a known-good EGL draw afterward passed; and dmesg gained no
engine error.  This proves that X8Z24 sampling state is independent of the
current Z16 destination state and that the producer/consumer transition does
not accidentally reuse the sampled D24 resource as the active depth target.
It also closes the ordinary combined and depth-only Gallium attachment paths.

This result does not replace the 64-iteration LOW/HIGH cache-stress oracle or
the mixed D24-plus-color-sampler test.  Those remain useful to catch delayed
stale reuse and state leakage across multiple texture units.

## Required resource and state

Use `PIPE_FORMAT_X8Z24_UNORM`: Chrome9 stores depth in dword bits `[31:8]`.
The low byte is unused by X8Z24 and must never enter the sampled depth value.
The corresponding native private formats are D24X8 `2032`, depth destination
`A12=123e0000`, and sampled raw depth `A30=30fe2000`.

For the bounded 64-by-64 linear oracle:

```text
logical width/height     64 / 64
cpp                      4
pitch                    0x100
useful bytes             0x4000
native guarded BO        0x6000
data offset              0x1000
layout                   guard/data/guard = 0x1000/0x4000/0x1000
```

The producer depth state, where `Z` is the guarded BO's mapped SF base plus
`0x1000`, is:

```text
A10 = 10000000 | (Z >> 8)
A11 = 11010001                 # 0x100-byte pitch, SF, read-Z cache off
A12 = 123e0000                 # linear D24X8/D24S8
A13 = 13070000                 # ALWAYS for the alternating overwrite test
A01 = 01c0154c                 # depth test and write enabled
```

Keep reading-Z cache, fast-Z, compression, tiling, and direct write-through
disabled.  None is needed for ordinary D24 writes or for the later texture
view.  A depth-only public framebuffer is lowered to a distinct 64-by-64
guarded BGRX scratch target with `A53=53010c00`; color remains globally
enabled and the scratch remains reserved WRITE until retirement.

The consumer image state, where the same D24 data address is `Z`, is:

```text
image base = Z >> 8
A18       = 18001101           # 2D, relative base, linear SF
A20       = 20040040           # H=64, W=64
A21       = 21000001
A22       = 22008066           # POT bit plus log2(H/W)=6/6
A30 image = 30fe2000           # format FE, Z mode 4 (D -> native channels)

A30 samp  = 30000000           # L0 only
A31       = 31100000           # nearest for the first oracle
A32       = 32000040           # clamp-to-edge
FE A00    = 00000011           # one active texture plus cache-clear pulse
```

`30fe0000` is not equivalent to `30fe2000`: it omits the vendor-selected
Z-mode-4 routing.  Likewise `22000066` is not the canonical POT state; A22
bit 15 must be set when both dimensions are exact powers of two.

## Exact ordering boundary

The minimal synchronous ordering is:

```text
producer draw
  -> H0 006c=00800000
  -> owned status/cookie and PID00 PAUSE
  -> wait cookie and complete 3D-engine idle
  -> bind the completed D24 BO as a texture READ
  -> emit its image and sampler state
  -> emit type-FE A00 bit 0
  -> consumer draw
```

The producer fence must be in the D24 BO reservation as WRITE; the consumer
waits it and attaches a READ fence.  The scratch target is a separate WRITE
object.  The consumer color target is also distinct.  Reject simultaneous
depth-write and texture-read aliases in one draw; GL feedback is undefined
and is not a reason to guess a cache mode.

For a later asynchronous implementation, `FE330004` may strengthen the owned
completion oracle, but it must not replace the already required packet-tail,
cookie, PAUSE, and full-idle proof until its release semantics pass live.

No `SR6F` pulse belongs in an ordinary transition when the persistent SF PTE
mapping is unchanged.  `SR6F.bit7` publishes translation changes, not depth
writes.  No CPU CLFLUSH belongs between the producer and consumer when the BO
remains device-owned.  A conservative synchronous implementation may return
all BOs to CPU ownership after every ioctl and reacquire them for the next
one, but that is an expensive CPU round trip rather than a GPU barrier.

## Two-part correctness oracle

### 1. Decoder and low-byte isolation

Before testing a GPU producer, initialize four guarded D24 regions through
the CPU, publish them to the device, and sample with the exact consumer state
above:

| stored dword | expected 24-bit depth | expected grayscale |
|---:|---:|---:|
| `0000005a` | `000000` | near `00` |
| `555555a5` | `555555` | near `55` |
| `aaaaaa3c` | `aaaaaa` | near `aa` |
| `ffffff00` | `ffffff` | near `ff` |

Use the generic semantic fragment program `TEX r0, varying0, sampler0; MOV
oC0, r0`.  Z-mode 4 supplies the same depth component to the native color
lanes, so the useful RGB bytes must agree.  Permit at most one 8-bit LSB of
conversion error, and require the different low poison bytes to have no
effect.  This phase catches the wrong Gallium packed format, `30fe0000`, a
wrong base, and low-byte/stencil leakage independently of render coherency.

### 2. Pure GPU D24 write-to-sample transition

Initialize the guarded D24 BO once, then leave it unmapped for the sequence.
Alternate these producer values for at least 64 iterations:

```text
LOW:   constant post-viewport z = 0.25, depth ALWAYS/write
HIGH:  constant post-viewport z = 0.75, depth ALWAYS/write
```

Each producer consists of two full-target triangles with color mask zero.
Each consumer uses two full-target triangles and the `TEX; MOV` program above.
Read only the consumer color target after it retires.  Sample interior pixels,
excluding the shared diagonal and outermost row/column, and require:

```text
LOW  RGB channels each in [0x3e, 0x42]
HIGH RGB channels each in [0xbd, 0xc1]
```

The tolerance avoids making the gate depend on an unmeasured D24 rounding
tie, while the disjoint intervals make stale-frame reuse unambiguous.  Check
all three BO guards after the sequence.  Do not map/read the D24 BO between
producer and consumer; that would turn this into a CPU-coherency test.

Run enable/disable/enable state closure in the same context:

```text
D24 producer -> D24 consumer -> ordinary BGRA sampler -> D24 consumer
D24 producer -> ordinary D16 destination + D24 sampler -> D24 producer
```

The second row proves that sampled depth format is independent of the current
depth-destination format.

## Failure discriminator

Only after the normal oracle fails, rerun separate disposable jobs with:

```text
normal       FE A00=00000011  A18=18001101
no-clear     FE A00=00000010  A18=18001101
forced-miss  FE A00=00000010  A18=18801101
```

Interpret the result narrowly:

| result | likely boundary |
|---|---|
| normal new, no-clear old | ordinary FE cache-clear path is working |
| normal old, forced-miss new | FE clear placement/order is wrong |
| all three old | producer retirement, address/format, or render visibility |
| all three new but CPU sees stale color | consumer-target CPU ownership |

`A18.bit23` force-miss is diagnostic only.  Never ship it as a permanent
render-to-texture workaround.

## Gallium and kernel rules

The generic implementation must track BO access per binding, not by a
historical struct slot:

| binding | producer | consumer |
|---|---|---|
| public D24 resource | WRITE | READ |
| private scratch RT | WRITE | absent |
| consumer color RT | absent | WRITE |

An X8Z24 resource may be created with any useful subset of
`DEPTH_STENCIL`, `SAMPLER_VIEW`, and `LINEAR`.  Sampling alone must not require
the depth-attachment bind.  Initial restrictions are 2D, one sample, one
level, canonical 32-byte pitch, native private guarded storage, no import,
export, display, scanout, or render-target bind.  S8Z24 may later share the
same depth view, but stencil-only sampling remains rejected.

Color mask is independent of blending.  Kernel packet construction must
lower `blend.color_mask` into A53 even when blending is disabled; forcing
blend merely to obtain `A53=53010c00` changes destination-read behavior and
is not valid depth-only lowering.

When a CPU clear or transfer touches D24, wait all device users, acquire CPU
ownership, perform the operation using depth in bits `[31:8]`, then publish
the full useful range back to the device.  A full X8Z24 clear stores
`round(clamp(d)*0xffffff)<<8`, with the low byte zero.  For an uninterrupted
GPU producer-to-consumer chain, retain device ownership and use only the
producer fence plus FE texture-cache pulse.

## Audit of the in-progress generic fragment path

The current kernel semantic fragment path has already closed the packet and
ownership pieces which earlier versions of this note listed as gaps:

1. `chrome9_build_fragment_program()` emits `0x2000` in image A30 for
   `FRAGMENT_TEXTURE_X8Z24`.
2. It sets A22 bit 15 when the selected width and height are both exact powers
   of two.
3. The Mesa generic path treats a non-full color mask as a blend-state
   successor even when arithmetic blending is disabled, and the kernel lowers
   that request to A53 independently of `DRM_CHROME9_BLEND_ENABLE`.
4. `chrome9_validate_fragment_texture()` checks every selected level's
   canonical pitch, 256-byte offset alignment, non-overlap, full guarded
   object extent, and L0-only restriction for X8Z24.
5. Submission attaches WRITE ownership to destination/depth and READ ownership
   to texture objects, and rejects any destination/depth object aliased by an
   active texture.
6. Target, depth, and all sampled objects remain in the same synchronous job
   and quarantine set through exact retirement.

Mesa now advertises and allocates guarded `PIPE_FORMAT_X8Z24_UNORM`, packs D24
clear/transfer values in bits `[31:8]`, selects the semantic depth and texture
formats, lowers depth-only draws through the private mask-zero scratch target,
and has passed the guarded producer/consumer plus independent-Z16 live gate.
Remaining qualification is the long alternating cache sequence, deliberate
alias rejection, and mixed multi-sampler transition closure before broad
format exposure.

Mip fields in the semantic UAPI are a separate color-texture issue.  Until
the H3 address table and sampler LOD state are emitted and validated, accept
only `first_level == last_level` rebased views here; D24 glmark consumers need
only L0.

## Promotion ladder

1. CPU-initialized D24 decoder/low-byte oracle.
2. D24 render/readback with scratch and A53 zero.
3. One LOW producer-to-consumer transition with normal FE clear.
4. Normal/no-clear/forced-miss discriminator.
5. Sixty-four alternating LOW/HIGH device-only transitions.
6. Depth-only shadow-sized framebuffer, then color+D24 refract producer.
7. Mixed consumer with D24 plus BGRA/image samplers and an unrelated D16
   destination.

Advertise sampled X8Z24/renderable X8Z24 only after rungs 1--5 pass.  This is
a format-and-resource capability gate, not a scene-name exception.
