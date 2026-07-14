#!/usr/bin/env bash
# wine-x11.sh -- headless X desktop for running the Windows binaries under wine.
#
# Boots Xvfb at 1920x1080x24 with the i3 window manager on it, then runs
# commands (typically `wine winxterm.exe`) on that display. Intended for
# automated verification: launching the app, driving it with xdotool, and
# taking screenshots with ImageMagick `import`.
#
# Usage:
#   ./wine-x11.sh start                  boot Xvfb + i3 (idempotent)
#   ./wine-x11.sh winxterm [args...]     launch DIST_DIR/winxterm.exe under
#                                        wine, wait for its window, print the
#                                        window id and geometry
#   ./wine-x11.sh run <command...>       run any command on the display
#   ./wine-x11.sh screenshot [out.png]   capture the whole desktop as PNG
#   ./wine-x11.sh status                 show server state and visible windows
#   ./wine-x11.sh stop                   kill wine apps, i3, and Xvfb
#
# Environment:
#   WINX11_DISPLAY   X display to use (default :99)
#   DIST_DIR         directory containing winxterm.exe/dstshell.exe
#                    (default <user>-dist-linux-msvc, as produced by
#                    ./build-windows-binaries-on-linux.sh)
#   WINX11_TILING=1  let i3 tile windows to fill the screen instead of
#                    floating them at their requested size
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

WINX11_DISPLAY="${WINX11_DISPLAY:-:99}"
DISPLAY_NUM="${WINX11_DISPLAY#:}"
STATE_DIR="/tmp/wine-x11-$(id -un)"
XVFB_PIDFILE="$STATE_DIR/xvfb-${DISPLAY_NUM}.pid"
I3_PIDFILE="$STATE_DIR/i3-${DISPLAY_NUM}.pid"
X_SOCKET="/tmp/.X11-unix/X${DISPLAY_NUM}"

BUILD_USER="${BUILD_USER:-$(id -un)}"
DIST_DIR="${DIST_DIR:-${SCRIPT_DIR}/${BUILD_USER}-dist-linux-msvc}"

log() {
    printf '[wine-x11] %s\n' "$*"
}

die() {
    printf '[wine-x11] error: %s\n' "$*" >&2
    exit 1
}

usage() {
    sed -n '2,/^set -Eeuo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//; $d'
}

# Wine refuses parts of its startup without a writable 0700 XDG_RUNTIME_DIR.
ensure_xdg_runtime() {
    if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -w "${XDG_RUNTIME_DIR:-/nonexistent}" ]]; then
        export XDG_RUNTIME_DIR="$STATE_DIR/xdg"
        mkdir -p -m 700 "$XDG_RUNTIME_DIR"
    fi
}

