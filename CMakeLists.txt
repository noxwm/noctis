cmake_minimum_required(VERSION 3.16)
project(noctis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS    REQUIRED wlroots)
pkg_check_modules(WAYLAND    REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON  REQUIRED xkbcommon)
pkg_check_modules(PIXMAN     REQUIRED pixman-1)
pkg_check_modules(TOMLPP     REQUIRED tomlplusplus)

# ── Patch wlroots headers (C99 [static N] not valid in C++) ─────────────────
set(PATCHED_HEADER_DIR "${CMAKE_BINARY_DIR}/wlr_patched")
file(MAKE_DIRECTORY "${PATCHED_HEADER_DIR}/wlr/types")
file(MAKE_DIRECTORY "${PATCHED_HEADER_DIR}/wlr/render")

foreach(HDR
    "types/wlr_scene.h"
    "types/wlr_matrix.h"
    "render/wlr_renderer.h"
    "render/interface.h")
  set(SRC "/usr/include/wlr/${HDR}")
  set(DST "${PATCHED_HEADER_DIR}/wlr/${HDR}")
  if(NOT EXISTS "${DST}")
    execute_process(
      COMMAND bash -c "sed 's/\\[static [0-9]*\\]/[]/g' ${SRC} > ${DST}"
      RESULT_VARIABLE PATCH_RESULT)
    if(NOT PATCH_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to patch ${SRC}")
    endif()
    message(STATUS "Patched: wlr/${HDR}")
  endif()
endforeach()

# ── Generate xdg-shell protocol header ──────────────────────────────────────
set(XDG_SHELL_XML "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_H   "${CMAKE_SOURCE_DIR}/protocols/xdg-shell-protocol.h")
set(XDG_SHELL_C   "${CMAKE_SOURCE_DIR}/protocols/xdg-shell-protocol.c")
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
if(NOT EXISTS "${XDG_SHELL_H}")
  execute_process(COMMAND ${WAYLAND_SCANNER} server-header ${XDG_SHELL_XML} ${XDG_SHELL_H})
endif()
if(NOT EXISTS "${XDG_SHELL_C}")
  execute_process(COMMAND ${WAYLAND_SCANNER} private-code  ${XDG_SHELL_XML} ${XDG_SHELL_C})
endif()

# ── Sources ──────────────────────────────────────────────────────────────────
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

target_include_directories(noctis PRIVATE
    ${PATCHED_HEADER_DIR}
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
