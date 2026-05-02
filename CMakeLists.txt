cmake_minimum_required(VERSION 3.16)
project(noctis C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

# Try wlroots-0.18 (Arch) then wlroots (Ubuntu/Debian)
pkg_check_modules(WLROOTS wlroots-0.18)
if(NOT WLROOTS_FOUND)
    pkg_check_modules(WLROOTS REQUIRED wlroots)
    set(WLROOTS_MODULE_NAME "wlroots")
    message(STATUS "Using wlroots (system default)")
else()
    set(WLROOTS_MODULE_NAME "wlroots-0.18")
    message(STATUS "Using wlroots-0.18 (Arch)")
endif()

pkg_check_modules(WAYLAND   REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED pixman-1)

# Detect wlroots version for API compat
execute_process(
    COMMAND pkg-config --modversion ${WLROOTS_MODULE_NAME}
    OUTPUT_VARIABLE WLR_VERSION_STRING
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "." ";" WLR_VERSION_LIST "${WLR_VERSION_STRING}")
list(GET WLR_VERSION_LIST 1 WLR_MINOR)
message(STATUS "wlroots version: ${WLR_VERSION_STRING} (minor=${WLR_MINOR})")

# Locate wlroots headers
if(EXISTS "/usr/include/wlroots-0.18/wlr")
    set(WLR_INCLUDE_BASE "/usr/include/wlroots-0.18")
elseif(EXISTS "/usr/include/wlr")
    set(WLR_INCLUDE_BASE "/usr/include")
else()
    list(GET WLROOTS_INCLUDE_DIRS 0 WLR_INCLUDE_BASE)
endif()
message(STATUS "wlroots headers: ${WLR_INCLUDE_BASE}")

# xdg-shell protocol
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
endif()
if(NOT EXISTS "${XDG_SHELL_C}")
    execute_process(COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_C})
endif()

# Build
add_executable(noctis
    src/noctis.c
    protocols/xdg-shell-protocol.c
)

set(NOCTIS_COMPILE_DEFS WLR_USE_UNSTABLE)
if(WLR_MINOR GREATER_EQUAL 18)
    list(APPEND NOCTIS_COMPILE_DEFS WLR_018)
    message(STATUS "Enabling wlroots 0.18 API compat")
endif()

target_compile_definitions(noctis PRIVATE ${NOCTIS_COMPILE_DEFS})

target_include_directories(noctis PRIVATE
    ${WLR_INCLUDE_BASE}
    ${CMAKE_SOURCE_DIR}/protocols
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${WLROOTS_INCLUDE_DIRS}
)

target_link_libraries(noctis
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${PIXMAN_LIBRARIES}
)
