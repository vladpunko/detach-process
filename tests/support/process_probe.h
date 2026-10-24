// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_PROCESS_PROBE_H_
#define TESTS_SUPPORT_PROCESS_PROBE_H_

#include <sys/types.h>

#include <string>

#include "gtest/gtest.h"
#include "tests/support/io.h"
#include "tests/support/probe_state.h"

namespace detach_test {

// Controls a detached test process through a private Unix socket.
// Keeps control messages separate from the standard streams under test.
class ProcessProbe {
public:
    // Opens the listener before `detach` starts so the probe can connect at once.
    void Listen(const std::string& path);

    // Saves the process ID for cleanup even if the test stops early.
    // Values below 1 mean no process needs cleanup.
    void ExpectProcess(pid_t pid);

    // Receives state after the probe flushes its output, then keeps it alive.
    ProbeState ReadState();

    // Requires a reply from the running probe to verify it is still responsive.
    void Ping();

    // Releases the probe and waits until its files can be removed safely.
    // Kills the process if graceful shutdown fails.
    void Close();

private:
    // Accepts the probe and receives its complete readiness record by a deadline.
    testing::AssertionResult ReceiveState(ProbeState& state);

    // Releases the probe and waits for `EOF` before its files can be removed.
    testing::AssertionResult Stop();

    FileDescriptor listener_;
    FileDescriptor peer_;
    pid_t pid_ = -1;
};

}  // namespace detach_test

#endif  // TESTS_SUPPORT_PROCESS_PROBE_H_
