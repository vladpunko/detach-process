// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

using ProcessTest = DetachTest;

// Session and process lifetime.

TEST_F(ProcessTest, CommandRunsInANewSessionWithoutBeingItsLeader) {
    const auto report = RunProbe();

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.pid, report.command_pid);
    EXPECT_NE(state.parent_pid, report.detach_pid);
    EXPECT_NE(state.parent_pid, getpid());
    EXPECT_NE(state.session_id, getsid(0));
    EXPECT_NE(state.session_id, state.pid);
    EXPECT_EQ(state.process_group, state.session_id);
    EXPECT_EQ(state.has_terminal, 0);
}

TEST_F(ProcessTest, LauncherReturnsWhileTheCommandIsStillRunning) {
    const auto report = RunProbe();

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    // The probe stays alive until the fixture closes its control socket.
    PingProbe();
}

// Command lookup and scripts.

TEST_F(ProcessTest, LooksUpTheCommandInPath) {
    auto arguments = ProbeCommand();
    const std::string link_path = Path("probe-command");
    const int status = symlink(arguments.front().c_str(), link_path.c_str());
    ASSERT_TRUE(CheckSysCall(status, "create command link"));

    // A relative command name makes `execvp` search `PATH`.
    arguments.front() = "probe-command";
    arguments.insert(arguments.begin(), "--");

    const LaunchOptions options{.environment = {{"PATH", Path(".")}}};

    const auto report = Run(arguments, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().pid, report.command_pid);
}

TEST_F(ProcessTest, MissingCommandInPathIsAnExecFailure) {
    const LaunchOptions options{.environment = {{"PATH", Path(".")}}};

    const auto report = Run({"--", "no-such-command"}, options);

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", report.error);
}

TEST_F(ProcessTest, ExecutableScriptRunsItsInterpreter) {
    // Replace the shell with the probe so the reported PID stays verifiable.
    ASSERT_NO_FATAL_FAILURE(Write(Path("command"), "#!/bin/sh\nexec \"$@\"\n"));
    ASSERT_TRUE(CheckSysCall(chmod(Path("command").c_str(), 0700), "chmod script"));

    auto arguments = ProbeCommand();
    arguments.insert(arguments.begin(), {"--", Path("command")});

    const auto report = Run(arguments);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().pid, report.command_pid);
    PingProbe();
}

