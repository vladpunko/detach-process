// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "tests/faults.h"
#include "tests/support.h"

namespace detach_test {
namespace {

enum class Role { kLauncher, kIntermediate, kCommand };

struct FaultRule {
    const char *operation;
    Role role;
    unsigned int call;
    int error;
};

// Test names and expected messages describe a scenario, not its injection rule.
struct FaultCase {
    const char *name;
    FaultRule rule;
    const char *diagnostic = "";
};

const char *RoleName(Role role) {
    switch (role) {
    case Role::kLauncher:
        return "launcher";
    case Role::kIntermediate:
        return "intermediate";
    case Role::kCommand:
        return "command";
    }
    ADD_FAILURE() << "Unknown fault process role";
    return "";
}

std::string FaultName(const testing::TestParamInfo<FaultCase> &info) {
    return info.param.name;
}

class FaultTest : public DetachTest {
 protected:
    void SetUp() override {
        DetachTest::SetUp();
        if (!HasFatalFailure()) {
            WriteFile(Path("events"), "");
            events_ready_ = !HasFatalFailure();
        }
    }

    void TearDown() override {
        const auto paths = TemporaryFiles();
        // A broken event log must not prevent releasing the probe and sockets.
        DetachTest::TearDown();
        // Also clean files from intentionally failed unlink calls.
        for (const auto &path : paths) {
            if (unlink(path.c_str()) < 0) {
                EXPECT_EQ(errno, ENOENT) << path;
            }
        }
    }

    LaunchOptions FaultOptions() const {
        LaunchOptions options;
        options.executable = FAULT_PATH;
        options.environment["DETACH_TEST_EVENTS"] = Path("events");
        return options;
    }

    LaunchOptions Inject(std::initializer_list<FaultRule> rules) const {
        auto options = FaultOptions();
        std::string plan;
        for (const auto &rule : rules) {
            if (!plan.empty()) {
                plan += ";";
            }
            plan += std::string(RoleName(rule.role)) + ":" + rule.operation +
                    ":" + std::to_string(rule.call) + ":" +
                    std::to_string(rule.error);
        }
        options.environment["DETACH_TEST_FAULTS"] = plan;
        return options;
    }

    LaunchOptions Inject(const FaultRule &rule) const {
        return Inject({rule});
    }

