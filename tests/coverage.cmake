# Clang instrumentation and the complete coverage target live together so the
# integration test build stays independent of LLVM tool discovery.

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
