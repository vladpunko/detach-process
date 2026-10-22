# -*- coding: utf-8 -*-

cmake_minimum_required(VERSION 3.8...3.16)

project(detach C)

add_executable(detach detach.c)
set_target_properties(detach PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

# Load test dependencies only when tests are enabled.
option(BUILD_TESTING "Build the test suite" OFF)
if(BUILD_TESTING)
    cmake_minimum_required(VERSION 3.16)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(tests)
endif()
