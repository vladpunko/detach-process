// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/probe_client.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

#include "tests/probe_state.h"
#include "tests/test_files.h"

namespace detach_test {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr auto kTimeout = std::chrono::seconds(5);

// Waits for probe I/O within one deadline so interrupted polls cannot
// extend a failed test indefinitely.
testing::AssertionResult WaitReadable(int fd, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    // Recalculate after every interruption so signals cannot restart the full
    // timeout. Round a submillisecond remainder up to avoid a busy poll loop.
    const auto remaining = deadline - Clock::now();
    const auto timeout = std::chrono::duration_cast<Milliseconds>(remaining);
    struct pollfd event = {fd, POLLIN, 0};
    const int result =
        poll(&event, 1, std::max(1, static_cast<int>(timeout.count())));
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
    // A stream socket preserves byte order, but one read need not contain the
    // entire record. Continue exactly where this partial transfer stopped.
    next += length;
    size -= static_cast<size_t>(length);
  }
  return testing::AssertionSuccess();
}

}  // namespace

void ProbeClient::Listen(const std::string& path) {
  listener_.Reset(socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_TRUE(CheckCall(listener_.get(), "socket"));
  // The test's listener is not an inherited command descriptor. Hiding it at
  // exec keeps descriptor-leak checks focused on the utility's own behavior.
  ASSERT_TRUE(CheckCall(fcntl(listener_.get(), F_SETFD, FD_CLOEXEC), "fcntl"));
  // Zero initialization also terminates sun_path after the copied name.
  // Check its fixed capacity before copying into the Unix socket address.
  struct sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  ASSERT_LT(path.size(), sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
  auto* socket_address = reinterpret_cast<struct sockaddr*>(&address);
  ASSERT_TRUE(CheckCall(bind(listener_.get(), socket_address, sizeof(address)),
                        "bind"));
  ASSERT_TRUE(CheckCall(listen(listener_.get(), 1), "listen"));
}

void ProbeClient::ExpectProcess(pid_t pid) { pid_ = pid; }

testing::AssertionResult ProbeClient::ReceiveState(ProbeState& state) {
  const auto deadline = Clock::now() + kTimeout;
  const auto ready = WaitReadable(listener_.get(), deadline);
  if (!ready) {
    return ready;
  }
  int accepted;
  do {
    accepted = accept(listener_.get(), nullptr, nullptr);
  } while (accepted < 0 && errno == EINTR);
  // Own the accepted endpoint immediately so a later read failure still
  // closes it during cleanup and releases the helper waiting at the other end.
  peer_.Reset(accepted);
  if (accepted < 0) {
    return CheckCall(-1, "accept probe");
  }
  // The helper connects only after finishing and flushing its selected I/O.
  // Receiving the full state is our readiness barrier for output assertions.
  const auto reported =
      ReadExactly(peer_.get(), deadline, sizeof(state), &state);
  if (!reported) {
    return reported;
  }
  pid_ = static_cast<pid_t>(state.pid);
  return testing::AssertionSuccess();
}

ProbeState ProbeClient::ReadState() {
  ProbeState state = {};
  EXPECT_TRUE(ReceiveState(state));
  return state;
}

void ProbeClient::Ping() {
  // A successful send alone only proves that the socket accepted a byte.
  // Require its echo to prove that the detached helper is still executing.
  const char ping = 'p';
  ssize_t sent;
  do {
    sent = send(peer_.get(), &ping, 1, MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  ASSERT_EQ(sent, 1) << strerror(errno);
  char reply = 0;
  ASSERT_TRUE(ReadExactly(peer_.get(), Clock::now() + kTimeout, 1, &reply));
  EXPECT_EQ(reply, ping);
}

void ProbeClient::Close() {
  const auto stopped = Stop();
  EXPECT_TRUE(stopped);
  // Graceful shutdown has a deadline. If the helper cannot complete it, use
  // the remembered command PID to keep a failing test from leaking a process.
  if (!stopped && pid_ > 0) {
    kill(pid_, SIGKILL);
  }
  peer_.Reset();
  listener_.Reset();
  pid_ = -1;
}

testing::AssertionResult ProbeClient::Stop() {
  // A fatal assertion may skip ReadState after a successful launch. Accept the
  // pending probe here as well so that failure still gets normal cleanup.
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

  // EOF releases the probe even when an assertion stopped the test.
  // Drain until EOF to confirm it finished using the per-test files.
  // waitpid cannot reap this process because detach reparented it.
  // Close only our writing side first. The reading side stays open so we can
  // observe the helper finishing its loop and closing its own socket.
  shutdown(peer_.get(), SHUT_WR);
  const auto deadline = Clock::now() + kTimeout;
  char buffer[256];
  while (true) {
    const auto ready = WaitReadable(peer_.get(), deadline);
    if (!ready) {
      return ready;
    }
    const ssize_t length = read(peer_.get(), buffer, sizeof(buffer));
    if (length == 0) {
      return testing::AssertionSuccess();
    }
    if (length < 0 && errno != EINTR) {
      return CheckCall(-1, "read probe during cleanup");
    }
  }
}

}  // namespace detach_test