    std::vector<std::string> TemporaryFiles() const {
        std::vector<std::string> paths;
        if (!events_ready_) {
            return paths;
        }
        std::istringstream events(ReadFile(Path("events")));
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
                           public testing::WithParamInterface<FaultCase> {
 protected:
    void SetUp() override {
        FaultTest::SetUp();
        if (!HasFatalFailure()) {
            inherited_.Reset(open(Path("inherited").c_str(),
                O_CREAT | O_RDWR, 0600));
            ASSERT_TRUE(CheckCall(inherited_.Get(), "open inherited descriptor"));
        }
    }

    // Survive exec deliberately to exercise closing inherited descriptors.
    Descriptor inherited_;
};

TEST_P(StartupFailureTest, ReportsTheFailureAndRemovesTemporaryLogs) {
    const auto &fault = GetParam();
    const auto result = RunProbe({"--pid", Path("pid")},
                                 "state", {}, Inject(fault.rule));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
        << result.error;
    ExpectTemporaryFilesRemoved();
}

// Call numbers are local to each role and include earlier setup operations.
// The launcher checks three standard fds before marking the startup socket.
const FaultCase kStartupFailures[] = {
    {"SignalMask", {"sigemptyset", Role::kLauncher, 1, EIO}, "SIGPIPE:"},
    {"SignalDisposition", {"sigaction", Role::kLauncher, 1, EIO}, "SIGPIPE:"},
    {"StandardDescriptor", {"fcntl", Role::kLauncher, 1, EIO},
     "standard descriptors:"},
    {"TemporaryStdout", {"mkstemp", Role::kLauncher, 1, EIO}, "stdout:"},
    {"TemporaryStderr", {"mkstemp", Role::kLauncher, 2, EIO}, "stderr:"},
    {"StdoutStat", {"fstat", Role::kLauncher, 1, EIO}, "stdout:"},
    {"StderrStat", {"fstat", Role::kLauncher, 2, EIO}, "stderr:"},
    {"PidStat", {"fstat", Role::kLauncher, 3, EIO}, "pid:"},
    {"StdinStat", {"fstat", Role::kLauncher, 4, EIO}, "stdin:"},
    {"StdoutTruncate", {"ftruncate", Role::kLauncher, 1, EIO}, "stdout:"},
    {"StderrTruncate", {"ftruncate", Role::kLauncher, 2, EIO}, "stderr:"},
    {"PidTruncate", {"ftruncate", Role::kLauncher, 3, EIO}, "pid:"},
    {"StdoutRewind", {"lseek", Role::kLauncher, 1, EIO}, "stdout:"},
    {"StderrRewind", {"lseek", Role::kLauncher, 2, EIO}, "stderr:"},
    {"PidRewind", {"lseek", Role::kLauncher, 3, EIO}, "pid:"},
    {"PidWrite", {"dprintf", Role::kLauncher, 1, EIO}, "pid:"},
    {"PidClose", {"close", Role::kLauncher, 3, EIO}, "pid:"},
    {"StartupSocket", {"socketpair", Role::kLauncher, 1, EIO},
     "startup channel:"},
    {"StartupCloseOnExec", {"fcntl", Role::kLauncher, 4, EIO},
     "startup channel:"},
    {"FirstFork", {"fork", Role::kLauncher, 1, EAGAIN}, "fork:"},
    {"NewSession", {"setsid", Role::kIntermediate, 1, EPERM},
     "Failed to start the command"},
    {"SecondFork", {"fork", Role::kIntermediate, 1, EAGAIN},
     "Failed to start the command"},
    {"WaitIntermediate", {"waitpid", Role::kLauncher, 1, EIO}, "waitpid:"},
    {"ReceivePidError", {"read", Role::kLauncher, 1, EIO}, "startup channel:"},
    {"ReceivePidEof", {"read", Role::kLauncher, 1, FAULT_EOF},
     "Failed to start the command"},
    {"SendPermission", {"send", Role::kLauncher, 1, EIO}, "startup channel:"},
    {"SendPidError", {"send", Role::kCommand, 1, EIO},
     "Failed to start the command"},
    {"SendPidZero", {"send", Role::kCommand, 1, FAULT_EOF},
     "Failed to start the command"},
    {"ReceivePermission", {"read", Role::kCommand, 1, EIO}, "startup channel:"},
    {"RedirectStdin", {"dup2", Role::kCommand, 1, EBADF}, "stdin:"},
    {"RedirectStdout", {"dup2", Role::kCommand, 2, EBADF}, "stdout:"},
    {"RedirectStderr", {"dup2", Role::kCommand, 3, EBADF}, "stderr:"},
    {"EnumerateDescriptors", {"opendir", Role::kCommand, 1, EIO}, "/dev/fd:"},
    {"ReadDescriptors", {"readdir", Role::kCommand, 1, EIO}, "/dev/fd:"},
    {"CloseInherited", {"close", Role::kCommand, 4, EIO},
     "close inherited descriptor:"},
    {"CloseDirectory", {"closedir", Role::kCommand, 1, EIO}, "/dev/fd:"},
    {"RestoreSigpipe", {"sigaction", Role::kCommand, 1, EIO}, "SIGPIPE:"},
};

INSTANTIATE_TEST_SUITE_P(SystemCalls, StartupFailureTest,
                         testing::ValuesIn(kStartupFailures), FaultName);

class StartupRetryTest : public FaultTest,
                         public testing::WithParamInterface<FaultCase> {
};

TEST_P(StartupRetryTest, RetriesWithoutChangingTheStartedCommand) {
    const auto result = RunProbe({}, "state", {}, Inject(GetParam().rule));

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().pid, result.program_pid);
    PingProbe();
}

const FaultCase kStartupRetries[] = {
    {"InterruptedPidRead", {"read", Role::kLauncher, 1, EINTR}},
    {"InterruptedStatusRead", {"read", Role::kLauncher, 2, EINTR}},
    {"InterruptedPermissionRead", {"read", Role::kCommand, 1, EINTR}},
    {"InterruptedPidSend", {"send", Role::kCommand, 1, EINTR}},
    {"InterruptedPermissionSend", {"send", Role::kLauncher, 1, EINTR}},
    {"InterruptedWait", {"waitpid", Role::kLauncher, 1, EINTR}},
    {"AlreadyReapedIntermediate", {"waitpid", Role::kLauncher, 1, ECHILD}},
    {"PartialPidRead", {"read", Role::kLauncher, 1, FAULT_SHORT_IO}},
    {"PartialPidSend", {"send", Role::kCommand, 1, FAULT_SHORT_IO}},
};

INSTANTIATE_TEST_SUITE_P(StartupChannel, StartupRetryTest,
                         testing::ValuesIn(kStartupRetries), FaultName);

class DiagnosticFailureTest : public FaultTest,
                              public testing::WithParamInterface<FaultCase> {
};

TEST_P(DiagnosticFailureTest, StillReportsStartupFailureAndCleansUp) {
    const auto &fault = GetParam();
    const auto result = Run({"--", Path("missing-command")},
                             Inject(fault.rule));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
        << result.error;
    ExpectTemporaryFilesRemoved();
}

// Without a PID file, the first two seeks reset stdout and stderr.
// The third seeks back through the error log after the command fails.
const FaultCase kDiagnosticFailures[] = {
    {"SeekLog", {"lseek", Role::kLauncher, 3, EIO}, "stderr:"},
    {"ReadLog", {"read", Role::kLauncher, 3, EIO}, "stderr:"},
    {"InterruptedLogRead", {"read", Role::kLauncher, 3, EINTR}, "exec:"},
    {"WriteDiagnostic", {"fwrite", Role::kLauncher, 1, EIO},
     "Failed to start the command"},
    {"RestoreIgnoredSigpipe", {"sigaction", Role::kCommand, 2, EIO},
     "Failed to start the command"},
    {"InterruptedFailureReport", {"send", Role::kCommand, 2, EINTR}, "exec:"},
    {"UnlinkStdout", {"unlink", Role::kCommand, 1, EACCES}, "/tmp/stdout."},
    {"UnlinkStderr", {"unlink", Role::kCommand, 2, EACCES}, "/tmp/stderr."},
};

INSTANTIATE_TEST_SUITE_P(Diagnostics, DiagnosticFailureTest,
                         testing::ValuesIn(kDiagnosticFailures), FaultName);

TEST_F(FaultTest, InputAliasFailurePreservesInputAndRemovesTemporaryLogs) {
    WriteFile(Path("input"), "keep stdin\n");
    const auto result = RunProbe({"--stdin", Path("input"),
                                  "--stdout", Path("input")},
                                 "state", {}, FaultOptions());

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_EQ(result.program_pid, -1);
    EXPECT_EQ(ReadFile(Path("input")), "keep stdin\n");
    ExpectTemporaryFilesRemoved();
}

TEST_F(FaultTest, FailureToReopenDevNullIsReported) {
    auto options = Inject({"open", Role::kLauncher, 1, EACCES});
    options.closed_standard_descriptors = 1 << STDIN_FILENO;
    const auto result = Run({"--", PROBE_PATH, "state", SocketPath()}, options);

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("/dev/null:"), std::string::npos);
}

TEST_F(FaultTest, FailureToShareOutputOffsetsIsReported) {
    const auto result = RunProbe({"--stdout", Path("combined"),
                                  "--stderr", Path("combined")}, "state", {},
        Inject({"dup2", Role::kLauncher, 1, EBADF}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("stderr:"), std::string::npos);
}

TEST_F(FaultTest, UnexpectedParserReturnIsRejected) {
    const auto result = Run({"--help"},
        Inject({"getopt_long", Role::kLauncher, 1, EINVAL}));

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("Unknown option."), std::string::npos);
}

TEST_F(FaultTest, NullOptionArgumentIsTreatedAsEmpty) {
    auto options = FaultOptions();
    options.environment["DETACH_TEST_HELPER"] = "empty-null";

    EXPECT_EQ(Run({}, options).exit_code, 0);
}

TEST_F(FaultTest, RedirectingADescriptorToItselfKeepsItOpen) {
    auto options = FaultOptions();
    options.environment["DETACH_TEST_HELPER"] = "same-descriptor";

    EXPECT_EQ(Run({}, options).exit_code, 0);
}

class HelpFailureTest : public FaultTest,
                        public testing::WithParamInterface<const char *> {
};

TEST_P(HelpFailureTest, ReportsAWriteOrCloseError) {
    const auto result = Run({"--help"},
        Inject({GetParam(), Role::kLauncher, 1, EIO}));

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
        Inject({GetParam(), Role::kLauncher, 1, EIO}));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find("The command may already be running"),
              std::string::npos);
    EXPECT_EQ(ReadProbe().pid, result.program_pid);
    PingProbe();
}

