# ghost_story — cmake/compiler_flags.cmake
# Warning policy per ADR 001: strict on first-party code.

function(ghost_story_apply_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            # -Wnull-dereference dropped: false positives inside libstdc++
            # headers (basic_string) under GCC 16 -O2 with C++26.
            -Wdouble-promotion
            -Wformat=2
        )
        if(GHOST_STORY_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
