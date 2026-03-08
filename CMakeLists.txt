cmake_minimum_required(VERSION 3.16)

project(LeoMem VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LEOMEM_BUILD_TESTS "Build LeoMem tests" ON)
option(LEOMEM_BUILD_BENCHMARKS "Build LeoMem benchmarks" ON)
option(LEOMEM_ENABLE_RDMA "Enable RDMA transport" OFF)
option(LEOMEM_ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

if(LEOMEM_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

add_subdirectory(src)

if(LEOMEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LEOMEM_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()