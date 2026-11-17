# external-board: a board project on ember

A complete out-of-tree board in the layout every ember board project uses (ember's
README, "Building a board on ember"). Copy the directory out of the ember tree to start
a board of your own; it does not depend on where ember's sources are.

```
meson.build              the project: ember as a pinned subproject, the runner build/ext-run
subprojects/ember.wrap   where ember comes from and the revision the project is pinned to
emulation/
  meson.build            the list of sources
  main.c                 registers the board, runs ember's stock command line
  ext_board.c  .h        the board "ext-mips": processor, memory, images, wiring
  ext_id.c     .h        a device of the project: one hardware component, one file
firmware/                the firmware images the board runs (README.md: where they come from)
tests/
  meson.build            the board's checks
  registered.sh          the runner lists the board
  boot.sh  boot.expect   the firmware boots to the console lines in boot.expect
```

```bash
meson setup build            # fetches ember at the revision in subprojects/ember.wrap
meson compile -C build       # ember's libraries and build/ext-run
meson test -C build          # registered; boot, once firmware/selftest-be.elf is there
build/ext-run -b ext-mips -o banner=hello firmware/selftest-be.elf
```

To make it yours: rename the project and the board in `meson.build` and the sources
(`<board>_board.c`, the descriptor `<board>_board`, the name in `.name`), put a full commit
hash in `subprojects/ember.wrap`, replace the memory map and the devices with your
hardware's, one file per device, each listed in `emulation/meson.build`, and put your
firmware in `firmware/` with the lines its boot must print in `tests/boot.expect`.

`ext-mips` has the memory map of ember's `mips-ref` board, so ember's MIPS self-test runs on
it; ember's own test suite builds this project both ways an external project can use ember
(as a subproject, and installed, through pkg-config) and runs that self-test on it
(`meson test -C build --suite example` in ember).
