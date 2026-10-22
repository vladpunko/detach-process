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

// Retries reads interrupted by signals without losing progress.
ssize_t ReadChunk(int fd, std::array<char, kReadBufferSize>& buffer) {
    while (true) {
        const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());
        if (bytes_read >= 0 || errno != EINTR) {
            return bytes_read;
        }
    }
}

}  // namespace

// Descriptor ownership.

Descriptor::Descriptor(int fd) : fd_(fd) {}

Descriptor::~Descriptor() {
    Reset();
}

int Descriptor::get() const {
    return fd_;
}

void Descriptor::Reset(int new_fd) {
    if (fd_ == new_fd) {
        return;
    }

    const int prev_fd = std::exchange(fd_, new_fd);
    if (prev_fd < 0) {
        return;
    }

    const int saved_errno = errno;
    // Do not retry close: the descriptor may already be released.
    const int status = close(prev_fd);
    EXPECT_TRUE(CheckSysCall(status, "close descriptor"));
    errno = saved_errno;
}

testing::AssertionResult CheckSysCall(int status, const char* operation) {
    if (status >= 0) {
        return testing::AssertionSuccess();
    }

    const int error = errno;
    return testing::AssertionFailure() << operation
                                       << ": "
                                       << std::strerror(error);
}

// Reading and writing.

std::string Read(const std::string& path) {
    Descriptor input(open(path.c_str(), O_RDONLY));
    if (input.get() < 0) {
        const int error = errno;
        ADD_FAILURE() << "It is not possible to open "
                      << path
                      << ": "
                      << std::strerror(error);
        return {};
    }

    std::array<char, kReadBufferSize> buffer;
    std::string contents;

    while (true) {
        const ssize_t bytes_read = ReadChunk(input.get(), buffer);
        if (bytes_read > 0) {
            contents.append(buffer.data(),
                            static_cast<std::size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0) {
            return contents;
        }

        const int read_error = errno;
        ADD_FAILURE() << "It is not possible to read "
                      << path
                      << ": "
                      << std::strerror(read_error);
        return contents;
    }
}

void Write(const std::string& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output) << "Cannot open " << path;

    const auto bytes_to_write = static_cast<std::streamsize>(contents.size());
    output.write(contents.data(), bytes_to_write);

    // Check after close so buffered write errors are included.
    output.close();
    ASSERT_TRUE(output) << "Cannot write " << path;
}

std::string ReadPipeOutput(int fd) {
    std::array<char, kReadBufferSize> buffer;
    std::string contents;

    while (true) {
        const ssize_t bytes_read = ReadChunk(fd, buffer);
        if (bytes_read > 0) {
            contents.append(buffer.data(),
                            static_cast<std::size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0) {
            return contents;
        }

        const int read_error = errno;
        if (read_error != EAGAIN && read_error != EWOULDBLOCK) {
            ADD_FAILURE() << "Cannot read pipe output: "
                          << std::strerror(read_error);
        }

        return contents;
    }
}

bool Exists(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) == 0) {
        return true;
    }

    const int error = errno;
    if (error != ENOENT && error != ENOTDIR) {
        ADD_FAILURE() << "Cannot check whether path exists: "
                      << path
                      << ": "
                      << std::strerror(error);
    }

    return false;
}

mode_t GetPermission(const std::string& path) {
    struct stat stats = {};
    const int status = stat(path.c_str(), &stats);

    EXPECT_TRUE(CheckSysCall(status, "stat")) << "Cannot read permissions for: "
                                              << path;

    return stats.st_mode & 0777;
}

}  // namespace detach_test
