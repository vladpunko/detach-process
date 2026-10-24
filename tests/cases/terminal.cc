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
    // Prepares the test listener and terminal before launching a command.
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());
        ASSERT_NO_FATAL_FAILURE(OpenMasterTerminal());
        ASSERT_NO_FATAL_FAILURE(PrepareSlaveTerminal());
    }

    FileDescriptor master_;
    std::string slave_path_;

private:
    // Opens the master so tests can trigger a terminal hangup by closing it.
    void OpenMasterTerminal() {
        const int fd = posix_openpt(O_RDWR | O_NOCTTY);
        master_.Reset(fd);
        ASSERT_TRUE(CheckSysCall(fd, "open terminal master"));

        const int status = fcntl(master_.get(), F_SETFD, FD_CLOEXEC);
        ASSERT_TRUE(CheckSysCall(status, "set terminal close-on-exec"));
    }

    // Grants access to the slave terminal and saves its path for the launcher.
    void PrepareSlaveTerminal() {
        ASSERT_TRUE(CheckSysCall(grantpt(master_.get()), "grant terminal access"));
        ASSERT_TRUE(CheckSysCall(unlockpt(master_.get()), "unlock slave terminal"));

        const char* path = ptsname(master_.get());
        ASSERT_NE(path, nullptr) << "cannot get slave terminal path";
        slave_path_ = path;
    }
};

TEST_F(TerminalTest, LosesTheOriginalControllingTerminal) {
    const LaunchOptions options{.terminal_path = slave_path_};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    const auto state = ReadProbe();

    EXPECT_EQ(state.has_terminal, 0);
    EXPECT_NE(state.session_id, report.detach_pid);
    EXPECT_EQ(state.sighup_ignored, 0);
}

TEST_F(TerminalTest, KeepsRunningAfterTheOriginalTerminalIsClosed) {
    const LaunchOptions options{.terminal_path = slave_path_};

    const auto report = RunProbe({}, options);

    ASSERT_EQ(report.exit_code, 0) << report.error;
    ReadProbe();
    PingProbe();

    master_.Reset();

    PingProbe();
}

TEST_F(TerminalTest, OpeningATerminalDoesNotMakeItTheControllingTerminal) {
    const auto report = RunProbeMode("terminal", {}, {slave_path_});

    ASSERT_EQ(report.exit_code, 0) << report.error;
    EXPECT_EQ(ReadProbe().has_terminal, 0);
}

}  // namespace
}  // namespace detach_test
