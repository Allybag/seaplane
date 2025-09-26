cmake_minimum_required(VERSION 3.22)
project(seaplane)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(seaplane INTERFACE)

target_include_directories(seaplane INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

add_executable(main main.cpp)
target_link_libraries(main PRIVATE seaplane)

