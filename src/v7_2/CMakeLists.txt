cmake_minimum_required(VERSION 3.5)

project(v1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(MAX_OPT_FLAGS
    -g
    -O3
    -march=native
    -mtune=native
    -flto
    -funroll-loops
    -ffast-math
    -fomit-frame-pointer
    -fno-asynchronous-unwind-tables
    -DNDEBUG
    -Wall
    -Wextra
    -mavx2
    -lrt
)

add_compile_options(${MAX_OPT_FLAGS})
add_link_options(-flto -static -no-pie)

add_executable(
    producer
    producer.c
)

add_executable(
    consumer
    consumer.c
)

find_package(
    Threads REQUIRED
)
target_link_libraries(
    consumer
    PRIVATE
    Threads::Threads
)
