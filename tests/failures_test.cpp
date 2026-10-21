// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tests/faults.h"
#include "tests/support.h"

namespace detach_test {
namespace {

struct Fault {
    const char *name;
    const char *operation;
    const char *role;
    unsigned int call;
    int error;
    const char *diagnostic;
};

std::string FaultName(const testing::TestParamInfo<Fault> &info) {
    return info.param.name;
}

class FaultTest : public DetachTest {
 protected:
    void SetUp() override {
        DetachTest::SetUp();
        if (!HasFatalFailure()) {
            WriteFile(Path("events"), "");
            events_ready_ = true;
        }
    }

    void TearDown() override {
        const auto paths = TemporaryFiles();
        DetachTest::TearDown();
        // Also clean files from intentionally failed unlink calls.
        for (const auto &path : paths) {
            if (unlink(path.c_str()) < 0) {
                EXPECT_EQ(errno, ENOENT) << path;
            }
        }
    }

    LaunchOptions Inject(const Fault &fault) const {
        LaunchOptions options;
        options.executable = FAULT_PATH;
        options.environment["DETACH_TEST_EVENTS"] = Path("events");
        options.environment["DETACH_TEST_FAULTS"] =
            std::string(fault.role) + ":" + fault.operation + ":" +
            std::to_string(fault.call) + ":" + std::to_string(fault.error);
        return options;
    }

    std::vector<std::string> TemporaryFiles() const {
        std::vector<std::string> paths;
        if (!events_ready_) {
            return paths;
        }
        std::ifstream events(Path("events"));
        std::string path;
        while (std::getline(events, path)) {
            paths.push_back(path);
        }
        return paths;
    }

    void ExpectTemporaryFilesRemoved() const {
        for (const auto &path : TemporaryFiles()) {
            EXPECT_FALSE(std::filesystem::exists(path)) << path;
        }
    }

