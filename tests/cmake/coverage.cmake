# -*- coding: utf-8 -*-

# Clang writes profiles in a toolchain-specific format.
# Use LLVM tools from that toolchain to merge and read those profiles.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "coverage reporting requires the Clang C compiler\n"
        "configure with -DCMAKE_C_COMPILER=clang to enable coverage reporting"
    )
endif()

# Prefer tools beside the compiler before searching the system paths.
get_filename_component(llvm_tool_directories "${CMAKE_C_COMPILER}" DIRECTORY)

# Find LLVM tools in the active Xcode toolchain.
if(APPLE)
    execute_process(
        COMMAND xcrun --find llvm-profdata
        OUTPUT_VARIABLE apple_profdata_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE xcrun_status
    )
    if(xcrun_status STREQUAL "0")
        get_filename_component(apple_tool_directory "${apple_profdata_path}" DIRECTORY)
        list(APPEND llvm_tool_directories "${apple_tool_directory}")
    endif()
endif()

string(REGEX MATCH "^[0-9]+" clang_major "${CMAKE_C_COMPILER_VERSION}")
find_program(LLVM_PROFDATA
    NAMES "llvm-profdata-${clang_major}" llvm-profdata
    HINTS ${llvm_tool_directories}
    NAMES_PER_DIR
    DOC "path to llvm-profdata from the selected Clang toolchain"
)
find_program(LLVM_COV
    NAMES "llvm-cov-${clang_major}" llvm-cov
    HINTS ${llvm_tool_directories}
    NAMES_PER_DIR
    DOC "path to llvm-cov from the selected Clang toolchain"
)
if(NOT LLVM_PROFDATA OR NOT LLVM_COV)
    message(FATAL_ERROR
        "cannot find llvm-profdata or llvm-cov for the selected Clang compiler\n"
        "set LLVM_PROFDATA and LLVM_COV to executable paths from the same "
        "toolchain as ${CMAKE_C_COMPILER}"
    )
endif()

# Instrument the executable used by both ordinary and injected failure cases.
target_compile_definitions(detach_faults PRIVATE DETACH_TEST_COVERAGE=1)
target_compile_options(detach_faults PRIVATE
    -fprofile-instr-generate -fcoverage-mapping
)
target_link_options(detach_faults PRIVATE -fprofile-instr-generate)

# A rebuilt executable must never reuse profiles from its previous version.
add_custom_command(TARGET detach_faults PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${CMAKE_BINARY_DIR}/profiles"
)

set(coverage_settings
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DSOURCE_FILE=${PROJECT_SOURCE_DIR}/detach.c"
    "-DCOVERAGE_EXECUTABLE=$<TARGET_FILE:detach_faults>"
    "-DLLVM_PROFDATA=${LLVM_PROFDATA}"
    "-DLLVM_COV=${LLVM_COV}"
    "-DCONFIG=$<CONFIG>"
)

# Run tests and generate the coverage report.
add_custom_target(coverage
    COMMAND "${CMAKE_COMMAND}" ${coverage_settings}
        -P "${CMAKE_CURRENT_LIST_DIR}/run_coverage.cmake"
    DEPENDS detach_tests
    COMMENT "run tests and verify full line, function and branch coverage of detach.c"
    USES_TERMINAL
    VERBATIM
)

# Reuse profiles when the caller has already run the tests.
add_custom_target(coverage_report
    COMMAND "${CMAKE_COMMAND}" ${coverage_settings}
        -DRUN_TESTS=OFF
        -P "${CMAKE_CURRENT_LIST_DIR}/run_coverage.cmake"
    DEPENDS detach_tests
    COMMENT "verify coverage from collected test profiles"
    USES_TERMINAL
    VERBATIM
)
