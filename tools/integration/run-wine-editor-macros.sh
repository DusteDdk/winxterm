#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/ubuntu-build-msvc-clang-probe}"
RUN_TIMEOUT="${RUN_TIMEOUT:-45}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
remove_test_prefix=0
if [[ -z "${TEST_WINEPREFIX:-}" ]]; then
    TEST_WINEPREFIX="$(mktemp -d /tmp/winxterm-editor-wineprefix.XXXXXX)"
    remove_test_prefix=1
fi

cmake --build "$BUILD_DIR" --target winxterm dstshell --parallel "${JOBS:-8}"

rm -f /tmp/winxterm-ctrl-l-{before,typed,cleared,result}.txt \
      /tmp/winxterm-ctrl-l-history.log \
      /tmp/winxterm-ctrl-r-{after-ls,search,inserted,executed}.txt \
      /tmp/winxterm-ctrl-r-history.log \
      /tmp/winxterm-inline-{result.txt,search-empty.txt,search-nomatch.txt,history.log} \
      /tmp/winxterm-editor-*-wine.log

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
    timeout 30s wineboot -u >/tmp/winxterm-editor-wineboot.log 2>&1

fail() {
    printf 'editor macro integration failure: %s\n' "$*" >&2
    exit 1
}

run_macro() {
    local name="$1"
    local macro="$ROOT/tools/integration/$2"
    local macro_windows
    macro_windows="$(WINEPREFIX="$TEST_WINEPREFIX" WINEDEBUG=-all winepath -w "$macro")"
    set +e
    "$ROOT/wine-x11.sh" run env WINEPREFIX="$TEST_WINEPREFIX" WINXTERM_HOST_TRANSPORT=stdio \
        timeout "${RUN_TIMEOUT}s" wine "$BUILD_DIR/winxterm.exe" --macro "$macro_windows" \
        >"/tmp/winxterm-editor-$name-wine.log" 2>&1
    local status=$?
    set -e
    if (( status != 0 && status != 1 )); then
        fail "$name Wine run exited with status $status"
    fi
}

grid_contains() {
    local path="$1"
    local text="$2"
    [[ -f "$path" ]] || fail "missing $path"
    grep -Fq -- "$text" "$path" || fail "$path does not contain: $text"
}

grid_excludes() {
    local path="$1"
    local text="$2"
    [[ -f "$path" ]] || fail "missing $path"
    ! grep -Fq -- "$text" "$path" || fail "$path unexpectedly contains: $text"
}

validate_grid_shape() {
    local path="$1"
    [[ -f "$path" ]] || fail "missing $path"
    awk 'BEGIN { rows=0 } { sub(/\r$/, ""); if (length($0) != 80) exit 1; rows++ }
         END { if (rows != 24) exit 1 }' "$path" || fail "$path is not an 80x24 cell dump"
}

validate_no_visible_control_fragments() {
    local path="$1"
    ! grep -aEq '\[[0-9;]*m' "$path" || fail "$path contains a visible SGR fragment"
}

validate_history() {
    local path="$1"
    [[ -f "$path" ]] || fail "missing $path"
    ! grep -aEq 'dstcmd:|Macro error|macro load failed' "$path" ||
        fail "$path contains an application error"
}

run_macro ctrl-l ctrl-l-text.macro
for path in /tmp/winxterm-ctrl-l-{before,typed,cleared,result}.txt; do
    validate_grid_shape "$path"
    validate_no_visible_control_fragments "$path"
done
grid_contains /tmp/winxterm-ctrl-l-before.txt 'ctrl-l-before'
grid_contains /tmp/winxterm-ctrl-l-typed.txt '$ draft survives ctrl+l'
grid_contains /tmp/winxterm-ctrl-l-cleared.txt '$ draft survives ctrl+l'
grid_excludes /tmp/winxterm-ctrl-l-cleared.txt 'ctrl-l-before'
grid_contains /tmp/winxterm-ctrl-l-result.txt 'ctrl-l-after'
validate_history /tmp/winxterm-ctrl-l-history.log

run_macro ctrl-r ctrl-r-ls-history.macro
for path in /tmp/winxterm-ctrl-r-{after-ls,search,inserted,executed}.txt; do
    validate_grid_shape "$path"
    validate_no_visible_control_fragments "$path"
done
grid_contains /tmp/winxterm-ctrl-r-after-ls.txt 'CMakeLists.txt'
grid_contains /tmp/winxterm-ctrl-r-search.txt 'history [best fuzzy] > ls CMake'
grid_contains /tmp/winxterm-ctrl-r-search.txt 'ls CMakeLists.txt'
grid_contains /tmp/winxterm-ctrl-r-inserted.txt '$ ls CMakeLists.txt'
grid_contains /tmp/winxterm-ctrl-r-executed.txt 'CMakeLists.txt'
validate_history /tmp/winxterm-ctrl-r-history.log

run_macro inline playmacro-inline-bootstrap.macro
validate_grid_shape /tmp/winxterm-inline-result.txt
validate_no_visible_control_fragments /tmp/winxterm-inline-result.txt
grid_contains /tmp/winxterm-inline-result.txt 'inline playback ok'
validate_grid_shape /tmp/winxterm-inline-search-empty.txt
validate_grid_shape /tmp/winxterm-inline-search-nomatch.txt
validate_no_visible_control_fragments /tmp/winxterm-inline-search-empty.txt
validate_no_visible_control_fragments /tmp/winxterm-inline-search-nomatch.txt
grid_contains /tmp/winxterm-inline-search-nomatch.txt \
    'history [best contains] > zzzz-no-match  0 matches'
grid_contains /tmp/winxterm-inline-search-nomatch.txt 'no history matches'
validate_history /tmp/winxterm-inline-history.log
grep -aFq 'cmd=playmacro;text=enterstring%20echo%20inline%20playback%20ok%0A' \
    /tmp/winxterm-inline-history.log || fail "inline history does not contain the encoded playmacro request"

printf '%s\n' \
    /tmp/winxterm-ctrl-l-before.txt \
    /tmp/winxterm-ctrl-l-typed.txt \
    /tmp/winxterm-ctrl-l-cleared.txt \
    /tmp/winxterm-ctrl-l-result.txt \
    /tmp/winxterm-ctrl-r-after-ls.txt \
    /tmp/winxterm-ctrl-r-search.txt \
    /tmp/winxterm-ctrl-r-inserted.txt \
    /tmp/winxterm-ctrl-r-executed.txt \
    /tmp/winxterm-inline-result.txt \
    /tmp/winxterm-inline-search-empty.txt \
    /tmp/winxterm-inline-search-nomatch.txt
