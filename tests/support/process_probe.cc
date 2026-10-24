// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/support/process_probe.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

#include "gtest/gtest.h"
#include "tests/support/io.h"
#include "tests/support/probe_state.h"

namespace detach_test {
namespace {

constexpr auto kTimeout = std::chrono::seconds(5);

// Keeps control sockets out of other programs started by the test.
testing::AssertionResult SetCloseOnExec(int fd) {
    const int status = fcntl(fd, F_SETFD, FD_CLOEXEC);
    return CheckSysCall(status, "set probe close-on-exec");
}

// Waits for data or `EOF` without extending the deadline after a signal.
testing::AssertionResult WaitReadable(int fd,
                                      std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto timeout
            = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);

        // Wait at least one millisecond to avoid spinning near the deadline.
        const int wait_ms = std::max(1, static_cast<int>(timeout.count()));
        struct pollfd event = {.fd = fd, .events = POLLIN, .revents = 0};
        const int status = poll(&event, 1, wait_ms);
        if (status < 0 && errno != EINTR) {
            return CheckSysCall(status, "poll probe");
        }
        if (status <= 0) {
            continue;
        }

        if (event.revents & POLLNVAL) {
            return testing::AssertionFailure() << "invalid probe descriptor";
        }

        // Let the caller read buffered data or observe `EOF` after a hangup.
        if (event.revents & (POLLIN | POLLHUP)) {
            return testing::AssertionSuccess();
        }
        if (event.revents & POLLERR) {
            return testing::AssertionFailure() << "cannot communicate with the probe";
        }
    }

    return testing::AssertionFailure() << "timed out waiting for the probe";
}

// Reads one complete record using a single deadline across partial transfers.
template <typename Record>
testing::AssertionResult ReadExactly(int fd,
                                     Record& record,
                                     std::chrono::steady_clock::time_point deadline) {
    static_assert(std::is_trivially_copyable_v<Record>,
                  "probe records must be safe to copy as raw bytes");

    auto buffer = std::as_writable_bytes(std::span{&record, 1});

    while (!buffer.empty()) {
        const auto ready = WaitReadable(fd, deadline);
        if (!ready) {
            return ready;
        }

        const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read < 0) {
            return CheckSysCall(-1, "read probe");
        }
        if (bytes_read == 0) {
            return testing::AssertionFailure()
                   << "probe connection closed before the full record was received";
        }

        // Continue after the received bytes because socket reads can be partial.
        buffer = buffer.subspan(static_cast<std::size_t>(bytes_read));
    }

    return testing::AssertionSuccess();
}

}  // namespace

// Creates a private control socket independent of the standard streams under test.
void ProcessProbe::Listen(const std::string& path) {
    listener_.Reset(socket(AF_UNIX, SOCK_STREAM, 0));
    ASSERT_TRUE(CheckSysCall(listener_.get(), "create probe socket"));

    // Keep the listener out of `detach` and the tested command.
    ASSERT_TRUE(SetCloseOnExec(listener_.get()));

    // Leave space for the terminating zero in `sun_path`.
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::ranges::copy(path, address.sun_path);

    auto* socket_address = reinterpret_cast<struct sockaddr*>(&address);
    const int bind_status = bind(listener_.get(), socket_address, sizeof(address));
    ASSERT_TRUE(CheckSysCall(bind_status, "bind probe socket"));

    const int listen_status = listen(listener_.get(), 1);
    ASSERT_TRUE(CheckSysCall(listen_status, "listen for probe"));
}

// Saves the process ID so cleanup works before the probe reports its state.
void ProcessProbe::ExpectProcess(pid_t pid) {
    pid_ = pid;
}

// Waits for the probe to connect and report its state before the timeout.
testing::AssertionResult ProcessProbe::ReceiveState(ProbeState& state) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;

    // Check the same deadline again if a signal interrupts `accept`.
    while (true) {
        const auto ready = WaitReadable(listener_.get(), deadline);
        if (!ready) {
            return ready;
        }

        const int accepted_fd = accept(listener_.get(), nullptr, nullptr);
        if (accepted_fd < 0 && errno == EINTR) {
            continue;
        }

        // Own the socket before reading so failed reads still allow cleanup.
        peer_.Reset(accepted_fd);
        if (accepted_fd < 0) {
            return CheckSysCall(-1, "accept probe");
        }
        break;
    }

    const auto flags_status = SetCloseOnExec(peer_.get());
    if (!flags_status) {
        return flags_status;
    }

    // The complete record confirms that probe output is ready for assertions.
    const auto read_status = ReadExactly(peer_.get(), state, deadline);
    if (read_status) {
        pid_ = static_cast<pid_t>(state.pid);
    }
    return read_status;
}

// Reads process state and reports communication errors.
ProbeState ProcessProbe::ReadState() {
    ProbeState state = {};
    EXPECT_TRUE(ReceiveState(state));
    return state;
}

// Requires an echoed byte to confirm that the probe still handles requests.
void ProcessProbe::Ping() {
    const char ping = 'p';

    while (true) {
        const ssize_t bytes_sent = send(peer_.get(), &ping, 1, MSG_NOSIGNAL);
        const int error = errno;
        if (bytes_sent < 0 && error == EINTR) {
            continue;
        }
        ASSERT_EQ(bytes_sent, 1) << "cannot send probe ping: " << std::strerror(error);
        break;
    }

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    char reply = 0;
    ASSERT_TRUE(ReadExactly(peer_.get(), reply, deadline));
    EXPECT_EQ(reply, ping);
}

// Releases the connection and uses `SIGKILL` if graceful shutdown fails.
void ProcessProbe::Close() {
    const auto stopped = Stop();
    EXPECT_TRUE(stopped);

    if (!stopped && pid_ > 0) {
        const int status = kill(pid_, SIGKILL);
        if (status < 0 && errno != ESRCH) {
            EXPECT_TRUE(CheckSysCall(status, "kill probe during cleanup"));
        }
    }

    peer_.Reset();
    listener_.Reset();
    pid_ = -1;
}

// Waits for the probe to close its socket before test files can be removed.
testing::AssertionResult ProcessProbe::Stop() {
    // Accept a pending connection if a failed assertion skipped `ReadState`.
    if (pid_ > 0 && peer_.get() < 0) {
        ProbeState state = {};
        const auto reported = ReceiveState(state);
        if (!reported) {
            return reported;
        }
    }

    if (peer_.get() < 0) {
        return testing::AssertionSuccess();
    }

    // Stop sending to release the probe, then read until it closes the socket.
    const int shutdown_status = shutdown(peer_.get(), SHUT_WR);
    if (shutdown_status < 0 && errno != ENOTCONN) {
        return CheckSysCall(shutdown_status, "shut down probe connection");
    }

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    std::array<char, 256> buffer;

    while (true) {
        const auto ready = WaitReadable(peer_.get(), deadline);
        if (!ready) {
            return ready;
        }

        const ssize_t bytes_read = read(peer_.get(), buffer.data(), buffer.size());
        if (bytes_read == 0) {
            return testing::AssertionSuccess();
        }
        if (bytes_read < 0 && errno != EINTR) {
            return CheckSysCall(-1, "read probe during cleanup");
        }
    }
}

}  // namespace detach_test
