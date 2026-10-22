// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "gtest/gtest.h"

#include "tests/fault_test.h"
#include "tests/faults.h"
#include "tests/test_files.h"

namespace detach_test {
namespace {

// The scenario name and expected diagnostic describe observable behavior.
struct FaultCase {
  const char* name;
  FaultRule rule;
  const char* diagnostic = "";
};

// Uses the scenario name in CTest failures instead of a numeric table index.
std::string FaultName(const testing::TestParamInfo<FaultCase>& info) {
  return info.param.name;
}

// Names reporting-failure cases after the operation being exercised.
std::string OperationName(const testing::TestParamInfo<const char*>& info) {
  return info.param;
}

class StartupFailureTest : public FaultTest,
                           public testing::WithParamInterface<FaultCase> {
 protected:
  // Keeps a real inherited descriptor open to exercise child handle cleanup
  // even when another startup operation is deliberately failing.
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(FaultTest::SetUp());
    inherited_.Reset(open(Path("inherited").c_str(), O_CREAT | O_RDWR, 0600));
    ASSERT_TRUE(CheckCall(inherited_.get(), "open inherited descriptor"));
  }

  // Survive exec deliberately to exercise closing inherited descriptors.
  Descriptor inherited_;
};

TEST_P(StartupFailureTest, ReportsTheFailureAndRemovesTemporaryLogs) {
  const auto& fault = GetParam();
  const auto result = RunProbe({"--pid", Path("pid")}, Inject(fault.rule));

  ASSERT_EQ(result.exit_code, 1) << result.error;
  EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
      << result.error;
  ExpectTemporaryFilesRemoved();
}

// Call numbers are local to each role and include earlier setup operations.
// The launcher checks three standard fds before marking the startup socket.
// Its fstat order is stdout, stderr, optional PID, then stdin. The command's
// first three close calls release original stream fds after redirecting them.
const FaultCase kStartupFailures[] = {
    {"SignalMask", {"sigemptyset", ProcessRole::kLauncher, 1, EIO}, "SIGPIPE:"},
    {"SignalDisposition",
     {"sigaction", ProcessRole::kLauncher, 1, EIO},
     "SIGPIPE:"},
    {"StandardDescriptor",
     {"fcntl", ProcessRole::kLauncher, 1, EIO},
     "standard descriptors:"},
    {"TemporaryStdout", {"mkstemp", ProcessRole::kLauncher, 1, EIO}, "stdout:"},
    {"TemporaryStderr", {"mkstemp", ProcessRole::kLauncher, 2, EIO}, "stderr:"},
    {"StdoutStat", {"fstat", ProcessRole::kLauncher, 1, EIO}, "stdout:"},
    {"StderrStat", {"fstat", ProcessRole::kLauncher, 2, EIO}, "stderr:"},
    {"PidStat", {"fstat", ProcessRole::kLauncher, 3, EIO}, "pid:"},
    {"StdinStat", {"fstat", ProcessRole::kLauncher, 4, EIO}, "stdin:"},
    {"StdoutTruncate",
     {"ftruncate", ProcessRole::kLauncher, 1, EIO},
     "stdout:"},
    {"StderrTruncate",
     {"ftruncate", ProcessRole::kLauncher, 2, EIO},
     "stderr:"},
    {"PidTruncate", {"ftruncate", ProcessRole::kLauncher, 3, EIO}, "pid:"},
    {"StdoutRewind", {"lseek", ProcessRole::kLauncher, 1, EIO}, "stdout:"},
    {"StderrRewind", {"lseek", ProcessRole::kLauncher, 2, EIO}, "stderr:"},
    {"PidRewind", {"lseek", ProcessRole::kLauncher, 3, EIO}, "pid:"},
    {"PidWrite", {"dprintf", ProcessRole::kLauncher, 1, EIO}, "pid:"},
    {"PidClose", {"close", ProcessRole::kLauncher, 3, EIO}, "pid:"},
    {"StartupSocket",
     {"socketpair", ProcessRole::kLauncher, 1, EIO},
     "startup channel:"},
    {"StartupCloseOnExec",
     {"fcntl", ProcessRole::kLauncher, 4, EIO},
     "startup channel:"},
    {"FirstFork", {"fork", ProcessRole::kLauncher, 1, EAGAIN}, "fork:"},
    {"NewSession",
     {"setsid", ProcessRole::kIntermediate, 1, EPERM},
     "Failed to start the command"},
    {"SecondFork",
     {"fork", ProcessRole::kIntermediate, 1, EAGAIN},
     "Failed to start the command"},
    {"WaitIntermediate",
     {"waitpid", ProcessRole::kLauncher, 1, EIO},
     "waitpid:"},
    {"ReceivePidError",
     {"read", ProcessRole::kLauncher, 1, EIO},
     "startup channel:"},
    {"ReceivePidEof",
     {"read", ProcessRole::kLauncher, 1, FAULT_EOF},
     "Failed to start the command"},
    {"SendPermission",
     {"send", ProcessRole::kLauncher, 1, EIO},
     "startup channel:"},
    {"SendPidError",
     {"send", ProcessRole::kCommand, 1, EIO},
     "Failed to start the command"},
    {"SendPidZero",
     {"send", ProcessRole::kCommand, 1, FAULT_EOF},
     "Failed to start the command"},
    {"ReceivePermission",
     {"read", ProcessRole::kCommand, 1, EIO},
     "startup channel:"},
    {"RedirectStdin", {"dup2", ProcessRole::kCommand, 1, EBADF}, "stdin:"},
    {"RedirectStdout", {"dup2", ProcessRole::kCommand, 2, EBADF}, "stdout:"},
    {"RedirectStderr", {"dup2", ProcessRole::kCommand, 3, EBADF}, "stderr:"},
    {"EnumerateDescriptors",
     {"opendir", ProcessRole::kCommand, 1, EIO},
     "/dev/fd:"},
    {"ReadDescriptors", {"readdir", ProcessRole::kCommand, 1, EIO}, "/dev/fd:"},
    {"CloseInherited",
     {"close", ProcessRole::kCommand, 4, EIO},
     "close inherited descriptor:"},
    {"CloseDirectory", {"closedir", ProcessRole::kCommand, 1, EIO}, "/dev/fd:"},
    {"RestoreSigpipe",
     {"sigaction", ProcessRole::kCommand, 1, EIO},
     "SIGPIPE:"},
};

INSTANTIATE_TEST_SUITE_P(SystemCalls, StartupFailureTest,
                         testing::ValuesIn(kStartupFailures), FaultName);

class StartupRetryTest : public FaultTest,
                         public testing::WithParamInterface<FaultCase> {
};

TEST_P(StartupRetryTest, RetriesWithoutChangingTheStartedCommand) {
  const auto result = RunProbe({}, Inject(GetParam().rule));

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
  PingProbe();
}

const FaultCase kStartupRetries[] = {
    {"InterruptedPidRead", {"read", ProcessRole::kLauncher, 1, EINTR}},
    {"InterruptedStatusRead", {"read", ProcessRole::kLauncher, 2, EINTR}},
    {"InterruptedPermissionRead", {"read", ProcessRole::kCommand, 1, EINTR}},
    {"InterruptedPidSend", {"send", ProcessRole::kCommand, 1, EINTR}},
    {"InterruptedPermissionSend", {"send", ProcessRole::kLauncher, 1, EINTR}},
    {"InterruptedWait", {"waitpid", ProcessRole::kLauncher, 1, EINTR}},
    {"AlreadyReapedIntermediate",
     {"waitpid", ProcessRole::kLauncher, 1, ECHILD}},
    {"PartialPidRead", {"read", ProcessRole::kLauncher, 1, FAULT_SHORT_IO}},
    {"PartialPidSend", {"send", ProcessRole::kCommand, 1, FAULT_SHORT_IO}},
};

INSTANTIATE_TEST_SUITE_P(StartupChannel, StartupRetryTest,
                         testing::ValuesIn(kStartupRetries), FaultName);

class DiagnosticFailureTest : public FaultTest,
                              public testing::WithParamInterface<FaultCase> {
};

TEST_P(DiagnosticFailureTest, StillReportsStartupFailureAndCleansUp) {
  const auto& fault = GetParam();
  const auto result = Run({"--", Path("missing-command")}, Inject(fault.rule));

  ASSERT_EQ(result.exit_code, 1) << result.error;
  EXPECT_NE(result.error.find(fault.diagnostic), std::string::npos)
      << result.error;
  ExpectTemporaryFilesRemoved();
}

// Without a PID file, the first two seeks reset stdout and stderr.
// The third seeks back through the error log after the command fails.
const FaultCase kDiagnosticFailures[] = {
    {"SeekLog", {"lseek", ProcessRole::kLauncher, 3, EIO}, "stderr:"},
    {"ReadLog", {"read", ProcessRole::kLauncher, 3, EIO}, "stderr:"},
    {"InterruptedLogRead", {"read", ProcessRole::kLauncher, 3, EINTR}, "exec:"},
    {"WriteDiagnostic",
     {"fwrite", ProcessRole::kLauncher, 1, EIO},
     "Failed to start the command"},
    {"RestoreIgnoredSigpipe",
     {"sigaction", ProcessRole::kCommand, 2, EIO},
     "Failed to start the command"},
    {"InterruptedFailureReport",
     {"send", ProcessRole::kCommand, 2, EINTR},
     "exec:"},
    {"UnlinkStdout",
     {"unlink", ProcessRole::kCommand, 1, EACCES},
     "/tmp/stdout."},
    {"UnlinkStderr",
     {"unlink", ProcessRole::kCommand, 2, EACCES},
     "/tmp/stderr."},
};

INSTANTIATE_TEST_SUITE_P(Diagnostics, DiagnosticFailureTest,
                         testing::ValuesIn(kDiagnosticFailures), FaultName);

TEST_F(FaultTest, InputAliasFailurePreservesInputAndRemovesTemporaryLogs) {
  WriteFile(Path("input"), "keep stdin\n");
  const auto result = RunProbe(
      {"--stdin", Path("input"), "--stdout", Path("input")}, FaultOptions());

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.program_pid, -1);
  EXPECT_EQ(ReadFile(Path("input")), "keep stdin\n");
  ExpectTemporaryFilesRemoved();
}

