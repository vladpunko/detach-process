// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using ArgumentsTest = DetachTest;

TEST_F(ArgumentsTest, HelpSucceedsWithoutStartingACommand) {
  const auto result = Run({"--help"});

  EXPECT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(result.output.find("Usage: detach"), 0u);
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.program_pid, -1);
}

TEST_F(ArgumentsTest, MissingCommandIsAnError) {
  const auto result = Run({});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error.find("Missing command"), std::string::npos);
}

TEST_F(ArgumentsTest, SeparatorAloneIsNotACommand) {
  const auto result = Run({"--"});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing command"), std::string::npos);
}

TEST_F(ArgumentsTest, EmptyCommandIsAnExecError) {
  const auto result = Run({"--", ""});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("exec:"), std::string::npos);
}

TEST_F(ArgumentsTest, OptionsWithoutACommandDoNotCreateFiles) {
  const auto result = Run({"--stdout", Path("output"), "--"});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing command"), std::string::npos);
  EXPECT_FALSE(FileExists(Path("output")));
}

TEST_F(ArgumentsTest, CommandDoesNotRequireASeparatorWhenNoOptionsAreUsed) {
  const auto result = Run(ProbeCommand());

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().pid, result.program_pid);
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
  const auto result = RunProbe({}, "arguments", arguments);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  std::string expected;
  for (const auto& argument : arguments) {
    expected += std::to_string(argument.size()) + ":" + argument + "\n";
  }
  EXPECT_EQ(ReadFile(result.stdout_path), expected);
}

TEST_F(ArgumentsTest, ParsingStopsAtTheCommandWithoutASeparator) {
  const auto result = Run(ProbeCommand("arguments", {"--help", "--pid", "x"}));

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(result.stdout_path), "6:--help\n5:--pid\n1:x\n");
}

TEST_F(ArgumentsTest, RepeatedOptionsUseTheLastValue) {
  const auto result = RunProbe(
      {"--stdout", Path("unused"), "--stdout", Path("selected")}, "streams");

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(result.stdout_path, Path("selected"));
  EXPECT_EQ(ReadFile(Path("selected")), "stdout one\nstdout two\n");
  EXPECT_FALSE(FileExists(Path("unused")));
}

TEST_F(ArgumentsTest, SeparatorCanAlsoBeAnOptionValue) {
  const auto result = RunProbe({"--stdout", "--"}, "streams");

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(Path("--")), "stdout one\nstdout two\n");
}

TEST_F(ArgumentsTest, AttachedOptionValueIsAccepted) {
  const auto result = RunProbe({"--stdout=" + Path("output")}, "streams");

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(Path("output")), "stdout one\nstdout two\n");
}

class ValueOptionTest : public DetachTest,
                        public testing::WithParamInterface<const char*> {
};

TEST_P(ValueOptionTest, RequiresAValue) {
  const auto result = Run({GetParam()});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing an argument for the option"),
            std::string::npos);
  EXPECT_NE(result.error.find(GetParam()), std::string::npos);
}

TEST_P(ValueOptionTest, RejectsAnEmptyValue) {
  const auto result = Run({GetParam(), "", "--", "/bin/true"});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing an argument for the option"),
            std::string::npos);
}

TEST_P(ValueOptionTest, RejectsAnAttachedEmptyValue) {
  const auto result = Run({std::string(GetParam()) + "=", "--", "/bin/true"});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing an argument for the option"),
            std::string::npos);
}

TEST_P(ValueOptionTest, RequiresASeparatorBeforeTheCommand) {
  auto arguments = ProbeCommand();
  arguments.insert(arguments.begin(), {GetParam(), Path("file")});
  const auto result = Run(arguments);

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("Missing -- separator"), std::string::npos);
}

std::string FileOptionName(const testing::TestParamInfo<const char*>& info) {
  return std::string(info.param + 2);
}

INSTANTIATE_TEST_SUITE_P(FileOptions, ValueOptionTest,
                         testing::Values("--stdin", "--stdout", "--stderr",
                                         "--pid"),
                         FileOptionName);

class UnknownOptionTest : public DetachTest,
                          public testing::WithParamInterface<const char*> {
};

TEST_P(UnknownOptionTest, ReportsTheOriginalArgument) {
  const auto result = Run({GetParam()});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find(std::string("Unknown option: ") + GetParam()),
            std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(InvalidOptions, UnknownOptionTest,
                         testing::Values("--unknown", "--std", "-h", "-ho",
                                         "--help=unexpected"));

}  // namespace
}  // namespace detach_test
