// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include "tests/fault_test.h"

#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/test_files.h"
#include "tests/test_launcher.h"
#include "tests/test_support.h"

namespace detach_test {
namespace {

// Maps process roles to the names understood by the fault executable.
const char* RoleName(ProcessRole role) {
  switch (role) {
    case ProcessRole::kLauncher:
      return "launcher";
    case ProcessRole::kIntermediate:
      return "intermediate";
    case ProcessRole::kCommand:
      return "command";
  }
  ADD_FAILURE() << "Unknown fault process role";
  return "";
}

}  // namespace

void FaultTest::SetUp() {
  ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());
  ASSERT_NO_FATAL_FAILURE(WriteFile(Path("events"), ""));
  event_log_ready_ = true;
  ASSERT_NO_FATAL_FAILURE(WriteFile(Path("applied-faults"), ""));
  fault_log_ready_ = true;
}

void FaultTest::TearDown() {
  const auto paths = TemporaryFiles();
  // Keep the log open across base cleanup, which removes the private directory.
  // Reading afterwards includes injections made while the probe was stopping.
  std::ifstream applied;
  if (fault_log_ready_) {
    applied.open(Path("applied-faults"));
    EXPECT_TRUE(applied.is_open()) << "Open applied fault log";
  }
  DetachTest::TearDown();
  if (applied.is_open()) {
    ExpectAppliedFaults(applied);
  }
  for (const auto& path : paths) {
    if (unlink(path.c_str()) < 0) {
      EXPECT_EQ(errno, ENOENT) << path;
    }
  }
}

LaunchOptions FaultTest::FaultOptions() const {
  LaunchOptions options;
  options.executable = FAULT_PATH;
  options.environment["DETACH_TEST_EVENTS"] = Path("events");
  options.environment["DETACH_TEST_APPLIED_FAULTS"] = Path("applied-faults");
  return options;
}

LaunchOptions FaultTest::Inject(std::initializer_list<FaultRule> rules) {
  auto options = FaultOptions();
  std::string plan;
  for (const auto& rule : rules) {
    if (!plan.empty()) {
      plan += ";";
    }
    const std::string entry = std::string(RoleName(rule.role)) + ":" +
                              rule.operation + ":" + std::to_string(rule.call) +
                              ":" + std::to_string(rule.error);
    plan += entry;
    expected_faults_.insert(entry);
  }
  options.environment["DETACH_TEST_FAULTS"] = plan;
  return options;
}

LaunchOptions FaultTest::Inject(const FaultRule& rule) {
  return Inject({rule});
}

std::vector<std::string> FaultTest::TemporaryFiles() const {
  std::vector<std::string> paths;
  if (!event_log_ready_) {
    return paths;
  }
  std::istringstream events(ReadFile(Path("events")));
  std::string path;
  while (std::getline(events, path)) {
    paths.push_back(path);
  }
  return paths;
}

void FaultTest::ExpectAppliedFaults(std::istream& log) const {
  std::multiset<std::string> actual;
  std::string rule;
  while (std::getline(log, rule)) {
    actual.insert(rule);
  }
  EXPECT_TRUE(log.eof() && !log.bad()) << "Read applied fault log";
  EXPECT_EQ(actual, expected_faults_) << "Every planned fault must fire";
}

void FaultTest::ExpectTemporaryFilesRemoved() const {
  for (const auto& path : TemporaryFiles()) {
    EXPECT_FALSE(FileExists(path)) << path;
  }
}

}  // namespace detach_test
