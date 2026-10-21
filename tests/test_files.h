// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_TEST_FILES_H_
#define DETACH_PROCESS_TESTS_TEST_FILES_H_

#include <sys/types.h>

#include <string>

#include "gtest/gtest.h"

namespace detach_test {

// Takes ownership of one descriptor. Reset closes the previous descriptor and
// the destructor closes the last one, including after a fatal test assertion.
// Callers can inspect get() without taking ownership back.
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

// Formats a POSIX result for ASSERT_TRUE or EXPECT_TRUE. This function alone
// does not record a failure. ASSERT_TRUE stops the current function on failure.
testing::AssertionResult CheckCall(int result, const char* operation);
// Returns an existing file's contents and records a test failure on I/O errors.
// Use FileExists when absence is expected instead of trying to read that path.
std::string ReadFile(const std::string& path);
// Drains a nonblocking pipe after ReadProbe confirms its writes are complete.
// The test still owns a writer, so reaching EOF is not a valid stopping
// condition. EAGAIN means all bytes already written have been collected.
std::string ReadAvailable(int fd);
void WriteFile(const std::string& path, const std::string& contents);
// Checks existence and records filesystem errors instead of hiding them.
bool FileExists(const std::string& path);
mode_t FileMode(const std::string& path);

}  // namespace detach_test

#endif  // DETACH_PROCESS_TESTS_TEST_FILES_H_
