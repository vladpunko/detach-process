// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

constexpr char kStdoutContents[] = "stdout one\nstdout two\n";
constexpr char kStderrContents[] = "stderr one\nstderr two\n";
constexpr char kProbeErrorContents[] = "probe stderr\n";

using RedirectionTest = DetachTest;

// Checks file-open errors before a detached command can start.
void ExpectOpenError(const RunReport& report, const std::string& label) {
    EXPECT_EQ(report.exit_code, 1) << report.error;
    EXPECT_EQ(report.command_pid, -1);
    EXPECT_TRUE(report.output.empty());
    EXPECT_TRUE(report.stdout_path.empty());
    EXPECT_TRUE(report.stderr_path.empty());
    EXPECT_PRED_FORMAT2(testing::IsSubstring, label + ":", report.error);
}

// Default streams.

TEST_F(RedirectionTest, DefaultLogsAreDistinctPrivateTemporaryFiles) {
    const LaunchOptions options{.file_creation_mask = 0};

    const auto report = RunProbeMode("streams", {}, {}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_TRUE(report.stdout_path.starts_with("/tmp/stdout."));
    EXPECT_TRUE(report.stderr_path.starts_with("/tmp/stderr."));
    EXPECT_NE(report.stdout_path, report.stderr_path);
    EXPECT_EQ(GetPermission(report.stdout_path), 0600);
    EXPECT_EQ(GetPermission(report.stderr_path), 0600);
    EXPECT_EQ(Read(report.stdout_path), kStdoutContents);
    EXPECT_EQ(Read(report.stderr_path), kStderrContents);
}

TEST_F(RedirectionTest, DefaultInputIsDevNull) {
    const auto report = RunProbeMode("stdio");

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().stdin_is_null, 1);

    EXPECT_TRUE(Read(report.stdout_path).empty());
    EXPECT_EQ(Read(report.stderr_path), kProbeErrorContents);
}

// Selected files.

TEST_F(RedirectionTest, RedirectsAllThreeStreams) {
    constexpr char input[] = "input\nwith\0binary data\n";
    const std::string contents(input, sizeof(input) - 1);
    Write(Path("input"), contents);
    const std::vector<std::string> options = {
        "--stdin",      Path("input"), "--stdout",
        Path("output"), "--stderr",    Path("errors"),
    };

    const auto report = RunProbeMode("stdio", options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().stdin_is_null, 0);

    EXPECT_EQ(Read(Path("input")), contents);
    EXPECT_EQ(Read(Path("output")), contents);
    EXPECT_EQ(Read(Path("errors")), kProbeErrorContents);
}

TEST_F(RedirectionTest, CopiesLargeInputWithoutLosingBytes) {
    // Numbered blocks reveal duplicated or reordered output.
    std::string contents;
    for (int block = 0; block < 256; ++block) {
        contents += std::to_string(block) + ":" + std::string(4096, 'x') + "\n";
    }

    Write(Path("input"), contents);

    const auto report = RunProbeMode("stdio", {"--stdin", Path("input")});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(Read(report.stdout_path), contents);
}

TEST_F(RedirectionTest, EmptyInputFileProducesNoOutput) {
    Write(Path("input"), "");

    const auto report = RunProbeMode("stdio", {"--stdin", Path("input")});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().stdin_is_null, 0);

    EXPECT_TRUE(Read(report.stdout_path).empty());
    EXPECT_EQ(Read(report.stderr_path), kProbeErrorContents);
}

TEST_F(RedirectionTest, RelativePathsAndSpacesInPathsWork) {
    Write(Path("input file"), "contents\n");
    const std::vector<std::string> options = {
        "--stdin",  "input file", "--stdout", "output file",
        "--stderr", "error file", "--pid",    "pid file",
    };

    const auto report = RunProbeMode("stdio", options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(Read(Path("output file")), "contents\n");
    EXPECT_EQ(Read(Path("error file")), kProbeErrorContents);
    EXPECT_EQ(Read(Path("pid file")), std::to_string(state.pid) + "\n");
}

TEST_F(RedirectionTest, PidFileIsCompleteBeforeTheCommandStarts) {
    Write(Path("pid"), std::string(1000, '9'));
    ASSERT_TRUE(CheckSysCall(chmod(Path("pid").c_str(), 0640), "chmod"));

    const auto report = RunProbeMode("pid-file", {"--pid", Path("pid")}, {Path("pid")});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.pid_file, state.pid);
    EXPECT_EQ(report.command_pid, state.pid);
    EXPECT_EQ(Read(Path("pid")), std::to_string(state.pid) + "\n");
    EXPECT_EQ(GetPermission(Path("pid")), 0640);
}

