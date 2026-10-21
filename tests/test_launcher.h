// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_TEST_LAUNCHER_H_
#define DETACH_PROCESS_TESTS_TEST_LAUNCHER_H_

#include <sys/resource.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace detach_test {

// Describes caller state installed in a launcher without changing the runner.
struct LaunchOptions {
  // Empty selects the normal utility or the coverage override from the runner.
  std::string executable;
  std::map<std::string, std::string> environment;
  // A nonempty path gives the caller a real controlling pseudoterminal.
  std::string terminal;
  // Lower only the soft limit. Already-open descriptors above it remain open.
  rlim_t descriptor_limit = 0;
  // Bit n closes descriptor n after all other launcher setup is complete.
  int closed_standard_descriptors = 0;
  int file_creation_mask = -1;  // Negative keeps the runner's current umask.
  // Replace these streams with pipes whose readers are closed, provoking EPIPE.
  std::vector<int> broken_pipes;
  std::vector<int> ignored_signals;
  std::vector<int> blocked_signals;
};

struct LaunchResult {
  // The direct child running detach, followed by the command PID from its
  // report. Keeping both lets tests check that the command was reparented.
  pid_t launcher_pid = -1;
  pid_t program_pid = -1;
  // Raw waitpid status preserves signal information. exit_code stays -1 when
  // the launcher did not exit normally.
  int status = -1;
  int exit_code = -1;
  bool timed_out = false;
  // Captured launcher output contains reports and startup diagnostics.
  std::string output;
  std::string error;
  // These paths refer to the detached command's output, ready after ReadProbe.
  std::string stdout_path;
  std::string stderr_path;
};

// Forks a caller with the requested environment and waits for detach to return.
// The detached command keeps running independently after this function returns.
// Calls are sequential. Each call replaces its own captured report files.
LaunchResult RunDetach(const std::vector<std::string>& arguments,
                       const std::string& directory,
                       const LaunchOptions& options = {});

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_LAUNCHER_H_
