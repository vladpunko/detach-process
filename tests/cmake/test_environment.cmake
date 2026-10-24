# -*- coding: utf-8 -*-

# Set executable paths for each discovered test.
set(test_environment
    "DETACH_TEST_EXECUTABLE=${detach_executable}"
    "DETACH_TEST_PROBE_EXECUTABLE=$<TARGET_FILE:detach_probe>"
    "DETACH_TEST_FAULT_EXECUTABLE=$<TARGET_FILE:detach_faults>"
)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test_environment-$<CONFIG>.cmake"
    CONTENT "if(detach_tests_TESTS)
    set_tests_properties(\${detach_tests_TESTS}
        PROPERTIES ENVIRONMENT \"${test_environment}\"
    )
endif()
"
)

# Choose executable paths for the selected build configuration.
if(CMAKE_CONFIGURATION_TYPES)
    set(test_configuration "\${CTEST_CONFIGURATION_TYPE}")
else()
    set(test_configuration "${CMAKE_BUILD_TYPE}")
endif()
set(configuration_file
    "${CMAKE_CURRENT_BINARY_DIR}/test_environment-${test_configuration}.cmake"
)
set(environment_loader "${CMAKE_CURRENT_BINARY_DIR}/test_environment.cmake")
file(WRITE "${environment_loader}" "include(\"${configuration_file}\")\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${environment_loader}")
