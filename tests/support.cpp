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
#include <iterator>
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

void WaitReadable(int fd, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        struct pollfd event = {fd, POLLIN, 0};
        int result = poll(&event, 1, std::max(1, static_cast<int>(remaining)));
        if (result > 0) {
            return;
        }
        if (result < 0 && errno != EINTR) {
            CheckCall(result, "poll");
        }
    }
    throw std::runtime_error("Timed out waiting for the probe");
}

// All special caller state is installed in the launcher, never in GoogleTest.
void PrepareChild(const LaunchOptions &options, int report_fd, int error_fd) {
    auto require = [](int result) {
        if (result < 0) {
            perror("test launcher");
            _exit(125);
        }
    };
    require(dup2(report_fd, STDOUT_FILENO));
    require(dup2(error_fd, STDERR_FILENO));

    if (!options.terminal.empty()) {
        require(setsid());
        int terminal = open(options.terminal.c_str(), O_RDWR);
        require(terminal);
        require(ioctl(terminal, TIOCSCTTY, 0));
        require(dup2(terminal, STDIN_FILENO));
        close(terminal);
        // Fail setup if this caller did not actually obtain a terminal.
        int controlling_terminal = open("/dev/tty", O_RDWR | O_NOCTTY);
        require(controlling_terminal);
        close(controlling_terminal);
    }

    for (int signal : {SIGPIPE, SIGCHLD, SIGHUP, SIGALRM}) {
        bool ignored = (signal == SIGPIPE && options.ignore_sigpipe) ||
                       (signal == SIGCHLD && options.ignore_sigchld) ||
                       (signal == SIGHUP && options.ignore_sighup);
        struct sigaction action = {};
        action.sa_handler = ignored ? SIG_IGN : SIG_DFL;
        require(sigemptyset(&action.sa_mask));
        require(sigaction(signal, &action, nullptr));
    }
    sigset_t mask;
    require(sigemptyset(&mask));
    if (options.block_sigpipe) {
        require(sigaddset(&mask, SIGPIPE));
    }
    if (options.block_sigusr1) {
        require(sigaddset(&mask, SIGUSR1));
    }
    require(sigprocmask(SIG_SETMASK, &mask, nullptr));

    for (int fd : {STDOUT_FILENO, STDERR_FILENO}) {
        bool broken = (fd == STDOUT_FILENO && options.broken_stdout) ||
                      (fd == STDERR_FILENO && options.broken_stderr);
        if (broken) {
            int channel[2];
            require(pipe(channel));
            close(channel[0]);
            require(dup2(channel[1], fd));
            close(channel[1]);
        }
    }

    for (const auto &variable : options.environment) {
        require(setenv(variable.first.c_str(), variable.second.c_str(), 1));
    }
    if (options.file_creation_mask >= 0) {
        umask(static_cast<mode_t>(options.file_creation_mask));
    }
    if (options.descriptor_limit != 0) {
        struct rlimit limit;
        require(getrlimit(RLIMIT_NOFILE, &limit));
        limit.rlim_cur = options.descriptor_limit;
        require(setrlimit(RLIMIT_NOFILE, &limit));
    }
    // Close these last so setup cannot accidentally refill 0, 1, or 2.
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (options.closed_standard_descriptors & (1 << fd)) {
            close(fd);
        }
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

Descriptor::~Descriptor() {
    Reset();
}

void Descriptor::Reset(int fd) {
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = fd;
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
        throw std::runtime_error("Cannot read " + path);
    }
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
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
        if (probe_expected_ && peer_.Get() < 0) {
            ReadProbe();
        }
        if (peer_.Get() >= 0) {
            // EOF releases the probe even when an assertion stopped the test.
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
    Descriptor report(CheckCall(open(report_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600), "open report"));
    Descriptor errors(CheckCall(open(error_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600), "open diagnostics"));

    std::vector<char *> argv = {
        const_cast<char *>(executable.c_str())};
    for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    LaunchResult result;
    result.launcher_pid = CheckCall(fork(), "fork test launcher");
    if (result.launcher_pid == 0) {
        PrepareChild(options, report.Get(), errors.Get());
        if (chdir(directory_.c_str()) < 0) {
            perror("test working directory");
            _exit(125);
        }
        execv(executable.c_str(), argv.data());
        perror("exec detach");
        _exit(125);
    }

    const auto deadline = Clock::now() + kTimeout;
    while (true) {
        pid_t waited = waitpid(result.launcher_pid, &result.status, WNOHANG);
        if (waited == result.launcher_pid) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            CheckCall(-1, "waitpid test launcher");
        }
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            kill(result.launcher_pid, SIGKILL);
            while (waitpid(result.launcher_pid, &result.status, 0) < 0 &&
                   errno == EINTR) {}
            break;
        }
        // Retry reaping with a deadline, without assuming when exec completes.
        poll(nullptr, 0, 5);
    }
    if (WIFEXITED(result.status)) {
        result.exit_code = WEXITSTATUS(result.status);
    }
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
    EXPECT_NE(result.exit_code, 125) << result.error;
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

    // A stream socket may split a record across multiple reads.
    ProbeState state = {};
    auto *next = reinterpret_cast<char *>(&state);
    size_t remaining = sizeof(state);
    while (remaining > 0) {
        WaitReadable(peer_.Get(), deadline);
        ssize_t length = read(peer_.Get(), next, remaining);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        CheckCall(static_cast<int>(length), "read probe state");
        if (length == 0) {
            throw std::runtime_error("Probe exited before reporting its state");
        }
        next += length;
        remaining -= static_cast<size_t>(length);
    }
    probe_pid_ = static_cast<pid_t>(state.pid);
    probe_expected_ = true;
    return state;
}

void DetachTest::PingProbe() {
    const char ping = 'p';
    ASSERT_EQ(send(peer_.Get(), &ping, 1, MSG_NOSIGNAL), 1) << strerror(errno);
    WaitReadable(peer_.Get(), Clock::now() + kTimeout);
    char reply = 0;
    ASSERT_EQ(read(peer_.Get(), &reply, 1), 1);
    EXPECT_EQ(reply, ping);
}

}  // namespace detach_test
