# The board's checks. `registered` needs nothing; `boot` runs a firmware image from
# firmware/ and reports itself skipped while that image is absent.
test('registered', find_program('registered.sh'), args: [runner, board])
# The board's own monitor command (emulation/ext_board.c, "ext-id").
test('monitor', find_program('monitor.sh'), args: [runner, board])

boot_image = meson.project_source_root() / 'firmware' / 'selftest-be.elf'
test('boot', find_program('boot.sh'),
  args: [runner, board, boot_image, meson.current_source_dir() / 'boot.expect'],
  timeout: 300)
