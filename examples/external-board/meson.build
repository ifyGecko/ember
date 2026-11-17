# An out-of-tree board project on ember, in the layout every such project uses
# (README.md here; ember's README, "Building a board on ember"):
#
#   meson.build              this file: the project, ember as a pinned subproject, the runner
#   subprojects/ember.wrap   where ember comes from and the revision the project is pinned to
#   emulation/               the C sources: main.c, the board (wiring only), one file per device
#   firmware/                the firmware images the board runs (inputs, never edited)
#   tests/                   the board's checks, run by `meson test`
#
#   meson setup build && meson compile -C build      -> build/ext-run
#   meson test -C build

project('external-board', 'c',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
    'buildtype=debugoptimized',
    # Always the pinned ember of subprojects/ember.wrap, never one the system happens to
    # have. (-Dwrap_mode=nofallback builds against an installed ember through pkg-config.)
    'wrap_mode=forcefallback',
  ],
)

board = 'ext-mips'    # the name the board registers: <runner> -b ext-mips
runner_name = 'ext-run'

add_project_arguments('-D_DEFAULT_SOURCE',
  meson.get_compiler('c').get_supported_arguments('-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes'),
  language: 'c')

# ember: its public headers and libuam.a, built with this project's options.
uam_dep = dependency('uam', version: '>= 0.2')

subdir('emulation')   # sets board_sources

runner = executable(runner_name, board_sources,
  include_directories: include_directories('emulation'),
  dependencies: uam_dep)

subdir('tests')