TEST_F(FaultTest, FailureToReopenDevNullIsReported) {
  auto options = Inject({"open", ProcessRole::kLauncher, 1, EACCES});
  options.closed_standard_descriptors = 1 << STDIN_FILENO;
  const auto result = Run({"--", PROBE_PATH, "state", SocketPath()}, options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("/dev/null:"), std::string::npos);
}

TEST_F(FaultTest, FailureToShareOutputOffsetsIsReported) {
  const auto result =
      RunProbe({"--stdout", Path("combined"), "--stderr", Path("combined")},
               Inject({"dup2", ProcessRole::kLauncher, 1, EBADF}));

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("stderr:"), std::string::npos);
}

TEST_F(FaultTest, UnexpectedParserReturnIsRejected) {
  const auto result = Run(
      {"--help"}, Inject({"getopt_long", ProcessRole::kLauncher, 1, EINVAL}));

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
                        public testing::WithParamInterface<const char*> {
};

TEST_P(HelpFailureTest, ReportsAWriteOrCloseError) {
  const auto result =
      Run({"--help"}, Inject({GetParam(), ProcessRole::kLauncher, 1, EIO}));

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("help:"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(HelpOutput, HelpFailureTest,
                         testing::Values("fprintf", "fclose"), OperationName);

class ReportFailureTest : public FaultTest,
                          public testing::WithParamInterface<const char*> {
};

TEST_P(ReportFailureTest, KeepsTheCommandAndItsLogsAvailable) {
  const auto result =
      RunProbe({}, Inject({GetParam(), ProcessRole::kLauncher, 1, EIO}));

  ASSERT_EQ(result.exit_code, 1) << result.error;
  EXPECT_NE(result.error.find("The command may already be running"),
            std::string::npos);
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
  PingProbe();
}

INSTANTIATE_TEST_SUITE_P(LaunchReport, ReportFailureTest,
                         testing::Values("printf", "fclose"), OperationName);

TEST_F(FaultTest, StatusReadErrorPreservesThePossiblyRunningCommand) {
  const auto result =
      RunProbe({}, Inject({"read", ProcessRole::kLauncher, 2, EIO}));

  ASSERT_EQ(result.exit_code, 1) << result.error;
  EXPECT_NE(result.error.find("startup channel:"), std::string::npos);
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
  PingProbe();
}

TEST_F(FaultTest, AliasedTemporaryStderrIsReadThroughTheOpenDescriptor) {
  auto options = FaultOptions();
  options.environment["DETACH_TEST_STDERR_ALIAS"] = Path("stderr-alias");
  const auto result =
      Run({"--stderr", Path("stderr-alias"), "--", Path("missing-command")},
          options);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("exec:"), std::string::npos) << result.error;
  EXPECT_NE(ReadFile(Path("stderr-alias")).find("exec:"), std::string::npos);
  ExpectTemporaryFilesRemoved();
}

TEST_F(FaultTest, LostFailureReportHasTheDocumentedEofStartupStatus) {
  const auto result = Run({"--stdout", Path("output"), "--stderr",
                           Path("errors"), "--", Path("missing-command")},
                          Inject({"send", ProcessRole::kCommand, 2, EPIPE}));

  // Without a failure byte, EOF cannot distinguish exec from early exit.
  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
}

// A malformed permission byte stops the command before exec. The parent sees
// EOF without a failure byte, exercising the documented handshake limitation.
TEST_F(FaultTest, InvalidPermissionStopsTheCommandBeforeExec) {
  const auto result =
      Run({"--stdout", Path("output"), "--stderr", Path("errors"), "--",
           Path("missing-command")},
          Inject({"read", ProcessRole::kCommand, 1, FAULT_BAD_PERMISSION}));

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_TRUE(ReadFile(Path("errors")).empty());
  EXPECT_TRUE(ReadFile(Path("output")).empty());
}

TEST_F(FaultTest, UnexpectedParserAdvanceIsNotASeparator) {
  const auto result = Run(
      {"ordinary-argument"},
      Inject({"getopt_long", ProcessRole::kLauncher, 1, FAULT_PARSER_ADVANCE}));

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing command"), std::string::npos);
}

struct DirectoryEntry {
  const char* name;
  const char* value;
};

class DescriptorEntryTest : public FaultTest,
                            public testing::WithParamInterface<DirectoryEntry> {
};

TEST_P(DescriptorEntryTest, IgnoresNamesThatAreNotValidDescriptors) {
  auto options =
      Inject({"readdir", ProcessRole::kCommand, 1, FAULT_DIRECTORY_ENTRY});
  options.environment["DETACH_TEST_ENTRY"] = GetParam().value;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

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

INSTANTIATE_TEST_SUITE_P(DirectoryEntries, DescriptorEntryTest,
                         testing::ValuesIn(kInvalidEntries), EntryName);

TEST_F(FaultTest, IgnoresNonNumericEntriesWhenConversionLeavesErrnoUnchanged) {
  auto options = Inject({
      {"readdir", ProcessRole::kCommand, 1, FAULT_DIRECTORY_ENTRY},
      {"strtoimax", ProcessRole::kCommand, 1, 0},
  });
  options.environment["DETACH_TEST_ENTRY"] = "not-a-number";
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().extra_descriptors, 0);
}

}  // namespace
}  // namespace detach_test
