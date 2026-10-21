// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_TEST_FILES_H_
#define DETACH_PROCESS_TESTS_TEST_FILES_H_

#include <sys/types.h>

#include <string>

#include "gtest/gtest.h"

namespace detach_test {

// Owns a descriptor so failed assertions still release sockets and terminals.
class Descriptor {
 public:
  explicit Descriptor(int fd = -1) : fd_(fd) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor();
  int get() const { return fd_; }
  void Reset(int fd = -1);

 private:
  int fd_;
};

// Describes a failed POSIX call for ASSERT_TRUE or EXPECT_TRUE.
testing::AssertionResult CheckCall(int result, const char* operation);
// Returns file contents and records a test failure if reading fails.
std::string ReadFile(const std::string& path);
// Drains a nonblocking pipe after the probe confirms its writes are complete.
std::string ReadAvailable(int fd);
void WriteFile(const std::string& path, const std::string& contents);
// Checks existence and records filesystem errors instead of hiding them.
bool FileExists(const std::string& path);
mode_t FileMode(const std::string& path);

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_FILES_H_
