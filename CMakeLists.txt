cmake_minimum_required(VERSION 3.1)

project(TaskBlockExample)

include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("/std:c++17" _cpp_latest_flag_supported )
if (_cpp_latest_flag_supported)
    add_compile_options("/std:c++17")
elseif()
    message(FATAL_ERROR "This project needs a c++17 compatible compiler, CMake will exit." )
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(TaskBlockExample main.cpp ThreadPoolQueue.hpp ThreadPool.hpp TaskBlock.hpp)
