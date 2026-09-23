# ioquake3 GL1 kind-7 cookie-timeout audit

Status: static audit plus one live timeout capture from VX900 target
`192.168.2.60`.  This note does not change the kernel or userspace driver.
It identifies what the capture proves, what it cannot localize because of CR
read-ahead, and the smallest diagnostic sequence that separates a malformed
draw from a completion-tail stall.

The audited kernel Chrome9 nested-repository revision is
`9115766435063c7fe93f5e9ea2df881fb9d679a4`; the SHA-256 of its
`chrome9_engine.c` is
`cda93dc05de1db84459127e68871c8c5966c08bf1b1aaf0c1d273f634d998a5b`.

## Live capture

The first timeout line was:

```text
job timeout phase=cookie seq=3941 kind=7 flags=0x2 epoch=998
cookie=00020002/00010001 current=00007200 pause=0x7270 stop=0x72f0
cr=20000000 engine=00000010
```

The kernel enum makes kind 7
`CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH`.  Public flag bit 1 is
`DRM_CHROME9_TRIANGLE_BATCH_BLEND`, so this was a one-texture, no-D16,
blend-aware batch.  It was not the new programmable PS ioctl and not a
two-texture draw.  The cookie still had the preceding alternating value
`00020002`, not this job's `00010001`, so retirement correctly failed at its
first proof.  Neither PAUSE nor engine-idle is sufficient when the exact
cookie is absent.

The last fetched bytes were:

```text
6fe0..71dc  final sixteen 8-dword textured vertex records

71e0  fe000000 00000001 00000000 cc000000
71f0  0000006c 00800000 cc000000 cc000000

7200  fe020440 0000043c 00010000 cc000000
7210  1c000000 1d000000 cccccccc cccccccc
```

The current builder necessarily continues with:

```text
7220  fe020420 0000041c 00300000 cc000000   # CR dummy
7230  fe000000 00000001 00000000 cc000000   # H0 header
7240  00000010 00010001                     # DST_POS cookie
7248  ccccccc0 ddd00000 ...                 # PAUSE padding
7270  ccccccc0 ddd00000                     # owned PID00 PAUSE pair
7280  fe020420 0000041c 00300000 cc000000   # immutable STOP trailer
...
72f0  ccccccc0 ddd00000                     # PID10 STOP pair
```

This is internally aligned.  The H0 at `71e0`, H2 at `7200`, following H2 at
`7220`, cookie H0 at `7230`, PAUSE at `7270`, and STOP at `72f0` all meet
their 16- or 128-byte requirements.  `stop-pause=0x80`, as intended.

The vertex dump is also structurally plausible: finite X/Y, Z zero, RHW one,
packed diffuse `feff0000`, specular zero, and finite texture coordinates.
The first dumped record is at vertex index two modulo three, exactly what is
expected when the dump contains the last 16 records of a triangle list.
There is no NaN, infinity, or command-header word in an X/Y/Z/RHW slot.

The separately retained segment start is `0x6880`.  The builder starts its
body at `start+0x20=0x68a0`.  MODULATE plus the closed blend insertion has a
232-dword fixed body before vertices; 48 vertices add `48*8=384` dwords, so
the immediate payload ends exactly at `0x71e0`.  This proves that the request
was the full **16-triangle / 48-vertex** batch and that A24 was `24000030`.
The H0 synchronization and alignment occupy `0x71e0..0x71ff`, placing the
post-draw H2 header exactly at the observed fetch cursor `0x7200`.

This exact arithmetic rules out a body-length mismatch that made the decoder
consume the post-draw H0/H2 tail as vertex attributes.  It does not prove
that raster, texture, or pixel-engine work for every fetched vertex retired;
`043c` is a fetch/request cursor with read-ahead, not an execution PC.

## What `current=0x7200` proves, and what it does not

`CHROME9_CR_READ_AHEAD` is `0x220`.  The diagnostic window deliberately
begins at:

