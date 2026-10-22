// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <signal.h>

#include "gtest/gtest.h"

#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

using SignalsTest = DetachTest;

TEST_F(SignalsTest, KeepsTheCallersDefaultSigpipeDisposition) {
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_ignored, 0);
}

TEST_F(SignalsTest, KeepsTheCallersIgnoredSigpipeDisposition) {
  LaunchOptions options;
  options.ignored_signals = {SIGPIPE};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_ignored, 1);
}

TEST_F(SignalsTest, KeepsTheCallersBlockedSigpipe) {
  LaunchOptions options;
  options.blocked_signals = {SIGPIPE};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigpipe_blocked, 1);
}

TEST_F(SignalsTest, KeepsOtherBlockedSignals) {
  LaunchOptions options;
  options.blocked_signals = {SIGUSR1};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigusr1_blocked, 1);
}

TEST_F(SignalsTest, WorksWhenTheCallerIgnoresSigchld) {
  LaunchOptions options;
  options.ignored_signals = {SIGCHLD};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sigchld_ignored, 1);
}

TEST_F(SignalsTest, KeepsTheCallersIgnoredSighupDisposition) {
  LaunchOptions options;
  options.ignored_signals = {SIGHUP};
  const auto result = RunProbe({}, options);

  ASSERT_EQ(result.exit_code, 0) << result.error;
  EXPECT_EQ(ReadProbe().sighup_ignored, 1);
}

}  // namespace
}  // namespace detach_test
