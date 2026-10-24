// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/support/runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/io.h"

namespace detach_test {
namespace {

constexpr auto kTimeout = std::chrono::seconds(5);

// Reserved exit code for errors before `detach` starts.
constexpr int kSetupErrorExitCode = 125;

// Launch preparation.

// Keeps capture files above standard descriptors so closed streams stay closed.
int OpenCaptureFile(const std::string& path, const char* operation) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);

    // Keep `stdin`, `stdout` and `stderr` available for child setup.
    if (fd >= 0 && fd <= STDERR_FILENO) {
        FileDescriptor original(fd);
        fd = fcntl(original.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    }

    EXPECT_TRUE(CheckSysCall(fd, operation)) << path;
    return fd;
}

// Child process setup.

// Exits on setup errors so the forked copy cannot continue running tests.
void ExitOnSysCallError(int status, const char* operation) {
    if (status < 0) {
        perror(operation);

        _exit(kSetupErrorExitCode);
    }
}

// Redirects a descriptor and closes the extra source handle.
void MoveDescriptor(int source_fd, int target_fd, const char* operation) {
    ExitOnSysCallError(dup2(source_fd, target_fd), operation);
    if (source_fd != target_fd) {
        ExitOnSysCallError(close(source_fd), operation);
    }
}

// Attaches a controlling terminal to check that `detach` releases it.
void AttachControllingTerminal(const std::string& path) {
    if (path.empty()) {
        return;
    }

    // Only a session leader can acquire a controlling terminal.
    ExitOnSysCallError(setsid(), "test setsid");

    const int terminal_fd = open(path.c_str(), O_RDWR);
    ExitOnSysCallError(terminal_fd, "open test terminal");

    const int attach_status = ioctl(terminal_fd, TIOCSCTTY, 0);
    ExitOnSysCallError(attach_status, "acquire test terminal");

    MoveDescriptor(terminal_fd, STDIN_FILENO, "redirect test terminal");

    // Opening `/dev/tty` confirms that the controlling terminal was attached.
    const int controlling_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    ExitOnSysCallError(controlling_fd, "open controlling terminal");

    const int close_status = close(controlling_fd);
    ExitOnSysCallError(close_status, "close controlling terminal handle");
}

// Sets signal dispositions and masks only in the child process.
void SetUpSignals(const LaunchOptions& options) {
    struct sigaction action = {};
    ExitOnSysCallError(sigemptyset(&action.sa_mask), "test signal mask");

    // Clear dispositions inherited from the test runner before each scenario.
    action.sa_handler = SIG_DFL;
    for (int signal_number : {SIGPIPE, SIGCHLD, SIGHUP, SIGALRM}) {
        const int status = sigaction(signal_number, &action, nullptr);
        ExitOnSysCallError(status, "reset signal");
    }

    action.sa_handler = SIG_IGN;
    for (int signal_number : options.ignored_signals) {
        const int status = sigaction(signal_number, &action, nullptr);
        ExitOnSysCallError(status, "ignore signal");
    }

    sigset_t blocked_mask = {};
    ExitOnSysCallError(sigemptyset(&blocked_mask), "test blocked signals");

    for (int signal_number : options.blocked_signals) {
        const int status = sigaddset(&blocked_mask, signal_number);
        ExitOnSysCallError(status, "add blocked signal");
    }

    const int mask_status = sigprocmask(SIG_SETMASK, &blocked_mask, nullptr);
    ExitOnSysCallError(mask_status, "set blocked signals");
}

// Provokes `EPIPE` and `SIGPIPE` without changing test runner streams.
void BreakPipes(const std::vector<int>& descriptors) {
    for (int target_fd : descriptors) {
        std::array<int, 2> channel = {};
        ExitOnSysCallError(pipe(channel.data()), "create broken pipe");

        const int reader_fd = channel[0];
        const int writer_fd = channel[1];

        // Remove the reader so writes produce `EPIPE` or `SIGPIPE`.
        ExitOnSysCallError(close(reader_fd), "close broken pipe reader");

        MoveDescriptor(writer_fd, target_fd, "redirect broken pipe");
    }
}

// Applies caller settings in the child so the test runner keeps its own state.
void PrepareChild(const LaunchOptions& options) {
    AttachControllingTerminal(options.terminal_path);
    SetUpSignals(options);
    BreakPipes(options.broken_pipe_descriptors);

    // Apply the requested environment overrides.
    for (const auto& [name, value] : options.environment) {
        const int status = setenv(name.c_str(), value.c_str(), 1);
        ExitOnSysCallError(status, "set environment");
    }

    // Set the `umask` inherited by `detach`.
    if (options.file_creation_mask >= 0) {
        umask(static_cast<mode_t>(options.file_creation_mask));
    }

    // Preserve the hard limit when changing `RLIMIT_NOFILE`.
    if (options.descriptor_limit != 0) {
        struct rlimit limits = {};
        const int get_status = getrlimit(RLIMIT_NOFILE, &limits);
        ExitOnSysCallError(get_status, "get descriptor limit");

        limits.rlim_cur = options.descriptor_limit;
        const int set_status = setrlimit(RLIMIT_NOFILE, &limits);
        ExitOnSysCallError(set_status, "set descriptor limit");
    }

    // Close selected standard streams last so setup cannot reopen them.
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if ((options.closed_standard_descriptors & (1 << fd)) != 0) {
            close(fd);
        }
    }
}

