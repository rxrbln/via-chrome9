# Chrome9 development handoff — 2026-09-24

Goal: ioquake3 GL1 and GL2 render the menu and first map; then hardware TCL
(vertex shading still runs in Gallium Draw software); then glxgears >100 fps,
ioquake3 GL1 >60 fps, GL2 >30 fps.  glmark2 work matters only where it
unblocks generic paths ioquake3 needs.  This is not a claim of GL feature
completion.

## Trees and current target

- Workspace: `/home/ai/src/openchrome`.
- Kernel: `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2`, with
  nested Chrome9 repo at `drivers/gpu/drm/chrome9`.
- Mesa: `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2`, with
  nested Chrome9 repo at `src/gallium/drivers/chrome9`.
- Tools: `tools/` (own repo); research notes: `docs/` (own repo).
- T2 sysroot: `/srv/t2/build/x86-64-desktop-26.9.15-desktop-x86-64-nocona-linux/`.
- Test target: `root@192.168.2.60`, kernel `7.2.5-t2`, VX900 `1106:7122`.
- Last live state: stencil module loaded, SHA-256
  `2d8c0a158e59525f18ed5e3f6b44f8a539c77f10edec99db721c7dde3535fd0a`,
  Build ID `b0b0a2e199cec812328724ad85e6749f99c8ec0d` (deployed 11:57 UTC),
  Xorg PID 9392, no new kernel fault.  Revalidate connectivity, Build ID, Xorg
  PID, `renderD128`, `lslocks` and the dmesg delta before any new run.
- Rollback module (shadow fix only, no stencil): SHA-256
  `76934b91623d4c22240dffc33285182f4308047039e1146ccf819fa08a7cc50f`,
  Build ID `aec88bb5cd29c7afa5d44ec107ef1716193c5099`.
- Target persistent artifacts are below `/root/tmp/chrome9-live/`; do not use
  the volatile target `/tmp` for handoff artifacts.

## Resolved: glmark2 shadow computed-coordinate blocker

Kernel `561cc56` keeps the ALU-read W lane in the textured FE consumer width
(A01-A03, A28/A29); `3926dbc` had capped it and dropped that lane.  The
vendor-terminal, threshold/sample and complete projected-shadow oracles pass
on silicon with the fixed module.  Mesa lowers `flt`/`iand`/`bcsel` into the
matching nested-CMP graph.  Packets, hashes and the negative-run chain are in
[`chrome9-glmark-shadow-depth-proxy.md`](chrome9-glmark-shadow-depth-proxy.md).

With the matching Mesa library and `CHROME9_EXPERIMENTAL_X8Z24=1`, bounded
`glmark2 shadow:nframes=1` no longer reports an unsupported shader or FBO
error; it exits with `Validation: Unknown`, not a correctness pass.  Without
the experimental depth gate the FBO is incomplete (`0x8cd6`), as expected.

## Stencil — uncommitted, kernel silicon-tested

- UAPI: `DRM_CHROME9_FRAGMENT_STENCIL` flag, `DRM_CHROME9_STENCIL_OP_*`
  (native order, INVERT=5), `struct drm_chrome9_stencil_state {cw, ccw}`
  carved from `reserved[]` (struct size unchanged), cap bit 57
  `DRM_CHROME9_CAP_FRAGMENT_STENCIL`.
- Kernel (`chrome9_engine.c`, `chrome9_drv.c`): `chrome9_lower_stencil()`
  (cw to A23/A24, ccw to A25/A26), `chrome9_validate_stencil()`,
  `chrome9_stencil_selftest()`, A01 bit 4.  Stencil requires DEPTH with X8Z24
  (A12=`123e0000` is D24S8).  Built W=1 clean; this is the live module above.
- Oracle: `tools/re/chrome9_stencil_guard_smoke.c`, modes `--compare-mask`,
  `--write-mask`, `--ops`, `--depth-fail`, `--faces`.  Built with host
  `cc -Wall -Wextra -Werror`; every mode passed under the target GPU lock.
  `--faces` confirms A23/A24 maps to screen-space clockwise winding.
