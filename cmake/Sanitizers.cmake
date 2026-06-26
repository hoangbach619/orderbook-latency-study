# Sanitiser configurations, selected by a single cache variable so a build tree commits
# to one sanitiser at a time. ThreadSanitizer is mutually exclusive with AddressSanitizer
# by construction, which is why they are separate selections rather than flags that
# could be combined.

set(OBLS_SANITIZER "none" CACHE STRING "Sanitiser: none, asan-ubsan, or tsan")
set_property(CACHE OBLS_SANITIZER PROPERTY STRINGS none asan-ubsan tsan)

function(obls_enable_sanitizers target)
    if(OBLS_SANITIZER STREQUAL "asan-ubsan")
        set(flags -fsanitize=address,undefined -fno-omit-frame-pointer
                  -fno-sanitize-recover=all)
    elseif(OBLS_SANITIZER STREQUAL "tsan")
        set(flags -fsanitize=thread -fno-omit-frame-pointer)
    else()
        return()
    endif()
    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
endfunction()
