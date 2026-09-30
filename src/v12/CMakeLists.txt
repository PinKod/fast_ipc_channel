cmake_minimum_required(VERSION 3.5)

project(v12)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BuildTypes.cmake NO_POLICY_SCOPE)

add_executable(
    producer
    producer.c
)

add_executable(
    consumer
    consumer.c
)

enable_lto_for_target(producer)
enable_lto_for_target(consumer)
enable_opt_info_for_target(producer)
enable_opt_info_for_target(consumer)
