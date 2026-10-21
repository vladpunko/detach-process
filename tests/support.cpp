// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/support.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace detach_test {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(5);
// Separate fixture setup failures from exit codes returned by detach itself.
constexpr int kLauncherFailure = 125;

void WaitReadable(int fd, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        struct pollfd event = {fd, POLLIN, 0};
        int result = poll(&event, 1, std::max(1, static_cast<int>(remaining)));
        if (result > 0) {
            if (event.revents & POLLNVAL) {
                throw std::runtime_error("Probe descriptor is invalid");
            }
            // Read on hangup too so callers can distinguish EOF from data.
            if (event.revents & (POLLIN | POLLHUP)) {
                return;
            }
            if (event.revents & POLLERR) {
                throw std::runtime_error("Probe socket reported an I/O error");
            }
        }
        if (result < 0 && errno != EINTR) {
            CheckCall(result, "poll");
        }
    }
    throw std::runtime_error("Timed out waiting for the probe");
}

void ReadExactly(int fd, void *buffer, size_t remaining,
                 Clock::time_point deadline) {
    // A stream socket may split a record across multiple reads.
    auto *next = static_cast<char *>(buffer);
    while (remaining > 0) {
        WaitReadable(fd, deadline);
        ssize_t length = read(fd, next, remaining);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        CheckCall(static_cast<int>(length), "read probe");
        if (length == 0) {
            throw std::runtime_error(
                "Probe closed before sending the expected data");
        }
        next += length;
        remaining -= static_cast<size_t>(length);
    }
}

// Child setup must exit immediately on failure, without unwinding GoogleTest.
void RequireChildCall(int result, const char *operation) {
    if (result < 0) {
        perror(operation);
        _exit(kLauncherFailure);
    }
}

void PrepareTerminal(const std::string &path) {
    if (path.empty()) {
        return;
    }
    RequireChildCall(setsid(), "test setsid");
    int terminal = open(path.c_str(), O_RDWR);
    RequireChildCall(terminal, "open test terminal");
    RequireChildCall(ioctl(terminal, TIOCSCTTY, 0), "acquire test terminal");
    RequireChildCall(dup2(terminal, STDIN_FILENO), "redirect test terminal");
    close(terminal);
    // Fail setup if this caller did not actually obtain a terminal.
    int controlling_terminal = open("/dev/tty", O_RDWR | O_NOCTTY);
    RequireChildCall(controlling_terminal, "open controlling terminal");
    close(controlling_terminal);
}

void ConfigureSignals(const LaunchOptions &options) {
    struct sigaction action = {};
    RequireChildCall(sigemptyset(&action.sa_mask), "test signal mask");
    // Clear dispositions inherited from the test runner before each scenario.
    action.sa_handler = SIG_DFL;
    for (int signal : {SIGPIPE, SIGCHLD, SIGHUP, SIGALRM}) {
        RequireChildCall(sigaction(signal, &action, nullptr), "reset signal");
    }
    action.sa_handler = SIG_IGN;
    for (int signal : options.ignored_signals) {
        RequireChildCall(sigaction(signal, &action, nullptr), "ignore signal");
    }

    sigset_t mask;
    RequireChildCall(sigemptyset(&mask), "test blocked signals");
    for (int signal : options.blocked_signals) {
        RequireChildCall(sigaddset(&mask, signal), "add blocked signal");
    }
    RequireChildCall(sigprocmask(SIG_SETMASK, &mask, nullptr),
                     "set blocked signals");
}

void BreakPipes(const std::vector<int> &descriptors) {
    for (int fd : descriptors) {
        int channel[2];
        RequireChildCall(pipe(channel), "create broken pipe");
        close(channel[0]);
        RequireChildCall(dup2(channel[1], fd), "redirect broken pipe");
        close(channel[1]);
    }
}

