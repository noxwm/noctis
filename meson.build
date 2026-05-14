project(
  'noxwm',
  ['cpp', 'c'],
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ]
)

# Dependencies
wlroots   = dependency('wlroots-0.18', version: '>=0.18.0')
wayland   = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman    = dependency('pixman-1')
libinput  = dependency('libinput')
wp        = dependency('wayland-protocols')
libdrm    = dependency('libdrm', required: false)

all_deps = [wlroots, wayland, xkbcommon, pixman, libinput]
if libdrm.found()
  all_deps += libdrm
endif

# Required flag for wlroots unstable API
add_project_arguments('-DWLR_USE_UNSTABLE', language: ['cpp', 'c'])

# Generate xdg-shell protocol files using wayland-scanner
wl_scanner = find_program('wayland-scanner')
wp_dir     = wp.get_variable('pkgdatadir')
xdg_xml    = wp_dir + '/stable/xdg-shell/xdg-shell.xml'

xdg_shell_h = custom_target(
  'xdg-shell-server-header',
  output:  'xdg-shell-protocol.h',
  input:   xdg_xml,
  command: [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_c = custom_target(
  'xdg-shell-private-code',
  output:  'xdg-shell-protocol.c',
  input:   xdg_xml,
  command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Include dirs — '.' picks up generated headers in the build dir
inc = include_directories('include', '.')

# Sources — include generated C file so the xdg-shell symbols are linked in
sources = [
  'src/main.cpp',
  'src/core/server.cpp',
  'src/core/output.cpp',
  'src/core/xdg_shell.cpp',
  'src/layout/workspace.cpp',
  'src/layout/column_layout.cpp',
  'src/input/keyboard.cpp',
  'src/input/cursor.cpp',
  'src/config/parser.cpp',
  'src/config/config.cpp',
  xdg_shell_c,
  xdg_shell_h,
]

executable(
  'noxwm',
  sources,
  dependencies: all_deps,
  include_directories: inc,
  install: true,
)
