# Syntax-only stubs

Just enough of ESP-IDF for `cc -fsyntax-only` to parse board-dependent code, so
the pin map can be checked for every target in under a second without ESP-IDF
and without hardware.

**A pass here proves the pin map is complete and self-consistent. It does not
prove the firmware builds** — only `idf.py build` does that, and the harness
runs it separately. Read that distinction before trusting a green result.

Borrowed from the christmas-tree-sensor repo's `tools/idf_stubs`, where the same
check caught a board revision whose pin map had rotted while hardware for it was
already being packaged for fab.
