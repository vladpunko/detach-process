// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <sys/wait.h>
#include <unistd.h>

#include <initializer_list>
#include <string>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using OutputTest = DetachTest;

TEST_F(OutputTest, HelpSucceedsWithoutStartingACommand) {
  const auto result = Run({"--help"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(result.output.find("Usage: detach"), 0u);
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.program_pid, -1);
  for (const char* option : {"--stdin FILE", "--stdout FILE", "--stderr FILE",
                             "--pid FILE", "--help"}) {
    EXPECT_NE(result.output.find(option), std::string::npos) << option;
  }
}

TEST_F(OutputTest, PrintsCommandPidAndGeneratedLogPathsInOrder) {
  const auto result = RunProbeMode("streams", {"--pid", "job.pid"});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  const std::string expected_report = "PID: " + std::to_string(state.pid) +
                                      "\nstdout: " + result.stdout_path +
                                      "\nstderr: " + result.stderr_path + "\n";
  EXPECT_EQ(result.output, expected_report);
  EXPECT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(ReadFile(Path("job.pid")), std::to_string(state.pid) + "\n");
}

struct LaunchReportCase {
  const char* name;
  const char* stdout_path;
  const char* stderr_path;
};

class LaunchReportTest : public DetachTest,
                         public testing::WithParamInterface<LaunchReportCase> {
};

TEST_P(LaunchReportTest, PrintsCommandPidAndLogPathsInOrder) {
  const auto& test_case = GetParam();
  const auto result = RunProbeMode(
      "streams", {"--pid", "job.pid", "--stdout", test_case.stdout_path,
                  "--stderr", test_case.stderr_path});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  // Use the running command's PID, independent of the launcher's report parser.
  const std::string expected_report = "PID: " + std::to_string(state.pid) +
                                      "\nstdout: " + test_case.stdout_path +
                                      "\nstderr: " + test_case.stderr_path +
                                      "\n";
  EXPECT_EQ(result.output, expected_report);
  EXPECT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(ReadFile(Path("job.pid")), std::to_string(state.pid) + "\n");
}

const LaunchReportCase kLaunchReports[] = {
    {"SelectedFiles", "job.out", "job.err"},
    {"SharedFile", "job.log", "job.log"},
    {"PathsWithSpaces", "job output", "job errors"},
};

// Names report-layout cases so a failed format check identifies the setup.
std::string LaunchReportName(
    const testing::TestParamInfo<LaunchReportCase>& info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(ReportLayout, LaunchReportTest,
                         testing::ValuesIn(kLaunchReports), LaunchReportName);

TEST_F(OutputTest, ReportsStartupSuccessEvenWhenTheCommandExitsWithAnError) {
  const auto result = Run({"--", "/bin/sh", "-c", "exit 42"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_GT(result.program_pid, 0);
}

TEST_F(OutputTest, ReportsStartupSuccessEvenWhenTheCommandKillsItself) {
  const auto result = Run({"--", "/bin/sh", "-c", "kill -TERM $$"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_GT(result.program_pid, 0);
}

TEST_F(OutputTest, BrokenReportPipeDoesNotStopTheStartedCommand) {
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

TEST_F(OutputTest, BrokenDiagnosticPipeDoesNotInterruptExecFailureHandling) {
  LaunchOptions options;
  options.broken_pipes = {STDERR_FILENO};
  const auto result = Run({"--", Path("missing-command")}, options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(WIFEXITED(result.status));
}

TEST_F(OutputTest, HelpReportsFailureWhenItsOutputCannotBeWritten) {
  LaunchOptions options;
  options.broken_pipes = {STDOUT_FILENO};
  const auto result = Run({"--help"}, options);

  EXPECT_NE(result.exit_code, 0);
}

}  // namespace
}  // namespace detach_test
