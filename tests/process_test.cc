// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using ProcessTest = DetachTest;

TEST_F(ProcessTest, CommandRunsInANewSessionWithoutBeingItsLeader) {
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.pid, result.program_pid);
  EXPECT_NE(state.parent_pid, result.launcher_pid);
  EXPECT_NE(state.parent_pid, getpid());
  EXPECT_NE(state.session_id, getsid(0));
  EXPECT_NE(state.session_id, state.pid);
  EXPECT_EQ(state.process_group, state.session_id);
  EXPECT_EQ(state.has_terminal, 0);
}

TEST_F(ProcessTest, LauncherReturnsWhileTheCommandIsStillRunning) {
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  // The probe cannot finish until the fixture closes its control socket.
  PingProbe();
}

TEST_F(ProcessTest, ReportsStartupSuccessEvenWhenTheCommandExitsWithAnError) {
  const auto result = Run({"--", "/bin/sh", "-c", "exit 42"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_GT(result.program_pid, 0);
}

TEST_F(ProcessTest, ReportsStartupSuccessEvenWhenTheCommandKillsItself) {
  const auto result = Run({"--", "/bin/sh", "-c", "kill -TERM $$"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_GT(result.program_pid, 0);
}

TEST_F(ProcessTest, LooksUpTheCommandInPath) {
  ASSERT_TRUE(
      CheckCall(symlink(PROBE_PATH, Path("probe-command").c_str()), "symlink"));
  LaunchOptions options;
  options.environment["PATH"] = Path(".");
  const auto result =
      Run({"--", "probe-command", "state", SocketPath()}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
}

TEST_F(ProcessTest, MissingCommandInPathIsAnExecFailure) {
  LaunchOptions options;
  options.environment["PATH"] = Path(".");
  const auto result = Run({"--", "no-such-command"}, options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("exec:"), std::string::npos);
}

TEST_F(ProcessTest, ExecutableScriptRunsItsInterpreter) {
  // An explicit interpreter works across macOS, glibc and musl.
  // Replace the shell with the probe so the reported PID remains verifiable.
  WriteFile(Path("command"), "#!/bin/sh\nexec \"${@}\"\n");
  ASSERT_TRUE(CheckCall(chmod(Path("command").c_str(), 0700), "chmod script"));
  const auto result =
      Run({"--", Path("command"), PROBE_PATH, "state", SocketPath()});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
  PingProbe();
}

TEST_F(ProcessTest, MissingScriptInterpreterIsAStartupFailure) {
  WriteFile(Path("command"), "#!" + Path("missing-interpreter") + "\n");
  ASSERT_TRUE(CheckCall(chmod(Path("command").c_str(), 0700), "chmod script"));
  const auto result = Run({"--", Path("command")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("exec:"), std::string::npos) << result.error;
}

TEST_F(ProcessTest, PreservesEnvironmentVariables) {
  LaunchOptions options;
  options.environment["DETACH_TEST_VALUE"] = "value with spaces\nand newline";
  const auto result =
      RunProbeMode("environment", {}, {"DETACH_TEST_VALUE"}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(result.stdout_path),
            options.environment.at("DETACH_TEST_VALUE"));
}

TEST_F(ProcessTest, PreservesTheWorkingDirectory) {
  const auto result = RunProbeMode("cwd");

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  // macOS resolves the /tmp symlink when reporting the current directory.
  const auto directory = ReadFile(result.stdout_path);
  std::error_code error;
  EXPECT_TRUE(std::filesystem::equivalent(directory, Path("."), error));
  EXPECT_FALSE(error) << error.message();
}

TEST_F(ProcessTest, PreservesTheFileCreationMask) {
  LaunchOptions options;
  options.file_creation_mask = 0027;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().creation_mask, 0027);
}

TEST_F(ProcessTest, ClosesInternalDescriptorsBeforeExec) {
  const auto result = RunProbe({"--pid", Path("pid")});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

TEST_F(ProcessTest, ClosesInheritedDescriptorsWithoutClosingTheCallersCopies) {
  Descriptor inherited(open(Path("inherited").c_str(), O_CREAT | O_RDWR, 0600));
  ASSERT_TRUE(CheckCall(inherited.get(), "open inherited descriptor"));
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().extra_descriptors, 0);
  EXPECT_GE(fcntl(inherited.get(), F_GETFD), 0);
}

TEST_F(ProcessTest, ClosesDescriptorsAboveTheCurrentSoftLimit) {
  Descriptor source(open("/dev/null", O_RDONLY));
  ASSERT_TRUE(CheckCall(source.get(), "open /dev/null"));
  Descriptor inherited(fcntl(source.get(), F_DUPFD, 128));
  ASSERT_TRUE(CheckCall(inherited.get(), "dup fd"));
  LaunchOptions options;
  options.descriptor_limit = 32;
  const auto result = RunProbeMode("descriptor", {},
                                   {std::to_string(inherited.get())}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.descriptor_open, 0);
  EXPECT_EQ(state.extra_descriptors, 0);
  EXPECT_GE(fcntl(inherited.get(), F_GETFD), 0);
}

TEST_F(ProcessTest, KeepsTheCallersDefaultSigpipeDisposition) {
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_ignored, 0);
}

TEST_F(ProcessTest, KeepsTheCallersIgnoredSigpipeDisposition) {
  LaunchOptions options;
  options.ignored_signals = {SIGPIPE};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_ignored, 1);
}

TEST_F(ProcessTest, KeepsTheCallersBlockedSigpipe) {
  LaunchOptions options;
  options.blocked_signals = {SIGPIPE};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_blocked, 1);
}

TEST_F(ProcessTest, KeepsOtherBlockedSignals) {
  LaunchOptions options;
  options.blocked_signals = {SIGUSR1};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigusr1_blocked, 1);
}

TEST_F(ProcessTest, WorksWhenTheCallerIgnoresSigchld) {
  LaunchOptions options;
  options.ignored_signals = {SIGCHLD};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigchld_ignored, 1);
}

