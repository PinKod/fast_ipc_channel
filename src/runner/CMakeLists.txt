cmake_minimum_required(VERSION 3.5)

project(runner)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BuildTypes.cmake NO_POLICY_SCOPE)

add_executable(
    ${PROJECT_NAME}
    main.c
)

enable_lto_for_target(${PROJECT_NAME})
enable_opt_info_for_target(${PROJECT_NAME})

find_package(
    Threads REQUIRED
)
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
    Threads::Threads
    m
)
