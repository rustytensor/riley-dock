# SPDX-License-Identifier: GPL-3.0-or-later
#
# riley-dock-pointer: the capture gate's pointer helper (gates/qt_capture.py). It moves the pointer with the wlr
# virtual pointer protocol and draws a red ring at the scripted position. A test tool: built only with
# RILEY_DOCK_GATE_TOOLS=ON and never installed.

find_package(Qt6 6.11 REQUIRED COMPONENTS Gui)
find_package(LayerShellQt 6.8 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(_riley_wayland_scanner wayland-scanner wayland_scanner)
find_program(RILEY_WAYLAND_SCANNER NAMES wayland-scanner HINTS "${_riley_wayland_scanner}" REQUIRED)

set(_protocol "${PROJECT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml")
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")
add_custom_command(
    OUTPUT "${_generated}/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    COMMAND "${RILEY_WAYLAND_SCANNER}" client-header "${_protocol}"
            "${_generated}/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    DEPENDS "${_protocol}"
    VERBATIM)
add_custom_command(
    OUTPUT "${_generated}/wlr-virtual-pointer-unstable-v1-protocol.c"
    COMMAND "${RILEY_WAYLAND_SCANNER}" private-code "${_protocol}"
            "${_generated}/wlr-virtual-pointer-unstable-v1-protocol.c"
    DEPENDS "${_protocol}"
    VERBATIM)

add_executable(riley-dock-pointer
    main.cpp
    "${_generated}/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    "${_generated}/wlr-virtual-pointer-unstable-v1-protocol.c")
target_include_directories(riley-dock-pointer PRIVATE "${_generated}")
target_link_libraries(riley-dock-pointer PRIVATE Qt6::Gui LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT)
# Next to riley-dock-qt, where gates/qt-capture.sh looks for it.
set_target_properties(riley-dock-pointer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
