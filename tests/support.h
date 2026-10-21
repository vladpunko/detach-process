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

// Apply caller state in a forked launcher so tests never alter their runner.
struct LaunchOptions {
    std::string executable;
    std::map<std::string, std::string> environment;
    std::string terminal;
    rlim_t descriptor_limit = 0;
    // Bit n closes descriptor n after all other launcher setup is complete.
    int closed_standard_descriptors = 0;
    int file_creation_mask = -1;  // Negative keeps the runner's current umask.
    std::vector<int> broken_pipes;
    std::vector<int> ignored_signals;
    std::vector<int> blocked_signals;
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
// Read a nonblocking pipe after the probe confirms its writes are complete.
std::string ReadAvailable(int fd);
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
    void StopProbe();

    std::string directory_;
    // Accept one report, then use the peer for liveness checks and shutdown.
    Descriptor listener_;
    Descriptor peer_;
    std::set<std::string> temporary_logs_;
    pid_t probe_pid_ = -1;
    // A successful report still needs cleanup if an assertion skipped accept.
    bool probe_expected_ = false;
    unsigned int launch_count_ = 0;
};

}  // namespace detach_test

#endif  // TESTS_SUPPORT_H_
