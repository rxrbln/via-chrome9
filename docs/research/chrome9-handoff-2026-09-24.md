# Chrome9 development handoff — 2026-09-24

Status: paused at the user's request.  All RE/build subagents were interrupted;
no further GPU test was started after the request.  The full objective remains
all glmark2 scenes, then ioquake3 GL1, then GL2, with conforming Mesa/kernel
implementations and performance work where required.  This is not a claim of
GL feature completion.

## Trees and current target

- Workspace: `/home/ai/src/openchrome`.
- Kernel: `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2`, with
  nested Chrome9 repo at `drivers/gpu/drm/chrome9`.
- Mesa: `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2`, with
  nested Chrome9 repo at `src/gallium/drivers/chrome9`.
- T2 sysroot: `/srv/t2/build/x86-64-desktop-26.9.15-desktop-x86-64-nocona-linux/`.
- Test target: `root@192.168.2.60`, kernel `7.2.5-t2`, VX900 `1106:7122`.
  Revalidate connectivity, module identity and GPU health before any new run.
- At last read-only check: Xorg PID 8551, `renderD128` present, no new kernel
  fault and no GPU-lock owner.  Live module Build ID
  `5a605b25f17a6187da7abfcf5e7d69ab9e0e4001`, SHA-256
  `07b1c6856c122daee7faf82070a0865332a80e235f1c0fce89c3f4f03a32b703`,
  with `cursor_sf=Y` and `fragment_sparse_textures=Y`.
- Target persistent artifacts are below `/root/tmp/chrome9-live/`, especially
  `shadow-a28-20260924/`, `shadow-inspect-20260924/`, and
  `sparse-s5-20260924/`.  Do not use a volatile target `/tmp` for handoff
  artifacts.

## Exact current evidence

The Chrome9 command/GART, KMS/glamor, guarded 3D render, D16, blending,
texture fetch and sparse five-texture paths have live coverage, but the
renderer is not general or GL 2.1 conformant.  The current 2x1 X8Z24
computed-coordinate shadow diagnostic is the immediate blocker:

| VX900 guarded test | Result |
|---|---|
| Four-op RCP/MUL/biased-ADD/full-COLOR without TEX | Pass, all BGRA lanes `8c` ±1; D16/guards/source intact |
| Known-good sparse five-texture test after new module handoff | Pass, RGB `50/44/30` with guards/source intact |
| Five-op threshold/sample inspection on old and A28-corrected modules | Same wrong pixels `00ffcc00`, `00ffcc00`, `00003300`; expected `008c3300`, `008ccc00`, `008c3300` |
| Five-op inspection with vendor XYXY bridge on new module | Same wrong pixels; clean retirement, X/render healthy |

The A28/A29 source correction is kernel commit `3926dbc`; source, module build,
and a native-object selftest are present.  The live A/B shows that A28 alone
does not fix the pixels.  The vendor XYXY bridge alone also does not.  The
Linux 5.75/Windows audit found no remaining descriptor/TAU byte difference
for this test: X8Z24 image and clamp/nearest sampler, `TAU=301211b0`, PS
controls and FE tuple match.  The inspection-only terminal ADD remains a
native-program difference.  See
[`chrome9-glmark-shadow-depth-proxy.md`](chrome9-glmark-shadow-depth-proxy.md)
for packets, hashes, and the negative-run chain.

## Prepared next test — do not run while paused

Tools commit `2673e13` adds `--inspect-vendor-terminal` to
`tools/re/chrome9_shadow_fragment_guard_smoke.c`.  It retains the guarded
five-op prefix, vendor XYXY bridge, and exact vendor terminal
`ADD oC0,r0.xxxx,r2.xxxx` (`02020000 00140924 00f20000 00000820`).
Expected host BGRA is `bfbfbfbf`, `ffffffff`, `bfbfbfbf`, with one-byte
tolerance, D16 `8000`, unchanged source and full BO/exterior guards.  The
strict offline build passed; binary SHA-256 is
`4f617495f56e66a44066db304a563429283e2f7c99a6d90b4524acb0f3083d97`.
It is **not silicon-tested**.  The exact rebuild command from the workspace is:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -D__user= \
  -I src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/include/uapi/drm \
  tools/re/chrome9_shadow_fragment_guard_smoke.c \
  -o /tmp/chrome9_shadow_fragment_guard_smoke_vendor_terminal
```

On resumption, independently review this tool diff, revalidate the target,
stage/hash-check the binary in target `/root/tmp`, and perform at most one
bounded locked draw with immediate dmesg/X/render checks.  A pass would
isolate the prior failure to the inspection-only terminal lane packing; a
failure would keep the computed-TEX/phase or sample-return route open.  Do
not turn either outcome into a product fix without a source-backed mechanism.

## Lock and rollback safety

Every GPU draw uses the shared target lock
`/tmp/chrome9-vx900-gpu.lock`, e.g. `flock -w 5 -x ... timeout -k 2s 12s`.
For a script that starts Xorg, use **`flock -x -o`** so Xorg cannot inherit the
lock descriptor.  The first A28 handoff accidentally used `flock -x` without
`-o`; the next diagnostic waited on Xorg's inherited lock and never reached
the GPU.  It was stopped; Xorg was restarted once under `flock --close`,
restoring a free lock.  This was not a GPU wedge.  The hash-gated
`tools/chrome9_shadow_a28_handoff.sh` now rejects an inherited lock FD and
supports `deploy|rollback`, preserving cursor and sparse mode.  Never use the
older cursor or sparse rollback scripts for this module: their target build
or parameter mode differs.  Never force-unbind or unload after a failed
quiesce/timeout; inspect and cold-boot if necessary.

## Other scene boundaries and priorities

- Gouraud cat and three vertex-loop variants have reported glmark2
  `Validation: Success`; the native vertex/TCL backend is still absent, so
  vertex shaders execute in Gallium Draw software.  Gouraud has a documented
  16-triangle cmd22 batch performance cost.
- Terrain's main linked pair compiles to 73 semantic records (68 ALU + 5 TEX)
  but its earlier procedural noise pass first rejects 28 `b2f32(fge)` values
  and has a lower bound of 100 non-fusible ALUs before other operations,
  exceeding the current 96-record store.  Implementing only `b2f` cannot
  complete the scene.  See
  [`chrome9-glmark-terrain-scene-current-boundary.md`](chrome9-glmark-terrain-scene-current-boundary.md).
- Refract still has the default Z16 depth-sampling conflict and, with X8Z24
  enabled, a zero-sign `bcsel(flt)` fragment lowering gap.  Other glmark2
  variants are not all validated; do not claim scene-wide completion.
- ioquake3 GL1 material paths have partial live coverage, not full gameplay
  correctness.  GL2 default lightall still has FACE/resource/compiler
  blockers.  Keep the agreed scene order: complete glmark2, then GL1, then
  GL2, while doing safe read-only RE ahead.

## Repository and ownership notes

Relevant commits: kernel `3926dbc`; tools `f624356`, `affbd97`, `a806006`,
`2673e13`; Mesa Chrome9 `722d91b`, `d56f61f`.  The research note and this
handoff are in the `docs` nested repository.  At pause, the kernel Chrome9
and Mesa Chrome9 nested repos were clean.  Preserve unrelated dirty work:
`tools/chrome9_alpha_depth.c` and the existing modified/untracked research
files outside the shadow note and this handoff belong to other lanes/the user.
