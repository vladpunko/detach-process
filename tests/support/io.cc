// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/support/io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include "gtest/gtest.h"

namespace detach_test {
namespace {

constexpr std::size_t kReadBufferSize = 4096;

// Reports a path operation failure using the saved system error.
void ReportPathError(const char* message, const std::string& path, int error) {
    ADD_FAILURE()
        << message
        << path
        << ": "
        << std::strerror(error);
}

// Collects bytes and returns zero on `EOF` or a saved `errno` on error.
int ReadContents(int fd, std::string& contents) {
    std::array<char, kReadBufferSize> buffer;

    while (true) {
        const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());
        if (bytes_read > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0) {
            return 0;
        }

        const int error = errno;
        if (error == EINTR) {
            continue;
        }

        return error;
    }
}

}  // namespace

// Descriptor ownership.

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
    Reset();
}

int FileDescriptor::get() const {
    return fd_;
}

// Replaces the descriptor without losing the system error saved by the caller.
void FileDescriptor::Reset(int new_fd) {
    if (fd_ == new_fd) {
        return;
    }

    const int prev_fd = std::exchange(fd_, new_fd);
    if (prev_fd < 0) {
        return;
    }

    const int saved_errno = errno;
    // Retrying `close` could close a descriptor reused by another thread.
    const int status = close(prev_fd);
    EXPECT_TRUE(CheckSysCall(status, "close descriptor"));
    errno = saved_errno;
}

// Reports the operation and system error when a syscall fails.
testing::AssertionResult CheckSysCall(int status, const char* operation) {
    if (status >= 0) {
        return testing::AssertionSuccess();
    }

    const int error = errno;
    return testing::AssertionFailure()
        << operation
        << ": "
        << std::strerror(error);
}

// Builds a `/dev/fd` path for inherited descriptor checks.
std::string DescriptorPath(int fd) {
    return "/dev/fd/" + std::to_string(fd);
}

// Reading and writing.

// Reads file contents and preserves any bytes received before a read error.
std::string Read(const std::string& path) {
    FileDescriptor input(open(path.c_str(), O_RDONLY));
    if (input.get() < 0) {
        const int error = errno;
        ReportPathError("it is not possible to open ", path, error);
        return {};
    }

    std::string contents;
    const int read_error = ReadContents(input.get(), contents);
    if (read_error != 0) {
        ReportPathError("it is not possible to read ", path, read_error);
    }

    return contents;
}

// Replaces file contents and checks for buffered errors after closing.
void Write(const std::string& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output)
        << "cannot open "
        << path;

    const auto bytes_to_write = static_cast<std::streamsize>(contents.size());
    output.write(contents.data(), bytes_to_write);

    output.close();
    ASSERT_TRUE(output)
        << "cannot write "
        << path;
}

// Collects pipe output until `EOF` or until a nonblocking read would wait.
std::string ReadPipeOutput(int fd) {
    std::string contents;
    const int read_error = ReadContents(fd, contents);
    if (read_error != 0 && read_error != EAGAIN && read_error != EWOULDBLOCK) {
        ADD_FAILURE()
            << "cannot read pipe output: "
            << std::strerror(read_error);
    }

    return contents;
}

// Checks whether a path exists and reports unexpected filesystem errors.
bool Exists(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) == 0) {
        return true;
    }

    const int error = errno;
    if (error != ENOENT && error != ENOTDIR) {
        ReportPathError("cannot check whether path exists: ", path, error);
    }

    return false;
}

// Reads permission bits so tests can verify file creation modes.
mode_t GetPermission(const std::string& path) {
    struct stat stats = {};
    const int status = stat(path.c_str(), &stats);

    EXPECT_TRUE(CheckSysCall(status, "stat"))
        << "cannot read permissions for: "
        << path;

    return stats.st_mode & 0777;
}

}  // namespace detach_test
