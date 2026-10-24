// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-24

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <limits>
#include <string>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

constexpr char kValidReport[] = "PID: 37\nstdout: output\nstderr: errors\n";
constexpr char kFakeScript[]
    = "#!/bin/sh\n"
      "/bin/cat stdout.txt\n"
      "/bin/cat stderr.txt >&2\n";

class ReportsTest : public DetachTest {
protected:
    // Replays report bytes without starting a detached command.
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());

        ASSERT_NO_FATAL_FAILURE(Write(Path("fake-detach"), kFakeScript));
        const int status = chmod(Path("fake-detach").c_str(), 0700);
        ASSERT_TRUE(CheckSysCall(status, "make fake executable runnable"));
    }

    // Keeps report data separate from the script so shell syntax stays literal.
    void WriteReport(const std::string& output, const std::string& error = {}) {
        ASSERT_NO_FATAL_FAILURE(Write(Path("stdout.txt"), output));
        ASSERT_NO_FATAL_FAILURE(Write(Path("stderr.txt"), error));
    }

    // Exercises public capture and parsing through the normal fixture entry point.
    RunReport CaptureReport() {
        const LaunchOptions options = {
            .executable_path = Path("fake-detach"),
        };

        const auto report = Run({}, options);

        EXPECT_EQ(report.exit_code, 0) << report.error;
        return report;
    }
};

// PID validation.

class ValidPidTest : public ReportsTest, public testing::WithParamInterface<pid_t> {
};

TEST_P(ValidPidTest, AcceptsTheSupportedRange) {
    const std::string output
        = "PID: " + std::to_string(GetParam()) + "\nstdout: output\nstderr: errors\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, GetParam());
    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
}

// Names the supported PID boundaries explicitly in test output.
std::string ValidPidName(const testing::TestParamInfo<pid_t>& info) {
    return info.param == 1 ? "Minimum" : "Maximum";
}

INSTANTIATE_TEST_SUITE_P(PidRange,
                         ValidPidTest,
                         testing::Values(pid_t{1}, std::numeric_limits<pid_t>::max()),
                         ValidPidName);

struct InvalidPidCase {
    const char* name;
    std::string text;
};

class InvalidPidTest : public ReportsTest,
                       public testing::WithParamInterface<InvalidPidCase> {
};

TEST_P(InvalidPidTest, RejectsThePidAndFollowingPaths) {
    const std::string output
        = "PID: " + GetParam().text + "\nstdout: output\nstderr: errors\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, -1);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
    EXPECT_EQ(report.output, output);
}

const InvalidPidCase kInvalidPids[] = {
    {
        .name = "Empty",
        .text = "",
    },
    {
        .name = "Zero",
        .text = "0",
    },
    {
        .name = "Negative",
        .text = "-37",
    },
    {
        .name = "Text",
        .text = "unknown",
    },
    {
        .name = "TrailingText",
        .text = "37extra",
    },
    {
        .name = "LeadingSpace",
        .text = " 37",
    },
    {
        .name = "TrailingSpace",
        .text = "37 ",
    },
    {
        .name = "PlusSign",
        .text = "+37",
    },
    {
        .name = "AboveRange",
        .text = std::to_string(
            static_cast<std::uintmax_t>(std::numeric_limits<pid_t>::max()) + 1),
    },
    {
        .name = "HugeNumber",
        .text = std::string(128, '9'),
    },
};

