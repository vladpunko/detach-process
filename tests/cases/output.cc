// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/probe_state.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

// Uses the probe PID to check the exact public report layout.
std::string ExpectedStartupReport(const ProbeState& state,
                                  const std::string& stdout_path,
                                  const std::string& stderr_path) {
    std::string expected = "PID: " + std::to_string(state.pid) + "\n";
    expected += "stdout: " + stdout_path + "\n";
    expected += "stderr: " + stderr_path + "\n";
    return expected;
}

class OutputTest : public DetachTest {
protected:
    // Checks report order and PID file contents against the running probe.
    void ExpectStartupReport(const RunReport& report,
                             const std::string& stdout_text,
                             const std::string& stderr_text) {
        ASSERT_EQ(report.exit_code, 0)
            << report.error;
        const auto state = ReadProbe();

        const auto expected = ExpectedStartupReport(state, stdout_text, stderr_text);

        EXPECT_EQ(report.output, expected);
        EXPECT_TRUE(report.error.empty())
            << report.error;
        EXPECT_EQ(report.command_pid, state.pid);
        EXPECT_EQ(Read(Path("job.pid")), std::to_string(state.pid) + "\n");
    }
};

// Help output.

TEST_F(OutputTest, HelpSucceedsWithoutStartingACommand) {
    const char* options[] = {
        "--stdin FILE", "--stdout FILE", "--stderr FILE", "--pid FILE", "--help",
    };

    const auto report = Run({"--help"});

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_TRUE(report.output.starts_with("Usage: detach"));
    EXPECT_TRUE(report.error.empty());
    EXPECT_EQ(report.command_pid, -1);

    for (const char* option : options) {
        EXPECT_PRED_FORMAT2(testing::IsSubstring, option, report.output);
    }
}

// Startup report.

TEST_F(OutputTest, PrintsCommandPidAndGeneratedLogPathsInOrder) {
    const auto report = RunProbeMode("streams", {"--pid", "job.pid"});

    ExpectStartupReport(report, report.stdout_path, report.stderr_path);
}

struct LaunchReportCase {
    const char* name;
    const char* stdout_path;
    const char* stderr_path;
};

class LaunchReportTest : public OutputTest,
                         public testing::WithParamInterface<LaunchReportCase> {
};

TEST_P(LaunchReportTest, PrintsCommandPidAndLogPathsInOrder) {
    const auto& test_case = GetParam();
    const std::vector<std::string> options = {
        "--pid", "job.pid",
        "--stdout", test_case.stdout_path,
        "--stderr", test_case.stderr_path,
    };

    const auto report = RunProbeMode("streams", options);

    ASSERT_NO_FATAL_FAILURE(
        ExpectStartupReport(report, test_case.stdout_path, test_case.stderr_path));
    EXPECT_EQ(report.stdout_path, test_case.stdout_path);
    EXPECT_EQ(report.stderr_path, test_case.stderr_path);
}

const LaunchReportCase kLaunchReports[] = {
    {
        .name = "SelectedFiles",
        .stdout_path = "job.out",
        .stderr_path = "job.err",
    },
    {
        .name = "SharedFile",
        .stdout_path = "job.log",
        .stderr_path = "job.log",
    },
    {
        .name = "PathsWithSpaces",
        .stdout_path = "job output",
        .stderr_path = "job errors",
    },
};

