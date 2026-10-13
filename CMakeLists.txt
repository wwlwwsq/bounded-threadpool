cmake_minimum_required(VERSION 3.14)
project(bounded_threadpool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BPOOL_BUILD_TESTS "构建规格测试" ON)
option(BPOOL_BUILD_BENCH "构建基准" ON)
option(BPOOL_SANITIZE "AddressSanitizer + UBSan" OFF)
option(BPOOL_TSAN "ThreadSanitizer（与 SANITIZE 互斥，仅 Linux/macOS）" OFF)

find_package(Threads REQUIRED)

add_library(bpool
    src/thread_pool.cpp
    src/sharded_pool.cpp)
target_include_directories(bpool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bpool PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bpool PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BPOOL_SANITIZE AND BPOOL_TSAN)
    message(FATAL_ERROR "ASan 与 TSan 不能同开")
endif()

function(apply_sanitizers target)
    if(BPOOL_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    elseif(BPOOL_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

apply_sanitizers(bpool)

if(BPOOL_BUILD_TESTS)
    enable_testing()
    add_executable(test_pool tests/test_pool.cpp)
    target_link_libraries(test_pool PRIVATE bpool)
    apply_sanitizers(test_pool)
    add_test(NAME spec COMMAND test_pool)

    # 混合压力：TSan 模式下与 test_pool 同为重点观察对象
    add_executable(stress_mix tests/stress_mix.cpp)
    target_link_libraries(stress_mix PRIVATE bpool)
    apply_sanitizers(stress_mix)
    add_test(NAME stress_mix COMMAND stress_mix)
endif()

if(BPOOL_BUILD_BENCH)
    # 基准不挂 sanitizer，保持 Release 纯净
    add_executable(bench_pool benchmarks/bench_pool.cpp)
    target_link_libraries(bench_pool PRIVATE bpool)
endif()