 private:
    bool events_ready_ = false;
};

class StartupFailureTest : public FaultTest,
                           public testing::WithParamInterface<Fault> {
 protected:
    void SetUp() override {
        FaultTest::SetUp();
        if (!HasFatalFailure()) {
            inherited_.Reset(CheckCall(open(Path("inherited").c_str(),
                O_CREAT | O_RDWR, 0600), "open inherited descriptor"));
        }
    }

    Descriptor inherited_;
};

TEST_P(StartupFailureTest, ReportsTheFailureAndRemovesTemporaryLogs) {
    const auto &fault = GetParam();
    const auto result = RunProbe({"--pid", Path("pid")},
                                 "state", {}, Inject(fault));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
        << result.error;
    ExpectTemporaryFilesRemoved();
}

const Fault kStartupFailures[] = {
    {"SignalMask", "sigemptyset", "launcher", 1, EIO, "SIGPIPE:"},
    {"SignalDisposition", "sigaction", "launcher", 1, EIO, "SIGPIPE:"},
    {"StandardDescriptor", "fcntl", "launcher", 1, EIO,
     "standard descriptors:"},
    {"TemporaryStdout", "mkstemp", "launcher", 1, EIO, "stdout:"},
    {"TemporaryStderr", "mkstemp", "launcher", 2, EIO, "stderr:"},
    {"StdoutStat", "fstat", "launcher", 1, EIO, "stdout:"},
    {"StderrStat", "fstat", "launcher", 2, EIO, "stderr:"},
    {"PidStat", "fstat", "launcher", 3, EIO, "pid:"},
    {"StdinStat", "fstat", "launcher", 4, EIO, "stdin:"},
    {"StdoutTruncate", "ftruncate", "launcher", 1, EIO, "stdout:"},
    {"StderrTruncate", "ftruncate", "launcher", 2, EIO, "stderr:"},
    {"PidTruncate", "ftruncate", "launcher", 3, EIO, "pid:"},
    {"PidWrite", "dprintf", "launcher", 1, EIO, "pid:"},
    {"PidClose", "close", "launcher", 3, EIO, "pid:"},
    {"StartupSocket", "socketpair", "launcher", 1, EIO, "startup channel:"},
    {"StartupCloseOnExec", "fcntl", "launcher", 4, EIO, "startup channel:"},
    {"FirstFork", "fork", "launcher", 1, EAGAIN, "fork:"},
    {"NewSession", "setsid", "intermediate", 1, EPERM,
     "Failed to start the command"},
    {"SecondFork", "fork", "intermediate", 1, EAGAIN,
     "Failed to start the command"},
    {"WaitIntermediate", "waitpid", "launcher", 1, EIO, "waitpid:"},
    {"ReceivePidError", "read", "launcher", 1, EIO, "startup channel:"},
    {"ReceivePidEof", "read", "launcher", 1, FAULT_EOF,
     "Failed to start the command"},
    {"SendPermission", "send", "launcher", 1, EIO, "startup channel:"},
    {"SendPidError", "send", "command", 1, EIO,
     "Failed to start the command"},
    {"SendPidZero", "send", "command", 1, FAULT_EOF,
     "Failed to start the command"},
    {"ReceivePermission", "read", "command", 1, EIO, "startup channel:"},
    {"RedirectStdin", "dup2", "command", 1, EBADF, "stdin:"},
    {"RedirectStdout", "dup2", "command", 2, EBADF, "stdout:"},
    {"RedirectStderr", "dup2", "command", 3, EBADF, "stderr:"},
    {"EnumerateDescriptors", "opendir", "command", 1, EIO, "/dev/fd:"},
    {"ReadDescriptors", "readdir", "command", 1, EIO, "/dev/fd:"},
    {"CloseInherited", "close", "command", 4, EIO,
     "close inherited descriptor:"},
    {"CloseDirectory", "closedir", "command", 1, EIO, "/dev/fd:"},
    {"RestoreSigpipe", "sigaction", "command", 1, EIO, "SIGPIPE:"},
};

INSTANTIATE_TEST_SUITE_P(SystemCalls, StartupFailureTest,
                         testing::ValuesIn(kStartupFailures), FaultName);

class StartupRetryTest : public FaultTest,
                         public testing::WithParamInterface<Fault> {
};

TEST_P(StartupRetryTest, RetriesWithoutChangingTheStartedCommand) {
    const auto result = RunProbe({}, "state", {}, Inject(GetParam()));

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().at("pid"), result.program_pid);
    PingProbe();
}

const Fault kStartupRetries[] = {
    {"InterruptedPidRead", "read", "launcher", 1, EINTR, ""},
    {"InterruptedStatusRead", "read", "launcher", 2, EINTR, ""},
    {"InterruptedPermissionRead", "read", "command", 1, EINTR, ""},
    {"InterruptedPidSend", "send", "command", 1, EINTR, ""},
    {"InterruptedPermissionSend", "send", "launcher", 1, EINTR, ""},
    {"InterruptedWait", "waitpid", "launcher", 1, EINTR, ""},
    {"AlreadyReapedIntermediate", "waitpid", "launcher", 1, ECHILD, ""},
    {"PartialPidRead", "read", "launcher", 1, FAULT_SHORT_IO, ""},
    {"PartialPidSend", "send", "command", 1, FAULT_SHORT_IO, ""},
};

INSTANTIATE_TEST_SUITE_P(StartupChannel, StartupRetryTest,
                         testing::ValuesIn(kStartupRetries), FaultName);

class DiagnosticFailureTest : public FaultTest,
                              public testing::WithParamInterface<Fault> {
};

TEST_P(DiagnosticFailureTest, StillReportsStartupFailureAndCleansUp) {
    const auto &fault = GetParam();
    const auto result = Run({"--", Path("missing-command")}, Inject(fault));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
        << result.error;
    ExpectTemporaryFilesRemoved();
}

const Fault kDiagnosticFailures[] = {
    {"SeekLog", "lseek", "launcher", 1, EIO, "stderr:"},
    {"ReadLog", "read", "launcher", 3, EIO, "stderr:"},
    {"InterruptedLogRead", "read", "launcher", 3, EINTR, "exec:"},
    {"WriteDiagnostic", "fwrite", "launcher", 1, EIO,
     "Failed to start the command"},
    {"RestoreIgnoredSigpipe", "sigaction", "command", 2, EIO,
     "Failed to start the command"},
    {"InterruptedFailureReport", "send", "command", 2, EINTR, "exec:"},
    {"UnlinkStdout", "unlink", "command", 1, EACCES, "/tmp/stdout."},
    {"UnlinkStderr", "unlink", "command", 2, EACCES, "/tmp/stderr."},
};

INSTANTIATE_TEST_SUITE_P(Diagnostics, DiagnosticFailureTest,
                         testing::ValuesIn(kDiagnosticFailures), FaultName);

TEST_F(FaultTest, FailureToReopenDevNullIsReported) {
    auto options = Inject({"", "open", "launcher", 1, EACCES, ""});
    options.closed_standard_descriptors = 1 << STDIN_FILENO;
    const auto result = Run({"--", PROBE_PATH, "state", SocketPath()}, options);

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("/dev/null:"), std::string::npos);
}

