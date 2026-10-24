// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

constexpr char kStdoutContents[] = "stdout one\nstdout two\n";

class ArgumentsTest : public DetachTest {
protected:
    // Runs the probe to check every argument in its original position.
    void ExpectArgumentsPreserved(const std::vector<std::string>& arguments,
                                  const LaunchOptions& options = {}) {
        std::string expected_output;
        for (const auto& argument : arguments) {
            expected_output += std::to_string(argument.size()) + ":" + argument + "\n";
        }

        const auto report = RunProbeMode("arguments", {}, arguments, options);

        ASSERT_EQ(report.exit_code, 0)
            << report.error;
        ReadProbe();

        EXPECT_EQ(Read(report.stdout_path), expected_output);
    }
};

// Checks the exit status and diagnostic shared by rejected command arguments.
void ExpectError(const RunReport& report, const std::string& message) {
    EXPECT_EQ(report.exit_code, 1)
        << report.error;
    EXPECT_PRED_FORMAT2(testing::IsSubstring, message, report.error);
}

// Checks the full diagnostic so option names in the usage text cannot satisfy it.
void ExpectMissingValue(const RunReport& report, const char* option) {
    std::string message = "Missing an argument for the option: ";
    message += option;
    message += '\n';

    ExpectError(report, message);
}

// Command selection.

TEST_F(ArgumentsTest, MissingCommandIsAnError) {
    const auto report = Run({});

    ExpectError(report, "Missing command");
    EXPECT_TRUE(report.output.empty());
}

TEST_F(ArgumentsTest, SeparatorAloneIsNotACommand) {
    const auto report = Run({"--"});

    ExpectError(report, "Missing command");
}

TEST_F(ArgumentsTest, EmptyCommandIsAnExecError) {
    const auto report = Run({"--", ""});

    ExpectError(report, "exec:");
}

TEST_F(ArgumentsTest, OptionAfterTheSeparatorIsTreatedAsACommand) {
    LaunchOptions options;
    options.environment["PATH"] = Path("missing-directory");

    const auto report = Run({"--", "--help"}, options);

    ExpectError(report, "exec:");
    EXPECT_TRUE(report.output.empty());
}

TEST_F(ArgumentsTest, OptionsWithoutACommandDoNotCreateFiles) {
    const auto report = Run({"--stdout", Path("output"), "--"});

    ExpectError(report, "Missing command");
    EXPECT_FALSE(Exists(Path("output")));
}

// Argument forwarding.

TEST_F(ArgumentsTest, CommandDoesNotRequireASeparatorWhenNoOptionsAreUsed) {
    const auto report = Run(ProbeCommand());

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().pid, report.command_pid);
}

TEST_F(ArgumentsTest, CommandArgumentsArePreservedExactly) {
    const std::vector<std::string> arguments = {
        "",
        "two words",
        "'single' and \"double\"",
        "line\nbreak",
        "*?[literal]",
        "--",
        "--help",
        "--stdout",
        "-h",
        "роза",
        std::string(16384, 'x'),
    };

    ExpectArgumentsPreserved(arguments);
}

TEST_F(ArgumentsTest, ShellSyntaxIsPassedLiterally) {
    // A real match makes accidental wildcard expansion visible in the output.
    Write(Path("matching-file"), "contents");

    LaunchOptions options;
    options.environment["DETACH_ARGUMENT_TEST_VALUE"] = "expanded";
    const std::vector<std::string> arguments = {
        "*",
        "$DETACH_ARGUMENT_TEST_VALUE",
        "$(printf expanded)",
        "`printf expanded`",
    };

    ExpectArgumentsPreserved(arguments, options);
}

TEST_F(ArgumentsTest, ConsecutiveEmptyArgumentsArePreserved) {
    const std::vector<std::string> arguments = {
        "", "", "middle", "", "", "last", "", "",
    };

    ExpectArgumentsPreserved(arguments);
}

TEST_F(ArgumentsTest, ManyArgumentsPreserveTheirOrder) {
    std::vector<std::string> arguments;
    for (int index = 0; index < 256; ++index) {
        arguments.push_back("argument-" + std::to_string(index));
    }

    ExpectArgumentsPreserved(arguments);
}

TEST_F(ArgumentsTest, ParsingStopsAtTheCommandWithoutASeparator) {
    const auto report = Run(ProbeCommand("arguments", {"--help", "--pid", "x"}));

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(Read(report.stdout_path), "6:--help\n5:--pid\n1:x\n");
}

// Option values.

struct RepeatedOutputCase {
    const char* name;
    std::vector<std::string> options;
};

class RepeatedOutputTest : public DetachTest,
                           public testing::WithParamInterface<RepeatedOutputCase> {
};

TEST_P(RepeatedOutputTest, UsesTheLastValue) {
    const auto report = RunProbeMode("streams", GetParam().options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, "selected");
    EXPECT_EQ(Read(Path("selected")), kStdoutContents);
    EXPECT_FALSE(Exists(Path("unused")));
}

const RepeatedOutputCase kRepeatedOutputs[] = {
    {"SeparateValues", {"--stdout", "unused", "--stdout", "selected"}},
    {"AttachedThenSeparate", {"--stdout=unused", "--stdout", "selected"}},
    {"SeparateThenAttached", {"--stdout", "unused", "--stdout=selected"}},
    {"AttachedValues", {"--stdout=unused", "--stdout=selected"}},
};

