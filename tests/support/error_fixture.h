// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#ifndef TESTS_SUPPORT_ERROR_FIXTURE_H_
#define TESTS_SUPPORT_ERROR_FIXTURE_H_

#include <initializer_list>
#include <iosfwd>
#include <set>
#include <string>
#include <vector>

#include "tests/support/fixture.h"
#include "tests/support/runner.h"

namespace detach_test {

enum class ProcessRole { kLauncher, kIntermediate, kCommand };

// Selects a numbered call in one process and the result to simulate.
struct FaultRule {
    const char* operation;
    ProcessRole role;
    unsigned int call_number;
    int result;
};

// Runs failure scenarios and checks that every planned fault was applied.
// Requires `DETACH_TEST_FAULT_EXECUTABLE` to select the instrumented binary.
class ErrorTest : public DetachTest {
protected:
    // Creates private logs before any fault can be triggered.
    void SetUp() override;

    // Stops the probe, verifies applied faults and removes leftover logs.
    void TearDown() override;

    // Selects the instrumented executable and its recording files.
    LaunchOptions FaultOptions() const;

    // Records the faults that must occur when these launch options are used.
    LaunchOptions Inject(std::initializer_list<FaultRule> rules);

    // Builds launch options for a single planned fault.
    LaunchOptions Inject(const FaultRule& rule);

    // Checks that the recorded temporary logs were removed by `detach`.
    void ExpectTemporaryFilesRemoved() const;

private:
    // Stores the executable path before creating test resources.
    void SetUpExecutable();

    // Reads log paths before base cleanup removes the event record.
    std::vector<std::string> TemporaryLogPaths() const;

    // Compares applied faults and their counts independently of process scheduling.
    void ExpectAppliedFaults(std::istream& log) const;

    std::string fault_executable_path_;
    bool event_log_ready_ = false;
    bool fault_log_ready_ = false;
    std::multiset<std::string> expected_faults_;
};

}  // namespace detach_test

#endif  // TESTS_SUPPORT_ERROR_FIXTURE_H_
