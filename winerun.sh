#!/bin/bash

set -e
DIST_DIR="${DIST_DIR:-$(id -un)-dist-linux-msvc}"
export DIST_DIR
export WINXTERM_HOST_TRANSPORT=stdio
./build-windows-binaries-on-linux.sh
rm -Rf ~/.wine/drive_c/$DIST_DIR
cp -a "$DIST_DIR" ~/.wine/drive_c/$DIST_DIR
# wine maps the Windows cwd from the Unix cwd, so run from drive_c to start in C:\
(cd ~/.wine/drive_c && wine 'C:/'"$DIST_DIR"'/winxterm.exe' "$@")