// Names repeated option cases after the supplied value forms.
std::string RepeatedOutputName(const testing::TestParamInfo<RepeatedOutputCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(OutputForms,
                         RepeatedOutputTest,
                         testing::ValuesIn(kRepeatedOutputs),
                         RepeatedOutputName);

TEST_F(ArgumentsTest, RepeatedInputUsesTheLastPathBeforeOpeningFiles) {
    const std::string contents = "selected input\n";
    Write(Path("selected"), contents);
    const std::vector<std::string> options = {
        "--stdin", Path("missing"),
        "--stdin=" + Path("selected"),
    };

    const auto report = RunProbeMode("stdio", options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(Read(report.stdout_path), contents);
    EXPECT_FALSE(Exists(Path("missing")));
}

TEST_F(ArgumentsTest, SeparatorCanAlsoBeAnOptionValue) {
    const auto report = RunProbeMode("streams", {"--stdout", "--"});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, "--");
    EXPECT_EQ(Read(Path("--")), kStdoutContents);
}

TEST_F(ArgumentsTest, OptionNameCanAlsoBeAnOptionValue) {
    const auto report = RunProbeMode("streams", {"--stdout", "--help"});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, "--help");
    EXPECT_EQ(Read(Path("--help")), kStdoutContents);
}

TEST_F(ArgumentsTest, AttachedOptionValueIsAccepted) {
    const auto report = RunProbeMode("streams", {"--stdout=" + Path("output")});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, Path("output"));
    EXPECT_EQ(Read(Path("output")), kStdoutContents);
}

TEST_F(ArgumentsTest, AttachedValuePreservesSpacesAndEqualsSigns) {
    const std::string output_path = Path("output name=backup");

    const auto report = RunProbeMode("streams", {"--stdout=" + output_path});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();

    EXPECT_EQ(report.stdout_path, output_path);
    EXPECT_EQ(Read(output_path), kStdoutContents);
}

TEST_F(ArgumentsTest, LaterMissingValuePreservesExistingOutput) {
    const std::string contents = "keep these contents\n";
    const std::string path = Path("existing");
    Write(path, contents);

    const auto report = Run({"--stdout", path, "--pid"});

    ExpectMissingValue(report, "--pid");
    EXPECT_EQ(Read(path), contents);
}

class ValueOptionTest : public DetachTest,
                        public testing::WithParamInterface<const char*> {
};

TEST_P(ValueOptionTest, RequiresAValue) {
    const auto report = Run({GetParam()});

    ExpectMissingValue(report, GetParam());
}

TEST_P(ValueOptionTest, RejectsAnEmptyValue) {
    const auto report = RunProbe({GetParam(), ""});

    ExpectMissingValue(report, GetParam());
}

TEST_P(ValueOptionTest, RejectsAnAttachedEmptyValue) {
    const auto report = RunProbe({std::string(GetParam()) + "="});

    ExpectMissingValue(report, GetParam());
}

TEST_P(ValueOptionTest, RequiresASeparatorBeforeTheCommand) {
    auto arguments = ProbeCommand();
    arguments.insert(arguments.begin(), {GetParam(), Path("file")});

    const auto report = Run(arguments);

    ExpectError(report, "Missing -- separator");
}

TEST_P(ValueOptionTest, LaterErrorPreservesExistingFile) {
    const std::string contents = "keep these contents\n";
    const std::string path = Path("existing");
    Write(path, contents);

    const auto report = RunProbe({GetParam(), path, "--unknown"});

    ExpectError(report, "Unknown option: --unknown");
    EXPECT_EQ(Read(path), contents);
}

TEST_P(ValueOptionTest, HelpDoesNotModifyTheOptionFile) {
    const std::string contents = "keep these contents\n";
    const std::string path = Path("existing");
    Write(path, contents);

    const auto report = RunProbe({GetParam(), path, "--help"});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_PRED_FORMAT2(testing::IsSubstring, "Usage:", report.output);
    EXPECT_EQ(report.command_pid, -1);
    EXPECT_TRUE(report.error.empty());
    EXPECT_EQ(Read(path), contents);
}

// Removes the leading `--` to give each path option a readable test name.
std::string FileOptionName(const testing::TestParamInfo<const char*>& info) {
    return std::string(info.param).substr(2);
}

INSTANTIATE_TEST_SUITE_P(FileOptions,
                         ValueOptionTest,
                         testing::Values("--stdin", "--stdout", "--stderr", "--pid"),
                         FileOptionName);

// Unknown options.

struct InvalidOptionCase {
    const char* name;
    const char* argument;
};

class UnknownOptionTest : public DetachTest,
                          public testing::WithParamInterface<InvalidOptionCase> {
};

TEST_P(UnknownOptionTest, ReportsTheOriginalArgument) {
    const auto& option = GetParam();
    const std::string message = std::string("Unknown option: ") + option.argument;

    const auto report = Run({option.argument});

    ExpectError(report, message);
}

const InvalidOptionCase kInvalidOptions[] = {
    {"UnknownLongOption", "--unknown"},
    {"AmbiguousLongOption", "--std"},
    {"ShortOption", "-h"},
    {"GroupedShortOptions", "-ho"},
    {"HelpWithValue", "--help=unexpected"},
};

// Uses scenario names so failures identify the rejected option.
std::string InvalidOptionName(const testing::TestParamInfo<InvalidOptionCase>& info) {
    return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(InvalidOptions,
                         UnknownOptionTest,
                         testing::ValuesIn(kInvalidOptions),
                         InvalidOptionName);

}  // namespace
}  // namespace detach_test