- Mesa, built in the T2 full build but **not yet GL stencil-tested**:
  - `include/drm-uapi/chrome9_drm.h` synced with the kernel UAPI.
  - `chrome9_format_is_d24()` covers X8Z24 and S8_UINT_Z24_UNORM (depth
    31:8, stencil 7:0); S8Z24 is allocatable behind `CHROME9_EXPERIMENTAL_X8Z24`
    plus `DRM_CHROME9_CAP_FRAGMENT_STENCIL`.
  - `ctx->stencil_ref`, `ctx->fragment_stencil`; `chrome9_draw_state_supported`
    admits stencil only on the generic fragment path with S8Z24 (stencil
    without depth test runs as depth ALWAYS + READ_ONLY).
  - `chrome9_triangle_stencil()` resolves the GL face per triangle like Draw's
    cull stage and mirrors it into both native blocks, so native face
    orientation does not matter; batches split on stencil differences through
    the draw-struct key.
  - `chrome9_clear`/`chrome9_clear_depth_stencil` do CPU stencil clears
    honouring the write mask and per-channel masked 32-bpp colour clears via
    `chrome9_clear_zs_target` and `chrome9_clear_target_masked`.  The old
    `chrome9_clear_depth_target` has no remaining caller.
- Next check: `tools/re/chrome9_gl_stencil_smoke.c` (GLX, run with
  `CHROME9_EXPERIMENTAL_X8Z24=1`) exercises Gallium stencil state, clear and
  renderbuffer paths end to end.  It has not run yet.

## General select lowering — uncommitted, offline-tested

`chrome9_context.c`: `chrome9_ps_emit_predicate_select()` lowers a scalar
boolean predicate without integer ops: `fge`/`flt` as SUB+CMP (arms exchanged
for `flt`), `feq`/`fneu` as two ordered differences with nested CMP,
`iand`/`ior` as nested CMP, `inot` by exchanging arms, constant booleans
folded.  Operands must pass `chrome9_ps_default_finite`; `preserve_nan` is
rejected.  `chrome9_ps_lower_general_select()` is the last `bcsel` fallback
after the range-proven lowerings and also lowers `b2f32`.  The T2 Mesa full
build and `chrome9-shader-nir-test` pass, with new cases for `b2f32(fge)`,
`flt && fge`, `feq`, `fneu`, `ior`, `inot` and `preserve_nan` rejection; the
old general-select expectation was updated.  Pixel correctness still needs a
bounded scene comparison.  This covers roadmap G3's `b2f32(flt)`.

## Completed read-only RE (this session)

- [`chrome9-hw-tcl-re.md`](chrome9-hw-tcl-re.md) with
  [`chrome9-hw-tcl-vs-corpus.json`](chrome9-hw-tcl-vs-corpus.json): VS ISA,
  finaliser/viewport constants, VS constant selectors, per-element vertex
  streams, inline-index draws, vendor fixed-function VS; the first silicon VS
  test is defined but has not run.
- [`ioquake3-blocker-roadmap-re.md`](ioquake3-blocker-roadmap-re.md): ranked
  GL1/GL2 blockers; FACE back-colour producer (corpus
  [`chrome9-face-bfc-oracle-corpus.tsv`](chrome9-face-bfc-oracle-corpus.tsv));
  PS capacity is exactly 96 ALU + 32 TEX, 12 temps, no flow control, so extra
  room must come from Mesa scheduling.
- [`chrome9-throughput-re.md`](chrome9-throughput-re.md): every cmd22 ioctl
  clflushes every target/depth/texture BO twice (about 9 MiB per ioctl at
  1024x768; estimated 0.7-4.8 GiB per frame), plus a 64 KiB staging memset, a
  per-ioctl fragment recompile and a synchronous 3-phase poll whose DST_POS
  cookie allows one job in flight.  Ranked fixes: BO ownership tracking
  instead of whole-object clflush, FE33 fence IDs with async retirement, one
  multi-draw submit per flush, persistent command pages, compiled-program
  cache, then VB draws with inline indices and HW VS.  Measure first with
  debugfs phase counters.  Its errata for the PCI 7122 kickoff register
  (`0xb8be0`, chip row 4) were added to `cr-pause-stop-lifecycle.md`,
  `monotonic-command-ring.md`, `persistent-batched-submit-performance-plan.md`
  and `vendor-state-reuse-performance-audit.md`.

## FE33 3D completion fence — committed, not silicon-tested

