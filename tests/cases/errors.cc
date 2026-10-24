// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/error_fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"
#include "tests/support/syscall_overrides.h"

namespace detach_test {
namespace {

// Describes an injected fault and the diagnostic the test must observe.
struct ErrorCase {
    const char* name;
    FaultRule fault;
    const char* expected_diagnostic;
};

// Describes a fault that must leave the command running after recovery.
struct RetryCase {
    const char* name;
    FaultRule fault;
};

// Names error scenarios so failures identify the injected call.
std::string ErrorName(const testing::TestParamInfo<ErrorCase>& info) {
    return info.param.name;
}

// Names retry scenarios so reports identify the operation being exercised.
std::string RetryName(const testing::TestParamInfo<RetryCase>& info) {
    return info.param.name;
}

// Names output errors after the failed operation.
std::string OperationName(const testing::TestParamInfo<const char*>& info) {
    return info.param;
}

// Rejects empty diagnostics so an error check cannot succeed without checking text.
void ExpectError(const RunReport& report, const char* expected_diagnostic) {
    ASSERT_TRUE(expected_diagnostic != nullptr && expected_diagnostic[0] != '\0')
        << "expected diagnostic must not be empty";
    ASSERT_EQ(report.exit_code, 1)
        << report.error;
    EXPECT_PRED_FORMAT2(testing::IsSubstring, expected_diagnostic, report.error);
}

class CommandErrorTest : public ErrorTest {
protected:
    // Checks the live command and its completed output after a recoverable fault.
    void ExpectCommandAvailable(const RunReport& report) {
        ASSERT_GT(report.command_pid, 0)
            << report.error;
        const auto state = ReadProbe();

        EXPECT_EQ(state.pid, report.command_pid);
        ASSERT_FALSE(report.stdout_path.empty());
        ASSERT_FALSE(report.stderr_path.empty());
        EXPECT_NE(report.stdout_path, report.stderr_path);
        EXPECT_EQ(Read(report.stdout_path), "stdout one\nstdout two\n");
        EXPECT_EQ(Read(report.stderr_path), "stderr one\nstderr two\n");

        PingProbe();
    }
};

// Startup errors.

class StartupFailureTest : public ErrorTest,
                           public testing::WithParamInterface<ErrorCase> {
protected:
    // Exercises inherited descriptor cleanup even when another startup call fails.
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(ErrorTest::SetUp());
        inherited_.Reset(open(Path("inherited").c_str(), O_CREAT | O_RDWR, 0600));
        ASSERT_TRUE(CheckSysCall(inherited_.get(), "open inherited descriptor"));
    }

private:
    FileDescriptor inherited_;
};

// Call numbers start at one within each process role.
// Launcher `fcntl` calls first check the three standard descriptors.
// Launcher `fstat` calls inspect stdout, stderr, the optional PID file, then stdin.
// Command `close` calls first release the three redirected stream descriptors.
const ErrorCase kStartupFailures[] = {
    // Signal setup.
    {
        .name = "SignalMask",
        .fault = {
            .operation = "sigemptyset",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "sigpipe:",
    },
    {
        .name = "SignalDisposition",
        .fault = {
            .operation = "sigaction",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "sigpipe:",
    },

    // Standard descriptors and temporary logs.
    {
        .name = "StandardDescriptor",
        .fault = {
            .operation = "fcntl",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "standard descriptors:",
    },
    {
        .name = "TemporaryStdout",
        .fault = {
            .operation = "mkstemp",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "stdout:",
    },
    {
        .name = "TemporaryStderr",
        .fault = {
            .operation = "mkstemp",
            .role = ProcessRole::kLauncher,
            .call_number = 2,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },

    // File information.
    {
        .name = "StdoutStat",
        .fault = {
            .operation = "fstat",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "stdout:",
    },
    {
        .name = "StderrStat",
        .fault = {
            .operation = "fstat",
            .role = ProcessRole::kLauncher,
            .call_number = 2,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },
    {
        .name = "PidStat",
        .fault = {
            .operation = "fstat",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "pid:",
    },
    {
        .name = "StdinStat",
        .fault = {
            .operation = "fstat",
            .role = ProcessRole::kLauncher,
            .call_number = 4,
            .result = EIO,
        },
        .expected_diagnostic = "stdin:",
    },

    // Truncation, offsets and PID output.
    {
        .name = "StdoutTruncate",
        .fault = {
            .operation = "ftruncate",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "stdout:",
    },
    {
        .name = "StderrTruncate",
        .fault = {
            .operation = "ftruncate",
            .role = ProcessRole::kLauncher,
            .call_number = 2,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },
    {
        .name = "PidTruncate",
        .fault = {
            .operation = "ftruncate",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "pid:",
    },
    {
        .name = "StdoutRewind",
        .fault = {
            .operation = "lseek",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "stdout:",
    },
    {
        .name = "StderrRewind",
        .fault = {
            .operation = "lseek",
            .role = ProcessRole::kLauncher,
            .call_number = 2,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },
    {
        .name = "PidRewind",
        .fault = {
            .operation = "lseek",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "pid:",
    },
    {
        .name = "PidWrite",
        .fault = {
            .operation = "dprintf",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "pid:",
    },
    {
        .name = "PidClose",
        .fault = {
            .operation = "close",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "pid:",
    },

    // Startup socket and processes.
    {
        .name = "StartupSocket",
        .fault = {
            .operation = "socketpair",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "startup channel:",
    },
    {
        .name = "StartupCloseOnExec",
        .fault = {
            .operation = "fcntl",
            .role = ProcessRole::kLauncher,
            .call_number = 4,
            .result = EIO,
        },
        .expected_diagnostic = "startup channel:",
    },
    {
        .name = "FirstFork",
        .fault = {
            .operation = "fork",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EAGAIN,
        },
        .expected_diagnostic = "fork:",
    },
    {
        .name = "NewSession",
        .fault = {
            .operation = "setsid",
            .role = ProcessRole::kIntermediate,
            .call_number = 1,
            .result = EPERM,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "SecondFork",
        .fault = {
            .operation = "fork",
            .role = ProcessRole::kIntermediate,
            .call_number = 1,
            .result = EAGAIN,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "WaitIntermediate",
        .fault = {
            .operation = "waitpid",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "waitpid:",
    },

    // Startup channel messages.
    {
        .name = "ReceivePidError",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "startup channel:",
    },
    {
        .name = "ReceivePidEof",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = FAULT_EOF,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "SendPermission",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "startup channel:",
    },
    {
        .name = "SendPidError",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "SendPidZero",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = FAULT_EOF,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "ReceivePermission",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "startup channel:",
    },

    // Command streams.
    {
        .name = "RedirectStdin",
        .fault = {
            .operation = "dup2",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EBADF,
        },
        .expected_diagnostic = "stdin:",
    },
    {
        .name = "RedirectStdout",
        .fault = {
            .operation = "dup2",
            .role = ProcessRole::kCommand,
            .call_number = 2,
            .result = EBADF,
        },
        .expected_diagnostic = "stdout:",
    },
    {
        .name = "RedirectStderr",
        .fault = {
            .operation = "dup2",
            .role = ProcessRole::kCommand,
            .call_number = 3,
            .result = EBADF,
        },
        .expected_diagnostic = "stderr:",
    },

    // Descriptor cleanup and signal restoration.
    {
        .name = "EnumerateDescriptors",
        .fault = {
            .operation = "opendir",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "/dev/fd:",
    },
    {
        .name = "ReadDescriptors",
        .fault = {
            .operation = "readdir",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "/dev/fd:",
    },
    {
        .name = "CloseInherited",
        .fault = {
            .operation = "close",
            .role = ProcessRole::kCommand,
            .call_number = 4,
            .result = EIO,
        },
        .expected_diagnostic = "close inherited descriptor:",
    },
    {
        .name = "CloseDirectory",
        .fault = {
            .operation = "closedir",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "/dev/fd:",
    },
    {
        .name = "RestoreSigpipe",
        .fault = {
            .operation = "sigaction",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "sigpipe:",
    },
};

TEST_P(StartupFailureTest, ReportsTheFailureAndRemovesTemporaryLogs) {
    const auto& scenario = GetParam();

    const auto report = RunProbe({"--pid", Path("pid")}, Inject(scenario.fault));

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, scenario.expected_diagnostic));
    ExpectTemporaryFilesRemoved();
}

INSTANTIATE_TEST_SUITE_P(SystemCalls,
                         StartupFailureTest,
                         testing::ValuesIn(kStartupFailures),
                         ErrorName);

TEST_F(ErrorTest, InputAliasFailurePreservesInputAndRemovesTemporaryLogs) {
    const std::string input_path = Path("input");
    ASSERT_NO_FATAL_FAILURE(Write(input_path, "keep stdin\n"));
    const std::vector<std::string> options = {
        "--stdin", input_path,
        "--stdout", input_path,
    };

    const auto report = RunProbe(options, FaultOptions());

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_EQ(report.command_pid, -1);
    EXPECT_EQ(Read(input_path), "keep stdin\n");
    ExpectTemporaryFilesRemoved();
}

TEST_F(ErrorTest, FailureToReopenDevNullIsReported) {
    auto options = Inject({"open", ProcessRole::kLauncher, 1, EACCES});
    options.closed_standard_descriptors = 1 << STDIN_FILENO;

    const auto report = RunProbe({}, options);

    ExpectError(report, "/dev/null:");
}

TEST_F(ErrorTest, FailureToShareOutputOffsetsIsReported) {
    const auto options = Inject({"dup2", ProcessRole::kLauncher, 1, EBADF});
    const std::vector<std::string> file_options = {
        "--stdout", Path("combined"),
        "--stderr", Path("combined"),
    };

    const auto report = RunProbe(file_options, options);

    ExpectError(report, "stderr:");
}

// Startup retries and descriptor reuse.

class StartupRetryTest : public CommandErrorTest,
                         public testing::WithParamInterface<RetryCase> {
};

const RetryCase kStartupRetries[] = {
    {
        .name = "InterruptedPidRead",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EINTR,
        },
    },
    {
        .name = "InterruptedStatusRead",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 2,
            .result = EINTR,
        },
    },
    {
        .name = "InterruptedPermissionRead",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EINTR,
        },
    },
    {
        .name = "InterruptedPidSend",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EINTR,
        },
    },
    {
        .name = "InterruptedPermissionSend",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EINTR,
        },
    },
    {
        .name = "InterruptedWait",
        .fault = {
            .operation = "waitpid",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EINTR,
        },
    },
    {
        .name = "AlreadyReapedIntermediate",
        .fault = {
            .operation = "waitpid",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = ECHILD,
        },
    },
    {
        .name = "PartialPidRead",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = FAULT_SHORT_IO,
        },
    },
    {
        .name = "PartialPidSend",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = FAULT_SHORT_IO,
        },
    },
};

TEST_P(StartupRetryTest, RetriesWithoutChangingTheStartedCommand) {
    const auto& scenario = GetParam();

    const auto report = RunProbeMode("streams", {}, {}, Inject(scenario.fault));

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ASSERT_NO_FATAL_FAILURE(ExpectCommandAvailable(report));
}

INSTANTIATE_TEST_SUITE_P(StartupChannel,
                         StartupRetryTest,
                         testing::ValuesIn(kStartupRetries),
                         RetryName);

TEST_F(ErrorTest, RedirectingADescriptorToItselfKeepsItOpen) {
    auto options = FaultOptions();
    options.environment["DETACH_TEST_HELPER"] = "same-descriptor";

    const auto report = Run({}, options);

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
}

// Diagnostic errors.

class DiagnosticFailureTest : public ErrorTest,
                              public testing::WithParamInterface<ErrorCase> {
};

// Without a PID file, the first two seeks reset stdout and stderr.
// The third seeks back through the error log after the command fails.
const ErrorCase kDiagnosticFailures[] = {
    {
        .name = "SeekLog",
        .fault = {
            .operation = "lseek",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },
    {
        .name = "ReadLog",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EIO,
        },
        .expected_diagnostic = "stderr:",
    },
    {
        .name = "InterruptedLogRead",
        .fault = {
            .operation = "read",
            .role = ProcessRole::kLauncher,
            .call_number = 3,
            .result = EINTR,
        },
        .expected_diagnostic = "exec:",
    },
    {
        .name = "WriteDiagnostic",
        .fault = {
            .operation = "fwrite",
            .role = ProcessRole::kLauncher,
            .call_number = 1,
            .result = EIO,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "RestoreIgnoredSigpipe",
        .fault = {
            .operation = "sigaction",
            .role = ProcessRole::kCommand,
            .call_number = 2,
            .result = EIO,
        },
        .expected_diagnostic = "failed to start the command",
    },
    {
        .name = "InterruptedFailureReport",
        .fault = {
            .operation = "send",
            .role = ProcessRole::kCommand,
            .call_number = 2,
            .result = EINTR,
        },
        .expected_diagnostic = "exec:",
    },
    {
        .name = "UnlinkStdout",
        .fault = {
            .operation = "unlink",
            .role = ProcessRole::kCommand,
            .call_number = 1,
            .result = EACCES,
        },
        .expected_diagnostic = "/tmp/stdout.",
    },
    {
        .name = "UnlinkStderr",
        .fault = {
            .operation = "unlink",
            .role = ProcessRole::kCommand,
            .call_number = 2,
            .result = EACCES,
        },
        .expected_diagnostic = "/tmp/stderr.",
    },
};

TEST_P(DiagnosticFailureTest, StillReportsStartupFailureAndCleansUp) {
    const auto& scenario = GetParam();

    const auto report = Run({"--", Path("missing-command")}, Inject(scenario.fault));

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, scenario.expected_diagnostic));
    ExpectTemporaryFilesRemoved();
}

INSTANTIATE_TEST_SUITE_P(Diagnostics,
                         DiagnosticFailureTest,
                         testing::ValuesIn(kDiagnosticFailures),
                         ErrorName);

TEST_F(ErrorTest, AliasedTemporaryStderrIsReadThroughTheOpenDescriptor) {
    const std::string error_path = Path("stderr-alias");
    auto options = FaultOptions();
    options.environment["DETACH_TEST_STDERR_ALIAS"] = error_path;
    const std::vector<std::string> arguments = {
        "--stderr", error_path,
        "--", Path("missing-command"),
    };

    const auto report = Run(arguments, options);

    ExpectError(report, "exec:");
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", Read(error_path));
    ExpectTemporaryFilesRemoved();
}

// Help and launch reports.

class HelpFailureTest : public ErrorTest,
                        public testing::WithParamInterface<const char*> {
};

TEST_P(HelpFailureTest, ReportsAWriteOrCloseError) {
    const auto options = Inject({GetParam(), ProcessRole::kLauncher, 1, EIO});

    const auto report = Run({"--help"}, options);

    ExpectError(report, "help:");
}

INSTANTIATE_TEST_SUITE_P(HelpOutput,
                         HelpFailureTest,
                         testing::Values("fprintf", "fclose"),
                         OperationName);

class ReportFailureTest : public CommandErrorTest,
                          public testing::WithParamInterface<const char*> {
};

TEST_P(ReportFailureTest, KeepsTheCommandAndItsLogsAvailable) {
    const auto options = Inject({GetParam(), ProcessRole::kLauncher, 1, EIO});

    const auto report = RunProbeMode("streams", {}, {}, options);

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, "the command may already be running"));
    ASSERT_NO_FATAL_FAILURE(ExpectCommandAvailable(report));
}

INSTANTIATE_TEST_SUITE_P(LaunchReport,
                         ReportFailureTest,
                         testing::Values("printf", "fclose", "fprintf", "fputc"),
                         OperationName);

TEST_F(CommandErrorTest, StderrLabelFailurePreservesTheCommandAndItsLogs) {
    const auto options = Inject({
        .operation = "fprintf",
        .role = ProcessRole::kLauncher,
        .call_number = 2,
        .result = EIO,
    });

    const auto report = RunProbeMode("streams", {}, {}, options);

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, "the command may already be running"));
    ASSERT_NO_FATAL_FAILURE(ExpectCommandAvailable(report));
}

TEST_F(CommandErrorTest, StdoutNewlineFailurePreservesTheCommandAndItsLogs) {
    const std::string output_path = Path("output");

    // Every path byte uses one `fputc` call before the final newline.
    const auto options = Inject({
        .operation = "fputc",
        .role = ProcessRole::kLauncher,
        .call_number = static_cast<unsigned int>(output_path.size() + 1),
        .result = EIO,
    });

    const auto report = RunProbeMode("streams", {"--stdout", output_path}, {}, options);

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, "the command may already be running"));
    ASSERT_NO_FATAL_FAILURE(ExpectCommandAvailable(report));
}

TEST_F(CommandErrorTest, StatusReadErrorPreservesThePossiblyRunningCommand) {
    const auto options = Inject({"read", ProcessRole::kLauncher, 2, EIO});

    const auto report = RunProbeMode("streams", {}, {}, options);

    ASSERT_NO_FATAL_FAILURE(ExpectError(report, "startup channel:"));
    ASSERT_NO_FATAL_FAILURE(ExpectCommandAvailable(report));
}

// Startup protocol limits.

TEST_F(ErrorTest, LostFailureReportHasTheDocumentedEofStartupStatus) {
    const auto options = Inject({"send", ProcessRole::kCommand, 2, EPIPE});
    const std::vector<std::string> arguments = {
        "--stdout", Path("output"),
        "--stderr", Path("errors"),
        "--", Path("missing-command"),
    };

    const auto report = Run(arguments, options);

    // Without a failure byte, `EOF` cannot distinguish `exec` from an early exit.
    EXPECT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", Read(Path("errors")));
}

// An invalid permission byte stops the command before `exec`.
// The parent sees `EOF` without a failure byte and reports startup success.
TEST_F(ErrorTest, InvalidPermissionStopsTheCommandBeforeExec) {
    const auto options =
        Inject({"read", ProcessRole::kCommand, 1, FAULT_BAD_PERMISSION});
    const std::vector<std::string> arguments = {
        "--stdout", Path("output"),
        "--stderr", Path("errors"),
        "--", Path("missing-command"),
    };

    const auto report = Run(arguments, options);

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_TRUE(Read(Path("errors")).empty());
    EXPECT_TRUE(Read(Path("output")).empty());
}

// Option parser edge cases.

TEST_F(ErrorTest, UnexpectedParserReturnIsRejected) {
    const auto options = Inject({"getopt_long", ProcessRole::kLauncher, 1, EINVAL});

    const auto report = Run({"--help"}, options);

    ExpectError(report, "Unknown option.");
}

TEST_F(ErrorTest, UnexpectedParserAdvanceIsNotASeparator) {
    const auto options =
        Inject({"getopt_long", ProcessRole::kLauncher, 1, FAULT_PARSER_ADVANCE});

    const auto report = Run({"ordinary-argument"}, options);

    ExpectError(report, "Missing command");
}

TEST_F(ErrorTest, NullOptionArgumentIsTreatedAsEmpty) {
    auto options = FaultOptions();
    options.environment["DETACH_TEST_HELPER"] = "empty-null";

    const auto report = Run({}, options);

    EXPECT_EQ(report.exit_code, 0)
        << report.error;
}

// Descriptor directory entries.

struct DirectoryEntry {
    const char* name;
    const char* value;
};

class DescriptorEntryTest : public ErrorTest,
                            public testing::WithParamInterface<DirectoryEntry> {
};

// Names synthetic directory entries by the parsing edge they exercise.
std::string EntryName(const testing::TestParamInfo<DirectoryEntry>& info) {
    return info.param.name;
}

const DirectoryEntry kInvalidEntries[] = {
    {"NotANumber", "not-a-number"},
    {"TrailingCharacters", "4trailing"},
    {"NumericOverflow", "99999999999999999999999999999999999"},
    {"AboveIntMax", "2147483648"},
};

TEST_P(DescriptorEntryTest, IgnoresNamesThatAreNotValidDescriptors) {
    auto options = Inject({"readdir", ProcessRole::kCommand, 1, FAULT_DIRECTORY_ENTRY});
    options.environment["DETACH_TEST_ENTRY"] = GetParam().value;

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

INSTANTIATE_TEST_SUITE_P(DirectoryEntries,
                         DescriptorEntryTest,
                         testing::ValuesIn(kInvalidEntries),
                         EntryName);

TEST_F(ErrorTest, IgnoresNonNumericEntriesWhenConversionLeavesErrnoUnchanged) {
    auto options = Inject({
        {"readdir", ProcessRole::kCommand, 1, FAULT_DIRECTORY_ENTRY},
        {"strtoimax", ProcessRole::kCommand, 1, 0},
    });
    options.environment["DETACH_TEST_ENTRY"] = "not-a-number";

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

}  // namespace
}  // namespace detach_test
