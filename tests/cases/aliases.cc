// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

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

enum class AliasKind {
    kSamePath,
    kHardLink,
    kSymbolicLink,
    kRelativeSymbolicLink,
};

constexpr AliasKind kAliasKinds[] = {
    AliasKind::kSamePath,
    AliasKind::kHardLink,
    AliasKind::kSymbolicLink,
    AliasKind::kRelativeSymbolicLink,
};

// Names each alias mechanism so failures identify the path setup.
std::string AliasName(AliasKind kind) {
    switch (kind) {
        case AliasKind::kSamePath:
            return "SamePath";
        case AliasKind::kHardLink:
            return "HardLink";
        case AliasKind::kSymbolicLink:
            return "SymbolicLink";
        case AliasKind::kRelativeSymbolicLink:
            return "RelativeSymbolicLink";
    }
    return "Unknown";
}

struct ProtectedFile {
    const char* name;
    const char* contents;
    mode_t permissions;
};

constexpr ProtectedFile kProtectedFiles[] = {
    {
        .name = "input",
        .contents = "keep stdin\n",
        .permissions = 0640,
    },
    {
        .name = "output",
        .contents = "keep stdout\n",
        .permissions = 0604,
    },
    {
        .name = "errors",
        .contents = "keep stderr\n",
        .permissions = 0644,
    },
    {
        .name = "pid",
        .contents = "keep pid\n",
        .permissions = 0600,
    },
};

constexpr char kCombinedContents[] = "stdout one\nstderr one\nstdout two\nstderr two\n";

class AliasesTest : public DetachTest {
protected:
    // Gives conflict checks known contents and permissions to protect.
    void CreateFiles() {
        for (const auto& file : kProtectedFiles) {
            const std::string path = Path(file.name);
            ASSERT_NO_FATAL_FAILURE(Write(path, file.contents));

            const int status = chmod(path.c_str(), file.permissions);
            ASSERT_TRUE(CheckSysCall(status, "set file permissions"));
        }
    }

    // Checks every protected file for changes after a rejected launch.
    void ExpectFilesUnchanged() {
        for (const auto& file : kProtectedFiles) {
            SCOPED_TRACE(file.name);
            EXPECT_EQ(Read(Path(file.name)), file.contents);
            EXPECT_EQ(GetPermission(Path(file.name)), file.permissions);
        }
    }

    // Creates a checked filesystem alias for the next launch.
    testing::AssertionResult CreateAlias(AliasKind kind,
                                         const std::string& name,
                                         std::string& alias_path) {
        const std::string target = Path(name);
        alias_path = Path("alias");

        switch (kind) {
            case AliasKind::kSamePath:
                alias_path = target;
                return testing::AssertionSuccess();
            case AliasKind::kHardLink:
                return CheckSysCall(link(target.c_str(), alias_path.c_str()),
                                    "create hard link");
            case AliasKind::kSymbolicLink:
                return CheckSysCall(symlink(target.c_str(), alias_path.c_str()),
                                    "create symbolic link");
            case AliasKind::kRelativeSymbolicLink:
                return CheckSysCall(symlink(name.c_str(), alias_path.c_str()),
                                    "create relative symbolic link");
        }

        return testing::AssertionFailure()
            << "unknown alias kind";
    }
};

// Checks that alias validation rejects the launch before starting a command.
void ExpectAliasError(const RunReport& report, const std::string& message) {
    EXPECT_EQ(report.exit_code, 1)
        << report.error;
    EXPECT_EQ(report.command_pid, -1);
    EXPECT_TRUE(report.output.empty());
    EXPECT_PRED_FORMAT2(testing::IsSubstring, message, report.error);
}

// Shared output streams.

struct SharedOutputCase {
    AliasKind kind;
    bool write_pid;
};

// Checks each alias with and without a separate PID file.
std::vector<SharedOutputCase> MakeSharedOutputCases() {
    std::vector<SharedOutputCase> cases;
    for (AliasKind kind : kAliasKinds) {
        for (bool write_pid : {false, true}) {
            cases.push_back({.kind = kind, .write_pid = write_pid});
        }
    }
    return cases;
}

class SharedOutputTest : public AliasesTest,
                         public testing::WithParamInterface<SharedOutputCase> {
};

