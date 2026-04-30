cmake_minimum_required(VERSION 3.16)
project(noctis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# Try wlroots-0.18 first (Arch), fall back to wlroots (Ubuntu/Debian)
pkg_check_modules(WLROOTS wlroots-0.18)
if(NOT WLROOTS_FOUND)
    pkg_check_modules(WLROOTS REQUIRED wlroots)
    message(STATUS "Using wlroots (system default)")
else()
    message(STATUS "Using wlroots-0.18 (Arch)")
endif()

pkg_check_modules(WAYLAND   REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED pixman-1)
pkg_check_modules(TOMLPP    REQUIRED tomlplusplus)

# ── Locate wlroots headers ────────────────────────────────────────────────────
if(EXISTS "/usr/include/wlroots-0.18/wlr")
    set(WLR_INCLUDE_BASE "/usr/include/wlroots-0.18")
elseif(EXISTS "/usr/include/wlr")
    set(WLR_INCLUDE_BASE "/usr/include")
else()
    list(GET WLROOTS_INCLUDE_DIRS 0 WLR_INCLUDE_BASE)
endif()
message(STATUS "wlroots headers: ${WLR_INCLUDE_BASE}")

# ── Patch wlroots headers ─────────────────────────────────────────────────────
# wlroots uses C99 [static N] array qualifiers which are not valid C++.
# We create patched local copies and put them first on the include path.
set(PATCHED_HEADER_DIR "${CMAKE_BINARY_DIR}/wlr_patched")
file(MAKE_DIRECTORY "${PATCHED_HEADER_DIR}/wlr/types")
file(MAKE_DIRECTORY "${PATCHED_HEADER_DIR}/wlr/render")

foreach(HDR
        "types/wlr_scene.h"
        "types/wlr_matrix.h"
        "render/wlr_renderer.h"
        "render/interface.h")

    set(SRC "${WLR_INCLUDE_BASE}/wlr/${HDR}")
    set(DST "${PATCHED_HEADER_DIR}/wlr/${HDR}")

    if(EXISTS "${SRC}")
        execute_process(
            COMMAND sed "s/\\[static [0-9][0-9]*\\]/[]/g" "${SRC}"
            OUTPUT_FILE "${DST}"
            RESULT_VARIABLE PATCH_RESULT
        )
        if(NOT PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to patch ${SRC}")
        endif()
        message(STATUS "Patched: wlr/${HDR}")
    else()
        message(WARNING "Header not found, skipping: ${SRC}")
    endif()
endforeach()

# ── xdg-shell protocol ────────────────────────────────────────────────────────
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

execute_process(
    COMMAND pkg-config --variable=pkgdatadir wayland-protocols
    OUTPUT_VARIABLE WAYLAND_PROTOCOLS_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT WAYLAND_PROTOCOLS_DIR)
    set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols")
endif()

set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_H   "${CMAKE_SOURCE_DIR}/protocols/xdg-shell-protocol.h")
set(XDG_SHELL_C   "${CMAKE_SOURCE_DIR}/protocols/xdg-shell-protocol.c")

if(NOT EXISTS "${XDG_SHELL_H}")
    execute_process(COMMAND ${WAYLAND_SCANNER} server-header ${XDG_SHELL_XML} ${XDG_SHELL_H})
    message(STATUS "Generated: xdg-shell-protocol.h")
endif()
if(NOT EXISTS "${XDG_SHELL_C}")
    execute_process(COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_C})
    message(STATUS "Generated: xdg-shell-protocol.c")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(noctis
    src/main.cpp
    src/server.cpp
    src/output.cpp
    src/input.cpp
    src/seat.cpp
    src/view.cpp
    src/layout.cpp
    src/keybinds.cpp
    src/config.cpp
    src/util.cpp
    protocols/xdg-shell-protocol.c
)

# Patched headers must come BEFORE system wlr headers
target_include_directories(noctis PRIVATE
    ${PATCHED_HEADER_DIR}
    ${WLR_INCLUDE_BASE}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/protocols
    ${TOMLPP_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${WLROOTS_INCLUDE_DIRS}
)

target_compile_definitions(noctis PRIVATE WLR_USE_UNSTABLE)

target_link_libraries(noctis
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${PIXMAN_LIBRARIES}
)
