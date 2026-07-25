# PDP-11 emulator — cycle-accurate, 11/70 first (see docs/COMPLETION_PLAN.md).
# The core is a static library that knows nothing about any frontend; frontends
# depend on the core, never the reverse (emulator-setup-guide.md §4).
cmake_minimum_required(VERSION 3.21)

project(pdp11
    VERSION 0.0.1
    DESCRIPTION "Cycle-accurate PDP-11 emulator"
    LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(PDP11_WERROR "Treat warnings as errors" OFF)

include(cmake/CompilerWarnings.cmake)

# --- Vendored third-party (ext/ submodules), built without our warning set ----
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/unity/src/unity.c")
    message(FATAL_ERROR
        "ext/unity is missing. Run: git submodule update --init ext/unity")
endif()

add_library(unity STATIC ext/unity/src/unity.c)
# SYSTEM so Unity's assert macros don't trip our strict warning set in tests.
target_include_directories(unity SYSTEM PUBLIC ext/unity/src)

# --- First-party targets ------------------------------------------------------
enable_testing()
add_subdirectory(src/core)
add_subdirectory(src/frontend/headless)
add_subdirectory(src/frontend/sdl)
add_subdirectory(tests)
