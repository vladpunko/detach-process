// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_TEST_SUPPORT_H_
#define TESTS_TEST_SUPPORT_H_

#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/probe_client.h"
#include "tests/probe_state.h"
#include "tests/test_launcher.h"

namespace detach_test {

// Coordinates one integration test from setup to cleanup.
// Run waits for detach to return. The detached probe stays alive so the test
// can inspect it, then TearDown releases the probe before deleting its files.
class DetachTest : public testing::Test {
 protected:
  // Creates an isolated directory and listens before the detached probe starts.
  void SetUp() override;
  // Stops the probe before removing files, including after failed assertions.
  void TearDown() override;

  // Keeps each test's files within its isolated directory.
  std::string Path(const std::string& name) const;
  // Locates the private control channel independently of redirected streams.
  std::string SocketPath() const;
  // Builds a probe invocation with its readiness channel and selected behavior.
  std::vector<std::string> ProbeCommand(
      const std::string& mode = "state",
      const std::vector<std::string>& arguments = {}) const;

  // Runs the exact detach command line and captures the launcher's report.
  // Default log paths are remembered for cleanup even if a later assertion
  // fails.
  LaunchResult Run(const std::vector<std::string>& arguments,
                   const LaunchOptions& options = {});

  // Starts a state-reporting probe and retains it for assertions and cleanup.
  LaunchResult RunProbe(const std::vector<std::string>& options = {},
                        const LaunchOptions& launch_options = {});

  // Runs a probe scenario such as copying stdin or writing both output streams.
  LaunchResult RunProbeMode(const std::string& mode,
                            const std::vector<std::string>& options = {},
                            const std::vector<std::string>& arguments = {},
                            const LaunchOptions& launch_options = {});

  // Waits for the probe to finish its selected I/O and report its process
  // state. Read this before checking output files so their contents are
  // complete.
  ProbeState ReadProbe();
  // Confirms that the detached command is still responsive after an event such
  // as closing its original terminal or encountering a launcher report error.
  void PingProbe();

 private:
  std::string directory_;
  ProbeClient probe_;
  std::set<std::string> temporary_logs_;
};

}  // namespace detach_test

#endif  // TESTS_TEST_SUPPORT_H_
