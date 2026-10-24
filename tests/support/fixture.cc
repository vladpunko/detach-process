// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/support/fixture.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/io.h"

namespace detach_test {

// Prepares isolated files and a control connection for this test.
void DetachTest::SetUp() {
    ASSERT_NO_FATAL_FAILURE(SetUpProbePath());
    ASSERT_NO_FATAL_FAILURE(SetUpDirectory());
    probe_.Listen(SocketPath());
}

// Rejects missing probe configuration before creating test resources.
void DetachTest::SetUpProbePath() {
    const char* executable_path = getenv("DETACH_TEST_PROBE_EXECUTABLE");
    ASSERT_TRUE(executable_path != nullptr && executable_path[0] != '\0')
        << "set DETACH_TEST_PROBE_EXECUTABLE to the probe executable path";
    probe_executable_path_ = executable_path;
}

// Creates the directory removed after the probe has stopped.
void DetachTest::SetUpDirectory() {
    char directory_template[] = "/tmp/detach-test.XXXXXX";
    const char* created_path = mkdtemp(directory_template);
    const int error = errno;
    ASSERT_NE(created_path, nullptr)
        << "cannot create test directory: "
        << std::strerror(error);
    directory_ = created_path;
}

// Releases the probe before removing files that it may still be using.
void DetachTest::TearDown() {
    probe_.Close();

    // Default logs are created outside the test directory.
    for (const auto& path : default_logs_) {
        const int status = unlink(path.c_str());
        EXPECT_TRUE(CheckSysCall(status, "remove default log"))
            << path;
    }

    if (directory_.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error)
        << "cannot remove test directory: "
        << directory_
        << ": "
        << error.message();
}

// Keeps test files inside the directory removed during cleanup.
std::string DetachTest::Path(const std::string& name) const {
    return directory_ + "/" + name;
}

// Keeps the control connection separate from standard stream redirection.
std::string DetachTest::SocketPath() const {
    return Path("probe.socket");
}

// Places the mode and control socket before arguments handled by the probe.
std::vector<std::string> DetachTest::ProbeCommand(
    const std::string& mode,
    const std::vector<std::string>& arguments) const {
    std::vector<std::string> command = {probe_executable_path_, mode, SocketPath()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

// Captures the startup report and tracks logs outside the test directory.
RunReport DetachTest::Run(const std::vector<std::string>& arguments,
                          const LaunchOptions& options) {
    auto report = RunDetach(arguments, directory_, options);

    for (const auto& path : {report.stdout_path, report.stderr_path}) {
        if (path.rfind("/tmp/stdout.", 0) == 0 || path.rfind("/tmp/stderr.", 0) == 0) {
            default_logs_.insert(path);
        }
    }
    return report;
}

// Selects the state-reporting mode used by process and signal checks.
RunReport DetachTest::RunProbe(const std::vector<std::string>& options,
                               const LaunchOptions& launch_options) {
    return RunProbeMode("state", options, {}, launch_options);
}

// Starts the probe and remembers its PID before assertions can stop the test.
RunReport DetachTest::RunProbeMode(const std::string& mode,
                                   const std::vector<std::string>& options,
                                   const std::vector<std::string>& arguments,
                                   const LaunchOptions& launch_options) {
    auto command = options;
    command.push_back("--");

    const auto probe_command = ProbeCommand(mode, arguments);
    command.insert(command.end(), probe_command.begin(), probe_command.end());

    auto report = Run(command, launch_options);
    probe_.ExpectProcess(report.command_pid);
    return report;
}

// reads process state after the probe finishes writing its output
ProbeState DetachTest::ReadProbe() {
    return probe_.ReadState();
}

// Checks the running probe through its private control connection.
void DetachTest::PingProbe() {
    probe_.Ping();
}

}  // namespace detach_test
