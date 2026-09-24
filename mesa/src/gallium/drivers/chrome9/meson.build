# SPDX-License-Identifier: MIT
# Copyright 2026 Chrome9 contributors
# Copyright 2026 René Rebe <rene@exactco.de>

# llvmpipe renders fragment programs beyond the hardware limits.
chrome9_sw_files = []
chrome9_sw_args = []
chrome9_sw_libs = []
chrome9_sw_deps = []
if with_any_llvmpipe
  chrome9_sw_files = ['chrome9_swfb.c']
  chrome9_sw_args = ['-DCHROME9_HAVE_LLVMPIPE']
  chrome9_sw_libs = [libllvmpipe]
  chrome9_sw_deps = [dep_llvm]
endif

libchrome9 = static_library(
  'chrome9',
  files(
    'chrome9_context.c',
    'chrome9_draw.c',
    'chrome9_drm.c',
    'chrome9_format.c',
    'chrome9_fragment.c',
    'chrome9_resource.c',
    'chrome9_screen.c',
    'chrome9_ps_schedule.c',
    'chrome9_shader_offline.c',
    'chrome9_specialise.c',
    'chrome9_tcl.c',
    'chrome9_vs_texture.c',
    chrome9_sw_files,
  ),
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                         inc_gallium_drivers],
  c_args : chrome9_sw_args,
  dependencies : [dep_libdrm, idep_nir, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

driver_chrome9 = declare_dependency(
  compile_args : '-DGALLIUM_CHROME9',
  link_with : [libchrome9, chrome9_sw_libs],
  dependencies : chrome9_sw_deps,
)

chrome9_screen_smoke = executable(
  'chrome9-screen-smoke',
  'chrome9_screen_smoke.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : [libchrome9, libgallium],
  install : false,
)

chrome9_mip_target_smoke = executable(
  'chrome9-mip-target-smoke',
  'chrome9_mip_target_smoke.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : [libchrome9, libgallium],
  install : false,
)

chrome9_shader_offline_test = executable(
  'chrome9-shader-offline-test',
  ['chrome9_shader_offline.c', 'chrome9_shader_offline_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  native : true,
  install : false,
)
test('chrome9-shader-offline', chrome9_shader_offline_test,
     suite : ['chrome9'])

chrome9_ps_schedule_test = executable(
  'chrome9-ps-schedule-test',
  ['chrome9_ps_schedule.c', 'chrome9_ps_schedule_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  native : true,
  install : false,
)
test('chrome9-ps-schedule', chrome9_ps_schedule_test, suite : ['chrome9'])

chrome9_vs_texture_test = executable(
  'chrome9-vs-texture-test',
  ['chrome9_format.c', 'chrome9_vs_texture.c', 'chrome9_vs_texture_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [idep_nir, idep_mesautil],
  link_with : [libgallium],
  install : false,
)
test('chrome9-vs-texture', chrome9_vs_texture_test, suite : ['chrome9'])

chrome9_specialise_test = executable(
  'chrome9-specialise-test',
  ['chrome9_specialise.c', 'chrome9_specialise_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [idep_nir, idep_mesautil],
  install : false,
)
test('chrome9-specialise', chrome9_specialise_test, suite : ['chrome9'])

chrome9_shader_nir_test = executable(
  'chrome9-shader-nir-test',
  'chrome9_shader_nir_test.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_nir, idep_mesautil],
  link_with : [libchrome9, libgallium],
  install : false,
)
test('chrome9-shader-nir', chrome9_shader_nir_test, suite : ['chrome9'])

chrome9_fragment_test = executable(
  'chrome9-fragment-test',
  ['chrome9_fragment.c', 'chrome9_fragment_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  native : true,
  install : false,
)
test('chrome9-fragment', chrome9_fragment_test, suite : ['chrome9'])

chrome9_format_test = executable(
  'chrome9-format-test',
  ['chrome9_format.c', 'chrome9_format_test.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  c_args : ['-DUTIL_ARCH_LITTLE_ENDIAN=1', '-DUTIL_ARCH_BIG_ENDIAN=0'],
  native : true,
  install : false,
)
test('chrome9-format', chrome9_format_test, suite : ['chrome9'])

chrome9_mipmap_test = executable(
  'chrome9-mipmap-test',
  'chrome9_mipmap_test.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : [libchrome9, libgallium],
  install : false,
)
test('chrome9-mipmap', chrome9_mipmap_test, suite : ['chrome9'])
