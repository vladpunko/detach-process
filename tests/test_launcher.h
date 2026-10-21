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

// Runs detach in directory and waits for its launcher to finish.
// Calls are sequential. Each call replaces its own captured report files.
LaunchResult RunDetach(const std::vector<std::string>& arguments,
                       const std::string& directory,
                       const LaunchOptions& options = {});

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_LAUNCHER_H_
