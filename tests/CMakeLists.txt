# One executable + one CTest entry per suite, so a crash in one suite doesn't
# take down the rest (emulator-setup-guide.md §6).
add_executable(cpu_suite cpu_suite.c)
target_link_libraries(cpu_suite PRIVATE pdp11_core unity)
pdp11_set_warnings(cpu_suite)
add_test(NAME cpu_suite COMMAND cpu_suite)

# No-reference golden regression: run the headless frontend on an image and diff
# its canonical state dump against a checked-in golden produced by the SimH
# oracle (tools/simh-oracle/). The golden is bit-identical across hosts and
# build types because it is emulated architectural state, not wall-clock timing.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME golden_add3
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/regress.py
            --headless $<TARGET_FILE:pdp11_headless>
            --image  ${CMAKE_SOURCE_DIR}/tests/images/add3.image
            --golden ${CMAKE_SOURCE_DIR}/tests/goldens/add3.golden)
endif()