TEST_F(FaultTest, FailureToShareOutputOffsetsIsReported) {
    const auto result = RunProbe({"--stdout", Path("combined"),
                                  "--stderr", Path("combined")}, "state", {},
        Inject({"", "dup2", "launcher", 1, EBADF, ""}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("stderr:"), std::string::npos);
}

TEST_F(FaultTest, UnexpectedParserReturnIsRejected) {
    const auto result = Run({"--help"},
        Inject({"", "getopt_long", "launcher", 1, EINVAL, ""}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("Unknown option."), std::string::npos);
}

TEST_F(FaultTest, NullOptionArgumentIsTreatedAsEmpty) {
    LaunchOptions options;
    options.executable = FAULT_PATH;
    options.environment["DETACH_TEST_HELPER"] = "empty-null";

    EXPECT_EQ(Run({}, options).exit_code, 0);
}

TEST_F(FaultTest, RedirectingADescriptorToItselfKeepsItOpen) {
    LaunchOptions options;
    options.executable = FAULT_PATH;
    options.environment["DETACH_TEST_HELPER"] = "same-descriptor";

    EXPECT_EQ(Run({}, options).exit_code, 0);
}

class HelpFailureTest : public FaultTest,
                        public testing::WithParamInterface<const char *> {
};

TEST_P(HelpFailureTest, ReportsAWriteOrCloseError) {
    const auto result = Run({"--help"},
        Inject({"", GetParam(), "launcher", 1, EIO, ""}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("help:"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(HelpOutput, HelpFailureTest,
                         testing::Values("fprintf", "fclose"));

class ReportFailureTest : public FaultTest,
                          public testing::WithParamInterface<const char *> {
};

TEST_P(ReportFailureTest, KeepsTheCommandAndItsLogsAvailable) {
    const auto result = RunProbe({}, "state", {},
        Inject({"", GetParam(), "launcher", 1, EIO, ""}));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find("The command may already be running"),
              std::string::npos);
    EXPECT_EQ(ReadProbe().at("pid"), result.program_pid);
    PingProbe();
}

INSTANTIATE_TEST_SUITE_P(LaunchReport, ReportFailureTest,
                         testing::Values("printf", "fclose"));

TEST_F(FaultTest, StatusReadErrorPreservesThePossiblyRunningCommand) {
    const auto result = RunProbe({}, "state", {},
        Inject({"", "read", "launcher", 2, EIO, ""}));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find("startup channel:"), std::string::npos);
    EXPECT_EQ(ReadProbe().at("pid"), result.program_pid);
    PingProbe();
}

TEST_F(FaultTest, AliasedTemporaryStderrIsReadThroughTheOpenDescriptor) {
    LaunchOptions options;
    options.executable = FAULT_PATH;
    options.environment["DETACH_TEST_EVENTS"] = Path("events");
    options.environment["DETACH_TEST_STDERR_ALIAS"] = Path("stderr-alias");
    const auto result = Run({"--stderr", Path("stderr-alias"), "--",
                             Path("missing-command")}, options);

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("exec:"), std::string::npos) << result.error;
    EXPECT_NE(ReadFile(Path("stderr-alias")).find("exec:"), std::string::npos);
    ExpectTemporaryFilesRemoved();
}

TEST_F(FaultTest, LostFailureReportHasTheDocumentedEofStartupStatus) {
    const auto result = Run({"--stdout", Path("output"),
                             "--stderr", Path("errors"),
                             "--", Path("missing-command")},
        Inject({"", "send", "command", 2, EPIPE, ""}));

    // Without a failure byte, EOF cannot distinguish exec from early exit.
    EXPECT_EQ(result.exit_code, 0) << result.error;
    EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
}

TEST_F(FaultTest, InvalidPermissionStopsTheCommandBeforeExec) {
    const auto result = Run({"--stdout", Path("output"),
                             "--stderr", Path("errors"),
                             "--", Path("missing-command")},
        Inject({"", "read", "command", 1, FAULT_BAD_PERMISSION, ""}));

    EXPECT_EQ(result.exit_code, 0) << result.error;
    EXPECT_TRUE(ReadFile(Path("errors")).empty());
    EXPECT_TRUE(ReadFile(Path("output")).empty());
}

TEST_F(FaultTest, UnexpectedParserAdvanceIsNotASeparator) {
    const auto result = Run({"ordinary-argument"},
        Inject({"", "getopt_long", "launcher", 1, FAULT_PARSER_ADVANCE, ""}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("Missing command"), std::string::npos);
}

struct DirectoryEntry {
    const char *name;
    const char *value;
};

class DescriptorEntryTest : public FaultTest,
                            public testing::WithParamInterface<DirectoryEntry> {
};

TEST_P(DescriptorEntryTest, IgnoresNamesThatAreNotValidDescriptors) {
    auto options = Inject({"", "readdir", "command", 1,
                            FAULT_DIRECTORY_ENTRY, ""});
    options.environment["DETACH_TEST_ENTRY"] = GetParam().value;
    const auto result = RunProbe({}, "state", {}, options);

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().at("extra_descriptors"), 0);
}

std::string EntryName(const testing::TestParamInfo<DirectoryEntry> &info) {
    return info.param.name;
}

const DirectoryEntry kInvalidEntries[] = {
    {"NotANumber", "not-a-number"},
    {"TrailingCharacters", "4trailing"},
    {"NumericOverflow", "99999999999999999999999999999999999"},
    {"AboveIntMax", "2147483648"},
};

INSTANTIATE_TEST_SUITE_P(DirectoryEntries, DescriptorEntryTest,
                         testing::ValuesIn(kInvalidEntries), EntryName);

TEST_F(FaultTest, IgnoresNonNumericEntriesWhenConversionLeavesErrnoUnchanged) {
    auto options = Inject({"", "readdir", "command", 1,
                            FAULT_DIRECTORY_ENTRY, ""});
    options.environment["DETACH_TEST_ENTRY"] = "not-a-number";
    options.environment["DETACH_TEST_FAULTS"] += ";command:strtoimax:1:0";
    const auto result = RunProbe({}, "state", {}, options);

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().at("extra_descriptors"), 0);
}

}  // namespace
}  // namespace detach_test
