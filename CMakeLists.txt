cmake_minimum_required(VERSION 3.16)
project(limit_order_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Core engine as a static library so the demo CLI, benchmark, and tests
# all link the same compiled code instead of recompiling it three times.
add_library(lob STATIC
  src/order_book.cpp
)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lob PUBLIC -Wall -Wextra)
endif()

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)

add_executable(lob_bench bench/benchmark.cpp)
target_link_libraries(lob_bench PRIVATE lob)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # -g so `perf record` / flamegraphs resolve symbols; benchmark still
  # builds with optimizations on via the top-level Release build type.
  target_compile_options(lob_bench PRIVATE -g)
endif()

enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE lob)
target_include_directories(test_order_book PRIVATE tests)
add_test(NAME test_order_book COMMAND test_order_book)

add_executable(test_matching_engine tests/test_matching_engine.cpp)
target_link_libraries(test_matching_engine PRIVATE lob)
target_include_directories(test_matching_engine PRIVATE tests)
add_test(NAME test_matching_engine COMMAND test_matching_engine)
