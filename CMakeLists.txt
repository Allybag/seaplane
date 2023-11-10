cmake_minimum_required(VERSION 3.25)
project(seaplane)

set(CMAKE_CXX_STANDARD 20)

add_executable(httpsclient httpsclient.cpp)

find_package(OpenSSL)
target_link_libraries(httpsclient PRIVATE OpenSSL::SSL)
