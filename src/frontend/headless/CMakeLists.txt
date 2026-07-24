# Deterministic headless frontend — links the identical core static lib.
add_executable(pdp11_headless main.c)
target_link_libraries(pdp11_headless PRIVATE pdp11_core)
pdp11_set_warnings(pdp11_headless)
