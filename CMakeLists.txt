cmake_minimum_required(VERSION 3.1)

project(TaskBlockExample)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckCXXCompilerFlag)

CHECK_CXX_COMPILER_FLAG("/std:c++17" CXX_HAS_C++17_FLAG)
if (CXX_HAS_C++17_FLAG)
    add_compile_options("/std:c++17")
endif()

find_package(Threads REQUIRED)

add_executable(TaskBlockExample main.cpp ThreadPoolQueue.hpp ThreadPool.hpp TaskBlock.hpp)

target_link_libraries(TaskBlockExample ${CMAKE_THREAD_LIBS_INIT})