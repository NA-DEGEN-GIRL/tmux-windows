cmake_minimum_required(VERSION 3.16)
project(tmux C)

include(CTest)

# The minimum Windows version (10 1809, for ConPTY) is set by _WIN32_WINNT and
# NTDDI_VERSION in win32/win32-platform.h; win32/tmux.manifest makes UTF-8 the
# process code page on Windows 10 1903 and later.

# Use MSVC runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Options
option(TMUX_ENABLE_UTF8PROC "Enable utf8proc support" OFF)
option(TMUX_ENABLE_SIXEL "Enable sixel image support" OFF)
option(TMUX_WIN32_MANIFEST
    "Embed win32/tmux.manifest (UTF-8 code page, long paths)" ON)
option(TMUX_ALLOW_STALE_CMD_PARSE
    "Build with a cmd-parse.c that does not match cmd-parse.y" OFF)

# Version: the upstream version from configure.ac with a Windows suffix,
# unless TMUX_VERSION is given (the release workflow passes the tag).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/configure.ac" TMUX_AC_INIT
    REGEX "^AC_INIT\\(")
if(NOT TMUX_AC_INIT MATCHES "^AC_INIT\\(\\[tmux\\], *\\[?([^],)]+)")
    message(FATAL_ERROR "cannot find the tmux version in configure.ac")
endif()
set(TMUX_VERSION_SUFFIX "-win32" CACHE STRING
    "Suffix appended to the configure.ac version")
set(TMUX_VERSION "" CACHE STRING
    "Version reported by tmux -V (empty: configure.ac version and suffix)")
if(TMUX_VERSION)
    set(TMUX_PACKAGE_VERSION "${TMUX_VERSION}")
else()
    set(TMUX_PACKAGE_VERSION "${CMAKE_MATCH_1}${TMUX_VERSION_SUFFIX}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/configure.ac")

# Sources are UTF-8: do not read them in the system code page.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Find libevent
find_package(libevent CONFIG QUIET)
if(NOT libevent_FOUND)
    find_package(Libevent CONFIG QUIET)
endif()
if(NOT libevent_FOUND)
    find_package(unofficial-libevent CONFIG QUIET)
endif()
if(NOT libevent_FOUND AND NOT Libevent_FOUND AND NOT unofficial-libevent_FOUND)
    # Try pkg-config as fallback
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBEVENT REQUIRED libevent)
    else()
        # Manual search
        find_path(LIBEVENT_INCLUDE_DIRS event2/event.h)
        find_library(LIBEVENT_LIBRARIES NAMES event libevent)
        if(NOT LIBEVENT_INCLUDE_DIRS OR NOT LIBEVENT_LIBRARIES)
            message(FATAL_ERROR "libevent not found. Install via vcpkg: vcpkg install libevent")
        endif()
    endif()
endif()

