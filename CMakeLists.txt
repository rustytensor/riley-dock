# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(riley-dock VERSION 0.1.0 LANGUAGES CXX)

get_filename_component(_riley_source "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(_riley_binary "${CMAKE_BINARY_DIR}" REALPATH)
cmake_path(IS_PREFIX _riley_source "${_riley_binary}" NORMALIZE _riley_in_source)
if(_riley_in_source)
    message(FATAL_ERROR "riley-dock is never built inside its source tree; pass -B with a directory outside it")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Test)

# The core compares doubles exactly with the Rust results: no fused multiply-add contraction, no fast maths.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -ffp-contract=off)
endif()

# One static library per Rust module (riley dock/src/<module>.rs), linking the modules its Rust `use` lines name.
function(riley_dock_module name)
    add_library(riley-dock-${name} STATIC
        src/core/${name}.cpp
        src/riley-dock/${name}.h)
    target_include_directories(riley-dock-${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(riley-dock-${name} PUBLIC cxx_std_20)
    target_link_libraries(riley-dock-${name} PUBLIC Qt6::Core ${ARGN})
endfunction()

riley_dock_module(rules)
riley_dock_module(pins)
riley_dock_module(desktop)
riley_dock_module(motion)
riley_dock_module(grid riley-dock-motion riley-dock-rules)
riley_dock_module(autohide riley-dock-motion riley-dock-rules)
riley_dock_module(ipc riley-dock-rules)
riley_dock_module(control riley-dock-ipc)

add_library(riley-dock-core INTERFACE)
target_link_libraries(riley-dock-core INTERFACE
    riley-dock-rules riley-dock-pins riley-dock-desktop riley-dock-motion
    riley-dock-grid riley-dock-autohide riley-dock-ipc riley-dock-control)

# One Qt Test executable per Rust test file, linking only the modules that file uses.
enable_testing()
function(riley_dock_test name)
    add_executable(${name} tests/core/${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

riley_dock_test(tst_autohide riley-dock-autohide riley-dock-control riley-dock-grid riley-dock-ipc
                riley-dock-motion riley-dock-rules)
riley_dock_test(tst_launcher riley-dock-grid riley-dock-motion riley-dock-rules)
riley_dock_test(tst_motion riley-dock-motion)
riley_dock_test(tst_parity riley-dock-desktop riley-dock-pins riley-dock-rules)
riley_dock_test(tst_reflections riley-dock-motion riley-dock-rules)
riley_dock_test(tst_ipc riley-dock-ipc)
