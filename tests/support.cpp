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
#include <string>
#include <vector>

namespace detach_test {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(5);
// Separate fixture setup failures from exit codes returned by detach itself.
constexpr int kLauncherFailure = 125;

testing::AssertionResult WaitReadable(int fd, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
        struct pollfd event = {fd, POLLIN, 0};
        const int result = poll(&event, 1,
                               std::max(1, static_cast<int>(remaining)));
        if (result > 0) {
            if (event.revents & POLLNVAL) {
                return testing::AssertionFailure() << "Invalid probe descriptor";
            }
            // Hangup is readable so the caller can distinguish EOF from data.
            if (event.revents & (POLLIN | POLLHUP)) {
                return testing::AssertionSuccess();
            }
            if (event.revents & POLLERR) {
                return testing::AssertionFailure() << "Probe socket I/O error";
            }
        }
        if (result < 0 && errno != EINTR) {
            return CheckCall(result, "poll probe");
        }
    }
    return testing::AssertionFailure() << "Timed out waiting for the probe";
}

// Reads one complete record using a single deadline across partial transfers.
testing::AssertionResult ReadExactly(int fd, Clock::time_point deadline,
                                     size_t size, void* buffer) {
    auto* next = static_cast<char*>(buffer);
    while (size > 0) {
        const auto ready = WaitReadable(fd, deadline);
        if (!ready) {
            return ready;
        }
        const ssize_t length = read(fd, next, size);
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            return CheckCall(-1, "read probe");
        }
        if (length == 0) {
            return testing::AssertionFailure()
                   << "Probe closed before sending the expected data";
        }
        next += length;
        size -= static_cast<size_t>(length);
    }
    return testing::AssertionSuccess();
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
testing::AssertionResult WaitForLauncher(LaunchResult& result) {
    const auto deadline = Clock::now() + kTimeout;
    while (true) {
        pid_t waited = waitpid(result.launcher_pid, &result.status, WNOHANG);
        if (waited == result.launcher_pid) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            return CheckCall(-1, "waitpid test launcher");
        }
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            kill(result.launcher_pid, SIGKILL);
            while (waitpid(result.launcher_pid, &result.status, 0) < 0 &&
                   errno == EINTR) {}
            return testing::AssertionFailure()
                   << "detach did not return before the deadline";
        }
        // Retry reaping with a deadline, without assuming when exec completes.
        poll(nullptr, 0, 5);
    }
    if (WIFEXITED(result.status)) {
        result.exit_code = WEXITSTATUS(result.status);
    }
    return testing::AssertionSuccess();
}

