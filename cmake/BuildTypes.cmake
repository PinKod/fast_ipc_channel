if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

function(enable_lto_for_target tgt)
    if(ipo_supported AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
        set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

function(enable_opt_info_for_target tgt)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(${tgt} PRIVATE -fopt-info-optall -fopt-info-vec-optimized)
    endif()
endfunction()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        -O3
        -march=native
        -mtune=native
        -funroll-loops
        -ffast-math
        -fomit-frame-pointer
        -fno-asynchronous-unwind-tables
        -DNDEBUG
        -Wall
        -Wextra
    )
    add_link_options(-static -no-pie)

elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(
        -O3
        -g
        -march=native
        -mtune=native
        -funroll-loops
        -ffast-math
        -fomit-frame-pointer
        -DNDEBUG
        -Wall
        -Wextra
    )
    add_link_options(-static -no-pie)

elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3 -fno-omit-frame-pointer -Wall -Wextra)
    add_link_options(-static)

elseif(CMAKE_BUILD_TYPE STREQUAL "AsanDebug")
    add_compile_options(
        -O1 -g3
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -fno-common
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
        -fsanitize-address-use-after-scope
        -fstack-protector-strong
        -Wall -Wextra
    )
    add_link_options(-fsanitize=address,undefined,leak)

elseif(CMAKE_BUILD_TYPE STREQUAL "TsanDebug")
    add_compile_options(
        -O1 -g3
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -fno-common
        -fno-sanitize-recover=all
        -fsanitize=thread
        -Wall -Wextra
    )
    add_link_options(-fsanitize=thread)

elseif(CMAKE_BUILD_TYPE STREQUAL "ValgrindDebug")
    add_compile_options(
        -O1 -g3
        -fno-omit-frame-pointer
        -fno-common
        -fno-inline
        -fno-optimize-sibling-calls
        -fno-strict-aliasing
        -Wall -Wextra
    )
    add_link_options(-static -g3)
    add_definitions(-D_FORTIFY_SOURCE=0)

else()
    message(FATAL_ERROR "Unknown CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}'. "
        "Expected one of: Release, RelWithDebInfo, Debug, AsanDebug, TsanDebug, ValgrindDebug")
endif()