TEST_P(SharedOutputTest, BothStreamsShareAnOffsetWhenTheyReferToTheSameFile) {
    Write(Path("combined"), std::string(1000, 'x'));
    ASSERT_TRUE(CheckSysCall(chmod(Path("combined").c_str(), 0640), "chmod"));

    const auto& test_case = GetParam();
    std::string alias_path;
    ASSERT_TRUE(CreateAlias(test_case.kind, "combined", alias_path));

    std::vector<std::string> options = {
        "--stdout", Path("combined"),
        "--stderr", alias_path,
    };
    if (test_case.write_pid) {
        options.insert(options.end(), {"--pid", Path("pid")});
    }

    const auto report = RunProbeMode("streams", options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(Read(Path("combined")), kCombinedContents);
    EXPECT_EQ(GetPermission(Path("combined")), 0640);
    if (test_case.write_pid) {
        EXPECT_EQ(Read(Path("pid")), std::to_string(state.pid) + "\n");
    } else {
        EXPECT_FALSE(Exists(Path("pid")));
    }
}

// Names the alias mechanism and optional PID file in failure output.
std::string SharedOutputName(const testing::TestParamInfo<SharedOutputCase>& info) {
    return AliasName(info.param.kind) +
           (info.param.write_pid ? "_WithPid" : "_WithoutPid");
}

INSTANTIATE_TEST_SUITE_P(OutputAliases,
                         SharedOutputTest,
                         testing::ValuesIn(MakeSharedOutputCases()),
                         SharedOutputName);

// PID file conflicts.

struct PidAliasCase {
    std::string target;
    AliasKind kind;
};

// Covers every PID-to-stream pairing through each supported alias kind.
std::vector<PidAliasCase> MakePidAliasCases() {
    std::vector<PidAliasCase> cases;
    for (const char* target : {"input", "output", "errors"}) {
        for (AliasKind kind : kAliasKinds) {
            cases.push_back({.target = target, .kind = kind});
        }
    }
    return cases;
}

class PidAliasTest : public AliasesTest,
                     public testing::WithParamInterface<PidAliasCase> {
};

TEST_P(PidAliasTest, RejectsAliasesBeforeChangingAnyFileContents) {
    ASSERT_NO_FATAL_FAILURE(CreateFiles());

    const auto& test_case = GetParam();
    std::string alias_path;
    ASSERT_TRUE(CreateAlias(test_case.kind, test_case.target, alias_path));

    const std::vector<std::string> options = {
        "--stdin", Path("input"),
        "--stdout", Path("output"),
        "--stderr", Path("errors"),
        "--pid", alias_path,
    };

    const auto report = RunProbe(options);

    ExpectAliasError(report, "pid file must differ");
    ExpectFilesUnchanged();
}

// Includes both the conflicting stream and alias kind in failure output.
std::string PidAliasName(const testing::TestParamInfo<PidAliasTest::ParamType>& info) {
    return info.param.target + "_" + AliasName(info.param.kind);
}

INSTANTIATE_TEST_SUITE_P(PidAliases,
                         PidAliasTest,
                         testing::ValuesIn(MakePidAliasCases()),
                         PidAliasName);

// Input file conflicts.

struct InputAliasCase {
    std::string option;
    AliasKind kind;
    bool write_pid;
};

// Alias checks must protect input with and without the optional PID file.
std::vector<InputAliasCase> MakeInputAliasCases() {
    std::vector<InputAliasCase> cases;
    for (const char* option : {"--stdout", "--stderr"}) {
        for (AliasKind kind : kAliasKinds) {
            for (bool write_pid : {false, true}) {
                cases.push_back(
                    {.option = option, .kind = kind, .write_pid = write_pid});
            }
        }
    }
    return cases;
}

class InputAliasTest : public AliasesTest,
                       public testing::WithParamInterface<InputAliasCase> {
};

TEST_P(InputAliasTest, RejectsAliasesBeforeChangingAnyFileContents) {
    ASSERT_NO_FATAL_FAILURE(CreateFiles());

    const auto& test_case = GetParam();
    std::string alias_path;
    ASSERT_TRUE(CreateAlias(test_case.kind, "input", alias_path));

    const auto output = test_case.option == "--stdout" ? alias_path : Path("output");
    const auto errors = test_case.option == "--stderr" ? alias_path : Path("errors");
    std::vector<std::string> options = {
        "--stdin", Path("input"),
        "--stdout", output,
        "--stderr", errors,
    };
    if (test_case.write_pid) {
        options.insert(options.end(), {"--pid", Path("pid")});
    }

    const auto report = RunProbe(options);

    ExpectAliasError(report, "stdin must differ");
    ExpectFilesUnchanged();
}

// Identifies the output stream, alias kind, and PID-file configuration.
std::string InputAliasName(
    const testing::TestParamInfo<InputAliasTest::ParamType>& info) {
    const InputAliasCase& test_case = info.param;
    return test_case.option.substr(2) + "_" + AliasName(test_case.kind) +
           (test_case.write_pid ? "_WithPid" : "_WithoutPid");
}

INSTANTIATE_TEST_SUITE_P(InputAliases,
                         InputAliasTest,
                         testing::ValuesIn(MakeInputAliasCases()),
                         InputAliasName);

// Devices and inherited descriptors.

TEST_F(AliasesTest, PidCannotAliasTheDefaultDevNullInput) {
    const auto report = RunProbe({"--pid", "/dev/null"});

    ExpectAliasError(report, "pid file must differ");
}

TEST_F(AliasesTest, InheritedSharedOutputsAreRewoundBeforeWriting) {
    Write(Path("combined"), std::string(1000, 'x'));
    FileDescriptor inherited(open(Path("combined").c_str(), O_WRONLY));
    ASSERT_TRUE(CheckSysCall(inherited.get(), "open shared output"));
    ASSERT_GT(lseek(inherited.get(), 0, SEEK_END), 0);
    const std::string path = DescriptorPath(inherited.get());
    const std::vector<std::string> options = {
        "--stdout", path,
        "--stderr", path,
    };

    const auto report = RunProbeMode("streams", options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(Read(Path("combined")), kCombinedContents);
    EXPECT_EQ(state.extra_descriptors, 0);
}

TEST_F(AliasesTest, InputDescriptorCannotAliasAnOutputFile) {
    ASSERT_NO_FATAL_FAILURE(CreateFiles());
    FileDescriptor inherited(open(Path("input").c_str(), O_RDONLY));
    ASSERT_TRUE(CheckSysCall(inherited.get(), "open inherited input"));
    const std::string path = DescriptorPath(inherited.get());
    const std::vector<std::string> options = {
        "--stdin", path,
        "--stdout", Path("input"),
        "--stderr", Path("errors"),
        "--pid", Path("pid"),
    };

    const auto report = RunProbe(options);

    ExpectAliasError(report, "stdin must differ");
    ExpectFilesUnchanged();
}

}  // namespace
}  // namespace detach_test
