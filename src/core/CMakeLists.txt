# The emulator core: a static library with zero frontend dependencies.
# Headers are included as "<subsystem>/<file>.h" from this directory root.
add_library(pdp11_core STATIC
    cpu/cpu.c
    cache/cache.c
    clk/clk.c
    console/console.c
    devices/rk11.c
    devices/rp11.c
    fp/fp.c
    memory/memory.c
    timing/timing.c)

target_include_directories(pdp11_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

pdp11_set_warnings(pdp11_core)
