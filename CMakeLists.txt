cmake_minimum_required(VERSION 3.20)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code / clangd understand the code

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

option(ME_SANITIZE "Build with Address + UndefinedBehavior sanitizers" OFF)
option(ME_TSAN     "Build with ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

add_library(me_core src/order_book.cpp src/matching_engine.cpp)
target_include_directories(me_core PUBLIC include)
target_compile_options(me_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
target_link_libraries(me_core PUBLIC Threads::Threads)

if(ME_SANITIZE)
  target_compile_options(me_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(me_core PUBLIC -fsanitize=address,undefined)
endif()
if(ME_TSAN)
  target_compile_options(me_core PUBLIC -fsanitize=thread)
  target_link_options(me_core PUBLIC -fsanitize=thread)
endif()

add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE me_core)

add_executable(unit_tests tests/test_main.cpp tests/test_spsc_queue.cpp
                          tests/test_order_book.cpp tests/test_matching_engine.cpp)
target_include_directories(unit_tests PRIVATE tests)
target_link_libraries(unit_tests PRIVATE me_core)

foreach(b engine spsc pipeline)
  add_executable(bench_${b} bench/bench_${b}.cpp)
  target_include_directories(bench_${b} PRIVATE bench)
  target_link_libraries(bench_${b} PRIVATE me_core)
endforeach()

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
