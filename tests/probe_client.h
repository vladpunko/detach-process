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

// Controls one detached probe through its private Unix socket.
class ProbeClient {
 public:
  void Listen(const std::string& path);
  void ExpectProcess(pid_t pid);

  ProbeState ReadState();
  void Ping();
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
