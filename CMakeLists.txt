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

# ---------------------------------------------------------------------------------------------------------------
# Milestone 2: the window. riley-dock-qt is the Qt Quick front end (riley dock/src/main.rs, ui.rs, view.rs) on
# layer-shell-qt, drawn from the core above. It is built under this name until the switch, so the running dock is
# never replaced by a build.
find_package(Qt6 6.11 REQUIRED COMPONENTS DBus Gui Qml Quick)
find_package(LayerShellQt 6.8 REQUIRED)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(riley-dock-qt
    src/app/main.cpp
    src/app/log.cpp src/app/log.h
    src/app/style.cpp src/app/style.h
    src/app/filewatch.cpp src/app/filewatch.h
    src/app/launch.cpp src/app/launch.h
    src/app/iconprovider.cpp src/app/iconprovider.h
    src/app/models.cpp src/app/models.h
    src/app/dockwindow.cpp src/app/dockwindow.h
    src/app/dockapp.cpp src/app/dockapp.h)

# The QML files sit at the module root (qrc:/qt/qml/RileyDock/), so they name each other by type.
set(riley_dock_qml
    qml/Dock.qml qml/Icon.qml qml/Reflection.qml qml/HoverName.qml
    qml/Menu.qml qml/Launcher.qml qml/Minimized.qml)
foreach(file IN LISTS riley_dock_qml)
    get_filename_component(alias "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()

qt_add_qml_module(riley-dock-qt
    URI RileyDock
    VERSION 1.0
    QML_FILES ${riley_dock_qml})

# The generated QML type registrations include the model headers by their bare names.
target_include_directories(riley-dock-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/app")
target_link_libraries(riley-dock-qt PRIVATE Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick LayerShellQt::Interface riley-dock-core)
