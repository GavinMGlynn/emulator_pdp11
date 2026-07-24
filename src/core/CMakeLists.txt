# The emulator core: a static library with zero frontend dependencies.
# Headers are included as "<subsystem>/<file>.h" from this directory root.
add_library(pdp11_core STATIC
    cpu/cpu.c
    memory/memory.c)

target_include_directories(pdp11_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

pdp11_set_warnings(pdp11_core)