TEST_F(ProcessTest, KeepsTheCallersIgnoredSighupDisposition) {
  LaunchOptions options;
  options.ignored_signals = {SIGHUP};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sighup_ignored, 1);
}

TEST_F(ProcessTest, BrokenReportPipeDoesNotStopTheStartedCommand) {
  LaunchOptions options;
  options.broken_pipes = {STDOUT_FILENO};
  const auto result = RunProbeMode("streams", {}, {}, options);

  ASSERT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("The command may already be running"),
            std::string::npos);
  const auto state = ReadProbe();
  EXPECT_EQ(state.pid, result.program_pid);
  EXPECT_EQ(state.sigpipe_ignored, 0);
  EXPECT_EQ(ReadFile(result.stdout_path), "stdout one\nstdout two\n");
  PingProbe();
}

TEST_F(ProcessTest, BrokenDiagnosticPipeDoesNotInterruptExecFailureHandling) {
  LaunchOptions options;
  options.broken_pipes = {STDERR_FILENO};
  const auto result = Run({"--", Path("missing-command")}, options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(WIFEXITED(result.status));
}

TEST_F(ProcessTest, HelpReportsFailureWhenItsOutputCannotBeWritten) {
  LaunchOptions options;
  options.broken_pipes = {STDOUT_FILENO};
  const auto result = Run({"--help"}, options);

  EXPECT_NE(result.exit_code, 0);
}

TEST_F(ProcessTest, DescriptorExhaustionIsReportedAsAStartupFailure) {
  LaunchOptions options;
  options.descriptor_limit = 7;
  const auto result = RunProbe(
      {"--stdout", Path("output"), "--stderr", Path("errors")}, options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("startup channel:"), std::string::npos);
  EXPECT_TRUE(ReadFile(Path("output")).empty());
  EXPECT_TRUE(ReadFile(Path("errors")).empty());
}

class ClosedStandardDescriptorsTest : public DetachTest,
                                      public testing::WithParamInterface<int> {
};

TEST_P(ClosedStandardDescriptorsTest, SuppliesValidRedirectedStreams) {
  LaunchOptions options;
  options.closed_standard_descriptors = GetParam();
  const auto result = RunProbeMode("stdio",
                                   {"--stdout", Path("output"), "--stderr",
                                    Path("errors"), "--pid", Path("pid")},
                                   {}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.stdin_is_null, 1);
  EXPECT_EQ(state.extra_descriptors, 0);
  EXPECT_TRUE(ReadFile(Path("output")).empty());
  EXPECT_EQ(ReadFile(Path("errors")), "probe stderr\n");
  EXPECT_EQ(ReadFile(Path("pid")), std::to_string(state.pid) + "\n");
}

std::string ClosedDescriptorName(const testing::TestParamInfo<int>& info) {
  const char* names[] = {
      "None",   "Stdin",       "Stdout",       "StdinStdout",
      "Stderr", "StdinStderr", "StdoutStderr", "StdinStdoutStderr",
  };
  return names[info.param];
}

INSTANTIATE_TEST_SUITE_P(StandardStreams, ClosedStandardDescriptorsTest,
                         testing::Range(1, 8), ClosedDescriptorName);

class TerminalTest : public DetachTest {
 protected:
  void SetUp() override {
    DetachTest::SetUp();
    if (HasFatalFailure()) {
      return;
    }
    terminal_.Reset(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(CheckCall(terminal_.get(), "posix_openpt"));
    ASSERT_TRUE(
        CheckCall(fcntl(terminal_.get(), F_SETFD, FD_CLOEXEC), "fcntl"));
    ASSERT_TRUE(CheckCall(grantpt(terminal_.get()), "grantpt"));
    ASSERT_TRUE(CheckCall(unlockpt(terminal_.get()), "unlockpt"));
    char* name = ptsname(terminal_.get());
    ASSERT_NE(name, nullptr);
    slave_ = name;
  }

  Descriptor terminal_;
  std::string slave_;
};

TEST_F(TerminalTest, LosesTheOriginalControllingTerminal) {
  LaunchOptions options;
  options.terminal = slave_;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.has_terminal, 0);
  EXPECT_NE(state.session_id, result.launcher_pid);
  EXPECT_EQ(state.sighup_ignored, 0);
}

TEST_F(TerminalTest, KeepsRunningAfterTheOriginalTerminalIsClosed) {
  LaunchOptions options;
  options.terminal = slave_;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  PingProbe();
  terminal_.Reset();
  PingProbe();
}

TEST_F(TerminalTest, OpeningATerminalDoesNotMakeItTheControllingTerminal) {
  const auto result = RunProbeMode("terminal", {}, {slave_});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().has_terminal, 0);
}

}  // namespace
}  // namespace detach_test