TEST_F(ProcessTest, MissingScriptInterpreterIsAStartupFailure) {
    ASSERT_NO_FATAL_FAILURE(
        Write(Path("command"), "#!" + Path("missing-interpreter") + "\n"));
    ASSERT_TRUE(CheckSysCall(chmod(Path("command").c_str(), 0700), "chmod script"));

    const auto report = Run({"--", Path("command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", report.error);
}

// Inherited process state.

TEST_F(ProcessTest, PreservesEnvironmentVariables) {
    const LaunchOptions options{
        .environment = {{"DETACH_TEST_VALUE", "value with spaces\nand newline"}},
    };

    const auto report = RunProbeMode("environment", {}, {"DETACH_TEST_VALUE"}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(Read(report.stdout_path), options.environment.at("DETACH_TEST_VALUE"));
}

TEST_F(ProcessTest, PreservesEmptyEnvironmentVariables) {
    const LaunchOptions options{.environment = {{"DETACH_TEST_VALUE", ""}}};

    const auto report = RunProbeMode("environment", {}, {"DETACH_TEST_VALUE"}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_TRUE(Read(report.stdout_path).empty());
    EXPECT_TRUE(Read(report.stderr_path).empty());
}

TEST_F(ProcessTest, PreservesTheWorkingDirectory) {
    const auto report = RunProbeMode("cwd");

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    // Directory identity handles the `/tmp` symlink on macOS.
    const std::string directory_path = Read(report.stdout_path);
    std::error_code error;
    const bool is_same_directory = std::filesystem::equivalent(
        directory_path, Path("."), error);

    EXPECT_FALSE(error)
        << "cannot compare working directories: "
        << error.message();
    EXPECT_TRUE(is_same_directory);
}

TEST_F(ProcessTest, PreservesTheFileCreationMask) {
    const LaunchOptions options{.file_creation_mask = 0027};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().creation_mask, 0027);
}

// Descriptor cleanup and limits.

TEST_F(ProcessTest, ClosesInternalDescriptorsBeforeExec) {
    const auto report = RunProbe({"--pid", Path("pid")});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

TEST_F(ProcessTest, ClosesInheritedDescriptorsWithoutClosingTheCallersCopies) {
    FileDescriptor inherited(open(Path("inherited").c_str(), O_CREAT | O_RDWR, 0600));
    ASSERT_TRUE(CheckSysCall(inherited.get(), "open inherited descriptor"));

    const auto report = RunProbe();

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
    EXPECT_GE(fcntl(inherited.get(), F_GETFD), 0);
}

TEST_F(ProcessTest, ClosesDescriptorsAboveTheCurrentSoftLimit) {
    FileDescriptor source(open("/dev/null", O_RDONLY));
    ASSERT_TRUE(CheckSysCall(source.get(), "open /dev/null"));

    // Enumeration must find a descriptor above the limit inherited by the child.
    FileDescriptor inherited(fcntl(source.get(), F_DUPFD, 128));
    ASSERT_TRUE(CheckSysCall(inherited.get(), "dup fd"));

    const std::string descriptor_number = std::to_string(inherited.get());

    const LaunchOptions options{.descriptor_limit = 32};

    const auto report = RunProbeMode("descriptor", {}, {descriptor_number}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.descriptor_open, 0);
    EXPECT_EQ(state.extra_descriptors, 0);
    EXPECT_GE(fcntl(inherited.get(), F_GETFD), 0);
}

TEST_F(ProcessTest, DescriptorExhaustionIsReportedAsAStartupFailure) {
    // One free descriptor cannot hold both ends of the startup socket.
    const LaunchOptions options{.descriptor_limit = 7};
    const std::vector<std::string> file_options = {
        "--stdout", Path("output"),
        "--stderr", Path("errors"),
    };

    const auto report = RunProbe(file_options, options);

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "startup channel:", report.error);
    EXPECT_TRUE(Read(Path("output")).empty());
    EXPECT_TRUE(Read(Path("errors")).empty());
}

// Closed standard streams.

class ClosedStandardDescriptorsTest : public DetachTest,
                                      public testing::WithParamInterface<int> {
};

TEST_P(ClosedStandardDescriptorsTest, SuppliesValidRedirectedStreams) {
    const LaunchOptions options{.closed_standard_descriptors = GetParam()};
    const std::vector<std::string> file_options = {
        "--stdout", Path("output"),
        "--stderr", Path("errors"),
        "--pid", Path("pid"),
    };

    const auto report = RunProbeMode("stdio", file_options, {}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.stdin_is_null, 1);
    EXPECT_EQ(state.extra_descriptors, 0);
    EXPECT_TRUE(Read(Path("output")).empty());
    EXPECT_EQ(Read(Path("errors")), "probe stderr\n");
    EXPECT_EQ(Read(Path("pid")), std::to_string(state.pid) + "\n");
}

// Names each combination of closed standard streams in test reports.
std::string ClosedDescriptorName(const testing::TestParamInfo<int>& info) {
    const char* names[] = {
        "None",   "Stdin",       "Stdout",       "StdinStdout",
        "Stderr", "StdinStderr", "StdoutStderr", "StdinStdoutStderr",
    };
    return names[info.param];
}

INSTANTIATE_TEST_SUITE_P(StandardStreams,
                         ClosedStandardDescriptorsTest,
                         testing::Range(1, 8),
                         ClosedDescriptorName);

}  // namespace
}  // namespace detach_test