// File creation permissions.

TEST_F(RedirectionTest, ExistingOutputsAreTruncatedWithoutChangingTheirPermissions) {
    Write(Path("output"), std::string(1000, 'x'));
    Write(Path("errors"), std::string(1000, 'y'));
    ASSERT_TRUE(CheckSysCall(chmod(Path("output").c_str(), 0640), "chmod"));
    ASSERT_TRUE(CheckSysCall(chmod(Path("errors").c_str(), 0644), "chmod"));

    const auto report = RunProbeMode(
        "streams", {"--stdout", Path("output"), "--stderr", Path("errors")});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(Read(Path("output")), kStdoutContents);
    EXPECT_EQ(Read(Path("errors")), kStderrContents);
    EXPECT_EQ(GetPermission(Path("output")), 0640);
    EXPECT_EQ(GetPermission(Path("errors")), 0644);
}

TEST_F(RedirectionTest, NewlyCreatedOutputAndPidFilesArePrivate) {
    const LaunchOptions options{.file_creation_mask = 0};
    const std::vector<std::string> arguments = {
        "--stdout", Path("output"), "--stderr", Path("errors"), "--pid", Path("pid"),
    };

    const auto report = RunProbe(arguments, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(GetPermission(Path("output")), 0600);
    EXPECT_EQ(GetPermission(Path("errors")), 0600);
    EXPECT_EQ(GetPermission(Path("pid")), 0600);
}

struct CreationMaskCase {
    const char* name;
    int mask;
    mode_t permissions;
};

class CreationMaskTest : public DetachTest,
                         public testing::WithParamInterface<CreationMaskCase> {
};

TEST_P(CreationMaskTest, SelectedFilesRespectTheMask) {
    const LaunchOptions options{.file_creation_mask = GetParam().mask};
    const std::vector<std::string> arguments = {
        "--stdout", Path("output"), "--stderr", Path("errors"), "--pid", Path("pid"),
    };

    const auto report = RunProbe(arguments, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    for (const char* name : {"output", "errors", "pid"}) {
        EXPECT_EQ(GetPermission(Path(name)), GetParam().permissions) << name;
    }
}

TEST_P(CreationMaskTest, TemporaryLogsRespectTheMask) {
    const LaunchOptions options{.file_creation_mask = GetParam().mask};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(GetPermission(report.stdout_path), GetParam().permissions);
    EXPECT_EQ(GetPermission(report.stderr_path), GetParam().permissions);
}

const CreationMaskCase kCreationMasks[] = {
    {
        .name = "Private",
        .mask = 0077,
        .permissions = 0600,
    },
    {
        .name = "NoOwnerRead",
        .mask = 0400,
        .permissions = 0200,
    },
    {
        .name = "NoPermissions",
        .mask = 0777,
        .permissions = 0000,
    },
};

// Names permission checks after the inherited mask.
std::string CreationMaskName(const testing::TestParamInfo<CreationMaskCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(CreationMasks,
                         CreationMaskTest,
                         testing::ValuesIn(kCreationMasks),
                         CreationMaskName);

// Devices and pipes.

TEST_F(RedirectionTest, DevNullCanReceiveBothOutputStreams) {
    const std::vector<std::string> options = {
        "--stdout",
        "/dev/null",
        "--stderr",
        "/dev/null",
    };

    const auto report = RunProbeMode("streams", options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, "/dev/null");
    EXPECT_EQ(report.stderr_path, "/dev/null");
}

TEST_F(RedirectionTest, PidCanBeWrittenToDevNullWithSeparateInput) {
    // The PID device must differ from the default `/dev/null` input.
    Write(Path("input"), "contents\n");

    const auto report = RunProbe({"--stdin", Path("input"), "--pid", "/dev/null"});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().pid, report.command_pid);

    EXPECT_EQ(Read(Path("input")), "contents\n");
}

class PipeRedirectionTest : public DetachTest {
protected:
    // Creates a pipe whose captured bytes can be read without waiting for `EOF`.
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());

        int channel[2];
        ASSERT_TRUE(CheckSysCall(pipe(channel), "create pipe"));
        reader_.Reset(channel[0]);
        writer_.Reset(channel[1]);

        const int flags = fcntl(reader_.get(), F_GETFL);
        ASSERT_TRUE(CheckSysCall(flags, "read pipe flags"));
        const int status = fcntl(reader_.get(), F_SETFL, flags | O_NONBLOCK);
        ASSERT_TRUE(CheckSysCall(status, "make pipe nonblocking"));
    }

    FileDescriptor reader_;
    FileDescriptor writer_;
};

