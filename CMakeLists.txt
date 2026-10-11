cmake_minimum_required(VERSION 3.20)

project(concurrency LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
)

find_package(Threads REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)

add_executable(treiber
        benchmark/benchmark.cpp
        hazard_pointers/hazard_pointer.cpp
)

target_link_libraries(treiber PRIVATE benchmark::benchmark Folly::folly Threads::Threads)
