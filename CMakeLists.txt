cmake_minimum_required(VERSION 3.24)

project(winxterm VERSION 0.1.0 LANGUAGES C CXX RC)

option(WINXTERM_BUILD_DSTSHELL_MODE_MUTATOR "Build dstshell console-mode smoke-test helper" ON)

add_library(sqlite3 STATIC
    dependencies/sqlite3/sqlite3.c
)

add_executable(winxterm WIN32
    resources/winxterm.rc
    src/dstcmd/api/unicode.c
    src/main.c
    src/winxterm_app.c
    src/winxterm_bridge.c
    src/winxterm_client.c
    src/winxterm_clipboard.c
    src/winxterm_glyph_fallback.cpp
    src/winxterm_host.c
    src/winxterm_input.c
    src/winxterm_log.c
    src/winxterm_macro.c
    src/winxterm_modes.c
    src/winxterm_mouse.c
    src/winxterm_options.c
    src/winxterm_replies.c
    src/winxterm_render.c
    src/winxterm_screen.c
    src/winxterm_smoke.c
    src/winxterm_text.c
    src/winxterm_threads.c
    src/winxterm_ux.c
    src/winxterm_window_placement.c
)

add_executable(dstshell
    src/dstshell_main.c
    src/dstcmd/api/env.c
    src/dstcmd/api/path.c
    src/dstcmd/api/scratch.c
    src/dstcmd/api/unicode.c
    src/dstcmd/commands/alias.c
    src/dstcmd/commands/cat.c
    src/dstcmd/commands/cd.c
    src/dstcmd/commands/cp.c
    src/dstcmd/commands/echo.c
    src/dstcmd/commands/export.c
    src/dstcmd/commands/help.c
    src/dstcmd/commands/highlight.c
    src/dstcmd/commands/ls.c
    src/dstcmd/commands/mv.c
    src/dstcmd/commands/playmacro.c
    src/dstcmd/commands/pwd.c
    src/dstcmd/commands/rm.c
    src/dstcmd/commands/set.c
    src/dstcmd/commands/which.c
    src/dstcmd/dispatch.c
    src/dstcmd/winxterm_dstcmd.c
    src/dstcmd/winxterm_dstcmd_dircache.c
    src/dstcmd/winxterm_dstcmd_exec.c
    src/dstcmd/winxterm_dstcmd_host.c
    src/dstcmd/winxterm_dstcmd_jobs.c
    src/dstcmd/winxterm_dstcmd_parse.c
)

if(WINXTERM_BUILD_DSTSHELL_MODE_MUTATOR)
    add_executable(dstshell_mode_mutator
        tools/dstshell_mode_mutator.c
    )
    add_dependencies(dstshell dstshell_mode_mutator)
endif()

add_executable(pcf_to_header
    tools/pcf_to_header.c
)

target_include_directories(sqlite3 PUBLIC
    dependencies/sqlite3
)

target_include_directories(winxterm PRIVATE
    generated
    resources
    src
)

target_include_directories(dstshell PRIVATE
    generated
    resources
    src
)

set_property(TARGET sqlite3 PROPERTY C_STANDARD 17)
set_property(TARGET sqlite3 PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET sqlite3 PROPERTY C_EXTENSIONS OFF)

set_property(TARGET winxterm PROPERTY C_STANDARD 17)
set_property(TARGET winxterm PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET winxterm PROPERTY C_EXTENSIONS OFF)
set_property(TARGET winxterm PROPERTY CXX_STANDARD 17)
set_property(TARGET winxterm PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET winxterm PROPERTY CXX_EXTENSIONS OFF)

set_property(TARGET dstshell PROPERTY C_STANDARD 17)
set_property(TARGET dstshell PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET dstshell PROPERTY C_EXTENSIONS OFF)

if(WINXTERM_BUILD_DSTSHELL_MODE_MUTATOR)
    set_property(TARGET dstshell_mode_mutator PROPERTY C_STANDARD 17)
    set_property(TARGET dstshell_mode_mutator PROPERTY C_STANDARD_REQUIRED ON)
    set_property(TARGET dstshell_mode_mutator PROPERTY C_EXTENSIONS OFF)
endif()

set_property(TARGET pcf_to_header PROPERTY C_STANDARD 17)
set_property(TARGET pcf_to_header PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET pcf_to_header PROPERTY C_EXTENSIONS OFF)

target_compile_definitions(sqlite3
    PUBLIC
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_DEPRECATED
    PRIVATE
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_THREADSAFE=1
)

target_compile_definitions(winxterm PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_definitions(dstshell PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(WINXTERM_BUILD_DSTSHELL_MODE_MUTATOR)
    target_compile_definitions(dstshell_mode_mutator PRIVATE
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

target_link_libraries(winxterm PRIVATE
    sqlite3
    d2d1
    d3d11
    dwrite
    dxgi
    gdi32
    ole32
    shell32
    user32
    windowscodecs
    uuid
)

target_link_libraries(dstshell PRIVATE
    sqlite3
)

if(MSVC)
    target_compile_options(sqlite3 PRIVATE
        /utf-8
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
    )

    target_compile_definitions(sqlite3 PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )

    target_compile_options(winxterm PRIVATE
        /W4
        $<$<COMPILE_LANGUAGE:C>:/std:c17>
        $<$<COMPILE_LANGUAGE:CXX>:/std:c++17>
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        /utf-8
        /arch:AVX2
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/favor:INTEL64>
    )

    target_link_options(winxterm PRIVATE
        $<$<CONFIG:Release>:/LTCG>
    )

    target_compile_options(dstshell PRIVATE
        /W4
        /std:c17
        /utf-8
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/favor:INTEL64>
    )

    target_link_options(dstshell PRIVATE
        $<$<CONFIG:Release>:/LTCG>
    )

    if(WINXTERM_BUILD_DSTSHELL_MODE_MUTATOR)
        target_compile_options(dstshell_mode_mutator PRIVATE
            /W4
            /std:c17
            /utf-8
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Ot>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/favor:INTEL64>
        )

        target_link_options(dstshell_mode_mutator PRIVATE
            $<$<CONFIG:Release>:/LTCG>
        )
    endif()

    target_compile_options(pcf_to_header PRIVATE
        /W4
        /std:c17
        /utf-8
    )

    target_compile_definitions(pcf_to_header PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()