// All special caller state is installed in the launcher, never in GoogleTest.
void PrepareChild(const LaunchOptions &options, int report_fd, int error_fd) {
    RequireChildCall(dup2(report_fd, STDOUT_FILENO), "capture launcher report");
    RequireChildCall(dup2(error_fd, STDERR_FILENO), "capture launcher errors");
    PrepareTerminal(options.terminal);
    ConfigureSignals(options);
    BreakPipes(options.broken_pipes);

    for (const auto &variable : options.environment) {
        RequireChildCall(setenv(variable.first.c_str(),
                                variable.second.c_str(), 1), "set environment");
    }
    if (options.file_creation_mask >= 0) {
        umask(static_cast<mode_t>(options.file_creation_mask));
    }
    if (options.descriptor_limit != 0) {
        struct rlimit limit;
        RequireChildCall(getrlimit(RLIMIT_NOFILE, &limit),
                         "get descriptor limit");
        limit.rlim_cur = options.descriptor_limit;
        RequireChildCall(setrlimit(RLIMIT_NOFILE, &limit),
                         "set descriptor limit");
    }
    // Close these last so setup cannot accidentally refill 0, 1, or 2.
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (options.closed_standard_descriptors & (1 << fd)) {
            close(fd);
        }
    }
}

// Reap our direct child before reading its report, with a bound on hangs.
// The detached probe has a different parent and is released through its socket.
void WaitForLauncher(LaunchResult *result) {
    const auto deadline = Clock::now() + kTimeout;
    while (true) {
        pid_t waited = waitpid(result->launcher_pid, &result->status, WNOHANG);
        if (waited == result->launcher_pid) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            CheckCall(-1, "waitpid test launcher");
        }
        if (Clock::now() >= deadline) {
            result->timed_out = true;
            kill(result->launcher_pid, SIGKILL);
            while (waitpid(result->launcher_pid, &result->status, 0) < 0 &&
                   errno == EINTR) {}
            break;
        }
        // Retry reaping with a deadline, without assuming when exec completes.
        poll(nullptr, 0, 5);
    }
    if (WIFEXITED(result->status)) {
        result->exit_code = WEXITSTATUS(result->status);
    }
}

void ParseReport(const std::string &text, LaunchResult *result) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("PID: ", 0) == 0) {
            std::istringstream(line.substr(5)) >> result->program_pid;
        } else if (line.rfind("stdout: ", 0) == 0) {
            result->stdout_path = line.substr(8);
        } else if (line.rfind("stderr: ", 0) == 0) {
            result->stderr_path = line.substr(8);
        }
    }
}

}  // namespace

// Resource helpers keep ownership and I/O failures out of individual tests.
Descriptor::~Descriptor() {
    Reset();
}

void Descriptor::Reset(int fd) {
    if (fd_ != fd) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }
}

int CheckCall(int result, const char *operation) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
    return result;
}

std::string ReadFile(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path);
    }
    std::string contents;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
        contents.append(buffer, static_cast<size_t>(stream.gcount()));
    }
    if (stream.bad() || !stream.eof()) {
        throw std::runtime_error("Cannot read " + path);
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
            CheckCall(-1, "read pipe");
        }
    }
}

void WriteFile(const std::string &path, const std::string &contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write " + path);
    }
}

mode_t FileMode(const std::string &path) {
    struct stat status;
    CheckCall(stat(path.c_str(), &status), "stat");
    return status.st_mode & 0777;
}

// The fixture owns the socket, probe lifetime and all per-test files.
void DetachTest::SetUp() {
    char directory[] = "/tmp/detach-test.XXXXXX";
    char *created = mkdtemp(directory);
    ASSERT_NE(created, nullptr) << strerror(errno);
    directory_ = created;

    listener_.Reset(CheckCall(socket(AF_UNIX, SOCK_STREAM, 0), "socket"));
    CheckCall(fcntl(listener_.Get(), F_SETFD, FD_CLOEXEC), "fcntl");
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    const std::string path = SocketPath();
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    auto *socket_address = reinterpret_cast<struct sockaddr *>(&address);
    CheckCall(bind(listener_.Get(), socket_address, sizeof(address)), "bind");
    CheckCall(listen(listener_.Get(), 1), "listen");
}

void DetachTest::TearDown() {
    try {
        StopProbe();
    } catch (const std::exception &error) {
        ADD_FAILURE() << error.what();
        if (probe_pid_ > 0) {
            kill(probe_pid_, SIGKILL);
        }
    }
    peer_.Reset();
    listener_.Reset();
    for (const auto &path : temporary_logs_) {
        EXPECT_EQ(unlink(path.c_str()), 0) << path << ": " << strerror(errno);
    }
    if (!directory_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error) << error.message();
    }
}

