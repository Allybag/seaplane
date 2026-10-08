cmake_minimum_required(VERSION 3.22)
project(seaplane)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(seaplane INTERFACE)

target_include_directories(seaplane INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

add_executable(seaplaneshow main.cpp)
target_link_libraries(seaplaneshow PRIVATE seaplane)

option(SEAPLANE_TESTS "Build ring_test" OFF)
option(SEAPLANE_TSAN "Build ring_test with ThreadSanitizer" OFF)
if (SEAPLANE_TESTS)
    find_package(Threads REQUIRED)
    add_executable(ring_test test/ring_test.cpp)
    target_link_libraries(ring_test PRIVATE seaplane Threads::Threads)
    if (SEAPLANE_TSAN)
        target_compile_options(ring_test PRIVATE -fsanitize=thread -g)
        target_link_options(ring_test PRIVATE -fsanitize=thread)
    endif()
endif()

add_executable(bench_ring bench/bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE seaplane)
target_compile_options(bench_ring PRIVATE -O2)