TEST_F(PipeRedirectionTest, OutputCanBeAPipe) {
    const std::string path = DescriptorPath(writer_.get());

    const auto report = RunProbeMode("streams", {"--stdout", path});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(ReadPipeOutput(reader_.get()), kStdoutContents);
}

TEST_F(PipeRedirectionTest, ErrorOutputCanBeAPipe) {
    const std::string path = DescriptorPath(writer_.get());

    const auto report = RunProbeMode("streams", {"--stderr", path});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();

    EXPECT_EQ(ReadPipeOutput(reader_.get()), kStderrContents);
    EXPECT_EQ(Read(report.stdout_path), kStdoutContents);
}

TEST_F(PipeRedirectionTest, InputCanBeAPipe) {
    constexpr char input[] = "input\nwith\0binary data\n";
    const std::string contents(input, sizeof(input) - 1);
    const auto size = contents.size();
    const ssize_t bytes_written = write(writer_.get(), contents.data(), size);
    ASSERT_EQ(bytes_written, static_cast<ssize_t>(size));

    // Closing the last writer lets the command reach `EOF` after these bytes.
    writer_.Reset();
    const std::string path = DescriptorPath(reader_.get());

    const auto report = RunProbeMode("stdio", {"--stdin", path});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().stdin_is_null, 0);

    EXPECT_EQ(Read(report.stdout_path), contents);
    EXPECT_EQ(Read(report.stderr_path), kProbeErrorContents);
}

TEST_F(PipeRedirectionTest, PidCanBeWrittenToAPipeBeforeTheCommandStarts) {
    const auto report = RunProbe({"--pid", DescriptorPath(writer_.get())});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(ReadPipeOutput(reader_.get()), std::to_string(state.pid) + "\n");
    EXPECT_EQ(state.extra_descriptors, 0);
}

// Reused output descriptors.

class InheritedOutputTest : public DetachTest,
                            public testing::WithParamInterface<const char*> {
};

TEST_P(InheritedOutputTest, WritesFromTheStartOfAReusedRegularFile) {
    Write(Path("reused"), "old contents\n");
    FileDescriptor inherited(open(Path("reused").c_str(), O_WRONLY));
    ASSERT_TRUE(CheckSysCall(inherited.get(), "open inherited output"));
    ASSERT_GT(lseek(inherited.get(), 0, SEEK_END), 0);

    // Opening `/dev/fd` on macOS preserves the inherited file offset.
    // Output must start at byte zero after truncation.
    const std::string option = GetParam();
    const std::string path = DescriptorPath(inherited.get());

    const auto report = RunProbeMode("streams", {option, path});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    std::string expected = std::to_string(state.pid) + "\n";
    if (option == "--stdout") {
        expected = kStdoutContents;
    } else if (option == "--stderr") {
        expected = kStderrContents;
    }

    EXPECT_EQ(Read(Path("reused")), expected);
    EXPECT_EQ(state.extra_descriptors, 0);
}

// Names descriptor and invalid-path cases after the stream being tested.
std::string OutputOptionName(const testing::TestParamInfo<const char*>& info) {
    return std::string(info.param).substr(2);
}

