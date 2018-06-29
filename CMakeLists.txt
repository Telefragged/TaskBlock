cmake_minimum_required(VERSION 3.1)

project(TaskBlockExample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(TaskBlockExample main.cpp ThreadPoolQueue.hpp ThreadPool.hpp TaskBlock.hpp)