```text
current - read_ahead = 0x7200 - 0x220 = 0x6fe0
```

That is exactly the first of the sixteen dumped vertex records.  MMIO `043c`
is a request/fetch cursor and can lead decode/retirement by the complete
`0x220` window.  Therefore the phrase “stopped at the H2 status header” is too
strong.  The consumer can be anywhere from the vertex at `0x6fe0` through the
post-draw synchronization while fetch has already reached `0x7200`.

The known segment start now proves the packet boundary and exact count.  The
capture still cannot distinguish these execution boundaries:

1. a late immediate vertex or the front-end list-end transition;
2. completion of the type-00 immediate list;
3. H0 `006c=00800000` 3D synchronization;
4. the A1C/A1D type-01 transaction;
5. H2-header decode itself.

The existing dump omits the immediate H2 header, A20/A23/A24, and the first
32 vertices.  A future timeout must dump at least from the immediate header,
or preferably the complete segment, rather than interpreting `043c` as the
decoder PC.  The retained request metadata should independently log the
triangle count and full immutable state so packet arithmetic is not the only
proof.

## Register classification

`0400=00000010` is only bit 4, the command regulator/reader.  Every recovered
non-CR work bit in `00101fe3` is clear: VP, clip, setup, raster, read-color,
pixel shader, pixel engine, burst, 2D, and DMA are not reported busy.

`0444=20000000` is `AGP_SEL`; it is neither the qualified PID00 PAUSE
(`bit31`) nor a completion indication.  The reader was in an active command
cycle and had not reached the owned PAUSE.  This is a real command-reader
timeout, not the normal `0400=10`, `0444.bit31=1` paused-idle state.

These values make a permanently busy shader or pixel engine unlikely at the
sample instant.  They do not prove that the preceding list was valid: an
invalid list count, a parser wait, stale fetched data, or a synchronization
handshake can leave only CR bit 4 visible.  MMIO `040c` was not logged, so the
capture lacks the engine-error discriminator.

The machine rebooted before a delayed second status sample was taken.
Post-reboot values must not be used as evidence that sequence 3941 completed
late.

## ioquake3 GL1 origin of the draw

The GL1 renderer source was audited at upstream revision
`588393618dbc82e7207c21c6ddecca229944a03a`.  This establishes the workload
shape, although the target executable's build ID should still be retained in
future captures.

