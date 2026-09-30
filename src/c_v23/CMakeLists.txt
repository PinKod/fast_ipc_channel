cmake_minimum_required(VERSION 3.5)

project(c_v23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BuildTypes.cmake NO_POLICY_SCOPE)

include_directories(/usr/local/include)

add_executable(
    producer
    producer.c
)

add_executable(
    consumer
    consumer.c
)

target_link_libraries(producer PRIVATE /usr/local/lib/liblz4.a)
target_link_libraries(consumer PRIVATE /usr/local/lib/liblz4.a)

enable_lto_for_target(producer)
enable_lto_for_target(consumer)
enable_opt_info_for_target(producer)
enable_opt_info_for_target(consumer)