void ParseReport(const std::string& text, LaunchResult& result) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("PID: ", 0) == 0) {
            std::istringstream(line.substr(5)) >> result.program_pid;
        } else if (line.rfind("stdout: ", 0) == 0) {
            result.stdout_path = line.substr(8);
        } else if (line.rfind("stderr: ", 0) == 0) {
            result.stderr_path = line.substr(8);
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

testing::AssertionResult CheckCall(int result, const char* operation) {
    if (result < 0) {
        return testing::AssertionFailure() << operation << ": " << strerror(errno);
    }
    return testing::AssertionSuccess();
}

std::string ReadFile(const std::string &path) {
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

void WriteFile(const std::string &path, const std::string &contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream) << "Cannot open " << path;
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    stream.close();
    ASSERT_TRUE(stream) << "Cannot write " << path;
}

mode_t FileMode(const std::string &path) {
    struct stat status = {};
    EXPECT_TRUE(CheckCall(stat(path.c_str(), &status), "stat")) << path;
    return status.st_mode & 0777;
}

// The fixture owns the socket, probe lifetime and all per-test files.
void DetachTest::SetUp() {
    char directory[] = "/tmp/detach-test.XXXXXX";
    char *created = mkdtemp(directory);
    ASSERT_NE(created, nullptr) << strerror(errno);
    directory_ = created;

    listener_.Reset(socket(AF_UNIX, SOCK_STREAM, 0));
    ASSERT_TRUE(CheckCall(listener_.Get(), "socket"));
    ASSERT_TRUE(CheckCall(fcntl(listener_.Get(), F_SETFD, FD_CLOEXEC), "fcntl"));
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    const std::string path = SocketPath();
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    auto *socket_address = reinterpret_cast<struct sockaddr *>(&address);
    ASSERT_TRUE(CheckCall(
        bind(listener_.Get(), socket_address, sizeof(address)), "bind"));
    ASSERT_TRUE(CheckCall(listen(listener_.Get(), 1), "listen"));
}

void DetachTest::TearDown() {
    const auto stopped = StopProbe();
    EXPECT_TRUE(stopped);
    if (!stopped && probe_pid_ > 0) {
        kill(probe_pid_, SIGKILL);
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

testing::AssertionResult DetachTest::StopProbe() {
    if (probe_expected_ && peer_.Get() < 0) {
        ProbeState state = {};
        const auto reported = ReadProbeState(state);
        if (!reported) {
            return reported;
        }
    }
    if (peer_.Get() < 0) {
        return testing::AssertionSuccess();
    }

    // EOF releases the probe even when an assertion stopped the test.
    // Drain until EOF to confirm it finished using the per-test files.
    shutdown(peer_.Get(), SHUT_WR);
    const auto deadline = Clock::now() + kTimeout;
    char buffer[256];
    while (true) {
        const auto ready = WaitReadable(peer_.Get(), deadline);
        if (!ready) {
            return ready;
        }
        const ssize_t length = read(peer_.Get(), buffer, sizeof(buffer));
        if (length == 0) {
            return testing::AssertionSuccess();
        }
        if (length < 0 && errno != EINTR) {
            return CheckCall(-1, "read probe during cleanup");
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
    LaunchResult result;
    // Captures belong to the fixture and must not leak into the utility.
    Descriptor report(open(report_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600));
    EXPECT_TRUE(CheckCall(report.Get(), "open report"));
    if (report.Get() < 0) {
        return result;
    }
    Descriptor errors(open(error_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600));
    EXPECT_TRUE(CheckCall(errors.Get(), "open diagnostics"));
    if (errors.Get() < 0) {
        return result;
    }

    // Own mutable argv storage before fork and keep it stable until exec.
    std::vector<std::string> command = {executable};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &argument : command) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    result.launcher_pid = fork();
    EXPECT_TRUE(CheckCall(result.launcher_pid, "fork test launcher"));
    if (result.launcher_pid < 0) {
        return result;
    }
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

    EXPECT_TRUE(WaitForLauncher(result));
    result.output = ReadFile(report_path);
    result.error = ReadFile(error_path);
    ParseReport(result.output, result);
    ParseReport(result.error, result);
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

testing::AssertionResult DetachTest::ReadProbeState(ProbeState& state) {
    const auto deadline = Clock::now() + kTimeout;
    const auto ready = WaitReadable(listener_.Get(), deadline);
    if (!ready) {
        return ready;
    }
    int accepted;
    do {
        accepted = accept(listener_.Get(), nullptr, nullptr);
    } while (accepted < 0 && errno == EINTR);
    peer_.Reset(accepted);
    if (accepted < 0) {
        return CheckCall(-1, "accept probe");
    }
    const auto reported = ReadExactly(peer_.Get(), deadline, sizeof(state), &state);
    if (!reported) {
        return reported;
    }
    probe_pid_ = static_cast<pid_t>(state.pid);
    probe_expected_ = true;
    return testing::AssertionSuccess();
}

ProbeState DetachTest::ReadProbe() {
    ProbeState state = {};
    EXPECT_TRUE(ReadProbeState(state));
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
    ASSERT_TRUE(ReadExactly(peer_.Get(), Clock::now() + kTimeout, 1, &reply));
    EXPECT_EQ(reply, ping);
}

}  // namespace detach_test
