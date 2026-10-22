// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using FilesTest = DetachTest;

TEST_F(FilesTest, DefaultLogsAreDistinctPrivateTemporaryFiles) {
  LaunchOptions options;
  options.file_creation_mask = 0;
  const auto result = RunProbeMode("streams", {}, {}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(result.stdout_path.find("/tmp/stdout."), 0u);
  EXPECT_EQ(result.stderr_path.find("/tmp/stderr."), 0u);
  EXPECT_NE(result.stdout_path, result.stderr_path);
  EXPECT_EQ(FileMode(result.stdout_path), 0600);
  EXPECT_EQ(FileMode(result.stderr_path), 0600);
  EXPECT_EQ(ReadFile(result.stdout_path), "stdout one\nstdout two\n");
  EXPECT_EQ(ReadFile(result.stderr_path), "stderr one\nstderr two\n");
}

TEST_F(FilesTest, DefaultInputIsDevNull) {
  const auto result = RunProbeMode("stdio");

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().stdin_is_null, 1);
  EXPECT_TRUE(ReadFile(result.stdout_path).empty());
  EXPECT_EQ(ReadFile(result.stderr_path), "probe stderr\n");
}

TEST_F(FilesTest, RedirectsAllThreeStreams) {
  constexpr char input[] = "input\nwith\0binary data\n";
  const std::string contents(input, sizeof(input) - 1);
  WriteFile(Path("input"), contents);
  const auto result =
      RunProbeMode("stdio", {"--stdin", Path("input"), "--stdout",
                             Path("output"), "--stderr", Path("errors")});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().stdin_is_null, 0);
  EXPECT_EQ(ReadFile(Path("input")), contents);
  EXPECT_EQ(ReadFile(Path("output")), contents);
  EXPECT_EQ(ReadFile(Path("errors")), "probe stderr\n");
}

TEST_F(FilesTest, CopiesInputLargerThanAnIoBuffer) {
  const std::string contents(1024 * 1024, 'x');
  WriteFile(Path("input"), contents);
  const auto result = RunProbeMode("stdio", {"--stdin", Path("input")});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(result.stdout_path), contents);
}

TEST_F(FilesTest, ExistingOutputsAreTruncatedWithoutChangingTheirPermissions) {
  WriteFile(Path("output"), std::string(1000, 'x'));
  WriteFile(Path("errors"), std::string(1000, 'y'));
  ASSERT_TRUE(CheckCall(chmod(Path("output").c_str(), 0640), "chmod"));
  ASSERT_TRUE(CheckCall(chmod(Path("errors").c_str(), 0644), "chmod"));
  const auto result = RunProbeMode(
      "streams", {"--stdout", Path("output"), "--stderr", Path("errors")});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(Path("output")), "stdout one\nstdout two\n");
  EXPECT_EQ(ReadFile(Path("errors")), "stderr one\nstderr two\n");
  EXPECT_EQ(FileMode(Path("output")), 0640);
  EXPECT_EQ(FileMode(Path("errors")), 0644);
}

TEST_F(FilesTest, NewlyCreatedOutputAndPidFilesArePrivate) {
  LaunchOptions options;
  options.file_creation_mask = 0;
  const auto result = RunProbe({"--stdout", Path("output"), "--stderr",
                                Path("errors"), "--pid", Path("pid")},
                               options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(FileMode(Path("output")), 0600);
  EXPECT_EQ(FileMode(Path("errors")), 0600);
  EXPECT_EQ(FileMode(Path("pid")), 0600);
}

TEST_F(FilesTest, RelativePathsAndSpacesInPathsWork) {
  WriteFile(Path("input file"), "contents\n");
  const auto result =
      RunProbeMode("stdio", {"--stdin", "input file", "--stdout", "output file",
                             "--stderr", "error file", "--pid", "pid file"});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(ReadFile(Path("output file")), "contents\n");
  EXPECT_EQ(ReadFile(Path("error file")), "probe stderr\n");
  EXPECT_EQ(ReadFile(Path("pid file")), std::to_string(state.pid) + "\n");
}

TEST_F(FilesTest, PidFileIsCompleteBeforeTheCommandStarts) {
  WriteFile(Path("pid"), std::string(1000, '9'));
  const auto result =
      RunProbeMode("pid-file", {"--pid", Path("pid")}, {Path("pid")});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.pid_file, state.pid);
  EXPECT_EQ(result.program_pid, state.pid);
  EXPECT_EQ(ReadFile(Path("pid")), std::to_string(state.pid) + "\n");
}

TEST_F(FilesTest, DevNullCanReceiveBothOutputStreams) {
  const auto result = RunProbeMode(
      "streams", {"--stdout", "/dev/null", "--stderr", "/dev/null"});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(result.stdout_path, "/dev/null");
  EXPECT_EQ(result.stderr_path, "/dev/null");
}

TEST_F(FilesTest, OutputCanBeAPipe) {
  int channel[2];
  ASSERT_TRUE(CheckCall(pipe(channel), "pipe"));
  Descriptor reader(channel[0]);
  Descriptor writer(channel[1]);
  ASSERT_TRUE(CheckCall(fcntl(reader.get(), F_SETFL, O_NONBLOCK), "fcntl"));
  const auto result = RunProbeMode(
      "streams", {"--stdout", "/dev/fd/" + std::to_string(writer.get())});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadAvailable(reader.get()), "stdout one\nstdout two\n");
}