INSTANTIATE_TEST_SUITE_P(InheritedFiles,
                         InheritedOutputTest,
                         testing::Values("--stdout", "--stderr", "--pid"),
                         OutputOptionName);

// File-open errors.

TEST_F(RedirectionTest, MissingStdinFailsBeforeStartingTheCommand) {
    Write(Path("output"), "keep stdout\n");
    Write(Path("errors"), "keep stderr\n");
    Write(Path("pid"), "keep pid\n");
    const std::vector<std::string> options = {
        "--stdin",  Path("missing"), "--stdout", Path("output"),
        "--stderr", Path("errors"),  "--pid",    Path("pid"),
    };

    const auto report = RunProbe(options);

    ExpectOpenError(report, "stdin");
    EXPECT_EQ(Read(Path("output")), "keep stdout\n");
    EXPECT_EQ(Read(Path("errors")), "keep stderr\n");
    EXPECT_EQ(Read(Path("pid")), "keep pid\n");
}

TEST_F(RedirectionTest, StderrOpenFailurePreservesAUserSelectedStdoutFile) {
    Write(Path("output"), "keep this\n");

    const auto report
        = RunProbe({"--stdout", Path("output"), "--stderr", Path("missing/errors")});

    ExpectOpenError(report, "stderr");
    EXPECT_EQ(Read(Path("output")), "keep this\n");
}

TEST_F(RedirectionTest, PidOpenFailurePreservesBothSelectedOutputs) {
    Write(Path("output"), "keep stdout\n");
    Write(Path("errors"), "keep stderr\n");

    const auto report = RunProbe({"--stdout", Path("output"), "--stderr",
                                  Path("errors"), "--pid", Path("missing/pid")});

    ExpectOpenError(report, "pid");
    EXPECT_EQ(Read(Path("output")), "keep stdout\n");
    EXPECT_EQ(Read(Path("errors")), "keep stderr\n");
}

// Command startup errors.

TEST_F(RedirectionTest, ExecFailureKeepsUserSelectedLogsAndReportsTheirLocation) {
    const auto report = Run({"--stdout", Path("output"), "--stderr", Path("errors"),
                             "--", Path("missing-command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_TRUE(Read(Path("output")).empty());
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", Read(Path("errors")));
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "check stderr: " + Path("errors"),
                        report.error);
}

TEST_F(RedirectionTest, ExecFailureCopiesTemporaryDiagnosticsToTheCaller) {
    const auto report = Run({"--", Path("missing-command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "failed to start the command",
                        report.error);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", report.error);
}

TEST_F(RedirectionTest, ExecFailureReportsAnExplicitStderrWithTemporaryStdout) {
    const auto report
        = Run({"--stderr", Path("errors"), "--", Path("missing-command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "check stderr: " + Path("errors"),
                        report.error);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", Read(Path("errors")));
}

TEST_F(RedirectionTest, NonExecutableCommandIsAnExecFailure) {
    Write(Path("command"), "not executable\n");
    ASSERT_TRUE(CheckSysCall(chmod(Path("command").c_str(), 0600), "chmod"));

    const auto report = Run({"--", Path("command")});

    EXPECT_EQ(report.exit_code, 1);
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "exec:", report.error);
}

// Invalid output paths.

class InvalidOutputTest : public DetachTest,
                          public testing::WithParamInterface<const char*> {
};

TEST_P(InvalidOutputTest, MissingParentDirectoryIsAnError) {
    const auto report = RunProbe({GetParam(), Path("missing/file")});

    ExpectOpenError(report, std::string(GetParam()).substr(2));
}

TEST_P(InvalidOutputTest, DirectoryCannotBeAnOutputFile) {
    const auto report = RunProbe({GetParam(), Path(".")});

    ExpectOpenError(report, std::string(GetParam()).substr(2));
}

INSTANTIATE_TEST_SUITE_P(OutputPaths,
                         InvalidOutputTest,
                         testing::Values("--stdout", "--stderr", "--pid"),
                         OutputOptionName);

}  // namespace
}  // namespace detach_test
