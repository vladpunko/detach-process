// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_PROBE_CLIENT_H_
#define DETACH_PROCESS_TESTS_PROBE_CLIENT_H_

#include <sys/types.h>

#include <string>

#include "gtest/gtest.h"

#include "tests/probe_state.h"
#include "tests/test_files.h"

namespace detach_test {

// Controls one detached probe through a private Unix socket.
// This channel stays independent of stdin, stdout and stderr because those
// streams are the subject of the redirection tests.
class ProbeClient {
 public:
  // Open the listener before launching detach so the probe can connect at once.
  void Listen(const std::string& path);
  // Remember the reported PID so cleanup can release the probe even when an
  // assertion stops the test before ReadState. Nonpositive means no command.
  void ExpectProcess(pid_t pid);

  // Receive state after the probe flushes its output, then keep it alive.
  ProbeState ReadState();
  void Ping();
  // Release the probe and wait for its socket to close before files are
  // removed. A bounded failure path kills the owned process instead of leaving
  // it behind.
  void Close();

 private:
  testing::AssertionResult ReceiveState(ProbeState& state);
  testing::AssertionResult Stop();

  Descriptor listener_;
  Descriptor peer_;
  pid_t pid_ = -1;
};

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_PROBE_CLIENT_H_
