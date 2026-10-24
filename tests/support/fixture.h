// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_FIXTURE_H_
#define TESTS_SUPPORT_FIXTURE_H_

#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/probe_state.h"
#include "tests/support/process_probe.h"
#include "tests/support/runner.h"

namespace detach_test {

// Gives each test its own files and control connection to the probe.
// Requires `DETACH_TEST_PROBE_EXECUTABLE` to select the probe binary.
class DetachTest : public testing::Test {
protected:
    // Creates an isolated directory and listens before the probe starts.
    void SetUp() override;

    // Stops the probe before removing files, including after failed assertions.
    void TearDown() override;

    // Builds a path inside the directory owned by this test.
    std::string Path(const std::string& name) const;

    // Locates the control socket independently of the redirected streams.
    std::string SocketPath() const;

    // Builds a probe command with its mode, control socket and arguments.
    std::vector<std::string> ProbeCommand(const std::string& mode = "state",
                                          const std::vector<std::string>& arguments
                                          = {}) const;

    // Runs `detach` and remembers default log file paths for cleanup.
    RunReport Run(const std::vector<std::string>& arguments,
                  const LaunchOptions& options = {});

    // Starts a probe that reports process state and waits for test requests.
    RunReport RunProbe(const std::vector<std::string>& options = {},
                       const LaunchOptions& launch_options = {});

    // Runs a probe mode and keeps its process available for checks and cleanup.
    RunReport RunProbeMode(const std::string& mode,
                           const std::vector<std::string>& options = {},
                           const std::vector<std::string>& arguments = {},
                           const LaunchOptions& launch_options = {});

    // Waits for completed probe output before returning its process state.
    ProbeState ReadProbe();

    // Confirms that the probe still responds after the event under test.
    void PingProbe();

private:
    // Validates and stores the executable path before creating test resources.
    void SetUpProbePath();

    // Creates the temporary directory owned by this test.
    void SetUpDirectory();

    std::string directory_;
    std::string probe_executable_path_;
    ProcessProbe probe_;
    std::set<std::string> default_logs_;
};

}  // namespace detach_test

#endif  // TESTS_SUPPORT_FIXTURE_H_
