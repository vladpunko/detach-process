# -*- coding: utf-8 -*-

# Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
# Created date: 2026-10-21

if(NOT DEFINED REPORT OR NOT EXISTS "${REPORT}")
    message(FATAL_ERROR "Pass the LLVM LCOV report with -DREPORT=path")
endif()
file(READ "${REPORT}" coverage)

# Require exactly the production source so unrelated files cannot hide a gap.
string(REGEX MATCHALL "(^|\n)SF:[^\n]+" sources "${coverage}")
list(LENGTH sources source_count)
if(NOT source_count EQUAL 1 OR NOT sources MATCHES "/detach[.]c$")
    message(FATAL_ERROR "The report must contain only detach.c")
endif()

function(require_full_coverage label found_key hit_key)
    string(REGEX MATCH "(^|\n)${found_key}:([0-9]+)" found "${coverage}")
    set(total "${CMAKE_MATCH_2}")
    string(REGEX MATCH "(^|\n)${hit_key}:([0-9]+)" hit "${coverage}")
    set(covered "${CMAKE_MATCH_2}")
    if(NOT found OR NOT hit OR total LESS 1 OR NOT total EQUAL covered)
        message(FATAL_ERROR "${label}: ${covered}/${total}, expected 100%")
    endif()
    message(STATUS "${label}: ${covered}/${total} (100%)")
endfunction()

require_full_coverage("Lines" "LF" "LH")
require_full_coverage("Functions" "FNF" "FNH")
require_full_coverage("Branches" "BRF" "BRH")
