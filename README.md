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

## Developing On Linux

- `./build-windows-binaries-on-linux.sh` cross-compiles `winxterm.exe` and
  `dstshell.exe` with clang-cl against an xwin-provided Microsoft CRT/SDK
  sysroot.
- `./wine-x11.sh` boots a headless X desktop (Xvfb at 1920x1080x24 plus i3)
  and runs the built binaries under wine for tests, interactive runs, and
  screenshots. See `CLAUDE.md` for the full workflow.
- wine is a test harness only, never a target platform: the source must not
  special-case wine or work around wine bugs. `CLAUDE.md` documents the
  policy.

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
- `CLAUDE.md` documents the Linux build/test/run workflow for coding agents,
  including the wine policy.
- `RELEASE_AUDIT.md` records the public-release cleanup pass.
- `THIRD_PARTY_NOTICES.md` lists bundled third-party materials and licenses.

## License

Project-owned `winxterm` and `dstshell` source/assets are released under the
WTFPL. Bundled third-party files keep their original licenses; see
`THIRD_PARTY_NOTICES.md` for attribution and license details.
# Agent notes for winxterm

`winxterm.exe`/`dstshell.exe` are native Windows (Win32/ConPTY) programs, but
this repo builds and runs them on Linux for development. Use the tools below
for unit tests, integration tests, running the program, and screenshots —
don't assume "it's a Windows app" makes verification impossible here.

## Building

```
./build-windows-binaries-on-linux.sh
```

Cross-compiles with clang-cl against a cached xwin CRT/SDK sysroot. Outputs
land in `<user>-dist-linux-msvc/` (`BUILD_DIR`/`DIST_DIR`/`XWIN_DIR`/
`BUILD_USER` overrides are honored). Generated directories are
username-prefixed because the working tree may be shared between machines —
never delete another user's build dirs.

## Running under wine (unit tests / smoke tests)

wine is installed and runs the built binaries, including winxterm hosting
dstshell through ConPTY.

- `wine <dist>/winxterm.exe --smoke` and `wine <dist>/dstshell.exe --smoke`
  run the built-in self-tests. The process exit code is the pass/fail signal;
  failure detail is only visible with `WINEDEBUG=+debugstr`.
- wine needs `XDG_RUNTIME_DIR` pointing at a writable mode-0700 directory
  (`./wine-x11.sh run ...` sets this up for you).
- dstshell also accepts piped stdin for non-UI integration tests:
  `printf 'help\r\nexit\r\n' | wine <dist>/dstshell.exe`
- Some smoke expectations fail under wine but pass on real Windows. Before
  treating a wine smoke failure as a regression, check whether it already
  fails at the merge-base/HEAD you started from.

## Running the UI, driving it, screenshots

Xvfb, i3, xdotool, and ImageMagick (`import`) are installed. `./wine-x11.sh`
wraps them:

```
./wine-x11.sh start                 # Xvfb :99 at 1920x1080x24 + i3 (idempotent)
./wine-x11.sh winxterm [args...]    # build output under wine; waits for the
                                    # window, prints window id + geometry
./wine-x11.sh run <command...>      # anything else on that display
./wine-x11.sh screenshot [out.png]  # capture the desktop, then Read the PNG
./wine-x11.sh status
./wine-x11.sh stop                  # kills wine apps, i3, and Xvfb
```

Windows float at their requested size by default (set `WINX11_TILING=1` for
full-screen tiling). Typical UI interaction:

```
./wine-x11.sh run xdotool search --classname winxterm.exe \
    windowactivate --sync type --delay 40 'echo hello'
./wine-x11.sh run xdotool key Return
./wine-x11.sh screenshot /tmp/after.png    # then Read /tmp/after.png
```

During wine runs, `%USERPROFILE%\.winxterm\` (settings.rc, env.rc, logs,
history) lands under `~/.wine/drive_c/users/<user>/.winxterm/`.

## Policy: wine is a test harness, not a target platform

**Never special-case wine in the source, and never work around a wine bug by
changing the code.** This is a Windows-dogmatic codebase; wine is only a
convenient way to exercise it on Linux, and its emulation is imperfect
(missing APIs, `fixme:` stubs, behavioral differences).

If something misbehaves under wine and the code looks correct for real
Windows, stop and discuss it with the user instead of patching around it.
That discussion should explain:

1. what the observed problem is,
2. how wine's behavior differs from real Windows at that API/feature,
3. which idiomatic, Windows-dogmatic approaches exist for the same goal, and
4. whether any of them happen to also work under wine **without compromising
   native Windows performance, stability, or correctness**.

Choosing among equally-correct Windows idioms to keep wine testability is
fine; degrading the Windows implementation to satisfy wine is not.
