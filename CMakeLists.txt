cmake_minimum_required(VERSION 3.28)
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)

include_directories(.)
include_directories(include)

add_executable(code src.cpp)
