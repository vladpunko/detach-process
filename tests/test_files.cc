// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/test_files.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "gtest/gtest.h"

namespace detach_test {

// Resource helpers keep ownership and I/O failures out of individual tests.
Descriptor::~Descriptor() { Reset(); }

void Descriptor::Reset(int fd) {
  // Reset may receive the descriptor already owned by this object. Closing
  // it in that case would leave an apparently valid owner of a closed handle.
  if (fd_ != fd) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }
}

testing::AssertionResult CheckCall(int result, const char* operation) {
  if (result < 0) {
    return testing::AssertionFailure() << operation << ": " << strerror(errno);
  }
  return testing::AssertionSuccess();
}

std::string ReadFile(const std::string& path) {
  // Preserve embedded NUL bytes and newlines exactly. Redirection tests compare
  // raw contents rather than treating command output as formatted text.
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    ADD_FAILURE() << "Cannot open " << path << ": " << strerror(errno);
    return {};
  }
  std::string contents;
  char buffer[4096];
  // A final short read sets EOF but still leaves gcount bytes to append.
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    contents.append(buffer, static_cast<size_t>(stream.gcount()));
  }
  // EOF is expected here. Other read errors must not look like a short file
  // and accidentally satisfy an assertion about empty or truncated output.
  if (stream.bad() || !stream.eof()) {
    ADD_FAILURE() << "Cannot read " << path;
  }
  return contents;
}

std::string ReadAvailable(int fd) {
  std::string contents;
  char buffer[4096];
  while (true) {
    ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length > 0) {
      contents.append(buffer, static_cast<size_t>(length));
    } else if (length == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      // After readiness, EAGAIN means all available output was read. Waiting
      // for EOF would hang while the test still owns a pipe writer.
      return contents;
    } else if (errno != EINTR) {
      ADD_FAILURE() << "read pipe: " << strerror(errno);
      return contents;
    }
  }
}

void WriteFile(const std::string& path, const std::string& contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(stream) << "Cannot open " << path;
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  // Closing flushes buffered writes. Check afterward so delayed write errors
  // cannot leave incomplete setup data and produce a misleading test failure.
  stream.close();
  ASSERT_TRUE(stream) << "Cannot write " << path;
}

bool FileExists(const std::string& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  // A permission or filesystem error must fail the test, rather than being
  // mistaken for the expected absence of a file.
  EXPECT_FALSE(error) << path << ": " << error.message();
  return exists;
}

mode_t FileMode(const std::string& path) {
  struct stat status = {};
  EXPECT_TRUE(CheckCall(stat(path.c_str(), &status), "stat")) << path;
  return status.st_mode & 0777;
}

}  // namespace detach_test
