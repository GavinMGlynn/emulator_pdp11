# Interactive SDL3 frontend — a VT-style terminal on the DL11 console plus a
# KY11 console panel (address/data lights + a run indicator). Optional: if SDL3
# is not installed the target is silently skipped so the core/headless build and
# CI are unaffected. SDL headers are included SYSTEM so they never trip the
# first-party strict warning set (which is -Werror in the debug preset).
find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(STATUS "pkg-config not found — skipping interactive frontend (pdp11_sdl)")
    return()
endif()

pkg_check_modules(SDL3 QUIET sdl3)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found — skipping interactive frontend (pdp11_sdl)")
    return()
endif()

add_executable(pdp11_sdl main.c)
target_include_directories(pdp11_sdl SYSTEM PRIVATE ${SDL3_INCLUDE_DIRS})
target_link_directories(pdp11_sdl PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(pdp11_sdl PRIVATE pdp11_core ${SDL3_LIBRARIES})
pdp11_set_warnings(pdp11_sdl)

# A headless smoke test: run a fixed number of frames under SDL's dummy video
# driver (no display, no copyrighted image) and require a clean exit. Proves the
# SDL init + event + render loop is sound in CI.
add_test(NAME sdl_frontend_smoke
    COMMAND pdp11_sdl --frames 30)
set_tests_properties(sdl_frontend_smoke PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