TEST_F(FilesTest, PidCanBeWrittenToAPipeBeforeTheCommandStarts) {
  int channel[2];
  ASSERT_TRUE(CheckCall(pipe(channel), "pipe"));
  Descriptor reader(channel[0]);
  Descriptor writer(channel[1]);
  ASSERT_TRUE(CheckCall(fcntl(reader.get(), F_SETFL, O_NONBLOCK), "fcntl"));
  const auto result =
      RunProbe({"--pid", "/dev/fd/" + std::to_string(writer.get())});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(ReadAvailable(reader.get()), std::to_string(state.pid) + "\n");
  EXPECT_EQ(state.extra_descriptors, 0);
}

class InheritedOutputTest : public DetachTest,
                            public testing::WithParamInterface<const char*> {
};

TEST_P(InheritedOutputTest, WritesFromTheStartOfAReusedRegularFile) {
  WriteFile(Path("reused"), "old contents\n");
  Descriptor inherited(open(Path("reused").c_str(), O_WRONLY));
  ASSERT_TRUE(CheckCall(inherited.get(), "open inherited output"));
  ASSERT_GT(lseek(inherited.get(), 0, SEEK_END), 0);

  // macOS /dev/fd preserves this offset when opening the inherited handle.
  // Truncating the file alone would leave a gap before the new contents.
  const std::string option = GetParam();
  const auto result = RunProbeMode(
      "streams", {option, "/dev/fd/" + std::to_string(inherited.get())});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  std::string expected = std::to_string(state.pid) + "\n";
  if (option == "--stdout") {
    expected = "stdout one\nstdout two\n";
  } else if (option == "--stderr") {
    expected = "stderr one\nstderr two\n";
  }
  EXPECT_EQ(ReadFile(Path("reused")), expected);
  EXPECT_EQ(state.extra_descriptors, 0);
}

// Names descriptor and invalid-path cases after the stream being tested.
std::string OutputOptionName(const testing::TestParamInfo<const char*>& info) {
  return info.param + 2;
}

INSTANTIATE_TEST_SUITE_P(InheritedFiles, InheritedOutputTest,
                         testing::Values("--stdout", "--stderr", "--pid"),
                         OutputOptionName);

TEST_F(FilesTest, MissingStdinFailsBeforeStartingTheCommand) {
  const auto result = RunProbe({"--stdin", Path("missing")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("stdin:"), std::string::npos);
  EXPECT_EQ(result.program_pid, -1);
}

TEST_F(FilesTest, StderrOpenFailurePreservesAUserSelectedStdoutFile) {
  WriteFile(Path("output"), "keep this\n");
  const auto result = RunProbe(
      {"--stdout", Path("output"), "--stderr", Path("missing/errors")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("stderr:"), std::string::npos);
  EXPECT_EQ(ReadFile(Path("output")), "keep this\n");
}

TEST_F(FilesTest, PidOpenFailurePreservesBothSelectedOutputs) {
  WriteFile(Path("output"), "keep stdout\n");
  WriteFile(Path("errors"), "keep stderr\n");
  const auto result = RunProbe({"--stdout", Path("output"), "--stderr",
                                Path("errors"), "--pid", Path("missing/pid")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("pid:"), std::string::npos);
  EXPECT_EQ(ReadFile(Path("output")), "keep stdout\n");
  EXPECT_EQ(ReadFile(Path("errors")), "keep stderr\n");
}

TEST_F(FilesTest, ExecFailureKeepsUserSelectedLogsAndReportsTheirLocation) {
  const auto result = Run({"--stdout", Path("output"), "--stderr",
                           Path("errors"), "--", Path("missing-command")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(ReadFile(Path("output")).empty());
  EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
  EXPECT_NE(result.error.find("Check stderr: " + Path("errors")),
            std::string::npos);
}

TEST_F(FilesTest, ExecFailureCopiesTemporaryDiagnosticsToTheCaller) {
  const auto result = Run({"--", Path("missing-command")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Failed to start the command"),
            std::string::npos);
  EXPECT_NE(result.error.find("exec:"), std::string::npos);
}

TEST_F(FilesTest, ExecFailureReportsAnExplicitStderrWithTemporaryStdout) {
  const auto result =
      Run({"--stderr", Path("errors"), "--", Path("missing-command")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Check stderr: " + Path("errors")),
            std::string::npos);
  EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
}

TEST_F(FilesTest, NonExecutableCommandIsAnExecFailure) {
  WriteFile(Path("command"), "not executable\n");
  ASSERT_TRUE(CheckCall(chmod(Path("command").c_str(), 0600), "chmod"));
  const auto result = Run({"--", Path("command")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("exec:"), std::string::npos);
}

class InvalidOutputTest : public DetachTest,
                          public testing::WithParamInterface<const char*> {
};

TEST_P(InvalidOutputTest, MissingParentDirectoryIsAnError) {
  const auto result = RunProbe({GetParam(), Path("missing/file")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find(std::string(GetParam() + 2) + ":"),
            std::string::npos);
}

TEST_P(InvalidOutputTest, DirectoryCannotBeAnOutputFile) {
  const auto result = RunProbe({GetParam(), Path(".")});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find(std::string(GetParam() + 2) + ":"),
            std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(OutputPaths, InvalidOutputTest,
                         testing::Values("--stdout", "--stderr", "--pid"),
                         OutputOptionName);

}  // namespace
}  // namespace detach_test
