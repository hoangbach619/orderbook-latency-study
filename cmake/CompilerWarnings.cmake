# A strict warning set, errors on every warning, shared by GCC and Clang.
#
# The conversion warnings are kept on deliberately: a study about integer ticks and
# cache sized structs should never silently narrow a width. Every narrowing in the code
# is therefore an explicit cast that states intent.

function(obls_set_warnings target)
    set(warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wimplicit-fallthrough
        -Wformat=2
        -Werror)
    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
