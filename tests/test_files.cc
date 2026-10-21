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
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    ADD_FAILURE() << "Cannot open " << path << ": " << strerror(errno);
    return {};
  }
  std::string contents;
  char buffer[4096];
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
  stream.close();
  ASSERT_TRUE(stream) << "Cannot write " << path;
}

bool FileExists(const std::string& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  EXPECT_FALSE(error) << path << ": " << error.message();
  return exists;
}

mode_t FileMode(const std::string& path) {
  struct stat status = {};
  EXPECT_TRUE(CheckCall(stat(path.c_str(), &status), "stat")) << path;
  return status.st_mode & 0777;
}

}  // namespace detach_test
