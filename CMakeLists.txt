cmake_minimum_required(VERSION 3.1)

project(TaskBlockExample)

include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("/std:c++17" CXX_HAS_C++17_FLAG)
if (CXX_HAS_C++17_FLAG)
    add_compile_options("/std:c++17")
else()
    message(FATAL_ERROR "This project needs a c++17 compatible compiler, CMake will exit." )
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(TaskBlockExample main.cpp ThreadPoolQueue.hpp ThreadPool.hpp TaskBlock.hpp)
