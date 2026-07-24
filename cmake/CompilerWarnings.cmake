# pdp11_set_warnings(<target>) — the strict warning set applied to every
# first-party target, and NEVER to ext/ (emulator-setup-guide.md §5).
# -Wconversion / -Wsign-conversion / -Wswitch-enum catch real emulator bugs.
function(pdp11_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wcast-qual -Wcast-align -Wpointer-arith -Wstrict-prototypes
        -Wmissing-prototypes -Wredundant-decls -Wundef -Wwrite-strings
        -Wdouble-promotion -Wformat=2 -Wswitch-enum -Wvla)
    if(PDP11_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
