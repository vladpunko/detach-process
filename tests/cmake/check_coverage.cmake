# -*- coding: utf-8 -*-

cmake_minimum_required(VERSION 3.16)

if(NOT EXISTS "${REPORT}" OR IS_DIRECTORY "${REPORT}")
    message(FATAL_ERROR "set REPORT to an LCOV report file: ${REPORT}")
endif()

if(NOT DEFINED SOURCE_FILE)
    set(SOURCE_FILE "${CMAKE_CURRENT_LIST_DIR}/../../detach.c")
endif()

if(NOT EXISTS "${SOURCE_FILE}" OR IS_DIRECTORY "${SOURCE_FILE}")
    message(FATAL_ERROR "coverage source is not a file: ${SOURCE_FILE}")
endif()

file(READ "${REPORT}" report_text)
file(READ "${REPORT}" report_hex HEX)
string(REGEX MATCHALL ".." report_bytes "${report_hex}")
if("00" IN_LIST report_bytes)
    message(FATAL_ERROR "coverage report contains a null byte: ${REPORT}")
endif()

# Preserve complete lines so malformed summaries cannot lose invalid bytes.
string(REPLACE ";" "\\;" report_text "${report_text}")
string(REPLACE "\n" ";" report_lines "${report_text}")

# A report for another file must never satisfy the coverage gate.
set(source_count 0)
foreach(line IN LISTS report_lines)
    if(line MATCHES "^SF:(.*)$")
        math(EXPR source_count "${source_count} + 1")
        set(reported_source "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT source_count EQUAL 1)
    message(FATAL_ERROR "expected one source file in coverage report: ${source_count}")
endif()

if(NOT EXISTS "${reported_source}" OR IS_DIRECTORY "${reported_source}")
    message(FATAL_ERROR "reported source is not a file: ${reported_source}")
endif()

# REALPATH resolves symlinks and normalizes relative paths from the working directory.
get_filename_component(expected_source "${SOURCE_FILE}" REALPATH)
get_filename_component(reported_source "${reported_source}" REALPATH)
if(NOT "${reported_source}" STREQUAL "${expected_source}")
    message(FATAL_ERROR
        "coverage source does not match the requested file:\n"
        "  reported: ${reported_source}\n"
        "  expected: ${expected_source}"
    )
endif()

# Reads one summary count and rejects missing, repeated or malformed fields.
function(read_count lines field output_count)
    list(FILTER lines INCLUDE REGEX "^${field}:")
    list(LENGTH lines record_count)
    if(NOT record_count EQUAL 1)
        message(FATAL_ERROR
            "expected one ${field} count in coverage report: ${record_count}"
        )
    endif()

    list(GET lines 0 record)
    if(NOT "${record}" MATCHES "^${field}:([0-9]+)$")
        message(FATAL_ERROR
            "expected a nonnegative ${field} count in coverage report: ${record}"
        )
    endif()

    # Decimal strings keep comparisons exact even beyond CMake numeric precision.
    string(REGEX REPLACE "^0+" "" count "${CMAKE_MATCH_1}")
    if("${count}" STREQUAL "")
        set(count "0")
    endif()
    set(${output_count} "${count}" PARENT_SCOPE)
endfunction()

# Rejects empty or partial coverage so missing instrumentation cannot pass.
function(require_full_coverage lines label total_field covered_field)
    read_count("${lines}" "${total_field}" total)
    read_count("${lines}" "${covered_field}" covered)

    if("${total}" STREQUAL "0")
        message(FATAL_ERROR "no ${label} entries in coverage report")
    endif()
    if(NOT "${covered}" STREQUAL "${total}")
        message(FATAL_ERROR "incomplete ${label} coverage: ${covered}/${total}")
    endif()
    message(STATUS "${label} coverage: ${covered}/${total}")
endfunction()

# `LF` and `LH` count lines, `FNF` and `FNH` functions, `BRF` and `BRH` branches.
# Each pair records found items followed by hit items.
require_full_coverage("${report_lines}" "line" "LF" "LH")
require_full_coverage("${report_lines}" "function" "FNF" "FNH")
require_full_coverage("${report_lines}" "branch" "BRF" "BRH")
message(STATUS "full coverage verified: ${expected_source}")