Kernel `665a532` adds opt-in `fe33_fence=1`; details and remaining gates in
[`fe33-persistent-ring-production.md`](fe33-persistent-ring-production.md).
Built W=1 clean, checkpatch clean: SHA-256
`d763d55710b62922c0df33edcd77708d4596f9e71f69afb3abc59c96f658a283`,
Build ID `65becbf9f36c5499d16cc07a67e40ebb314a54de` (includes stencil
`d76968e`).  With the parameter off the command bytes are unchanged.
First silicon step: hand off this module with `fe33_fence=1`, run the fixed
quad, stencil guard and sparse five-texture oracles, and require the
`FE33 first 3D completion` line with no canary warning.  Then run the
kind-7 discrimination ladder in `ioquake-gl1-kind7-cookie-timeout.md`.

## Next steps, in order

1. Run `chrome9_gl_stencil_smoke` on the target with the matching Mesa build;
   then commit kernel, UAPI and Mesa stencil plus the general select lowering.
2. ioquake3 GL1: kind-7 cookie timeout discriminators; uncapped q3dm1
   reference frame (q3dm17 black space still needs a known-good reference, see
   `ioquake3-gl1-shipped-material-add-census.md`).
3. ioquake3 GL2 (first map on the no-FBO path; Mesa lacks
   `mixed_framebuffer_sizes`): silicon pixel check of the lightall lightmap,
   software two-sided FACE marker, alpha modes 1/2, uncapped GL2 run.
4. Throughput: debugfs phase counters, then the ranked fixes above.
5. Hardware TCL: first silicon VS test from `chrome9-hw-tcl-re.md`.

Remaining glmark2 boundaries, relevant only where they share a generic path:
terrain's noise pass exceeds the 96-record ALU store
([`chrome9-glmark-terrain-scene-current-boundary.md`](chrome9-glmark-terrain-scene-current-boundary.md));
refract has the default Z16 depth-sampling conflict and, with X8Z24, a
zero-sign `bcsel(flt)` gap that the general select lowering may now cover.
Gouraud cat and three vertex-loop variants report `Validation: Success`.

## Lock and rollback safety

Every GPU draw uses the shared target lock `/tmp/chrome9-vx900-gpu.lock`,
e.g. `flock -w 5 -x ... timeout -k 2s 12s`.  A script that starts Xorg must
use **`flock -x -o`** so Xorg cannot inherit the lock descriptor; an inherited
FD once blocked every later GPU test.  Module swaps use the generic
hash-gated `tools/chrome9_module_handoff.sh` (NEW/OLD paths, SHA-256 and Build
IDs from the environment, `deploy|rollback`, engine quiesce, rejects an
inherited lock FD).  Never force-unbind or unload after a failed quiesce or
timeout; inspect and cold-boot instead.

## Repository state

Committed: kernel `3926dbc`, `561cc56`, `d76968e` (stencil), `665a532` (FE33); tools `a806006`, `2673e13`, `47695b0`;
Mesa Chrome9 `722d91b`, `d56f61f`; docs `469ebf9`.

Uncommitted, this lane:
- Kernel UAPI `include/uapi/drm/chrome9_drm.h` (stencil) in the untracked
  kernel tree.
- Mesa: `include/drm-uapi/chrome9_drm.h`; Chrome9 `chrome9_context.[ch]`,
  `chrome9_draw.c`, `chrome9_format.[ch]`, `chrome9_resource.c`,
  `chrome9_shader_nir_test.c`.
- Tools: `re/chrome9_stencil_guard_smoke.c`, `re/chrome9_gl_stencil_smoke.c`,
  `re/chrome9_cmd22_cost_model.c`, `re/chrome9_face_bfc_oracle_check.py`,
  `re/chrome9_tcl_*`, `re/chrome9_umd_*`, `re/chrome9_win_draw_submit_audit.py`.

Preserve unrelated dirty work from other lanes: `tools/chrome9_alpha_depth.c`,
and in `docs/research` the glmark2 notes and corpora dated 2026-09-23
(`glmark2-generic-*`, `glmark2-framebuffer-raster-state-contract.md`,
`chrome9-glmark-*-corpus.tsv`, `chrome9-glmark-blend-mip-closure.md`),
`hardware-lab.md`, `chrome9-ps-component-allocation-boundary.md`,
`ioquake3-gl2-eight-unit-linkage.md` and
`ioquake3-gl1-shipped-material-add-census.md`.
