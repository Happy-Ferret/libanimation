# /matchers/meson.build
#
# Build the libwobbly unit tests.
#
# See /LICENCE.md for Copyright information.

wobbly_test_sources = [
  'anchor_test.cpp',
  'constrainment_test.cpp',
  'glib_api_test.cpp',
  'euler_integration_test.cpp',
  'mesh_interpolation_test.cpp',
  'model_test.cpp',
  'ostream_point_operator.h',
  'point_test.cpp',
  'spring_test.cpp'
]

glib = dependency('glib-2.0')
gobject = dependency('gobject-2.0')

wobbly_test_executable = executable(
  'windowfx_wobbly_test',
  wobbly_test_sources,
  dependencies: [
    gtest_project.get_variable('gtest_main_dep'),
    gtest_project.get_variable('gtest_dep'),
    gtest_project.get_variable('gmock_dep'),
    glib,
    gobject,
    mathematical_model_matcher_dep,
    within_geometry_matcher_dep,
    wobbly_dep,
    wobbly_glib_dep
  ]
)

test('windowfx_wobbly_test', wobbly_test_executable)
