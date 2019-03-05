cmake_minimum_required(VERSION 3.14)
project(lockfree-queue VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable IPO/LTO for performance
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# Architecture-specific optimizations
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mtune=native")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc+crypto")
endif()

# Warning flags
if(MSVC)
    add_compile_options(/W4 /WX /utf-8 /O2 /Ob2 /Oi /Ot)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
    add_compile_options(-O3 -ffast-math -flto)
endif()

# Library target
add_library(lfqueue
    src/queue.cpp
    src/hp.cpp
)

target_include_directories(lfqueue
    PUBLIC include
    PRIVATE src
)

# Enable if available
target_compile_definitions(lfqueue PRIVATE LFQUEUE_HAS_SSE2=1)
target_compile_definitions(lfqueue PRIVATE LFQUEUE_HAS_SSE4_1=1)

# Test executable
option(LFQUEUE_BUILD_TESTS "Build unit tests" ON)
if(LFQUEUE_BUILD_TESTS)
    find_package(Threads REQUIRED)
    enable_testing()

    add_executable(lfqueue_test
        test/test_queue.cpp
        test/test_hp.cpp
        test/test_bench.cpp
    )

    target_link_libraries(lfqueue_test
        PRIVATE lfqueue
        PRIVATE Threads::Threads
    )

    target_include_directories(lfqueue_test PRIVATE test)
    add_test(NAME lfqueue_test COMMAND lfqueue_test)
endif()

# Benchmark executable (manual run)
option(LFQUEUE_BUILD_BENCH "Build benchmarks" ON)
if(LFQUEUE_BUILD_BENCH)
    find_package(Threads REQUIRED)

    add_executable(lfqueue_bench
        bench/bench_main.cpp
    )

    target_link_libraries(lfqueue_bench
        PRIVATE lfqueue
        PRIVATE Threads::Threads
    )
endif()
