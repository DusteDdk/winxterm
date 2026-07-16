#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/ubuntu-build-msvc-clang-probe}"
MACRO="$ROOT/tools/integration/stdio-shim-external.macro"
ROOT_CMD_MACRO="$ROOT/tools/integration/stdio-shim-root-cmd.macro"
EXE="$BUILD_DIR/winxterm.exe"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
remove_test_prefix=0
if [[ -z "${TEST_WINEPREFIX:-}" ]]; then
    TEST_WINEPREFIX="$(mktemp -d /tmp/winxterm-shim-wineprefix.XXXXXX)"
    remove_test_prefix=1
fi

fail() {
    printf '[stdio-shim-external] error: %s\n' "$*" >&2
    exit 1
}

cmake --build "$BUILD_DIR" --target winxterm dstshell --parallel "${JOBS:-8}"
[[ -f "$EXE" ]] || fail "missing $EXE"
rm -f /tmp/winxterm-shim-finite.txt /tmp/winxterm-shim-finite.log \
      /tmp/winxterm-shim-interactive.txt /tmp/winxterm-shim-interactive.log \
      /tmp/winxterm-shim-root-cmd.txt /tmp/winxterm-shim-root-cmd.log
rm -f /tmp/winxterm-shim-host.log

started_display=0
if ! "$ROOT/wine-x11.sh" status >/dev/null 2>&1; then
    "$ROOT/wine-x11.sh" start
    started_display=1
fi
cleanup() {
    WINEPREFIX="$TEST_WINEPREFIX" wineserver -k >/dev/null 2>&1 || true
    if (( started_display )); then
        "$ROOT/wine-x11.sh" stop >/dev/null
    fi
    if (( remove_test_prefix )); then
        rm -rf -- "$TEST_WINEPREFIX"
    fi
}
trap cleanup EXIT

"$ROOT/wine-x11.sh" run env WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all \
    timeout 30s wineboot -u >/tmp/winxterm-shim-wineboot.log 2>&1
macro_windows="$(WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all winepath -w "$MACRO")"
set +e
"$ROOT/wine-x11.sh" run env WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all \
    WINXTERM_USE_CONPTY_SHIM=1 timeout 45s wine "$EXE" --macro "$macro_windows" \
    >/tmp/winxterm-shim-external-wine.log 2>&1
status=$?
set -e
host_log="$(find "$TEST_WINEPREFIX/drive_c/users" -path '*/.winxterm/logs/*.txt' \
    -type f -print 2>/dev/null | sort | tail -n 1)"
if [[ -n "$host_log" ]]; then
    cp -- "$host_log" /tmp/winxterm-shim-host.log
fi
if (( status != 0 && status != 1 )); then
    fail "Wine run exited with status $status"
fi

root_cmd_macro_windows="$(WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all \
    winepath -w "$ROOT_CMD_MACRO")"
set +e
"$ROOT/wine-x11.sh" run env WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all \
    WINXTERM_USE_CONPTY_SHIM=1 timeout 20s wine "$EXE" \
    --macro "$root_cmd_macro_windows" cmd.exe /d /q \
    >/tmp/winxterm-shim-root-cmd-wine.log 2>&1
root_cmd_status=$?
set -e
if (( root_cmd_status != 0 && root_cmd_status != 1 )); then
    fail "root cmd.exe Wine run exited with status $root_cmd_status"
fi

for output in \
    /tmp/winxterm-shim-finite.txt \
    /tmp/winxterm-shim-finite.log \
    /tmp/winxterm-shim-interactive.txt \
    /tmp/winxterm-shim-interactive.log \
    /tmp/winxterm-shim-root-cmd.txt \
    /tmp/winxterm-shim-root-cmd.log; do
    [[ -f "$output" ]] || fail "missing $output"
done

grep -aEq '^SHIM_STDOUT[[:space:]]*$' /tmp/winxterm-shim-finite.txt ||
    fail 'finite external stdout did not remain in the visible shell session'
grep -aFq 'SHIM_STDOUT' /tmp/winxterm-shim-finite.log ||
    fail 'finite external stdout was not merged into shell history'
grep -aFq 'SHIM_INTERACTIVE' /tmp/winxterm-shim-interactive.txt ||
    fail 'interactive external command output did not remain visible'
grep -aFq 'SHIM_INTERACTIVE' /tmp/winxterm-shim-interactive.log ||
    fail 'interactive external command output was not merged into shell history'
grep -aEq '^SHIM_ROOT_CMD[[:space:]]*$' /tmp/winxterm-shim-root-cmd.txt ||
    fail 'root cmd.exe did not receive input or produce visible shim output'
grep -aFq 'SHIM_ROOT_CMD' /tmp/winxterm-shim-root-cmd.log ||
    fail 'root cmd.exe output was not recorded in terminal history'

printf '[stdio-shim-external] passed\n'
