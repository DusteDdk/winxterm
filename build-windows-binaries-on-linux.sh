#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

XWIN_VERSION="${XWIN_VERSION:-0.9.0}"
XWIN_SHA256="${XWIN_SHA256:-31e1033f30608ba6b821d17f1461042bd54c23424813c9b4e9ae15b6d32fa4cd}"
XWIN_SDK_VERSION="${XWIN_SDK_VERSION:-10.0.26100}"
XWIN_CRT_VERSION="${XWIN_CRT_VERSION:-14.44.17.14}"
JOBS="${JOBS:-$(nproc)}"

abs_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "$SCRIPT_DIR" "$1" ;;
    esac
}

BUILD_DIR="$(abs_path "${BUILD_DIR:-build-msvc-clang-probe}")"
DIST_DIR="$(abs_path "${DIST_DIR:-dist-linux-msvc}")"
XWIN_DIR="$(abs_path "${XWIN_DIR:-build-xwin}")"
XWIN_SYSROOT="${XWIN_DIR}/splat"
XWIN_CACHE="${XWIN_DIR}/cache"
TOOLCHAIN_BIN="${XWIN_DIR}/toolchain-bin"

XWIN_ARCHIVE="xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz"
XWIN_URL="https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/${XWIN_ARCHIVE}"
XWIN_ARCHIVE_PATH="${XWIN_DIR}/${XWIN_ARCHIVE}"
XWIN_EXTRACTED_DIR="${XWIN_DIR}/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl"
XWIN_BIN="${XWIN_EXTRACTED_DIR}/xwin"

log() {
    printf '[build-windows-on-linux] %s\n' "$*"
}

die() {
    printf '[build-windows-on-linux] error: %s\n' "$*" >&2
    exit 1
}

print_wine_conpty_note() {
    cat <<'EOF'

Wine ConPTY note:
  This script builds the Windows executables; it does not use Wine to validate
  the hosted-shell path. During the investigation, Ubuntu Wine 9.0 accepted
  PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE but left the child process standard
  handles NULL. As a result, winxterm.exe --demo can run under Wine while
  winxterm.exe hosting dstshell.exe or cmd.exe can close immediately. Treat that
  as a Wine 9.0 ConPTY implementation gap unless testing on real Windows or a
  newer Wine build proves otherwise.

EOF
}

add_missing_command_package() {
    local command_name="$1"
    local package_name="$2"
    if ! command -v "$command_name" >/dev/null 2>&1; then
        MISSING_APT_PACKAGES+=("$package_name")
    fi
}

dedupe_packages() {
    local package
    local -A seen=()
    DEDUPED_APT_PACKAGES=()
    for package in "$@"; do
        [[ -n "$package" ]] || continue
        if [[ -z "${seen[$package]:-}" ]]; then
            seen[$package]=1
            DEDUPED_APT_PACKAGES+=("$package")
        fi
    done
}