// Process results.

// Kills and reaps the direct child so a timeout leaves no process or zombie.
testing::AssertionResult StopLauncher(RunReport& report) {
    // The child may have exited between the timeout check and `kill`.
    const int status = kill(report.detach_pid, SIGKILL);
    if (status < 0 && errno != ESRCH) {
        return CheckSysCall(status, "kill test launcher");
    }

    // Reap the child, retrying `waitpid` if a signal interrupts the wait.
    while (true) {
        const pid_t waited_pid = waitpid(report.detach_pid, &report.status, 0);
        if (waited_pid >= 0 || errno != EINTR) {
            return CheckSysCall(waited_pid, "reap test launcher");
        }
    }
}

// Waits for `detach` with a deadline. Its command is managed separately.
testing::AssertionResult WaitForLauncher(RunReport& report) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;

    // Use nonblocking waits so the deadline can be enforced.
    while (true) {
        const pid_t waited_pid = waitpid(report.detach_pid, &report.status, WNOHANG);

        if (waited_pid == report.detach_pid) {
            if (WIFEXITED(report.status)) {
                report.exit_code = WEXITSTATUS(report.status);
            }
            return testing::AssertionSuccess();
        }

        if (waited_pid < 0 && errno != EINTR) {
            return CheckSysCall(waited_pid, "wait for test launcher");
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        // Avoid spinning while `waitpid` reports that the child is running.
        poll(nullptr, 0, 5);
    }

    // Stop and reap the child before reporting a timeout.
    report.is_timed_out = true;
    const auto stop_status = StopLauncher(report);
    if (!stop_status) {
        return stop_status;
    }
    return testing::AssertionFailure()
           << "detach did not return before the deadline";
}

// Extracts the PID and output paths from the public startup report of `detach`.
void ParseReport(const std::string& text, RunReport& report) {
    std::istringstream report_lines(text);
    std::string line;

    while (std::getline(report_lines, line)) {
        const auto separator = line.find(": ");
        if (separator == std::string::npos) {
            continue;
        }

        // Report lines use `name: value`. Keep spaces inside the value.
        const std::string name = line.substr(0, separator);
        const std::string value = line.substr(separator + 2);

        if (name == "PID") {
            std::istringstream(value) >> report.command_pid;
        } else if (name == "stdout") {
            report.stdout_path = value;
        } else if (name == "stderr") {
            report.stderr_path = value;
        }
    }
}

}  // namespace

// Running `detach`.

RunReport RunDetach(std::vector<std::string> arguments,
                    const std::string& directory,
                    const LaunchOptions& options) {
    std::string executable_path = options.executable_path;
    if (executable_path.empty()) {
        const char* selected = getenv("DETACH_TEST_EXECUTABLE");
        executable_path = selected == nullptr ? "" : selected;
    }

    if (executable_path.empty()) {
        ADD_FAILURE() << "No detach executable configured. "
                      << "Set executable_path or DETACH_TEST_EXECUTABLE.";
        return {};
    }

    const std::string output_path = directory + "/launcher.out";
    const std::string error_path = directory + "/launcher.err";
    RunReport report;

    // Files can be read while the detached command is still running.
    // `O_CLOEXEC` closes extra file descriptors when `detach` starts.
    FileDescriptor output(OpenCaptureFile(output_path, "open report"));
    if (output.get() < 0) {
        return report;
    }

    FileDescriptor errors(OpenCaptureFile(error_path, "open diagnostics"));
    if (errors.get() < 0) {
        return report;
    }

    // Keep the strings alive until `execv` reads the argument pointers.
    std::vector<char*> argv = {&executable_path[0]};
    for (std::string& argument : arguments) {
        argv.push_back(&argument[0]);
    }
    argv.push_back(nullptr);

    report.detach_pid = fork();
    if (report.detach_pid == 0) {
        MoveDescriptor(output.get(), STDOUT_FILENO, "capture launcher report");
        MoveDescriptor(errors.get(), STDERR_FILENO, "capture launcher errors");
        PrepareChild(options);
        ExitOnSysCallError(chdir(directory.c_str()), "test working directory");
        execv(executable_path.c_str(), argv.data());

        // A successful `execv` never returns to the test process.
        perror("exec detach");
        _exit(kSetupErrorExitCode);
    }

    EXPECT_TRUE(CheckSysCall(report.detach_pid, "fork test launcher"));
    if (report.detach_pid < 0) {
        return report;
    }

    // Wait for the direct child before reading the capture files.
    EXPECT_TRUE(WaitForLauncher(report));
    report.output = Read(output_path);
    report.error = Read(error_path);

    // `detach` prints the report to `stderr` if `stdout` is unavailable.
    ParseReport(report.output, report);
    ParseReport(report.error, report);

    EXPECT_NE(report.exit_code, kSetupErrorExitCode) << report.error;
    return report;
}

}  // namespace detach_test
