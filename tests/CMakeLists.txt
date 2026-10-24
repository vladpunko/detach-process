# -*- coding: utf-8 -*-

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()

    set(BUILD_GMOCK OFF)
    set(INSTALL_GTEST OFF)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    )
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(detach_probe support/command.c)

# Only this executable replaces system calls for deterministic failure tests.
add_executable(detach_faults support/syscall_overrides.c)

target_include_directories(detach_probe PRIVATE "${PROJECT_SOURCE_DIR}")
target_include_directories(detach_faults PRIVATE "${PROJECT_SOURCE_DIR}")
set_target_properties(detach_probe detach_faults PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

set(test_warning_flags -Wall -Wextra -Werror -Wpedantic)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(detach_probe PRIVATE ${test_warning_flags})
    target_compile_options(detach_faults PRIVATE ${test_warning_flags})
endif()

add_executable(detach_tests
    cases/arguments.cc
    cases/output.cc
    cases/redirection.cc
    cases/aliases.cc
    cases/process.cc
    cases/signals.cc
    cases/terminal.cc
    cases/errors.cc
    cases/reports.cc
    support/io.cc
    support/runner.cc
    support/process_probe.cc
    support/fixture.cc
    support/error_fixture.cc
)
set_target_properties(detach_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_link_libraries(detach_tests PRIVATE GTest::gtest_main)
target_include_directories(detach_tests PRIVATE "${PROJECT_SOURCE_DIR}")
add_dependencies(detach_tests detach detach_probe detach_faults)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(detach_tests PRIVATE
        ${test_warning_flags} -fno-exceptions
    )
endif()

option(ENABLE_COVERAGE "Enable test coverage reporting" OFF)
if(ENABLE_COVERAGE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/coverage.cmake")
    set(detach_executable "$<TARGET_FILE:detach_faults>")
else()
    set(detach_executable "$<TARGET_FILE:detach>")
endif()

include(GoogleTest)
gtest_discover_tests(detach_tests
    NO_PRETTY_VALUES
    PROPERTIES TIMEOUT 20
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_environment.cmake")
