// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_H_
#define TESTS_SUPPORT_H_

#include <sys/resource.h>
#include <sys/types.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/probe_state.h"

namespace detach_test {

// Own descriptors so failed assertions still close sockets and terminals.
class Descriptor {
 public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor();
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int Get() const { return fd_; }
    void Reset(int fd = -1);

 private:
    int fd_;
};

struct LaunchOptions {
    std::string executable;
    std::map<std::string, std::string> environment;
    std::string terminal;
    rlim_t descriptor_limit = 0;
    int closed_standard_descriptors = 0;
    int file_creation_mask = -1;
    bool broken_stdout = false;
    bool broken_stderr = false;
    bool ignore_sigpipe = false;
    bool ignore_sigchld = false;
    bool ignore_sighup = false;
    bool block_sigpipe = false;
    bool block_sigusr1 = false;
};

struct LaunchResult {
    pid_t launcher_pid = -1;
    pid_t program_pid = -1;
    int status = -1;
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
    std::string error;
    std::string stdout_path;
    std::string stderr_path;
};

int CheckCall(int result, const char *operation);
std::string ReadFile(const std::string &path);
void WriteFile(const std::string &path, const std::string &contents);
mode_t FileMode(const std::string &path);

class DetachTest : public testing::Test {
 protected:
    void SetUp() override;
    void TearDown() override;

    std::string Path(const std::string &name) const;
    std::string SocketPath() const;
    std::vector<std::string> ProbeCommand(
        const std::string &mode = "state",
        const std::vector<std::string> &arguments = {}) const;
    LaunchResult Run(const std::vector<std::string> &arguments,
                     const LaunchOptions &options = {});
    LaunchResult RunProbe(const std::vector<std::string> &options = {},
                          const std::string &mode = "state",
                          const std::vector<std::string> &arguments = {},
                          const LaunchOptions &launch_options = {});

    // The probe reports only after its output is flushed, then waits for us.
    ProbeState ReadProbe();
    void PingProbe();

 private:
    std::string directory_;
    Descriptor listener_;
    Descriptor peer_;
    std::set<std::string> temporary_logs_;
    pid_t probe_pid_ = -1;
    bool probe_expected_ = false;
    unsigned int launch_count_ = 0;
};

}  // namespace detach_test

#endif  // TESTS_SUPPORT_H_
