cmake_minimum_required(VERSION 3.16)
project(distributed_job_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings are errors: a project that claims concurrency correctness should not ship with
# a wall of ignored diagnostics.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# --- optional sanitizer builds -------------------------------------------------------
set(JOBENGINE_SANITIZER "none" CACHE STRING "none | thread | address")
set_property(CACHE JOBENGINE_SANITIZER PROPERTY STRINGS none thread address)
if(JOBENGINE_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(JOBENGINE_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# --- SQLite ---------------------------------------------------------------------------
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "SQLite3 development files not found. Install libsqlite3-dev (Linux) "
                      "or the Xcode command line tools (macOS).")
endif()

# --- core library ---------------------------------------------------------------------
add_library(jobengine_core STATIC
  src/core/json.cpp
  src/core/util.cpp
  src/core/job.cpp
  src/core/metrics.cpp
  src/queue/queue.cpp
  src/store/store.cpp
  src/engine/handlers.cpp
  src/engine/engine.cpp
  src/http/http_server.cpp
  src/http/http_client.cpp
  src/api/api.cpp
  src/worker/remote_worker.cpp
)
target_include_directories(jobengine_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${SQLITE3_INCLUDE_DIR}
)
target_link_libraries(jobengine_core PUBLIC ${SQLITE3_LIBRARY} Threads::Threads)
# Recorded in every benchmark CSV header so a result can never be mistaken for one
# produced by a different build configuration.
target_compile_definitions(jobengine_core PUBLIC
  CMAKE_BUILD_TYPE_STRING="${CMAKE_BUILD_TYPE}")

# --- executables -----------------------------------------------------------------------
add_executable(jobengine-server apps/server_main.cpp)
target_link_libraries(jobengine-server PRIVATE jobengine_core)

add_executable(jobengine-worker apps/worker_main.cpp)
target_link_libraries(jobengine-worker PRIVATE jobengine_core)

add_executable(jobengine-bench apps/bench_main.cpp)
target_link_libraries(jobengine-bench PRIVATE jobengine_core)

add_executable(jobengine-load apps/load_main.cpp)
target_link_libraries(jobengine-load PRIVATE jobengine_core)

# --- tests ------------------------------------------------------------------------------
option(JOBENGINE_BUILD_TESTS "Build the test suite" ON)
if(JOBENGINE_BUILD_TESTS)
  enable_testing()
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt)
    # An optional local checkout, used when present so the build can run with no network.
    # It is not committed; see docs/DESIGN_DECISIONS.md D-10 for the one command that
    # creates it.
    message(STATUS "Using vendored GoogleTest from third_party/googletest")
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(jobengine_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_job_state.cpp
    tests/test_queue.cpp
    tests/test_store.cpp
    tests/test_engine.cpp
    tests/test_retry_timeout.cpp
    tests/test_workflow.cpp
    tests/test_http.cpp
    tests/test_api.cpp
    tests/test_distributed.cpp
  )
  target_link_libraries(jobengine_tests PRIVATE jobengine_core GTest::gtest)
  # GoogleTest's own sources are not held to this project's -Werror policy.
  target_compile_options(gtest PRIVATE -w)
  include(GoogleTest)
  gtest_discover_tests(jobengine_tests DISCOVERY_TIMEOUT 60)
endif()