void DetachTest::StopProbe() {
    if (probe_expected_ && peer_.Get() < 0) {
        ReadProbe();
    }
    if (peer_.Get() >= 0) {
        // EOF releases the probe even when an assertion stopped the test.
        // Drain until EOF to confirm it finished using the per-test files.
        shutdown(peer_.Get(), SHUT_WR);
        const auto deadline = Clock::now() + kTimeout;
        char buffer[256];
        while (true) {
            WaitReadable(peer_.Get(), deadline);
            ssize_t length = read(peer_.Get(), buffer, sizeof(buffer));
            if (length == 0) {
                break;
            }
            if (length < 0 && errno != EINTR) {
                CheckCall(-1, "read probe during cleanup");
            }
        }
    }
}

std::string DetachTest::Path(const std::string &name) const {
    return directory_ + "/" + name;
}

std::string DetachTest::SocketPath() const {
    return Path("probe.socket");
}

std::vector<std::string> DetachTest::ProbeCommand(
        const std::string &mode,
        const std::vector<std::string> &arguments) const {
    std::vector<std::string> command = {PROBE_PATH, mode, SocketPath()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

LaunchResult DetachTest::Run(const std::vector<std::string> &arguments,
                             const LaunchOptions &options) {
    std::string executable = options.executable;
    if (executable.empty()) {
        const char *selected = getenv("DETACH_TEST_EXECUTABLE");
        executable = selected == nullptr ? DETACH_PATH : selected;
    }
    std::string name = "launch-" + std::to_string(++launch_count_);
    const std::string report_path = Path(name + ".out");
    const std::string error_path = Path(name + ".err");
    // Captures belong to the fixture and must not leak into the utility.
    Descriptor report(CheckCall(open(report_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600), "open report"));
    Descriptor errors(CheckCall(open(error_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600), "open diagnostics"));

    // Own mutable argv storage before fork and keep it stable until exec.
    std::vector<std::string> command = {executable};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &argument : command) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    LaunchResult result;
    result.launcher_pid = CheckCall(fork(), "fork test launcher");
    if (result.launcher_pid == 0) {
        PrepareChild(options, report.Get(), errors.Get());
        if (chdir(directory_.c_str()) < 0) {
            perror("test working directory");
            _exit(kLauncherFailure);
        }
        execv(executable.c_str(), argv.data());
        perror("exec detach");
        _exit(kLauncherFailure);
    }

    WaitForLauncher(&result);
    result.output = ReadFile(report_path);
    result.error = ReadFile(error_path);
    ParseReport(result.output, &result);
    ParseReport(result.error, &result);
    for (const auto &path : {result.stdout_path, result.stderr_path}) {
        if (path.rfind("/tmp/stdout.", 0) == 0 ||
            path.rfind("/tmp/stderr.", 0) == 0) {
            temporary_logs_.insert(path);
        }
    }
    EXPECT_FALSE(result.timed_out)
        << "detach did not return before the deadline";
    EXPECT_NE(result.exit_code, kLauncherFailure) << result.error;
    return result;
}

LaunchResult DetachTest::RunProbe(
        const std::vector<std::string> &options, const std::string &mode,
        const std::vector<std::string> &arguments,
        const LaunchOptions &launch_options) {
    std::vector<std::string> command = options;
    command.push_back("--");
    auto probe = ProbeCommand(mode, arguments);
    command.insert(command.end(), probe.begin(), probe.end());
    auto result = Run(command, launch_options);
    probe_expected_ = result.program_pid > 0;
    return result;
}

ProbeState DetachTest::ReadProbe() {
    const auto deadline = Clock::now() + kTimeout;
    WaitReadable(listener_.Get(), deadline);
    peer_.Reset(CheckCall(accept(listener_.Get(), nullptr, nullptr), "accept"));

    ProbeState state = {};
    ReadExactly(peer_.Get(), &state, sizeof(state), deadline);
    probe_pid_ = static_cast<pid_t>(state.pid);
    probe_expected_ = true;
    return state;
}

void DetachTest::PingProbe() {
    const char ping = 'p';
    ssize_t sent;
    do {
        sent = send(peer_.Get(), &ping, 1, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    ASSERT_EQ(sent, 1) << strerror(errno);
    char reply = 0;
    ReadExactly(peer_.Get(), &reply, 1, Clock::now() + kTimeout);
    EXPECT_EQ(reply, ping);
}

}  // namespace detach_test