pidfile_alive() {
    local pidfile="$1"
    [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null
}

display_up() {
    [[ -S "$X_SOCKET" ]]
}

write_i3_config() {
    local config="$STATE_DIR/i3.conf"
    {
        # Without the v4 marker i3 assumes a legacy v3 config and its
        # auto-converter drops the for_window rule.
        echo '# i3 config file (v4)'
        echo 'font pango:monospace 8'
        if [[ "${WINX11_TILING:-0}" != 1 ]]; then
            # Let windows keep their requested size, like a normal desktop,
            # instead of being tiled to fill the whole screen.
            echo 'for_window [all] floating enable'
        fi
    } >"$config"
    printf '%s\n' "$config"
}

cmd_start() {
    mkdir -p "$STATE_DIR"
    ensure_xdg_runtime

    if display_up; then
        log "Xvfb already running on ${WINX11_DISPLAY}"
    else
        log "starting Xvfb on ${WINX11_DISPLAY} (1920x1080x24)"
        Xvfb "$WINX11_DISPLAY" -screen 0 1920x1080x24 -nolisten tcp \
            >"$STATE_DIR/xvfb.log" 2>&1 &
        echo $! >"$XVFB_PIDFILE"
        local i
        for i in $(seq 1 50); do
            [[ -S "$X_SOCKET" ]] && break
            sleep 0.1
        done
        [[ -S "$X_SOCKET" ]] || die "Xvfb did not create ${X_SOCKET}; see $STATE_DIR/xvfb.log"
    fi

    if pidfile_alive "$I3_PIDFILE"; then
        log "i3 already running on ${WINX11_DISPLAY}"
    else
        local config
        config="$(write_i3_config)"
        log "starting i3"
        DISPLAY="$WINX11_DISPLAY" i3 -c "$config" >"$STATE_DIR/i3.log" 2>&1 &
        echo $! >"$I3_PIDFILE"
        sleep 0.5
        pidfile_alive "$I3_PIDFILE" || die "i3 exited early; see $STATE_DIR/i3.log"
    fi

    log "ready: DISPLAY=${WINX11_DISPLAY}"
}

cmd_run() {
    (( $# > 0 )) || die "run: missing command"
    cmd_start >/dev/null
    DISPLAY="$WINX11_DISPLAY" "$@"
}

cmd_winxterm() {
    local exe="$DIST_DIR/winxterm.exe"
    [[ -f "$exe" ]] || die "missing ${exe}; build first with ./build-windows-binaries-on-linux.sh (or set DIST_DIR)"
    cmd_start

    local wine_log="$STATE_DIR/winxterm.log"
    log "launching wine winxterm.exe $* (wine output: ${wine_log})"
    DISPLAY="$WINX11_DISPLAY" wine "$exe" "$@" >"$wine_log" 2>&1 &
    local pid=$!

    local wid
    if ! wid="$(DISPLAY="$WINX11_DISPLAY" timeout 30 \
            xdotool search --sync --onlyvisible --classname winxterm.exe 2>/dev/null | head -n1)"; then
        kill -0 "$pid" 2>/dev/null || die "winxterm exited before its window appeared; see ${wine_log}"
        die "no winxterm window after 30s; see ${wine_log}"
    fi

    log "winxterm is up (linux pid ${pid})"
    log "window id: ${wid}"
    DISPLAY="$WINX11_DISPLAY" xdotool getwindowgeometry "$wid"
}

cmd_screenshot() {
    display_up || die "display ${WINX11_DISPLAY} is not running; use: $0 start"
    local out="${1:-$STATE_DIR/screenshot-$(date +%Y%m%d-%H%M%S).png}"
    DISPLAY="$WINX11_DISPLAY" import -window root "$out"
    log "wrote ${out}"
}

cmd_status() {
    if display_up; then
        log "Xvfb: running on ${WINX11_DISPLAY} (socket ${X_SOCKET})"
    else
        log "Xvfb: not running on ${WINX11_DISPLAY}"
        return 1
    fi
    if pidfile_alive "$I3_PIDFILE"; then
        log "i3: running (pid $(cat "$I3_PIDFILE"))"
    else
        log "i3: not running"
    fi
    log "visible windows:"
    local wid
    DISPLAY="$WINX11_DISPLAY" xdotool search --onlyvisible --name '.*' 2>/dev/null |
        while read -r wid; do
            printf '  %s  %s\n' "$wid" \
                "$(DISPLAY="$WINX11_DISPLAY" xdotool getwindowname "$wid" 2>/dev/null || echo '?')"
        done
}

cmd_stop() {
    ensure_xdg_runtime
    DISPLAY="$WINX11_DISPLAY" wineserver -k 2>/dev/null || true
    local pidfile
    for pidfile in "$I3_PIDFILE" "$XVFB_PIDFILE"; do
        if pidfile_alive "$pidfile"; then
            kill "$(cat "$pidfile")" 2>/dev/null || true
        fi
        rm -f "$pidfile"
    done
    log "stopped"
}

main() {
    local command="${1:-}"
    shift || true
    case "$command" in
        start)      cmd_start ;;
        run)        cmd_run "$@" ;;
        winxterm)   cmd_winxterm "$@" ;;
        screenshot) cmd_screenshot "$@" ;;
        status)     cmd_status ;;
        stop)       cmd_stop ;;
        ""|-h|--help|help) usage ;;
        *)          die "unknown command: ${command} (try: $0 --help)" ;;
    esac
}

main "$@"
