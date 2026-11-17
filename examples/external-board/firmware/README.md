# firmware/

The firmware images this board runs: inputs, never edited. Record here where each image
came from (vendor, version, how it was extracted) and its SHA-256, and commit the image
itself only if you may redistribute it (otherwise list it in `.gitignore`).

This example runs ember's own MIPS self-test, which ember's build makes
(`build/tests/firmware-mips/selftest-be.elf` in an ember build tree). Copy it here as
`selftest-be.elf` and `meson test -C build` runs the `boot` check (`tests/meson.build`),
which reports itself skipped while the image is absent.
