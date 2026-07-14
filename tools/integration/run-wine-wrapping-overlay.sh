#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/ubuntu-build-msvc-clang-probe}"
MACRO="$ROOT/tools/integration/wrapping-overlay.macro"

cmake --build "$BUILD_DIR" --target winxterm dstshell --parallel "${JOBS:-8}"
rm -f /tmp/winxterm-grid-{initial,typed,cleared,search,final,wrapped,unicode}.txt \
      /tmp/winxterm-grid-transcript.log

macro_windows="$(winepath -w "$MACRO")"
# Wine's conhost currently counts VT control bytes as screen cells. The raw
# stdio test transport keeps Wine in the loop while letting Winxterm parse the
# same byte stream that native ConPTY delivers.
set +e
WINXTERM_HOST_TRANSPORT=stdio timeout 45s wine "$BUILD_DIR/winxterm.exe" --macro "$macro_windows"
wine_status=$?
set -e
if (( wine_status != 0 && wine_status != 1 )); then
    printf 'wine integration run failed with status %d\n' "$wine_status" >&2
    exit "$wine_status"
fi

for dump in initial typed cleared search final wrapped unicode; do
    test -f "/tmp/winxterm-grid-$dump.txt"
done
test -f /tmp/winxterm-grid-transcript.log

fail() {
    printf 'wrapping/overlay integration failure: %s\n' "$*" >&2
    exit 1
}

read_grid() {
    local path="$1"
    local array_name="$2"
    local row
    local -n rows="$array_name"
    rows=()
    while IFS= read -r row; do
        rows+=("${row%$'\r'}")
    done <"$path"
    (( ${#rows[@]} == 24 )) || fail "$path does not contain 24 visible rows"
    for row in "${rows[@]}"; do
        (( ${#row} == 80 )) || fail "$path contains a row that is not 80 cells wide"
    done
}

rtrim() {
    local value="$1"
    while [[ "$value" == *' ' ]]; do
        value="${value% }"
    done
    REPLY="$value"
}

read_grid /tmp/winxterm-grid-initial.txt initial
read_grid /tmp/winxterm-grid-typed.txt typed
read_grid /tmp/winxterm-grid-cleared.txt cleared
read_grid /tmp/winxterm-grid-search.txt search
read_grid /tmp/winxterm-grid-final.txt final
read_grid /tmp/winxterm-grid-wrapped.txt wrapped
read_grid /tmp/winxterm-grid-unicode.txt unicode

rtrim "${initial[0]}"
[[ "$REPLY" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\][[:space:]].+ ]] ||
    fail "initial prompt header is malformed"
rtrim "${initial[1]}"
[[ "$REPLY" == '$' ]] || fail "initial command prompt is malformed"

for name in typed cleared; do
    local_line="${name}[1]"
    rtrim "${!local_line}"
    [[ "$REPLY" == '$ there seems to be a problem' ]] ||
        fail "$name grid does not contain the intact input line"
done

rtrim "${search[2]}"
[[ "$REPLY" == 'history [best fuzzy] > there seems to be a problem  0 matches' ]] ||
    fail "Ctrl+R header is malformed"
rtrim "${search[3]}"
[[ "$REPLY" == '  no history matches' ]] || fail "Ctrl+R result row is malformed"
sgr_fragment_re='(\[[0-9;]*m|[0-9];[0-9].*m)'
for row in "${search[@]}"; do
    [[ ! "$row" =~ $sgr_fragment_re ]] ||
        fail "Ctrl+R leaked an SGR escape sequence into visible cells"
done

for ((i = 2; i < ${#final[@]}; ++i)); do
    rtrim "${final[i]}"
    [[ -z "$REPLY" ]] || fail "Escape did not remove the Ctrl+R overlay"
done
rtrim "${final[1]}"
[[ "$REPLY" == '$ there seems to be a problem' ]] ||
    fail "leaving Ctrl+R damaged the command prompt"

long_line=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz
rtrim "${wrapped[1]}"
[[ "$REPLY" == '$ '* ]] || fail "wrapped input lost the command prompt prefix"
wrapped_text="${REPLY#\$ }"
rtrim "${wrapped[2]}"
wrapped_text+="$REPLY"
[[ "$wrapped_text" == "$long_line" ]] || fail "long input did not soft-wrap without corruption"

rtrim "${unicode[1]}"
[[ "$REPLY" == '$ æøå你好' ]] || fail "multibyte Unicode input did not occupy one cell per character"

printf '%s\n' \
    /tmp/winxterm-grid-initial.txt \
    /tmp/winxterm-grid-typed.txt \
    /tmp/winxterm-grid-cleared.txt \
    /tmp/winxterm-grid-search.txt \
    /tmp/winxterm-grid-final.txt \
    /tmp/winxterm-grid-wrapped.txt \
    /tmp/winxterm-grid-unicode.txt \
    /tmp/winxterm-grid-transcript.log
