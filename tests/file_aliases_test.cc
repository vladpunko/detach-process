// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using FileAliasesTest = DetachTest;

TEST_F(FileAliasesTest, PidCannotAliasTheDefaultDevNullInput) {
  const auto result = RunProbe({"--pid", "/dev/null"});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("PID file must differ"), std::string::npos);
}

enum class AliasKind { kSamePath, kHardLink, kSymbolicLink };

constexpr AliasKind kAliasKinds[] = {
    AliasKind::kSamePath,
    AliasKind::kHardLink,
    AliasKind::kSymbolicLink,
};

// Gives each file-alias mechanism a readable test-case name.
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

// Creates the requested real filesystem alias so checks exercise inode
// identity rather than a mocked pathname comparison.
std::string CreateAlias(AliasKind kind, const std::string& target,
                        const std::string& alias) {
  if (kind == AliasKind::kSamePath) {
    return target;
  }
  if (kind == AliasKind::kHardLink) {
    EXPECT_TRUE(CheckCall(link(target.c_str(), alias.c_str()), "link"));
  } else if (kind == AliasKind::kSymbolicLink) {
    EXPECT_TRUE(CheckCall(symlink(target.c_str(), alias.c_str()), "symlink"));
  }
  return alias;
}

class SharedOutputTest : public DetachTest,
                         public testing::WithParamInterface<AliasKind> {
};

TEST_P(SharedOutputTest, BothStreamsShareAnOffsetWhenTheyReferToTheSameFile) {
  WriteFile(Path("combined"), std::string(1000, 'x'));
  const auto errors = CreateAlias(GetParam(), Path("combined"), Path("alias"));
  const auto result = RunProbeMode(
      "streams", {"--stdout", Path("combined"), "--stderr", errors});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  EXPECT_EQ(ReadFile(Path("combined")),
            "stdout one\nstderr one\nstdout two\nstderr two\n");
}

// Identifies the alias mechanism used by a shared-output test.
std::string SharedOutputName(const testing::TestParamInfo<AliasKind>& info) {
  return AliasName(info.param);
}

INSTANTIATE_TEST_SUITE_P(OutputAliases, SharedOutputTest,
                         testing::ValuesIn(kAliasKinds), SharedOutputName);

struct PidAliasCase {
  std::string file;
  AliasKind kind;
};

// Covers every PID-to-stream pairing through each supported alias kind.
std::vector<PidAliasCase> MakePidAliasCases() {
  std::vector<PidAliasCase> cases;
  for (const char* file : {"input", "output", "errors"}) {
    for (AliasKind kind : kAliasKinds) {
      cases.push_back({file, kind});
    }
  }
  return cases;
}

class PidAliasTest : public DetachTest,
                     public testing::WithParamInterface<PidAliasCase> {
};

TEST_P(PidAliasTest, RejectsAliasesBeforeChangingAnyFileContents) {
  WriteFile(Path("input"), "keep stdin\n");
  WriteFile(Path("output"), "keep stdout\n");
  WriteFile(Path("errors"), "keep stderr\n");
  const PidAliasCase& test_case = GetParam();
  const auto pid =
      CreateAlias(test_case.kind, Path(test_case.file), Path("pid"));
  const auto result =
      RunProbe({"--stdin", Path("input"), "--stdout", Path("output"),
                "--stderr", Path("errors"), "--pid", pid});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("PID file must differ"), std::string::npos);
  EXPECT_EQ(ReadFile(Path("input")), "keep stdin\n");
  EXPECT_EQ(ReadFile(Path("output")), "keep stdout\n");
  EXPECT_EQ(ReadFile(Path("errors")), "keep stderr\n");
}

// Includes both the conflicting stream and alias kind in failure output.
std::string PidAliasName(
    const testing::TestParamInfo<PidAliasTest::ParamType>& info) {
  return info.param.file + "_" + AliasName(info.param.kind);
}

INSTANTIATE_TEST_SUITE_P(PidAliases, PidAliasTest,
                         testing::ValuesIn(MakePidAliasCases()), PidAliasName);

struct InputAliasCase {
  std::string stream;
  AliasKind kind;
  bool write_pid;
};

// Alias checks must protect input with and without the optional PID file.
std::vector<InputAliasCase> MakeInputAliasCases() {
  std::vector<InputAliasCase> cases;
  for (const char* stream : {"--stdout", "--stderr"}) {
    for (AliasKind kind : kAliasKinds) {
      for (bool write_pid : {false, true}) {
        cases.push_back({stream, kind, write_pid});
      }
    }
  }
  return cases;
}

class InputAliasTest : public DetachTest,
                       public testing::WithParamInterface<InputAliasCase> {
};

TEST_P(InputAliasTest, RejectsAliasesBeforeChangingAnyFileContents) {
  const InputAliasCase& test_case = GetParam();
  WriteFile(Path("input"), "keep stdin\n");
  WriteFile(Path("output"), "keep stdout\n");
  WriteFile(Path("errors"), "keep stderr\n");
  WriteFile(Path("pid"), "keep pid\n");
  const auto alias = CreateAlias(test_case.kind, Path("input"), Path("alias"));
  const auto output = test_case.stream == "--stdout" ? alias : Path("output");
  const auto errors = test_case.stream == "--stderr" ? alias : Path("errors");
  std::vector<std::string> options = {"--stdin", Path("input"), "--stdout",
                                      output,    "--stderr",    errors};
  if (test_case.write_pid) {
    options.insert(options.end(), {"--pid", Path("pid")});
  }

  const auto result = RunProbe(options);

  ASSERT_EQ(result.exit_code, 1) << result.error;
  EXPECT_EQ(result.program_pid, -1);
  EXPECT_NE(result.error.find("stdin must differ"), std::string::npos);
  EXPECT_EQ(ReadFile(Path("input")), "keep stdin\n");
  EXPECT_EQ(ReadFile(Path("output")), "keep stdout\n");
  EXPECT_EQ(ReadFile(Path("errors")), "keep stderr\n");
  EXPECT_EQ(ReadFile(Path("pid")), "keep pid\n");
}

// Identifies the output stream, alias kind, and PID-file configuration.
std::string InputAliasName(
    const testing::TestParamInfo<InputAliasTest::ParamType>& info) {
  const InputAliasCase& test_case = info.param;
  return test_case.stream.substr(2) + "_" + AliasName(test_case.kind) +
         (test_case.write_pid ? "_WithPid" : "_WithoutPid");
}

INSTANTIATE_TEST_SUITE_P(InputAliases, InputAliasTest,
                         testing::ValuesIn(MakeInputAliasCases()),
                         InputAliasName);

}  // namespace
}  // namespace detach_test