# cmd-parse.c is generated from cmd-parse.y by bison. With bison (or
# win_bison from WinFlexBison) on PATH it is generated into the build
# directory; otherwise the committed copy is used, whose first line records
# the SHA256 of the grammar it was generated from. Refresh the committed copy
# after changing cmd-parse.y with:
#   cmake --build <build dir> --target update-cmd-parse
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.y" TMUX_CMD_PARSE_Y)
string(REPLACE "\r\n" "\n" TMUX_CMD_PARSE_Y "${TMUX_CMD_PARSE_Y}")
string(SHA256 TMUX_CMD_PARSE_Y_SHA256 "${TMUX_CMD_PARSE_Y}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.y"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.c")
find_program(BISON_EXECUTABLE NAMES win_bison bison)
if(BISON_EXECUTABLE)
    set(TMUX_CMD_PARSE_C "${CMAKE_CURRENT_BINARY_DIR}/cmd-parse.c")
    add_custom_command(
        OUTPUT "${TMUX_CMD_PARSE_C}"
        COMMAND "${CMAKE_COMMAND}"
            "-DBISON_EXECUTABLE=${BISON_EXECUTABLE}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.y"
            "-DOUTPUT=${TMUX_CMD_PARSE_C}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/win32/cmd-parse.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.y"
            "${CMAKE_CURRENT_SOURCE_DIR}/win32/cmd-parse.cmake"
        COMMENT "Generating cmd-parse.c from cmd-parse.y"
    )
    add_custom_target(update-cmd-parse
        COMMAND "${CMAKE_COMMAND}"
            "-DBISON_EXECUTABLE=${BISON_EXECUTABLE}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.y"
            "-DOUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.c"
            -DNO_LINES=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/win32/cmd-parse.cmake"
        COMMENT "Regenerating the committed cmd-parse.c"
    )
    message(STATUS "Generating cmd-parse.c with ${BISON_EXECUTABLE}")
else()
    set(TMUX_CMD_PARSE_C "${CMAKE_CURRENT_SOURCE_DIR}/cmd-parse.c")
    file(STRINGS "${TMUX_CMD_PARSE_C}" TMUX_CMD_PARSE_C_HEADER
        LIMIT_COUNT 1)
    if(NOT TMUX_CMD_PARSE_C_HEADER MATCHES
        "cmd-parse.y SHA256: ${TMUX_CMD_PARSE_Y_SHA256}")
        set(TMUX_CMD_PARSE_STALE
            "cmd-parse.c was not generated from the current cmd-parse.y "
            "and bison/win_bison is not on PATH. Install WinFlexBison and "
            "run 'cmake --build <build dir> --target update-cmd-parse', or "
            "configure with -DTMUX_ALLOW_STALE_CMD_PARSE=ON.")
        if(TMUX_ALLOW_STALE_CMD_PARSE)
            message(WARNING ${TMUX_CMD_PARSE_STALE})
        else()
            message(FATAL_ERROR ${TMUX_CMD_PARSE_STALE})
        endif()
    endif()
    message(STATUS "Using pre-generated cmd-parse.c")
endif()

# Core tmux source files
set(TMUX_SOURCES
    alerts.c
    arguments.c
    attributes.c
    cfg.c
    client.c
    cmd.c
    cmd-attach-session.c
    cmd-bind-key.c
    cmd-break-pane.c
    cmd-capture-pane.c
    cmd-choose-tree.c
    cmd-command-prompt.c
    cmd-confirm-before.c
    cmd-copy-mode.c
    cmd-detach-client.c
    cmd-display-menu.c
    cmd-display-message.c
    cmd-display-panes.c
    cmd-find.c
    cmd-find-window.c
    cmd-if-shell.c
    cmd-join-pane.c
    cmd-kill-pane.c
    cmd-kill-server.c
    cmd-kill-session.c
    cmd-kill-window.c
    cmd-list-buffers.c
    cmd-list-clients.c
    cmd-list-commands.c
    cmd-list-keys.c
    cmd-list-panes.c
    cmd-list-sessions.c
    cmd-list-windows.c
    cmd-load-buffer.c
    cmd-lock-server.c
    cmd-move-window.c
    cmd-new-session.c
    cmd-new-window.c
    ${TMUX_CMD_PARSE_C}
    cmd-paste-buffer.c
    cmd-pipe-pane.c
    cmd-queue.c
    cmd-refresh-client.c
    cmd-rename-session.c
    cmd-rename-window.c
    cmd-resize-pane.c
    cmd-resize-window.c
    cmd-respawn-pane.c
    cmd-respawn-window.c
    cmd-rotate-window.c
    cmd-run-shell.c
    cmd-save-buffer.c
    cmd-select-layout.c
    cmd-select-pane.c
    cmd-select-window.c
    cmd-send-keys.c
    cmd-server-access.c
    cmd-set-buffer.c
    cmd-set-environment.c
    cmd-set-option.c
    cmd-show-environment.c
    cmd-show-messages.c
    cmd-show-options.c
    cmd-show-prompt-history.c
    cmd-source-file.c
    cmd-split-window.c
    cmd-swap-pane.c
    cmd-swap-window.c
    cmd-switch-client.c
    cmd-unbind-key.c
    cmd-wait-for.c
    colour.c
    control.c
    control-notify.c
    environ.c
    file.c
    format.c
    format-draw.c
    fuzzy.c
    grid.c
    grid-reader.c
    grid-view.c
    hyperlinks.c
    input.c
    input-keys.c
    job.c
    key-bindings.c
    key-string.c
    layout.c
    layout-custom.c
    layout-set.c
    log.c
    menu.c
    mode-tree.c
    names.c
    notify.c
    options.c
    options-table.c
    paste.c
    popup.c
    proc.c
    prompt.c
    prompt-history.c
    regsub.c
    resize.c
    screen.c
    screen-redraw.c
    screen-write.c
    server.c
    server-acl.c
    server-client.c
    server-fn.c
    session.c
    sort.c
    spawn.c
    status.c
    style.c
    tmux.c
    tty.c
    tty-acs.c
    tty-draw.c
    tty-features.c
    tty-keys.c
    tty-term.c
    utf8.c
    utf8-combined.c
    window.c
    window-border.c
    window-buffer.c
    window-client.c
    window-clock.c
    window-copy.c
    window-customize.c
    window-switch.c
    window-tree.c
    window-visible.c
    xmalloc.c
)

# Windows-specific osdep
set(TMUX_OSDEP_SOURCES
    osdep-windows.c
)

# compat library sources (subset needed for Windows)
# Note: closefrom, daemon, setenv, getprogname, getpeereid, clock_gettime
# are provided by win32/win32-compat.c; forkpty/fdforkpty are replaced by ConPTY
set(TMUX_COMPAT_SOURCES
    compat/imsg.c
    compat/imsg-buffer.c
    compat/vis.c
    compat/unvis.c
    compat/strlcpy.c
    compat/strlcat.c
    compat/strtonum.c
    compat/asprintf.c
    compat/reallocarray.c
    compat/recallocarray.c
    compat/explicit_bzero.c
    compat/freezero.c
    compat/getdtablecount.c
    compat/getdtablesize.c
    # htonll.c/ntohll.c not needed - winsock2.h provides these
    compat/base64.c
    compat/strndup.c
    # strnlen: MSVC CRT provides strnlen; we define HAVE_STRNLEN in platform.h
    compat/memmem.c
    compat/getline.c
    compat/fgetln.c
    compat/strcasestr.c
    compat/getopt_long.c
    compat/err.c
)

# win32 layer sources
set(TMUX_WIN32_SOURCES
    win32/win32-compat.c
    win32/win32-ipc.c
    win32/win32-process.c
    win32/win32-pty.c
    win32/win32-signal.c
    win32/win32-spawn.c
    win32/win32-terminfo.c
    win32/win32-tty.c
    win32/win32-regex.c
    win32/win32-unicode.c
    win32/tre-regcomp.c
    win32/tre-regexec.c
    win32/tre-mem.c
)

# Optional: utf8proc
if(TMUX_ENABLE_UTF8PROC)
    find_package(utf8proc REQUIRED)
    list(APPEND TMUX_COMPAT_SOURCES compat/utf8proc.c)
endif()

if(TMUX_ENABLE_SIXEL)
    list(APPEND TMUX_SOURCES image.c image-sixel.c)
endif()

# Localized MSVC /showIncludes output is not always parsed correctly by Ninja.
# Keep project headers as explicit dependencies so a header change can never
# leave objects built against an older key or ABI definition.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    file(GLOB TMUX_ROOT_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    )
    file(GLOB TMUX_COMPAT_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/*.h"
    )
    file(GLOB TMUX_WIN32_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/win32/*.h"
    )
    set(TMUX_PROJECT_HEADERS
        ${TMUX_ROOT_HEADERS}
        ${TMUX_COMPAT_HEADERS}
        ${TMUX_WIN32_HEADERS}
    )
    set_property(SOURCE
        ${TMUX_SOURCES}
        ${TMUX_OSDEP_SOURCES}
        ${TMUX_COMPAT_SOURCES}
        ${TMUX_WIN32_SOURCES}
        APPEND PROPERTY OBJECT_DEPENDS ${TMUX_PROJECT_HEADERS}
    )
endif()

# Build tmux executable
add_executable(tmux
    ${TMUX_SOURCES}
    ${TMUX_OSDEP_SOURCES}
    ${TMUX_COMPAT_SOURCES}
    ${TMUX_WIN32_SOURCES}
)

# Application manifest: UTF-8 process code page and long path awareness.
if(TMUX_WIN32_MANIFEST)
    target_sources(tmux PRIVATE win32/tmux.manifest)
endif()

# Compiler definitions
target_compile_definitions(tmux PRIVATE
    _WIN32
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    PACKAGE_NAME="tmux"
    PACKAGE_VERSION="${TMUX_PACKAGE_VERSION}"
    # Functions provided by compat/ source files we compile:
    # Do NOT define HAVE_X for these - compat.h needs to declare them
    # and the compat .c files provide implementations.
    # The HAVE_IMSG flag tells tmux to use the compat imsg.
    HAVE_IMSG
    HAVE_EVENT2_EVENT_H
    HAVE_STDINT_H
)

# Include directories
target_include_directories(tmux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/win32
)

# libevent include/link
if(TARGET libevent::core)
    target_link_libraries(tmux PRIVATE libevent::core libevent::extra)
elseif(TARGET unofficial::libevent::core)
    target_link_libraries(tmux PRIVATE
        unofficial::libevent::core
        unofficial::libevent::extra
    )
elseif(LIBEVENT_INCLUDE_DIRS)
    target_include_directories(tmux PRIVATE ${LIBEVENT_INCLUDE_DIRS})
    target_link_libraries(tmux PRIVATE ${LIBEVENT_LIBRARIES})
endif()

# Windows system libraries
target_link_libraries(tmux PRIVATE
    ws2_32
    advapi32
    kernel32
    user32
    shell32
    ole32
    ntdll
)

# utf8proc
if(TMUX_ENABLE_UTF8PROC AND utf8proc_FOUND)
    target_link_libraries(tmux PRIVATE utf8proc)
    target_compile_definitions(tmux PRIVATE HAVE_UTF8PROC)
endif()

if(TMUX_ENABLE_SIXEL)
    target_compile_definitions(tmux PRIVATE ENABLE_SIXEL)
endif()

# MSVC-specific settings
if(MSVC)
    target_compile_options(tmux PRIVATE
        /W3
        /wd4005  # macro redefinition
        /wd4018  # signed/unsigned mismatch
        /wd4028  # parameter differs
        /wd4047  # indirection levels differ
        /wd4090  # const qualifier
        /wd4133  # incompatible types
        /wd4146  # unary minus on unsigned
        /wd4244  # conversion, possible loss of data
        /wd4267  # size_t to int
        /wd4456  # local declaration hides
        /wd4457  # parameter hides
        /wd4701  # potentially uninitialized
        /wd4702  # unreachable code
        /wd4996  # deprecated POSIX names
    )
    # Use C11
    set_property(TARGET tmux PROPERTY C_STANDARD 11)
endif()

if(BUILD_TESTING)
    add_executable(win32-tparm-test
        regress/win32-tparm.c
        win32/win32-compat.c
    )
    target_include_directories(win32-tparm-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/win32
    )
    target_compile_definitions(win32-tparm-test PRIVATE
        _WIN32
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
    )
    target_link_libraries(win32-tparm-test PRIVATE
        ws2_32
        advapi32
        kernel32
    )
    set_property(TARGET win32-tparm-test PROPERTY C_STANDARD 11)
    add_test(NAME win32-tparm COMMAND win32-tparm-test)
    set_tests_properties(win32-tparm PROPERTIES LABELS unit)

    # The regress/win32-*.sh scripts need Git for Windows' bash; never pick
    # up the WSL launcher in System32. Each script gets its own server label
    # so they can run in parallel with each other and with real sessions.
    find_program(TMUX_TEST_BASH bash
        HINTS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramW6432}/Git/bin"
        NO_DEFAULT_PATH)
    if(NOT TMUX_TEST_BASH)
        find_program(TMUX_TEST_BASH bash)
    endif()
    if(TMUX_TEST_BASH AND NOT TMUX_TEST_BASH MATCHES "[Ss]ystem32")
        file(GLOB TMUX_REGRESS_SCRIPTS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/regress/win32-*.sh")
        list(FILTER TMUX_REGRESS_SCRIPTS EXCLUDE REGEX "win32-run-all[.]sh$")
        foreach(script ${TMUX_REGRESS_SCRIPTS})
            get_filename_component(name "${script}" NAME_WE)
            string(REGEX REPLACE "^win32-" "" label "${name}")
            add_test(NAME ${name} COMMAND "${TMUX_TEST_BASH}" "${script}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
            set_tests_properties(${name} PROPERTIES
                LABELS regress
                TIMEOUT 900
                SKIP_RETURN_CODE 77
                ENVIRONMENT
                "TEST_TMUX=$<TARGET_FILE:tmux>;TMUX_TEST_LABEL=ctest-${label}")
        endforeach()
    else()
        message(STATUS "Git bash not found: regress scripts not added to ctest")
    endif()

    # kill() and waitpid() of the Windows child registry.
    add_executable(win32-kill-test
        regress/win32-kill.c
        win32/win32-process.c
        win32/win32-compat.c
        win32/win32-unicode.c
        xmalloc.c
        compat/asprintf.c
        compat/reallocarray.c
        compat/recallocarray.c
        compat/strndup.c
        compat/explicit_bzero.c
    )
    target_include_directories(win32-kill-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/win32
    )
    get_target_property(TMUX_TEST_DEFINITIONS tmux COMPILE_DEFINITIONS)
    target_compile_definitions(win32-kill-test PRIVATE
        ${TMUX_TEST_DEFINITIONS})
    get_target_property(TMUX_TEST_OPTIONS tmux COMPILE_OPTIONS)
    if(TMUX_TEST_OPTIONS)
        target_compile_options(win32-kill-test PRIVATE ${TMUX_TEST_OPTIONS})
    endif()
    get_target_property(TMUX_TEST_LIBRARIES tmux LINK_LIBRARIES)
    target_link_libraries(win32-kill-test PRIVATE ${TMUX_TEST_LIBRARIES})
    set_property(TARGET win32-kill-test PROPERTY C_STANDARD 11)
    add_test(NAME win32-kill COMMAND win32-kill-test)
    set_tests_properties(win32-kill PROPERTIES LABELS unit)
endif()

# Unit tests for the Windows text support: terminfo expansion, strftime,
# fnmatch, regex, UTF-8 and character widths.
if(BUILD_TESTING)
    set(TMUX_WIN32_TEXT_SOURCES
        win32/win32-unicode.c
        win32/win32-regex.c
        win32/tre-regcomp.c
        win32/tre-regexec.c
        win32/tre-mem.c
    )
    target_sources(win32-tparm-test PRIVATE ${TMUX_WIN32_TEXT_SOURCES})

    add_executable(win32-text-unit-test
        regress/win32-text-unit.c
        win32/win32-compat.c
        ${TMUX_WIN32_TEXT_SOURCES}
    )
    target_include_directories(win32-text-unit-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/win32
    )
    target_compile_definitions(win32-text-unit-test PRIVATE
        _WIN32
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
    )
    target_link_libraries(win32-text-unit-test PRIVATE
        ws2_32
        advapi32
        kernel32
    )
    set_property(TARGET win32-text-unit-test PROPERTY C_STANDARD 11)
    add_test(NAME win32-text-unit COMMAND win32-text-unit-test)
    set_tests_properties(win32-text-unit PROPERTIES LABELS unit)
endif()

# Install
install(TARGETS tmux RUNTIME DESTINATION bin)

# Print summary
message(STATUS "")
message(STATUS "tmux Windows build configuration:")
message(STATUS "  Version:    ${TMUX_PACKAGE_VERSION}")
message(STATUS "  UTF8PROC:   ${TMUX_ENABLE_UTF8PROC}")
message(STATUS "  SIXEL:      ${TMUX_ENABLE_SIXEL}")
message(STATUS "")
