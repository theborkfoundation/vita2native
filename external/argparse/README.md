# argparse

Vendored from <https://github.com/cofyc/argparse> at commit
`4e30aba50340af7f2d3932492c4bffdef7669db8` (2025-09-26).

A command-line parser in two files, so the emulator's options and its `--help`
come from a declarative table instead of a hand-rolled `argv` loop. MIT licence;
see `LICENSE`.

Only `argparse.c` and `argparse.h` are used, unmodified. Note that the library
calls `exit()` on a usage error and prints usage to stdout; `source/cli.c`
pre-validates arguments against the same option table so that `vita2native`
keeps its own exit codes and error stream. If upstream is ever updated, that
validation is what to re-check.

`tests/`, `CMakeLists.txt`, `Makefile` and the `.pc.in` are not vendored: the
file is compiled straight into `vita2native` like `sqlite3.c`, rather than built
as a separate library.
