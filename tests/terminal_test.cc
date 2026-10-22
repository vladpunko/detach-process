// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <fcntl.h>

#include <cstdlib>
#include <string>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

class TerminalTest : public DetachTest {
 protected:
  // Acquires a real pseudoterminal so detachment is tested against an actual
  // terminal-owning caller rather than a runner that already has no terminal.
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());
    terminal_.Reset(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(CheckCall(terminal_.get(), "posix_openpt"));
    ASSERT_TRUE(
        CheckCall(fcntl(terminal_.get(), F_SETFD, FD_CLOEXEC), "fcntl"));
    ASSERT_TRUE(CheckCall(grantpt(terminal_.get()), "grantpt"));
    ASSERT_TRUE(CheckCall(unlockpt(terminal_.get()), "unlockpt"));
    char* name = ptsname(terminal_.get());
    ASSERT_NE(name, nullptr);
    slave_ = name;
  }

  Descriptor terminal_;
  std::string slave_;
};

TEST_F(TerminalTest, LosesTheOriginalControllingTerminal) {
  LaunchOptions options;
  options.terminal = slave_;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  const auto state = ReadProbe();
  EXPECT_EQ(state.has_terminal, 0);
  EXPECT_NE(state.session_id, result.launcher_pid);
  EXPECT_EQ(state.sighup_ignored, 0);
}

TEST_F(TerminalTest, KeepsRunningAfterTheOriginalTerminalIsClosed) {
  LaunchOptions options;
  options.terminal = slave_;
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  PingProbe();
  terminal_.Reset();
  PingProbe();
}

TEST_F(TerminalTest, OpeningATerminalDoesNotMakeItTheControllingTerminal) {
  const auto result = RunProbeMode("terminal", {}, {slave_});

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().has_terminal, 0);
}

}  // namespace
}  // namespace detach_test
