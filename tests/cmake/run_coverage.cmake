# -*- coding: utf-8 -*-

cmake_minimum_required(VERSION 3.16)

# Validate inputs before removing profiles from a previous run.
foreach(required IN ITEMS
    BUILD_DIR
    SOURCE_FILE
    COVERAGE_EXECUTABLE
    LLVM_PROFDATA
    LLVM_COV
)
    if("${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing coverage setting: ${required}")
    endif()
endforeach()

if(NOT EXISTS "${BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "not a configured CMake build directory: ${BUILD_DIR}")
endif()

# Stop at the failed step so later reports cannot hide a test or tool error.
function(run_checked program)
    execute_process(
        COMMAND "${program}" ${ARGN}
        WORKING_DIRECTORY "${BUILD_DIR}"
        RESULT_VARIABLE status
    )
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "coverage command failed: ${program} (${status})")
    endif()
endfunction()

set(profile_directory "${BUILD_DIR}/profiles")
set(merged_profile "${BUILD_DIR}/coverage.profdata")
set(report "${BUILD_DIR}/coverage.lcov")
file(REMOVE "${merged_profile}" "${report}")

# Fresh profiles prevent an earlier run from hiding a coverage gap.
if(NOT DEFINED RUN_TESTS OR RUN_TESTS)
    file(REMOVE_RECURSE "${profile_directory}")
    file(MAKE_DIRECTORY "${profile_directory}")

    set(ctest_arguments --output-on-failure --parallel 2)
    if(NOT "${CONFIG}" STREQUAL "")
        list(APPEND ctest_arguments -C "${CONFIG}")
    endif()
    run_checked("${CMAKE_COMMAND}" -E env
        "LLVM_PROFILE_FILE=${profile_directory}/%m-%p.profraw"
        "${CMAKE_CTEST_COMMAND}" ${ctest_arguments}
    )
endif()

file(GLOB profiles "${profile_directory}/*.profraw")
if(NOT profiles)
    message(FATAL_ERROR "no coverage profiles found: ${profile_directory}")
endif()

# Umask tests can leave profiles unreadable. Preserve every profile for merging.
run_checked(chmod u+rw ${profiles})
run_checked("${LLVM_PROFDATA}" merge -sparse ${profiles} -o "${merged_profile}")

set(coverage_arguments
    "${COVERAGE_EXECUTABLE}"
    "-instr-profile=${merged_profile}"
    "${SOURCE_FILE}"
)
run_checked("${LLVM_COV}" report ${coverage_arguments})

# Export the same source and profile data used by the summary above.
run_checked("${LLVM_COV}" export ${coverage_arguments} -format=lcov
    OUTPUT_FILE "${report}"
)

run_checked("${CMAKE_COMMAND}"
    "-DREPORT=${report}"
    "-DSOURCE_FILE=${SOURCE_FILE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/check_coverage.cmake"
)
message(STATUS "coverage report: ${report}")