// Names report cases so failures identify the selected paths.
std::string LaunchReportName(const testing::TestParamInfo<LaunchReportCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(ReportLayout,
                         LaunchReportTest,
                         testing::ValuesIn(kLaunchReports),
                         LaunchReportName);

// Path escaping.

struct EscapedPathCase {
    const char* name;
    const char* path;
    const char* reported_path;
};

class EscapedPathTest : public OutputTest,
                        public testing::WithParamInterface<EscapedPathCase> {
};

TEST_P(EscapedPathTest, KeepsPathsOnSeparateReportLines) {
    const auto& test_case = GetParam();
    const std::string stdout_path = std::string("output-") + test_case.path;
    const std::string stderr_path = std::string("errors-") + test_case.path;
    const std::string printed_stdout = std::string("output-") + test_case.reported_path;
    const std::string printed_stderr = std::string("errors-") + test_case.reported_path;
    const std::vector<std::string> options = {
        "--pid", "job.pid",
        "--stdout", stdout_path,
        "--stderr", stderr_path,
    };

    const auto report = RunProbeMode("streams", options);

    ASSERT_NO_FATAL_FAILURE(
        ExpectStartupReport(report, printed_stdout, printed_stderr));
    ASSERT_EQ(report.stdout_path, stdout_path);
    ASSERT_EQ(report.stderr_path, stderr_path);

    EXPECT_EQ(Read(Path(report.stdout_path)), "stdout one\nstdout two\n");
    EXPECT_EQ(Read(Path(report.stderr_path)), "stderr one\nstderr two\n");
}

const EscapedPathCase kEscapedPaths[] = {
    {
        .name = "Backslash",
        .path = "part\\end",
        .reported_path = "part\\\\end",
    },
    {
        .name = "LiteralEscape",
        .path = "part\\x0aend",
        .reported_path = "part\\\\x0aend",
    },
    {
        .name = "Newline",
        .path = "part\nend",
        .reported_path = "part\\x0aend",
    },
    {
        .name = "CarriageReturn",
        .path = "part\rend",
        .reported_path = "part\\x0dend",
    },
    {
        .name = "Tab",
        .path = "part\tend",
        .reported_path = "part\\x09end",
    },
    {
        .name = "Escape",
        .path = "part\x1b-end",
        .reported_path = "part\\x1b-end",
    },
    {
        .name = "LastControlByte",
        .path = "part\x1f-end",
        .reported_path = "part\\x1f-end",
    },
    {
        .name = "Delete",
        .path = "part\x7f-end",
        .reported_path = "part\\x7f-end",
    },
    {
        .name = "Utf8",
        .path = "роза",
        .reported_path = "роза",
    },
};

// Names path cases after the byte whose printed form is checked.
std::string EscapedPathName(const testing::TestParamInfo<EscapedPathCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(PathEscaping,
                         EscapedPathTest,
                         testing::ValuesIn(kEscapedPaths),
                         EscapedPathName);

TEST_F(OutputTest, ExecFailureEscapesTheSelectedErrorPath) {
    const std::string error_path = "job\nerrors\\log";
    const std::string expected_message = "Check stderr: job\\x0aerrors\\\\log\n";

    const auto report = Run({"--stderr", error_path, "--", Path("missing-command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, expected_message, report.error);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", Read(Path(error_path)));
}

// Command exit status.

TEST_F(OutputTest, ReportsStartupSuccessEvenWhenTheCommandExitsWithAnError) {
    const auto report = Run({"--", "/bin/sh", "-c", "exit 42"});

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_GT(report.command_pid, 0);
}

TEST_F(OutputTest, ReportsStartupSuccessEvenWhenTheCommandKillsItself) {
    const auto report = Run({"--", "/bin/sh", "-c", "kill -TERM $$"});

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_GT(report.command_pid, 0);
}

// Broken output pipes.

TEST_F(OutputTest, BrokenReportPipeDoesNotStopTheStartedCommand) {
    const LaunchOptions options{.broken_pipe_descriptors = {STDOUT_FILENO}};

    const auto report = RunProbeMode("streams", {}, {}, options);

    ASSERT_EQ(report.exit_code, 1);
    const auto state = ReadProbe();
    std::string expected_report = "The command may already be running.\n";
    expected_report +=
        ExpectedStartupReport(state, report.stdout_path, report.stderr_path);

    EXPECT_TRUE(report.output.empty());
    ASSERT_FALSE(report.stdout_path.empty());
    ASSERT_FALSE(report.stderr_path.empty());
    EXPECT_PRED_FORMAT2(testing::IsSubstring, expected_report, report.error);
    EXPECT_EQ(state.pid, report.command_pid);
    EXPECT_EQ(state.sigpipe_ignored, 0);
    EXPECT_NE(report.stdout_path, report.stderr_path);
    EXPECT_EQ(Read(report.stdout_path), "stdout one\nstdout two\n");
    EXPECT_EQ(Read(report.stderr_path), "stderr one\nstderr two\n");
    PingProbe();
}

TEST_F(OutputTest, BrokenDiagnosticPipeDoesNotInterruptExecFailureHandling) {
    const LaunchOptions options{.broken_pipe_descriptors = {STDERR_FILENO}};

    const auto report = Run({"--", Path("missing-command")}, options);

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_TRUE(WIFEXITED(report.status));
}

TEST_F(OutputTest, HelpReportsFailureWhenItsOutputCannotBeWritten) {
    const LaunchOptions options{.broken_pipe_descriptors = {STDOUT_FILENO}};

    const auto report = Run({"--help"}, options);

    ASSERT_FALSE(report.is_timed_out);

    if (WIFSIGNALED(report.status)) {
        EXPECT_EQ(WTERMSIG(report.status), SIGPIPE);
    } else {
        ASSERT_TRUE(WIFEXITED(report.status));
        EXPECT_NE(report.exit_code, 0);
    }
}

}  // namespace
}  // namespace detach_test
