// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_TEST_SUPPORT_H_
#define DETACH_PROCESS_TESTS_TEST_SUPPORT_H_

#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/probe_client.h"
#include "tests/probe_state.h"
#include "tests/test_launcher.h"

namespace detach_test {

// Gives each test its own directory, launcher captures and controlled probe.
class DetachTest : public testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  std::string Path(const std::string& name) const;
  std::string SocketPath() const;
  std::vector<std::string> ProbeCommand(
      const std::string& mode = "state",
      const std::vector<std::string>& arguments = {}) const;

  // Runs detach with the exact command line supplied by the test.
  LaunchResult Run(const std::vector<std::string>& arguments,
                   const LaunchOptions& options = {});

  // Starts the probe that reports process state and waits for cleanup.
  LaunchResult RunProbe(const std::vector<std::string>& options = {},
                        const LaunchOptions& launch_options = {});

  // Runs a probe scenario such as copying stdin or writing both output streams.
  LaunchResult RunProbeMode(const std::string& mode,
                            const std::vector<std::string>& options = {},
                            const std::vector<std::string>& arguments = {},
                            const LaunchOptions& launch_options = {});

  ProbeState ReadProbe();
  void PingProbe();

 private:
  std::string directory_;
  ProbeClient probe_;
  std::set<std::string> temporary_logs_;
};

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_SUPPORT_H_
