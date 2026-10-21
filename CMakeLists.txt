# -*- coding: utf-8 -*-

# Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
# Created date: 2026-10-21

cmake_minimum_required(VERSION 3.8...3.16)

project(detach C)

# Create a new program on the current machine.
add_executable(detach detach.c)

# Keep the standalone C build independent of the test dependencies.
option(BUILD_TESTING "Build the GoogleTest integration suite" OFF)
if(BUILD_TESTING)
    cmake_minimum_required(VERSION 3.16)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(tests)
endif()
