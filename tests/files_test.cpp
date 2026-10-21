// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <tuple>
#include <vector>

#include "tests/support.h"

namespace detach_test {
namespace {

using FilesTest = DetachTest;

TEST_F(FilesTest, DefaultLogsAreDistinctPrivateTemporaryFiles) {
    LaunchOptions options;
    options.file_creation_mask = 0;
    const auto result = RunProbe({}, "streams", {}, options);

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
    const auto result = RunProbe({}, "stdio");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().at("stdin_is_null"), 1);
    EXPECT_TRUE(ReadFile(result.stdout_path).empty());
    EXPECT_EQ(ReadFile(result.stderr_path), "probe stderr\n");
}

TEST_F(FilesTest, RedirectsAllThreeStreams) {
    const std::string contents("input\nwith\0binary data\n", 23);
    WriteFile(Path("input"), contents);
    const auto result = RunProbe({"--stdin", Path("input"),
                                  "--stdout", Path("output"),
                                  "--stderr", Path("errors")}, "stdio");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    EXPECT_EQ(ReadProbe().at("stdin_is_null"), 0);
    EXPECT_EQ(ReadFile(Path("input")), contents);
    EXPECT_EQ(ReadFile(Path("output")), contents);
    EXPECT_EQ(ReadFile(Path("errors")), "probe stderr\n");
}

TEST_F(FilesTest, CopiesInputLargerThanAnIoBuffer) {
    const std::string contents(1024 * 1024, 'x');
    WriteFile(Path("input"), contents);
    const auto result = RunProbe({"--stdin", Path("input")}, "stdio");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    ReadProbe();
    EXPECT_EQ(ReadFile(result.stdout_path), contents);
}

TEST_F(FilesTest, ExistingOutputsAreTruncatedWithoutChangingTheirPermissions) {
    WriteFile(Path("output"), std::string(1000, 'x'));
    WriteFile(Path("errors"), std::string(1000, 'y'));
    CheckCall(chmod(Path("output").c_str(), 0640), "chmod");
    CheckCall(chmod(Path("errors").c_str(), 0644), "chmod");
    const auto result = RunProbe({"--stdout", Path("output"),
                                  "--stderr", Path("errors")}, "streams");

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
    const auto result = RunProbe({"--stdout", Path("output"),
                                  "--stderr", Path("errors"),
                                  "--pid", Path("pid")}, "state", {}, options);

    ASSERT_EQ(result.exit_code, 0) << result.error;
    ReadProbe();
    EXPECT_EQ(FileMode(Path("output")), 0600);
    EXPECT_EQ(FileMode(Path("errors")), 0600);
    EXPECT_EQ(FileMode(Path("pid")), 0600);
}

TEST_F(FilesTest, RelativePathsAndSpacesInPathsWork) {
    WriteFile(Path("input file"), "contents\n");
    const auto result = RunProbe({"--stdin", "input file",
                                  "--stdout", "output file",
                                  "--stderr", "error file",
                                  "--pid", "pid file"}, "stdio");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    const auto state = ReadProbe();
    EXPECT_EQ(ReadFile(Path("output file")), "contents\n");
    EXPECT_EQ(ReadFile(Path("error file")), "probe stderr\n");
    EXPECT_EQ(ReadFile(Path("pid file")),
              std::to_string(state.at("pid")) + "\n");
}

TEST_F(FilesTest, PidFileIsCompleteBeforeTheCommandStarts) {
    WriteFile(Path("pid"), std::string(1000, '9'));
    const auto result = RunProbe({"--pid", Path("pid")},
                                 "pid-file", {Path("pid")});

    ASSERT_EQ(result.exit_code, 0) << result.error;
    const auto state = ReadProbe();
    EXPECT_EQ(state.at("pid_file"), state.at("pid"));
    EXPECT_EQ(result.program_pid, state.at("pid"));
    EXPECT_EQ(ReadFile(Path("pid")), std::to_string(state.at("pid")) + "\n");
}

TEST_F(FilesTest, DevNullCanReceiveBothOutputStreams) {
    const auto result = RunProbe({"--stdout", "/dev/null",
                                  "--stderr", "/dev/null"}, "streams");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    ReadProbe();
    EXPECT_EQ(result.stdout_path, "/dev/null");
    EXPECT_EQ(result.stderr_path, "/dev/null");
}

TEST_F(FilesTest, OutputCanBeAPipe) {
    int channel[2];
    CheckCall(pipe(channel), "pipe");
    Descriptor reader(channel[0]);
    Descriptor writer(channel[1]);
    CheckCall(fcntl(reader.Get(), F_SETFL, O_NONBLOCK), "fcntl");
    const auto result = RunProbe({"--stdout",
        "/dev/fd/" + std::to_string(writer.Get())}, "streams");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    ReadProbe();
    char buffer[128];
    ssize_t length = read(reader.Get(), buffer, sizeof(buffer));
    ASSERT_GT(length, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(length)),
              "stdout one\nstdout two\n");
}

