// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include <fcntl.h>

#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "tests/support/fixture.h"
#include "tests/support/io.h"
#include "tests/support/runner.h"

namespace detach_test {
namespace {

class TerminalTest : public DetachTest {
protected:
    // Opens a real pseudoterminal to test release of the controlling terminal.
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());

        master_.Reset(posix_openpt(O_RDWR | O_NOCTTY));
        ASSERT_TRUE(CheckSysCall(master_.get(), "posix_openpt"));
        const int status = fcntl(master_.get(), F_SETFD, FD_CLOEXEC);
        ASSERT_TRUE(CheckSysCall(status, "set terminal close-on-exec"));

        ASSERT_TRUE(CheckSysCall(grantpt(master_.get()), "grantpt"));
        ASSERT_TRUE(CheckSysCall(unlockpt(master_.get()), "unlockpt"));
        const char* path = ptsname(master_.get());
        ASSERT_NE(path, nullptr);
        slave_path_ = path;
    }

    FileDescriptor master_;
    std::string slave_path_;
};

TEST_F(TerminalTest, LosesTheOriginalControllingTerminal) {
    LaunchOptions options;
    options.terminal_path = slave_path_;

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.has_terminal, 0);
    EXPECT_NE(state.session_id, report.detach_pid);
    EXPECT_EQ(state.sighup_ignored, 0);
}

TEST_F(TerminalTest, KeepsRunningAfterTheOriginalTerminalIsClosed) {
    LaunchOptions options;
    options.terminal_path = slave_path_;

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    ReadProbe();
    PingProbe();

    master_.Reset();

    PingProbe();
}

TEST_F(TerminalTest, OpeningATerminalDoesNotMakeItTheControllingTerminal) {
    const auto report = RunProbeMode("terminal", {}, {slave_path_});

    ASSERT_EQ(report.exit_code, 0)
        << report.error;
    EXPECT_EQ(ReadProbe().has_terminal, 0);
}

}  // namespace
}  // namespace detach_test
