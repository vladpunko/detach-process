cmake_minimum_required(VERSION 3.16)

# Runs one coverage step and stops immediately if it fails, preventing later
# reports from hiding a failed test or tool invocation.
function(run_checked)
    execute_process(COMMAND ${ARGV}
        WORKING_DIRECTORY "${BUILD_DIR}"
        RESULT_VARIABLE status
    )
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Coverage command failed (${status}): ${ARGV}")
    endif()
endfunction()

# Never let profiles or reports from an earlier run hide a coverage gap.
set(profile_directory "${BUILD_DIR}/profiles")
set(profile_data "${BUILD_DIR}/coverage.profdata")
set(REPORT "${BUILD_DIR}/coverage.lcov")
file(REMOVE_RECURSE "${profile_directory}")
file(REMOVE "${profile_data}" "${REPORT}")
file(MAKE_DIRECTORY "${profile_directory}")

set(ctest_arguments --output-on-failure --parallel 2)
if(NOT "${CONFIG}" STREQUAL "")
    list(APPEND ctest_arguments -C "${CONFIG}")
endif()
run_checked("${CMAKE_COMMAND}" -E env
    "LLVM_PROFILE_FILE=${profile_directory}/%m-%p.profraw"
    "DETACH_TEST_EXECUTABLE=${FAULT_EXECUTABLE}"
    "${CTEST_COMMAND}" ${ctest_arguments}
)

file(GLOB profiles "${profile_directory}/*.profraw")
if(NOT profiles)
    message(FATAL_ERROR "No coverage profiles were generated")
endif()
run_checked("${LLVM_PROFDATA}" merge -sparse ${profiles} -o "${profile_data}")
run_checked("${LLVM_COV}" report "${FAULT_EXECUTABLE}"
    "-instr-profile=${profile_data}" "${SOURCE_DIR}/detach.c"
)
execute_process(
    COMMAND "${LLVM_COV}" export "${FAULT_EXECUTABLE}"
        "-instr-profile=${profile_data}" -format=lcov "${SOURCE_DIR}/detach.c"
    OUTPUT_FILE "${REPORT}"
    RESULT_VARIABLE status
)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Coverage export failed (${status})")
endif()
include("${SOURCE_DIR}/tests/check_coverage.cmake")
message(STATUS "LCOV report: ${REPORT}")