// Names rejected PID forms so a failure identifies the malformed value.
std::string InvalidPidName(const testing::TestParamInfo<InvalidPidCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(InvalidPids,
                         InvalidPidTest,
                         testing::ValuesIn(kInvalidPids),
                         InvalidPidName);

// Report boundaries.

TEST_F(ReportsTest, IgnoresPathsWithoutAPid) {
    ASSERT_NO_FATAL_FAILURE(WriteReport("stdout: output\n", "stderr: errors\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, -1);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
}

TEST_F(ReportsTest, IgnoresPathsBeforeThePid) {
    ASSERT_NO_FATAL_FAILURE(
        WriteReport("stdout: early-output\nstderr: early-errors\nPID: 37\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
}

TEST_F(ReportsTest, PreservesTheReportWhenTheOtherStreamContainsDiagnostics) {
    ASSERT_NO_FATAL_FAILURE(WriteReport(
        kValidReport, "stdout: cannot open output\nstderr: cannot open errors\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
}

TEST_F(ReportsTest, DoesNotReuseThePidAcrossStreams) {
    ASSERT_NO_FATAL_FAILURE(
        WriteReport("PID: 37\n", "stdout: output\nstderr: errors\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
}

TEST_F(ReportsTest, UsesTheCompleteFallbackAfterPartialOutput) {
    const std::string error
        = std::string("report: broken pipe\nThe command may already be running.\n")
          + kValidReport;
    ASSERT_NO_FATAL_FAILURE(WriteReport("PID: 37\nstdout: partial\n", error));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
    EXPECT_EQ(report.error, error);
}

TEST_F(ReportsTest, InvalidPidDoesNotReplaceAnEarlierReport) {
    ASSERT_NO_FATAL_FAILURE(
        WriteReport(kValidReport, "PID: 0\nstdout: wrong\nstderr: wrong\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
}

TEST_F(ReportsTest, InvalidPidStopsFollowingPathFields) {
    const std::string output
        = std::string(kValidReport) + "PID: invalid\nstdout: wrong\nstderr: wrong\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
}

TEST_F(ReportsTest, NewPidDiscardsEarlierPathsInTheSameStream) {
    const std::string output
        = std::string(kValidReport) + "PID: 38\nstderr: new-errors\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 38);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_EQ(report.stderr_path, "new-errors");
}

TEST_F(ReportsTest, NewPidDiscardsEarlierPathsAcrossStreams) {
    ASSERT_NO_FATAL_FAILURE(WriteReport(kValidReport, "PID: 38\nstdout: new-output\n"));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 38);
    EXPECT_EQ(report.stdout_path, "new-output");
    EXPECT_TRUE(report.stderr_path.empty());
}

// Path decoding.

struct DecodedPathCase {
    const char* name;
    const char* encoded;
    const char* decoded;
};

class DecodedPathTest : public ReportsTest,
                        public testing::WithParamInterface<DecodedPathCase> {
};

TEST_P(DecodedPathTest, ReturnsRealPathsAndKeepsTheCapturedText) {
    const auto& test_case = GetParam();
    const std::string output = std::string("PID: 37\nstdout: ") + test_case.encoded
                               + "\nstderr: " + test_case.encoded + "\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_EQ(report.stdout_path, test_case.decoded);
    EXPECT_EQ(report.stderr_path, test_case.decoded);
    EXPECT_EQ(report.output, output);
}

constexpr DecodedPathCase kDecodedPaths[] = {
    {
        .name = "Plain",
        .encoded = "two words",
        .decoded = "two words",
    },
    {
        .name = "Backslash",
        .encoded = "part\\\\end",
        .decoded = "part\\end",
    },
    {
        .name = "LiteralEscape",
        .encoded = "part\\\\x0aend",
        .decoded = "part\\x0aend",
    },
    {
        .name = "Newline",
        .encoded = "part\\x0aend",
        .decoded = "part\nend",
    },
    {
        .name = "CarriageReturn",
        .encoded = "part\\x0dend",
        .decoded = "part\rend",
    },
    {
        .name = "Tab",
        .encoded = "part\\x09end",
        .decoded = "part\tend",
    },
    {
        .name = "Escape",
        .encoded = "part\\x1b-end",
        .decoded = "part\x1b-end",
    },
    {
        .name = "LastControlByte",
        .encoded = "part\\x1f-end",
        .decoded = "part\x1f-end",
    },
    {
        .name = "Delete",
        .encoded = "part\\x7f-end",
        .decoded = "part\x7f-end",
    },
    {
        .name = "PrintableEscape",
        .encoded = "part\\x41end",
        .decoded = "partAend",
    },
    {
        .name = "HighByteEscape",
        .encoded = "part\\x80-end",
        .decoded = "part\x80-end",
    },
    {
        .name = "Utf8",
        .encoded = "роза",
        .decoded = "роза",
    },
};

// Names decoding cases after the byte representation under test.
std::string DecodedPathName(const testing::TestParamInfo<DecodedPathCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(PathDecoding,
                         DecodedPathTest,
                         testing::ValuesIn(kDecodedPaths),
                         DecodedPathName);

struct InvalidPathCase {
    const char* name;
    std::string text;
};

class InvalidPathTest : public ReportsTest,
                        public testing::WithParamInterface<InvalidPathCase> {
};

TEST_P(InvalidPathTest, RejectsTheWholePath) {
    const std::string output
        = "PID: 37\nstdout: " + GetParam().text + "\nstderr: " + GetParam().text + "\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(output));

    const auto report = CaptureReport();

    EXPECT_EQ(report.command_pid, 37);
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
    EXPECT_EQ(report.output, output);
}

const InvalidPathCase kInvalidPaths[] = {
    {
        .name = "Empty",
        .text = "",
    },
    {
        .name = "TrailingBackslash",
        .text = "part\\",
    },
    {
        .name = "UnknownEscape",
        .text = "part\\q",
    },
    {
        .name = "NoHexDigits",
        .text = "part\\x",
    },
    {
        .name = "OneHexDigit",
        .text = "part\\x1",
    },
    {
        .name = "InvalidHexDigits",
        .text = "part\\xgg",
    },
    {
        .name = "PartialHexDigits",
        .text = "part\\x1g",
    },
    {
        .name = "NulEscape",
        .text = "part\\x00end",
    },
    {
        .name = "RawNul",
        .text = std::string("part\0end", 8),
    },
    {
        .name = "RawControlByte",
        .text = "part\tend",
    },
};

// Names rejected path forms so diagnostics identify the broken encoding.
std::string InvalidPathName(const testing::TestParamInfo<InvalidPathCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(InvalidPaths,
                         InvalidPathTest,
                         testing::ValuesIn(kInvalidPaths),
                         InvalidPathName);

TEST_F(ReportsTest, InvalidPathsDoNotReplaceEarlierPaths) {
    const std::string error = "PID: 37\nstdout: broken\\q\nstderr: broken\\x00\n";
    ASSERT_NO_FATAL_FAILURE(WriteReport(kValidReport, error));

    const auto report = CaptureReport();

    EXPECT_EQ(report.stdout_path, "output");
    EXPECT_EQ(report.stderr_path, "errors");
    EXPECT_EQ(report.error, error);
}

TEST_F(ReportsTest, InvalidPathDoesNotHideAnotherValidPath) {
    ASSERT_NO_FATAL_FAILURE(
        WriteReport("PID: 37\nstdout: broken\\q\nstderr: errors\n"));

    const auto report = CaptureReport();

    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_EQ(report.stderr_path, "errors");
}

}  // namespace
}  // namespace detach_test
