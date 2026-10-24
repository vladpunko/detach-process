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

get_filename_component(expected_source "${SOURCE_FILE}" REALPATH)
file(READ "${REPORT}" report_text)
file(READ "${REPORT}" report_hex HEX)
string(REGEX MATCHALL ".." report_bytes "${report_hex}")
if("00" IN_LIST report_bytes)
    message(FATAL_ERROR "coverage report contains a null byte: ${REPORT}")
endif()

# Preserve complete lines so malformed summaries cannot lose invalid bytes.
string(REPLACE ";" "\\;" report_text "${report_text}")
string(REPLACE "\n" ";" coverage_lines "${report_text}")

# A report for another file must never satisfy the coverage gate.
set(source_count 0)
foreach(record IN LISTS coverage_lines)
    if(record MATCHES "^SF:")
        math(EXPR source_count "${source_count} + 1")
        set(source_record "${record}")
    endif()
endforeach()
if(NOT source_count EQUAL 1)
    message(FATAL_ERROR "expected one source file in coverage report: ${source_count}")
endif()

string(REGEX REPLACE "^SF:" "" reported_source "${source_record}")
if(NOT EXISTS "${reported_source}" OR IS_DIRECTORY "${reported_source}")
    message(FATAL_ERROR "reported source is not a file: ${reported_source}")
endif()

# REALPATH resolves symlinks and normalizes relative paths from the working directory.
get_filename_component(reported_source "${reported_source}" REALPATH)
if(NOT "${reported_source}" STREQUAL "${expected_source}")
    message(FATAL_ERROR
        "coverage source does not match the requested file:\n"
        "  reported: ${reported_source}\n"
        "  expected: ${expected_source}")
endif()

# Reject missing, repeated, or malformed summaries before comparing counts.
function(read_metric key output)
    set(records "${coverage_lines}")
    list(FILTER records INCLUDE REGEX "^${key}:")
    list(LENGTH records count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "expected one ${key} count in coverage report: ${count}")
    endif()

    list(GET records 0 record)
    if(NOT "${record}" MATCHES "^${key}:([0-9]+)$")
        message(FATAL_ERROR
            "expected a nonnegative ${key} count in coverage report: ${record}")
    endif()

    # Decimal strings keep comparisons exact even beyond CMake numeric precision.
    string(REGEX REPLACE "^0+" "" value "${CMAKE_MATCH_1}")
    if("${value}" STREQUAL "")
        set(value "0")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# Empty summaries must not disguise missing instrumentation as complete coverage.
function(require_full_coverage label found_key hit_key)
    read_metric("${found_key}" found)
    read_metric("${hit_key}" hits)

    if("${found}" STREQUAL "0")
        message(FATAL_ERROR "no ${label} entries in coverage report")
    endif()
    if(NOT "${hits}" STREQUAL "${found}")
        message(FATAL_ERROR "incomplete ${label} coverage: ${hits}/${found}")
    endif()
    message(STATUS "${label} coverage: ${hits}/${found}")
endfunction()

# `LF` and `LH` count lines, `FNF` and `FNH` functions, `BRF` and `BRH` branches.
# Each pair records found items followed by hit items.
require_full_coverage("line" "LF" "LH")
require_full_coverage("function" "FNF" "FNH")
require_full_coverage("branch" "BRF" "BRH")
message(STATUS "full coverage verified: ${expected_source}")