install_missing_apt_packages() {
    MISSING_APT_PACKAGES=()
    add_missing_command_package cmake cmake
    add_missing_command_package ninja ninja-build
    add_missing_command_package wget wget
    add_missing_command_package sha256sum coreutils
    add_missing_command_package tar tar
    add_missing_command_package gzip gzip
    add_missing_command_package clang-19 clang-19
    add_missing_command_package lld-link-19 lld-19
    add_missing_command_package llvm-lib-19 llvm-19
    add_missing_command_package llvm-rc-19 llvm-19
    add_missing_command_package llvm-mt-19 llvm-19

    if [[ ! -s /etc/ssl/certs/ca-certificates.crt ]]; then
        MISSING_APT_PACKAGES+=(ca-certificates)
    fi

    dedupe_packages "${MISSING_APT_PACKAGES[@]}"

    if (( ${#DEDUPED_APT_PACKAGES[@]} == 0 )); then
        log "all required Ubuntu packages are already present"
        return
    fi

    log "installing missing Ubuntu packages: ${DEDUPED_APT_PACKAGES[*]}"
    sudo apt-get update
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "${DEDUPED_APT_PACKAGES[@]}"
}

require_commands() {
    local command_name
    for command_name in \
        cmake ninja wget sha256sum tar gzip \
        clang-19 lld-link-19 llvm-lib-19 llvm-rc-19 llvm-mt-19
    do
        command -v "$command_name" >/dev/null 2>&1 || die "required command not found after install: ${command_name}"
    done
}

download_xwin() {
    mkdir -p "$XWIN_DIR"

    if [[ ! -f "$XWIN_ARCHIVE_PATH" ]]; then
        log "downloading xwin ${XWIN_VERSION}"
        wget -O "${XWIN_ARCHIVE_PATH}.tmp" "$XWIN_URL"
        mv -f "${XWIN_ARCHIVE_PATH}.tmp" "$XWIN_ARCHIVE_PATH"
    else
        log "xwin archive already present"
    fi

    printf '%s  %s\n' "$XWIN_SHA256" "$XWIN_ARCHIVE_PATH" | sha256sum -c -

    if [[ ! -x "$XWIN_BIN" ]]; then
        log "extracting xwin"
        tar -xzf "$XWIN_ARCHIVE_PATH" -C "$XWIN_DIR"
    else
        log "xwin executable already present"
    fi

    [[ -x "$XWIN_BIN" ]] || die "xwin executable was not found at ${XWIN_BIN}"
}

sysroot_ready() {
    local required_file
    for required_file in \
        "$XWIN_SYSROOT/crt/include/vcruntime.h" \
        "$XWIN_SYSROOT/crt/lib/x86_64/vcruntime.lib" \
        "$XWIN_SYSROOT/crt/lib/x86_64/msvcrt.lib" \
        "$XWIN_SYSROOT/sdk/include/um/windows.h" \
        "$XWIN_SYSROOT/sdk/include/um/d2d1_3.h" \
        "$XWIN_SYSROOT/sdk/include/um/consoleapi.h" \
        "$XWIN_SYSROOT/sdk/lib/ucrt/x86_64/ucrt.lib" \
        "$XWIN_SYSROOT/sdk/lib/um/x86_64/kernel32.lib" \
        "$XWIN_SYSROOT/sdk/lib/um/x86_64/d2d1.lib" \
        "$XWIN_SYSROOT/sdk/lib/um/x86_64/dwrite.lib"
    do
        [[ -f "$required_file" ]] || return 1
    done
}

prepare_xwin_sysroot() {
    if sysroot_ready; then
        log "xwin Microsoft CRT/SDK sysroot already present"
        return
    fi

    log "creating Microsoft CRT/SDK sysroot with xwin"
    rm -rf "$XWIN_SYSROOT"
    "$XWIN_BIN" \
        --accept-license \
        --arch x86_64 \
        --sdk-version "$XWIN_SDK_VERSION" \
        --crt-version "$XWIN_CRT_VERSION" \
        --cache-dir "$XWIN_CACHE" \
        splat \
        --output "$XWIN_SYSROOT"

    sysroot_ready || die "xwin completed but required CRT/SDK files are still missing"
}

prepare_toolchain_bin() {
    mkdir -p "$TOOLCHAIN_BIN"
    ln -sfn "$(command -v clang-19)" "$TOOLCHAIN_BIN/clang-cl"
    ln -sfn "$(command -v lld-link-19)" "$TOOLCHAIN_BIN/lld-link"
    ln -sfn "$(command -v llvm-lib-19)" "$TOOLCHAIN_BIN/llvm-lib"
    ln -sfn "$(command -v llvm-mt-19)" "$TOOLCHAIN_BIN/llvm-mt"
    ln -sfn "$(command -v llvm-rc-19)" "$TOOLCHAIN_BIN/llvm-rc"
}

configure_and_build() {
    local imsvc_flags
    local libpath_flags
    local common_libraries

    imsvc_flags="/imsvc${XWIN_SYSROOT}/crt/include"
    imsvc_flags+=" /imsvc${XWIN_SYSROOT}/sdk/include/ucrt"
    imsvc_flags+=" /imsvc${XWIN_SYSROOT}/sdk/include/shared"
    imsvc_flags+=" /imsvc${XWIN_SYSROOT}/sdk/include/um"
    imsvc_flags+=" /imsvc${XWIN_SYSROOT}/sdk/include/winrt"
    imsvc_flags+=" /D_CRT_SECURE_NO_WARNINGS"

    libpath_flags="/libpath:${XWIN_SYSROOT}/crt/lib/x86_64"
    libpath_flags+=" /libpath:${XWIN_SYSROOT}/sdk/lib/ucrt/x86_64"
    libpath_flags+=" /libpath:${XWIN_SYSROOT}/sdk/lib/um/x86_64"

    common_libraries="kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib"

    log "configuring CMake build in ${BUILD_DIR}"
    cmake \
        -S "$SCRIPT_DIR" \
        -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TRY_COMPILE_CONFIGURATION=Release \
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL \
        -DWINXTERM_BUILD_DSTSHELL_MODE_MUTATOR=OFF \
        -DCMAKE_C_COMPILER="$TOOLCHAIN_BIN/clang-cl" \
        -DCMAKE_CXX_COMPILER="$TOOLCHAIN_BIN/clang-cl" \
        -DCMAKE_RC_COMPILER="$TOOLCHAIN_BIN/llvm-rc" \
        -DCMAKE_LINKER="$TOOLCHAIN_BIN/lld-link" \
        -DCMAKE_AR="$TOOLCHAIN_BIN/llvm-lib" \
        -DCMAKE_MT="$TOOLCHAIN_BIN/llvm-mt" \
        "-DCMAKE_C_FLAGS=${imsvc_flags}" \
        "-DCMAKE_CXX_FLAGS=${imsvc_flags}" \
        "-DCMAKE_RC_FLAGS=-DWIN32" \
        "-DCMAKE_EXE_LINKER_FLAGS=${libpath_flags}" \
        "-DCMAKE_SHARED_LINKER_FLAGS=/machine:x64" \
        "-DCMAKE_MODULE_LINKER_FLAGS=/machine:x64" \
        "-DCMAKE_STATIC_LINKER_FLAGS=/machine:x64" \
        "-DCMAKE_C_STANDARD_LIBRARIES=${common_libraries}" \
        "-DCMAKE_CXX_STANDARD_LIBRARIES=${common_libraries}"

    log "building winxterm.exe and dstshell.exe"
    cmake --build "$BUILD_DIR" --target winxterm dstshell --parallel "$JOBS"

    [[ -f "$BUILD_DIR/winxterm.exe" ]] || die "missing build output: ${BUILD_DIR}/winxterm.exe"
    [[ -f "$BUILD_DIR/dstshell.exe" ]] || die "missing build output: ${BUILD_DIR}/dstshell.exe"

    mkdir -p "$DIST_DIR"
    cp -f "$BUILD_DIR/winxterm.exe" "$DIST_DIR/winxterm.exe"
    cp -f "$BUILD_DIR/dstshell.exe" "$DIST_DIR/dstshell.exe"
}

write_linux_tools_log() {
    local log_file="${SCRIPT_DIR}/LinuxTools.md"
    local tmp_file="${log_file}.tmp.$$"
    local completed_at

    completed_at="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"

    if [[ -f "$log_file" ]]; then
        awk '
            /<!-- BEGIN build-windows-on-linux.sh generated tool log -->/ { skip = 1; next }
            /<!-- END build-windows-on-linux.sh generated tool log -->/ { skip = 0; next }
            !skip { print }
        ' "$log_file" > "$tmp_file"
    else
        {
            printf '# Linux Tools Investigation Log\n\n'
            printf 'This file tracks third-party software installed, downloaded, or built during Linux-hosted Windows build work.\n'
        } > "$tmp_file"
    fi

    cat >> "$tmp_file" <<EOF

<!-- BEGIN build-windows-on-linux.sh generated tool log -->
## build-windows-on-linux.sh generated tool log

- Run completed: ${completed_at}
- Output directory: \`${DIST_DIR}\`
- Build directory: \`${BUILD_DIR}\`

### cmake

- Name: \`cmake\`
- Why: Configures the Windows cross-build with the Ninja generator.
- How acquired: Checked locally first; if missing, installed from Ubuntu apt with \`sudo apt-get install cmake\`.
- Usefulness: Useful. It generated the Ninja build files for the MSVC-ABI cross-build.

### ninja-build

- Name: \`ninja-build\`
- Why: Provides the Ninja build executor used by CMake.
- How acquired: Checked locally first; if missing, installed from Ubuntu apt with \`sudo apt-get install ninja-build\`.
- Usefulness: Useful. It built \`winxterm.exe\` and \`dstshell.exe\`.

### wget and ca-certificates

- Name: \`wget\` and \`ca-certificates\`
- Why: Download the pinned \`xwin\` release over HTTPS.
- How acquired: Checked locally first; if missing, installed from Ubuntu apt.
- Usefulness: Useful. They allow the script to retrieve the \`xwin\` archive.

### clang-19

- Name: \`clang-19\`
- Why: Provides a Linux-hosted \`clang-cl\` compatible compiler targeting \`x86_64-pc-windows-msvc\`.
- How acquired: Checked locally first; if missing, installed from Ubuntu apt with \`sudo apt-get install clang-19\`.
- Usefulness: Useful. It compiles the existing MSVC-oriented source without modifying the implementation.

### lld-19 and llvm-19

- Name: \`lld-19\` and \`llvm-19\`
- Why: Provide \`lld-link\`, \`llvm-lib\`, \`llvm-rc\`, and \`llvm-mt\` for MSVC-style linking, static libraries, resources, and manifests.
- How acquired: Checked locally first; if missing, installed from Ubuntu apt.
- Usefulness: Useful. They provide the Windows/MSVC ABI build tools used by CMake.

### xwin

- Name: \`xwin ${XWIN_VERSION}\`
- Why: Downloads and assembles Microsoft CRT and Windows SDK files into a Linux-hosted sysroot.
- How acquired: Downloaded from \`${XWIN_URL}\` and verified with SHA256 \`${XWIN_SHA256}\`.
- Usefulness: Useful. It creates \`${XWIN_SYSROOT}\`, which supplies the official Microsoft headers and import libraries needed by this build.

### Microsoft CRT and Windows SDK via xwin

- Name: Microsoft Visual C++ CRT \`${XWIN_CRT_VERSION}\` and Windows SDK \`${XWIN_SDK_VERSION}\`
- Why: Provide MSVC CRT headers/libraries plus Windows SDK headers/import libraries, including the Direct2D/DirectWrite and ConPTY declarations required by \`winxterm\`.
- How acquired: Downloaded and assembled by \`xwin --accept-license --arch x86_64 --sdk-version ${XWIN_SDK_VERSION} --crt-version ${XWIN_CRT_VERSION} splat\`.
- Usefulness: Useful. This sysroot is what makes the feasible Linux-hosted Windows build work.

### Wine ConPTY runtime note

- Name: Wine ConPTY behavior note
- Why: Runtime testing under Ubuntu Wine 9.0 exposed a ConPTY bug separate from the build.
- How acquired: Source/runtime investigation, not installed by this build script.
- Usefulness: Useful context. Wine 9.0 can leave pseudoconsole child standard handles NULL, so \`winxterm.exe --demo\` may work under Wine while hosting \`dstshell.exe\` or \`cmd.exe\` fails. This is not evidence that the produced executables are invalid.
<!-- END build-windows-on-linux.sh generated tool log -->
EOF

    mv -f "$tmp_file" "$log_file"
}

main() {
    print_wine_conpty_note

    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        if [[ "${ID:-}" != "ubuntu" && "${ID_LIKE:-}" != *"ubuntu"* ]]; then
            log "warning: this script assumes Ubuntu; detected ID=${ID:-unknown}"
        fi
    fi

    install_missing_apt_packages
    require_commands
    download_xwin
    prepare_xwin_sysroot
    prepare_toolchain_bin
    configure_and_build
    write_linux_tools_log

    log "built Windows executables:"
    ls -lh "$DIST_DIR/winxterm.exe" "$DIST_DIR/dstshell.exe"
    print_wine_conpty_note
}

main "$@"