TEST_F(FilesTest, PidCanBeWrittenToAPipeBeforeTheCommandStarts) {
    int channel[2];
    CheckCall(pipe(channel), "pipe");
    Descriptor reader(channel[0]);
    Descriptor writer(channel[1]);
    CheckCall(fcntl(reader.Get(), F_SETFL, O_NONBLOCK), "fcntl");
    const auto result = RunProbe({"--pid",
        "/dev/fd/" + std::to_string(writer.Get())});

    ASSERT_EQ(result.exit_code, 0) << result.error;
    const auto state = ReadProbe();
    char buffer[64];
    ssize_t length = read(reader.Get(), buffer, sizeof(buffer));
    ASSERT_GT(length, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(length)),
              std::to_string(state.at("pid")) + "\n");
    EXPECT_EQ(state.at("extra_descriptors"), 0);
}

TEST_F(FilesTest, MissingStdinFailsBeforeStartingTheCommand) {
    const auto result = RunProbe({"--stdin", Path("missing")});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("stdin:"), std::string::npos);
    EXPECT_EQ(result.program_pid, -1);
}

TEST_F(FilesTest, StderrOpenFailurePreservesAUserSelectedStdoutFile) {
    WriteFile(Path("output"), "keep this\n");
    const auto result = RunProbe({"--stdout", Path("output"),
                                  "--stderr", Path("missing/errors")});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("stderr:"), std::string::npos);
    EXPECT_EQ(ReadFile(Path("output")), "keep this\n");
}

TEST_F(FilesTest, PidOpenFailurePreservesBothSelectedOutputs) {
    WriteFile(Path("output"), "keep stdout\n");
    WriteFile(Path("errors"), "keep stderr\n");
    const auto result = RunProbe({"--stdout", Path("output"),
                                  "--stderr", Path("errors"),
                                  "--pid", Path("missing/pid")});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("pid:"), std::string::npos);
    EXPECT_EQ(ReadFile(Path("output")), "keep stdout\n");
    EXPECT_EQ(ReadFile(Path("errors")), "keep stderr\n");
}

TEST_F(FilesTest, ExecFailureKeepsUserSelectedLogsAndReportsTheirLocation) {
    const auto result = Run({"--stdout", Path("output"),
                             "--stderr", Path("errors"),
                             "--", Path("missing-command")});

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
    const auto result = Run({"--stderr", Path("errors"),
                             "--", Path("missing-command")});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("Check stderr: " + Path("errors")),
              std::string::npos);
    EXPECT_NE(ReadFile(Path("errors")).find("exec:"), std::string::npos);
}

TEST_F(FilesTest, NonExecutableCommandIsAnExecFailure) {
    WriteFile(Path("command"), "not executable\n");
    CheckCall(chmod(Path("command").c_str(), 0600), "chmod");
    const auto result = Run({"--", Path("command")});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("exec:"), std::string::npos);
}

