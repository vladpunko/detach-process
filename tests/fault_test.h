// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#ifndef TESTS_FAULT_TEST_H_
#define TESTS_FAULT_TEST_H_

#include <initializer_list>
#include <iosfwd>
#include <set>
#include <string>
#include <vector>

#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {

enum class ProcessRole { kLauncher, kIntermediate, kCommand };

struct FaultRule {
  const char* operation;
  ProcessRole role;
  unsigned int call;
  int error;
};

// Runs the real CLI with planned syscall failures and verifies that every
// planned injection actually occurred, including failures in forked children.
class FaultTest : public DetachTest {
 protected:
  // Creates private event and injection logs before any fault can fire.
  void SetUp() override;
  // Releases the probe, verifies applied faults, and removes leftover logs.
  void TearDown() override;

  // Selects the instrumented executable and its private recording files.
  LaunchOptions FaultOptions() const;
  // Builds a fault plan and records the exact injections expected at teardown.
  LaunchOptions Inject(std::initializer_list<FaultRule> rules);
  // Builds a plan for a scenario that needs only one failing operation.
  LaunchOptions Inject(const FaultRule& rule);
  // Checks cleanup using the logs recorded by the executable itself.
  void ExpectTemporaryFilesRemoved() const;

 private:
  // Lists generated logs so even a simulated unlink failure can be cleaned up.
  std::vector<std::string> TemporaryFiles() const;
  // Compares injection identities and counts without depending on scheduling.
  void ExpectAppliedFaults(std::istream& log) const;

  bool event_log_ready_ = false;
  bool fault_log_ready_ = false;
  std::multiset<std::string> expected_faults_;
};

}  // namespace detach_test

#endif  // TESTS_FAULT_TEST_H_
