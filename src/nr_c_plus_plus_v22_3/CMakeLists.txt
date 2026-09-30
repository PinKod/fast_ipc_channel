cmake_minimum_required(VERSION 3.5)

find_program(CLANGXX_BIN NAMES clang++-18 clang++)
if(NOT CLANGXX_BIN)
    message(FATAL_ERROR "clang++ not found - install with: sudo apt-get install clang-18")
endif()
find_program(LLD_BIN NAMES ld.lld-18 ld.lld lld-18 lld)
if(NOT LLD_BIN)
    message(FATAL_ERROR "ld.lld not found - install with: sudo apt-get install lld-18")
endif()

set(CMAKE_CXX_COMPILER ${CLANGXX_BIN})

project(nr_c_plus_plus_v22_3 CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -Wno-macro-redefined")

cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_OK)
if(IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_link_options(-fuse-ld=lld-18)

include_directories(/usr/local/include)

add_executable(producer producer.cpp)
add_executable(consumer consumer.cpp)

find_package(Threads REQUIRED)
target_link_libraries(producer PRIVATE /usr/local/lib/liblz4.a Threads::Threads)
target_link_libraries(consumer PRIVATE /usr/local/lib/liblz4.a Threads::Threads)
