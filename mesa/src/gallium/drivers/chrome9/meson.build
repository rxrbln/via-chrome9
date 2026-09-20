# SPDX-License-Identifier: MIT
# Copyright 2026 Chrome9 contributors

libchrome9 = static_library(
  'chrome9',
  files(
    'chrome9_context.c',
    'chrome9_draw.c',
    'chrome9_drm.c',
    'chrome9_resource.c',
    'chrome9_screen.c',
  ),
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_nir, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

driver_chrome9 = declare_dependency(
  compile_args : '-DGALLIUM_CHROME9',
  link_with : libchrome9,
)

chrome9_screen_smoke = executable(
  'chrome9-screen-smoke',
  'chrome9_screen_smoke.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : [libchrome9, libgallium],
  install : false,
)
