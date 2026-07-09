# winxterm

`winxterm` is a native Windows terminal host inspired by xterm behavior. It uses
Win32, ConPTY, DirectWrite fallback glyph rendering, and a generated 6x13 bitmap
font atlas. `dstshell.exe` is the bundled default shell used when no client
executable is supplied.

This is a Windows/MSVC project. Generated build trees, local binaries, editor
metadata, and captured developer environment files are intentionally excluded
from the public tree.

## Layout

- `CMakeLists.txt` builds `winxterm`, `dstshell`, the PCF converter, and optional
  shell smoke-test helper targets for MSVC on Windows.
- `src/` contains the terminal host sources, `src/dstshell_main.c`, and the
  `src/dstcmd/` shell implementation.
- `resources/` contains checked-in Windows resources, font inputs, generated
  bitmap-font data, and bundled fallback fonts.
- `generated/` contains compatibility include shims for generated resources.
- `dependencies/sqlite3/` vendors the SQLite amalgamation used for persisted
  `dstshell` history.
- `tools/` contains resource conversion helpers.

## Runtime Status

- `--smoke` runs built-in non-UI self-tests.
- `--demo` runs the renderer demo producer and shows FPS/backend information in
  the window title.
- `--glyphbench` runs headless renderer benchmarks.
- `--rendermethod <spans|row-masks|precolored-cache|all>` selects the renderer.
- `-x <integer>` sets integer display scale from 1 through 100.
- With no client command, startup looks for `dstshell.exe` beside `winxterm.exe`.

`dstshell` supports practical GNU-like subsets of `ls`, `cd`, `pushd`, `popd`,
`cp`, `mv`, and `rm`, plus `alias`, `cat`, `echo`, `export`, `help`,
`highlight`, `playmacro`, `pwd`, `set`, `which`, and external command dispatch
through ConPTY-backed standard handles. Interactive history is stored under the
Windows home directory in `.winxterm/history.sqlite3`.

## Assets

The 6x13 PCF source, project-owned icon files, and startup fallback fonts live
under `resources/` so the Windows project is self-contained.
`resources/ASSETS.md` documents the checked-in resource inputs and generated
files.

`resources/winxterm_256.png` was created for this project and is not derived
from xterm. It is project-owned material distributed under the WTFPL along with
the rest of the project-owned source/assets unless otherwise stated.

## Documentation

- `ARCHITECTURE.md` describes the current implementation and extension points.
- `RELEASE_AUDIT.md` records the public-release cleanup pass.
- `THIRD_PARTY_NOTICES.md` lists bundled third-party materials and licenses.

## License

Project-owned `winxterm` and `dstshell` source/assets are released under the
WTFPL. Bundled third-party files keep their original licenses; see
`THIRD_PARTY_NOTICES.md` for attribution and license details.
