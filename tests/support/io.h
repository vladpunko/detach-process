// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_IO_H_
#define TESTS_SUPPORT_IO_H_

#include <sys/types.h>

#include <string>

#include "gtest/gtest.h"

namespace detach_test {

// Keeps descriptor cleanup safe when an assertion stops a test.
class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1);
    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;

    // Prevent two owners from closing the same descriptor.
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    // Returns the descriptor without transferring ownership.
    [[nodiscard]] int get() const;

    // Closes the current descriptor and takes ownership of the new one.
    void Reset(int new_fd = -1);

private:
    int fd_;
};

// Checks a system call result so test assertions show the operation and error.
[[nodiscard]] testing::AssertionResult CheckSysCall(int status,
                                                    const char* operation);

std::string Read(const std::string& path);
void Write(const std::string& path, const std::string& contents);

// Reads pipe output until `EOF` or until a nonblocking pipe has no data.
std::string ReadPipeOutput(int fd);

bool Exists(const std::string& path);
mode_t GetPermission(const std::string& path);

}  // namespace detach_test

#endif  // TESTS_SUPPORT_IO_H_
