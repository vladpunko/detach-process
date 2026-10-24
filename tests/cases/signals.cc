// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <signal.h>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

using SignalsTest = DetachTest;

// `SIGPIPE` disposition and blocked state.

TEST_F(SignalsTest, KeepsTheCallersDefaultSigpipeDisposition) {
    const auto report = RunProbe();

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigpipe_ignored, 0);
    EXPECT_EQ(state.sigpipe_blocked, 0);
    EXPECT_EQ(state.sigusr1_blocked, 0);
    EXPECT_EQ(state.sigchld_ignored, 0);
    EXPECT_EQ(state.sighup_ignored, 0);
}

TEST_F(SignalsTest, KeepsTheCallersIgnoredSigpipeDisposition) {
    const LaunchOptions options{.ignored_signals = {SIGPIPE}};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigpipe_ignored, 1);
    EXPECT_EQ(state.sigpipe_blocked, 0);
}

TEST_F(SignalsTest, KeepsTheCallersBlockedSigpipe) {
    const LaunchOptions options{.blocked_signals = {SIGPIPE}};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigpipe_blocked, 1);
    EXPECT_EQ(state.sigpipe_ignored, 0);
}

TEST_F(SignalsTest, KeepsTheCallersIgnoredAndBlockedSigpipe) {
    const LaunchOptions options{
        .ignored_signals = {SIGPIPE},
        .blocked_signals = {SIGPIPE},
    };

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigpipe_ignored, 1);
    EXPECT_EQ(state.sigpipe_blocked, 1);
}

// Other inherited signals.

TEST_F(SignalsTest, KeepsOtherBlockedSignals) {
    const LaunchOptions options{.blocked_signals = {SIGUSR1}};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigusr1_blocked, 1);
    EXPECT_EQ(state.sigpipe_blocked, 0);
}

TEST_F(SignalsTest, WorksWhenTheCallerIgnoresSigchld) {
    const LaunchOptions options{.ignored_signals = {SIGCHLD}};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().sigchld_ignored, 1);
}

TEST_F(SignalsTest, KeepsTheCallersIgnoredSighupDisposition) {
    const LaunchOptions options{.ignored_signals = {SIGHUP}};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().sighup_ignored, 1);
}

// Combined dispositions and blocked signals.

TEST_F(SignalsTest, PreservesCombinedSignalState) {
    const LaunchOptions options{
        .ignored_signals = {SIGPIPE, SIGCHLD, SIGHUP},
        .blocked_signals = {SIGPIPE, SIGUSR1},
    };

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.sigpipe_ignored, 1);
    EXPECT_EQ(state.sigchld_ignored, 1);
    EXPECT_EQ(state.sighup_ignored, 1);
    EXPECT_EQ(state.sigpipe_blocked, 1);
    EXPECT_EQ(state.sigusr1_blocked, 1);
    PingProbe();
}

}  // namespace
}  // namespace detach_test