INSTANTIATE_TEST_SUITE_P(LaunchReport, ReportFailureTest,
                         testing::Values("printf", "fclose"));

TEST_F(FaultTest, StatusReadErrorPreservesThePossiblyRunningCommand) {
    const auto result = RunProbe({}, "state", {},
        Inject({"read", Role::kLauncher, 2, EIO}));

    ASSERT_EQ(result.exit_code, 1) << result.error;
    EXPECT_NE(result.error.find("startup channel:"), std::string::npos);
    EXPECT_EQ(ReadProbe().pid, result.program_pid);
    PingProbe();
}

TEST_F(FaultTest, AliasedTemporaryStderrIsReadThroughTheOpenDescriptor) {
    auto options = FaultOptions();
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
        Inject({"send", Role::kCommand, 2, EPIPE}));

    // Without a failure byte, EOF cannot distinguish exec from early exit.
    EXPECT_EQ(result.exit_code, 0) << result.error;
    EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
}

TEST_F(FaultTest, InvalidPermissionStopsTheCommandBeforeExec) {
    const auto result = Run({"--stdout", Path("output"),
                             "--stderr", Path("errors"),
                             "--", Path("missing-command")},
        Inject({"read", Role::kCommand, 1, FAULT_BAD_PERMISSION}));

    EXPECT_EQ(result.exit_code, 0) << result.error;
    EXPECT_TRUE(ReadFile(Path("errors")).empty());
    EXPECT_TRUE(ReadFile(Path("output")).empty());
}

TEST_F(FaultTest, UnexpectedParserAdvanceIsNotASeparator) {
    const auto result = Run({"ordinary-argument"},
        Inject({"getopt_long", Role::kLauncher, 1, FAULT_PARSER_ADVANCE}));

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
    auto options = Inject(
        {"readdir", Role::kCommand, 1, FAULT_DIRECTORY_ENTRY});
    options.environment["DETACH_TEST_ENTRY"] = GetParam().value;
    const auto result = RunProbe({}, "state", {}, options);

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
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
    auto options = Inject({
        {"readdir", Role::kCommand, 1, FAULT_DIRECTORY_ENTRY},
        {"strtoimax", Role::kCommand, 1, 0},
    });
    options.environment["DETACH_TEST_ENTRY"] = "not-a-number";
    const auto result = RunProbe({}, "state", {}, options);

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

}  // namespace
}  // namespace detach_test