`code/renderergl1/tr_backend.c:698..720` (`RB_SetGL2D`) selects a full-screen
viewport/scissor, an orthographic projection, disabled depth testing, and
canonical source-over blending (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`).
`RB_StretchPic` at lines 833..900 accumulates four vertices and six indices
per same-shader UI rectangle.  Its two triangles are indexed `(3,0,2)` and
`(2,0,1)`, all four Z values are zero, and the rectangle's S/T coordinates
are copied directly.  A shader change flushes the previous surface; otherwise
successive glyph/image rectangles remain in the same indexed draw.

`code/renderergl1/tr_shade.c:829..923` copies or transforms the stage texture
coordinates.  `RB_IterateStagesGeneric` at lines 928..975 computes colors and
coordinates, binds one image for a single-texture stage, applies the stage
state, then sends the entire accumulated index array to `R_DrawElements`.
Consequently this 16-triangle request is naturally **eight UI rectangles from
one shader/texture/state**, not unusual world geometry.  The observed
texel-aligned normalized S/T values and small screen-space rectangles are
consistent with an atlas/glyph workload.

Mesa classifies the draw as `CHROME9_FS_TEXTURE_PROJECTED_MODULATE`, but the
projected classification is not a different hardware program here.  The
converter requires Q to be exactly float one and lowers the fragment stage to
the native MODULATE program.  It emits X/Y/Z/RHW, diffuse, specular, S and T
as an eight-dword vertex.  The one-texture, no-depth accumulator keys the
complete destination, texture, sampler, scissor and blend state, appends
independent triples, and flushes when its count reaches 16.

Only the last 16 of 48 vertices were retained.  Those records have safe
finite X/Y, Z=0, RHW=1, diffuse `feff0000`, zero specular, and normalized
S/T.  The kernel validates finite X/Y/Z/RHW and S/T plus nondegenerate
triangles, but it does not require S/T to lie in `[0,1]` because repeat and
mirror modes legitimately exceed that interval.  Therefore no obvious tail
poison exists, but the first 32 vertices and exact wrap/filter/format state
must be captured before vertex or texture state can be eliminated completely.

## What the post-draw packets actually mean

### H0 `006c=00800000`

This is the cross-driver 3D pipe wait/order operation.  It is the meaningful
ordering packet in the current tail.  The corresponding `00400000` form is
2D-only and `00c00000` combines them.  Retaining this packet is supported by
the old Linux/Xorg streams and the live fixed-draw oracles.

### H2 A1C/A1D

The eight dwords at `7200` are byte exact and are not malformed:

```text
FE020440 0000043C 00010000 CC000000
1C000000 1D000000 CCCCCCCC CCCCCCCC
```

The symbolized Linux ICD's `hwmUpdateStatus_h5` emits exactly this packet.
However, A1C/A1D are the low 24 and high eight bits of the occlusion counter;
writing zero resets that counter.  The ICD uses it for query-begin mode 1.
It is not a fence, an event write, or a proof that preceding color stores are
complete.  Calling it “the status completion packet” obscures its semantics.

Its presence after the historical fixed draw is strong evidence that the
packet is accepted by H6.  It has also survived thousands of new-driver
submissions.  There is no evidence in this capture that its bytes are wrong.
There is likewise no vendor evidence that resetting A1C/A1D is required after
every ordinary draw.  It remains a legitimate one-variable A/B candidate,
after preserving the real H0 synchronization and ring closure.

### H2 `00300000`

The no-payload `FE020420 ... 00300000` record is the vendor CR dummy used for
alignment, ring PAUSE construction, and STOP trailers.  It is not a render
completion record.  Do not remove it from the immutable PAUSE/STOP closure.

### H0 DST_POS cookie

The alternating `MMIO[0010]` write is a parser-reachability marker.  Because
it follows the H0 3D sync, it has been a useful live retirement component.
Its absence here proves that the tail did not reach the marker; it cannot say
which earlier packet stalled because of read-ahead.

## Vendor production completion sequence

The old Linux 5.75 KMD and the Windows 7 KMD use the Chrome9 FE3x fence class.
The Windows executable is the stronger workload-policy oracle.  For submitted
3D work it emits an FE33 record with wait contribution `00200000`; for 2D it
uses FE30 with composite wait `00300000`.  The old Linux VX900 FE30 path uses
a zero wait mask, so it proves grammar but not correct ordering of arbitrary
3D work.

The audited old-Linux `via_chrome9_dma.c` SHA-256 is
`958733a18208f5386ce12e4d2c5946974247bc91d448d8db49d29b88a7dcff2b`.
The Windows `VTGKModeDX32A.sys` SHA-256 is
`618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`;
its production builder is at image offsets `0x000b755f..0x000b85f4`.

For one bounded job, the recovered 3D record is:

```text
d0 = FE330004                        # system-memory write, no IRQ wait
d1 = (id[7:0] << 24) | 00200000     # wait for 3D
d2 = id[31:8] | (addr[11:4] << 24)
d3 = addr[31:12]
```

The first oracle must use a 16-byte-aligned, coherent, 32-bit DMA address and
an exact nonzero ID.  `FE330006` adds interrupt generation after polling is
proved.  Do not start with Windows' normal `FE330036`: interrupt-wait can
deadlock CR on a masked/lost IRQ, and queue-skip behavior is not yet measured.

The most discriminating conservative tail is therefore:

```text
3D body
H0 006c=00800000
FE330004, wait=00200000, unique coherent completion ID
H0 DST_POS alternating cookie
vendor H2 dummy and immutable PAUSE/STOP closure
```

Keep the current cookie and PAUSE checks.  The FE33 word separately proves
that the hardware's 3D wait point was crossed; the cookie proves later parser
progress; PAUSE proves CR ownership.  The A1C/A1D reset may be tested
separately and is not a replacement for FE33.

## Batch and blend audit

The kind-7 builder is state-complete for the currently accepted zero blend
constant:

* it writes all A38--A3F constant words;
* writes A34--A37 equations;
* writes A53 and the type-01 A00 enable word;
* normalizes COLOR0 through A02=`020041d1`;
* derives A24 from exactly `triangle_count*3`;
* emits exactly eight dwords per vertex;
* validates body length and keyed packet offsets before publication.

Canonical source-over and the transition back to COPY have passed separately
on VX900.  Thus `flags=0x2` does not by itself identify an unsupported blend
mode, and the captured vertex values are valid for normalized source alpha.

The remaining hazards are not equivalent and need separate gates:

1. **List-size/front-end hazard.** Split the identical vertex list into
   one-triangle commands, then test counts 2, 4, 8, 12, and 16.  A threshold
   implicates A24/list consumption or request-FIFO pressure, not A34--A3F.
2. **Blend-state hazard.** Replay identical geometry/texture first with
   canonical COPY, then the captured blend state.  Log the normalized blend
   enums and exact A00/A34--A3F/A53 words.  A failure only with blend localizes
   the state transition or PE handshake.
3. **MODULATE-program hazard.** The aligned layout strongly indicates
   MODULATE.  Compare REPLACE and MODULATE with all other state fixed.  Record
   A92, TAU, both ALU records, A02/A03, and output format.
4. **Ring-publication/fetch hazard.** Log `append_before`, whether the job
   wrapped, segment start, copy length, and the old/new PAUSE tuple.  A CPU
   ring dump proves what memory contains after timeout, not necessarily what
   was already present in the GPU request FIFO.
5. **Object-lifetime/translation hazard.** Record target/texture SF ranges,
   PTE values, SR6F completion, and alias decisions.  The ring is resident,
   but the resources are transiently mapped for each job.

No current evidence justifies changing the already closed blend equations,
shrinking the dummy closure, accepting cookie-less retirement, or rewriting
the command ring after timeout.

### Vendor evidence against a generic 16-triangle limit

The Chrome9 command regulator uses A24's low 24 bits as the exact vertex or
index count.  The vendor description says it fires each complete triangle and
generates PLEND only when that count is exhausted.  There is no separate
per-triangle fire word and no documented low list-count limit.

Two independent production implementations agree:

* Windows helper `VTGUModeDX32A.dll:101b0a20` emits
  `24000000|count`; helper `101b0ad0` emits A23/A24 for repeated
  three-index lists.
* The symbolized Linux ICD routine
  `__glS3InvDrawPrimitiveTriangleList_H6:215448..2155f2` chunks a list
  according to DMA space, emits A24 for each chunk, and streams sequential
  indices without replaying invariant state.

The current page/read-ahead construction can safely contain 24 one-texture
triangles even with the blend and first-mip deltas; 16 is below that software
construction bound.  These facts strongly argue against “A24 cannot encode
48 vertices” or “16 is a documented silicon maximum.”  They do not prove
that this exact immediate Type-00 MODULATE plus blend combination is reliable
on VX900, because the recovered vendor examples are predominantly indexed
draws and do not reproduce this exact live state tuple.

### Ranked interpretation

| Candidate | Evidence and present rank |
|---|---|
| A24 count/body framing | Low. Segment/body arithmetic ends exactly at `0x71e0`; A24 semantics and vendor emitters support much larger counts. |
| Full-list plus blend transition/retirement | Highest unproven combination. Source-over and short textured draws pass separately, but the exact full 16-triangle MODULATE+blend immediate list has not passed. Only CR busy remained, so this may be a command-regulator/list-end handshake rather than an actively busy PE. |
| Vertex/coverage sequence | Still possible. The last 16 records are safe and the kernel rejects non-finite/degenerate geometry, but the first 32 records were not retained and S/T range is intentionally not clamped. |
| Texture state/data | Still possible but below the combined list/blend hypothesis. Two BOs remain mapped and reserved through retirement and receive cache/device synchronization; exact format, dimensions, wrap/filter words and all sampled coordinates were not logged. |
| Ring publication/mapping lifetime | Low. The cursor reached the exact post-body boundary, packet closure is aligned, aliases/imports are rejected, and both BOs remain held. |

The safest containment is userspace-only: do not select command `0x1a` for a
one-texture draw when `blend_successor` is true.  Let the existing proven
one-triangle `DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND` path submit those UI
triangles, while retaining command `0x1a` for non-blended Present/opaque
work.  Do not redirect this case to the no-depth run merely for speed because
that exact tuple is not the established fallback oracle either.

The current uncommitted Mesa `chrome9_draw.c` implements precisely this gate
with `!blend_successor` in the command-`0x1a` selection.  Its explanatory
comment should say the *fetch cursor reached the post-draw H2 boundary before
the cookie*, not that execution stopped in the H2 packet.

After the containment survives ioquake, use this guarded discrimination
ladder rather than immediately restoring a guessed smaller batch cap:

1. Replay all 16 captured triangles as 16 legacy one-triangle blended
   commands.  This must pass first; it proves the complete geometry, sampled
   texture region, and blend state independently of list length.
2. Preserve the exact captured state and test command `0x1a` with prefixes of
   2, 4, 8, 12, 15, then 16 triangles.  Initially enable only one new rung
   per reboot.  Require the cookie, owned PAUSE/idle, zero error register,
   target samples, unchanged source texture, guard words, and a following
   known-good legacy draw.
3. If a prefix count K fails, repeat K using synthetic disjoint safe quads
   with the same texture and blend state.  Synthetic success implicates a
   vertex/coverage/texel sequence; the same failure implicates the list plus
   state transition/count.
4. At the first discriminating count, compare MODULATE without blending,
   REPLACE with source-over, then MODULATE with source-over.  This separates
   list length, blend/PE state, and the sample-times-primary fragment path.
5. Only if 15 repeatedly passes and 16 repeatedly fails should the blended
   cap become 15.  Otherwise retain the one-triangle containment; do not
   guess a cap of 8 from one timeout.

## Minimal next capture

On the next fresh-boot reproduction, retain every mapping and sample without
issuing recovery writes at the 100-ms deadline and again at 250 ms and 1 s:

```text
0010 cookie, 0400 engine, 040c error, 043c fetch, 0444 CR status,
045c/0460/0464 raw diagnostics, 0494 fence readback
```

Also retain/log:

```text
triangle_count, texture program, normalized blend state,
segment start/append_before/PAUSE/STOP/copy end/wrap flag,
A20/A23/A24, immediate H2 header, complete vertex payload hash,
post-draw tail, target/texture SF bases and PTE spans
```

Interpret the result strictly:

| Observation | Classification |
|---|---|
| exact cookie appears later, then PID00 PAUSE | slow/transient completion; measure latency before changing the timeout |
| FE33 ID appears, cookie absent | 3D completed; later parser/H0-cookie path stalled |
| FE33 ID absent, no non-CR busy bits | list/parser/sync handshake or stale-fetch issue |
| one non-CR bit remains | localize using the documented `0400` engine-bit table |
| `040c` changes from zero | hardware error/invalid-state evidence |
| split lists pass and count N fails | list-count/front-end threshold |
| COPY passes, identical blend fails | blend transition/PE state |
| status-reset omitted passes repeatedly | A1C/A1D per-draw reset is the tail hazard, not malformed H2 framing |

Until one of these discriminators fires, the best-supported classification is
**a genuine CR/parser-progress timeout with a valid-looking fetched stream**.
It is not yet evidence of an invalid H2 header, and it is not safe to call it
merely a too-short software timeout.
