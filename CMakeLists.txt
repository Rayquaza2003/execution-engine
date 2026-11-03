cmake_minimum_required(VERSION 3.20)
project(execution_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
# ThreadSanitizer: cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan
set(CMAKE_CXX_FLAGS_TSAN "-O1 -g -fsanitize=thread -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_TSAN "-fsanitize=thread")

include_directories(src)

enable_testing()
# One assert-based executable per module: tests/test_<name>.cpp
foreach(name orderbook id_index event_loop ring venue oms risk itch replay md fill_study fill_table tca algos router sor_sim spsc histogram logger fix fix_session)
  add_executable(test_${name} tests/test_${name}.cpp)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

add_executable(bench_book bench/bench_book.cpp)
add_executable(bench_risk bench/bench_risk.cpp)
add_executable(itch_replay tools/itch_replay.cpp)
add_executable(bench_md bench/bench_md.cpp)
add_executable(fill_study tools/fill_study.cpp)
add_executable(algo_compare tools/algo_compare.cpp)
add_executable(sor_compare tools/sor_compare.cpp)
add_executable(bench_spsc bench/bench_spsc.cpp)
add_executable(bench_pipeline bench/bench_pipeline.cpp)
