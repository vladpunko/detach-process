// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_TEST_SUPPORT_H_
#define DETACH_PROCESS_TESTS_TEST_SUPPORT_H_

#include <sys/resource.h>
#include <sys/types.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/probe_state.h"

namespace detach_test {

// Owns a descriptor so failed assertions still release sockets and terminals.
class Descriptor {
 public:
  explicit Descriptor(int fd = -1) : fd_(fd) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor();
  int get() const { return fd_; }
  void Reset(int fd = -1);

 private:
  int fd_;
};

// Describes caller state installed in a launcher without changing the runner.
struct LaunchOptions {
  std::string executable;
  std::map<std::string, std::string> environment;
  std::string terminal;
  rlim_t descriptor_limit = 0;
  // Bit n closes descriptor n after all other launcher setup is complete.
  int closed_standard_descriptors = 0;
  int file_creation_mask = -1;  // Negative keeps the runner's current umask.
  std::vector<int> broken_pipes;
  std::vector<int> ignored_signals;
  std::vector<int> blocked_signals;
};

struct LaunchResult {
  pid_t launcher_pid = -1;
  pid_t program_pid = -1;
  int status = -1;
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
  std::string error;
  std::string stdout_path;
  std::string stderr_path;
};

// Describes a failed POSIX call for ASSERT_TRUE or EXPECT_TRUE.
testing::AssertionResult CheckCall(int result, const char* operation);
// Returns file contents and records a test failure if reading fails.
std::string ReadFile(const std::string& path);
// Drains a nonblocking pipe after the probe confirms its writes are complete.
std::string ReadAvailable(int fd);
void WriteFile(const std::string& path, const std::string& contents);
// Checks existence and records filesystem errors instead of hiding them.
bool FileExists(const std::string& path);
mode_t FileMode(const std::string& path);

class DetachTest : public testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  std::string Path(const std::string& name) const;
  std::string SocketPath() const;
  std::vector<std::string> ProbeCommand(
      const std::string& mode = "state",
      const std::vector<std::string>& arguments = {}) const;
  LaunchResult Run(const std::vector<std::string>& arguments,
                   const LaunchOptions& options = {});
  LaunchResult RunProbe(const std::vector<std::string>& options = {},
                        const std::string& mode = "state",
                        const std::vector<std::string>& arguments = {},
                        const LaunchOptions& launch_options = {});

  // The probe reports only after its output is flushed, then waits for us.
  ProbeState ReadProbe();
  void PingProbe();

 private:
  testing::AssertionResult ReadProbeState(ProbeState& state);
  testing::AssertionResult StopProbe();

  std::string directory_;
  // Accept one report, then use the peer for liveness checks and shutdown.
  Descriptor listener_;
  Descriptor peer_;
  std::set<std::string> temporary_logs_;
  pid_t probe_pid_ = -1;
  // A successful report still needs cleanup if an assertion skipped accept.
  bool probe_expected_ = false;
  unsigned int launch_count_ = 0;
};

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_SUPPORT_H_
