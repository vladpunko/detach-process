# -*- coding: utf-8 -*-

# Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
# Created date: 2026-10-21

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

add_executable(detach_probe probe.c)
target_include_directories(detach_probe PRIVATE "${PROJECT_SOURCE_DIR}")
set_target_properties(detach_probe PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

# Only this executable replaces system calls for deterministic failure tests.
add_executable(detach_faults faults.c)
target_include_directories(detach_faults PRIVATE "${PROJECT_SOURCE_DIR}")
set_target_properties(detach_faults PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

option(ENABLE_COVERAGE "Instrument detach.c with Clang source coverage" OFF)
if(ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE requires Clang")
    endif()

    get_filename_component(compiler_directory "${CMAKE_C_COMPILER}" DIRECTORY)
    set(llvm_tool_directories "${compiler_directory}")
    if(APPLE)
        execute_process(COMMAND xcrun --find llvm-profdata
            OUTPUT_VARIABLE apple_profdata
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE xcrun_status
        )
        if(xcrun_status EQUAL 0)
            get_filename_component(apple_tool_directory "${apple_profdata}" DIRECTORY)
            list(APPEND llvm_tool_directories "${apple_tool_directory}")
        endif()
    endif()
    string(REGEX MATCH "^[0-9]+" clang_major "${CMAKE_C_COMPILER_VERSION}")
    find_program(LLVM_PROFDATA NAMES llvm-profdata-${clang_major} llvm-profdata
        HINTS ${llvm_tool_directories}
        NAMES_PER_DIR
    )
    find_program(LLVM_COV NAMES llvm-cov-${clang_major} llvm-cov
        HINTS ${llvm_tool_directories}
        NAMES_PER_DIR
    )
    if(NOT LLVM_PROFDATA OR NOT LLVM_COV)
        message(FATAL_ERROR
            "Coverage requires llvm-profdata and llvm-cov matching Clang. "
            "Set LLVM_PROFDATA and LLVM_COV to their paths if needed."
        )
    endif()

    target_compile_definitions(detach_faults PRIVATE DETACH_TEST_COVERAGE=1)
    target_compile_options(detach_faults PRIVATE
        -fprofile-instr-generate -fcoverage-mapping
    )
    target_link_options(detach_faults PRIVATE -fprofile-instr-generate)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/profiles")
endif()

add_executable(detach_tests
    arguments_test.cc
    files_test.cc
    failures_test.cc
    process_test.cc
    probe_client.cc
    test_files.cc
    test_launcher.cc
    test_support.cc
)
set_target_properties(detach_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_definitions(detach_tests PRIVATE
    DETACH_PATH="$<TARGET_FILE:detach>"
    FAULT_PATH="$<TARGET_FILE:detach_faults>"
    PROBE_PATH="$<TARGET_FILE:detach_probe>"
)
target_link_libraries(detach_tests PRIVATE GTest::gtest_main)
target_include_directories(detach_tests PRIVATE "${PROJECT_SOURCE_DIR}")
add_dependencies(detach_tests detach detach_probe detach_faults)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(detach_probe PRIVATE -Wall -Wextra -Werror -Wpedantic)
    target_compile_options(detach_faults PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(detach_tests PRIVATE
        -Wall -Wextra -Werror -Wpedantic -fno-exceptions
    )
endif()

include(GoogleTest)
gtest_discover_tests(detach_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 20)

if(ENABLE_COVERAGE)
    add_custom_target(coverage
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DFAULT_EXECUTABLE=$<TARGET_FILE:detach_faults>"
            "-DLLVM_PROFDATA=${LLVM_PROFDATA}"
            "-DLLVM_COV=${LLVM_COV}"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_coverage.cmake"
        DEPENDS detach_tests
        COMMENT "Run tests and require 100% coverage of detach.c"
        USES_TERMINAL
        VERBATIM
    )
endif()
