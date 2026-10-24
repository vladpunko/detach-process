// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_RUNNER_H_
#define TESTS_SUPPORT_RUNNER_H_

#include <sys/resource.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace detach_test {

// Configures the child process without changing the state of the test runner.
struct LaunchOptions {
    std::string executable_path;
    std::map<std::string, std::string> environment;

    // Path to the pseudoterminal used as the controlling terminal of the child.
    std::string terminal_path;

    // Soft descriptor limit. Zero preserves the inherited value.
    rlim_t descriptor_limit = 0;

    // Closes selected standard streams after setup.
    // Bitmask:
    //   `1` = `stdin`
    //   `2` = `stdout`
    //   `4` = `stderr`
    int closed_standard_descriptors = 0;

    // Negative preserves the inherited `umask`.
    int file_creation_mask = -1;

    // Pipes without readers provoke `EPIPE` and `SIGPIPE`.
    std::vector<int> broken_pipe_descriptors;

    std::vector<int> ignored_signals;
    std::vector<int> blocked_signals;
};

// Keeps the launcher status separate from the report of the detached command.
struct RunReport {
    pid_t detach_pid = -1;
    pid_t command_pid = -1;

    int status = -1;
    int exit_code = -1;
    bool is_timed_out = false;

    // Captured launcher output contains reports and startup diagnostics.
    std::string output;
    std::string error;

    std::string stdout_path;
    std::string stderr_path;
};

// Runs `detach` in a child process and captures its startup report.
// Requires `executable_path` or `DETACH_TEST_EXECUTABLE` to select the binary.
// Waits for `detach` to exit. The detached command may keep running.
// Reusing a directory overwrites the previous capture files.
RunReport RunDetach(std::vector<std::string> arguments,
                    const std::string& directory,
                    const LaunchOptions& options = {});

}  // namespace detach_test

#endif  // TESTS_SUPPORT_RUNNER_H_