TEST_F(FilesTest, PidCannotAliasTheDefaultDevNullInput) {
    const auto result = RunProbe({"--pid", "/dev/null"});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("PID file must differ"), std::string::npos);
}

enum class AliasKind { kSamePath, kHardLink, kSymbolicLink };

std::string AliasName(AliasKind kind) {
    switch (kind) {
    case AliasKind::kSamePath:
        return "SamePath";
    case AliasKind::kHardLink:
        return "HardLink";
    case AliasKind::kSymbolicLink:
        return "SymbolicLink";
    }
    return "Unknown";
}

void CreateAlias(AliasKind kind, const std::string &target,
                 const std::string &alias) {
    if (kind == AliasKind::kHardLink) {
        CheckCall(link(target.c_str(), alias.c_str()), "link");
    } else if (kind == AliasKind::kSymbolicLink) {
        CheckCall(symlink(target.c_str(), alias.c_str()), "symlink");
    }
}

class SharedOutputTest : public DetachTest,
                         public testing::WithParamInterface<AliasKind> {
};

TEST_P(SharedOutputTest, BothStreamsShareAnOffsetWhenTheyReferToTheSameFile) {
    WriteFile(Path("combined"), std::string(1000, 'x'));
    const std::string errors = GetParam() == AliasKind::kSamePath ?
                              Path("combined") : Path("alias");
    CreateAlias(GetParam(), Path("combined"), errors);
    const auto result = RunProbe({"--stdout", Path("combined"),
                                  "--stderr", errors}, "streams");

    ASSERT_EQ(result.exit_code, 0) << result.error;
    ReadProbe();
    EXPECT_EQ(ReadFile(Path("combined")),
              "stdout one\nstderr one\nstdout two\nstderr two\n");
}

std::string SharedOutputName(const testing::TestParamInfo<AliasKind> &info) {
    return AliasName(info.param);
}

INSTANTIATE_TEST_SUITE_P(
    OutputAliases, SharedOutputTest,
    testing::Values(AliasKind::kSamePath, AliasKind::kHardLink,
                    AliasKind::kSymbolicLink), SharedOutputName);

class PidAliasTest : public DetachTest,
                    public testing::WithParamInterface<
                        std::tuple<const char *, AliasKind>> {
};

TEST_P(PidAliasTest, RejectsAliasesBeforeChangingAnyFileContents) {
    WriteFile(Path("input"), "keep stdin\n");
    WriteFile(Path("output"), "keep stdout\n");
    WriteFile(Path("errors"), "keep stderr\n");
    const auto kind = std::get<1>(GetParam());
    const std::string target = Path(std::get<0>(GetParam()));
    const std::string pid = kind == AliasKind::kSamePath ? target : Path("pid");
    CreateAlias(kind, target, pid);
    const auto result = RunProbe({"--stdin", Path("input"),
                                  "--stdout", Path("output"),
                                  "--stderr", Path("errors"), "--pid", pid});

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.error.find("PID file must differ"), std::string::npos);
    EXPECT_EQ(ReadFile(Path("input")), "keep stdin\n");
    EXPECT_EQ(ReadFile(Path("output")), "keep stdout\n");
    EXPECT_EQ(ReadFile(Path("errors")), "keep stderr\n");
}

std::string PidAliasName(
        const testing::TestParamInfo<PidAliasTest::ParamType> &info) {
    return std::string(std::get<0>(info.param)) + "_" +
           AliasName(std::get<1>(info.param));
}

INSTANTIATE_TEST_SUITE_P(
    PidAliases, PidAliasTest,
    testing::Combine(
        testing::Values("input", "output", "errors"),
        testing::Values(AliasKind::kSamePath, AliasKind::kHardLink,
                        AliasKind::kSymbolicLink)), PidAliasName);

class InvalidOutputTest : public DetachTest,
                          public testing::WithParamInterface<const char *> {
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
                         testing::Values("--stdout", "--stderr", "--pid"));

}  // namespace
}  // namespace detach_test
