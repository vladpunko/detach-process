// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/test_launcher.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tests/test_files.h"

namespace detach_test {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(5);
// Reserve this status for failed test setup, separate from detach exit codes.
constexpr int kLauncherFailure = 125;

// Setup runs in a forked copy of GoogleTest. On failure, exit immediately so
// that copy cannot run the test body or repeat the parent's fixture cleanup.
void RequireChildCall(int result, const char* operation) {
  if (result < 0) {
    perror(operation);
    _exit(kLauncherFailure);
  }
}

// Make the caller a terminal owner first. Otherwise, a detachment test could
// pass merely because there was no controlling terminal to lose.
void PrepareTerminal(const std::string& path) {
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

void ConfigureSignals(const LaunchOptions& options) {
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

void BreakPipes(const std::vector<int>& descriptors) {
  for (int fd : descriptors) {
    int channel[2];
    RequireChildCall(pipe(channel), "create broken pipe");
    close(channel[0]);
    RequireChildCall(dup2(channel[1], fd), "redirect broken pipe");
    close(channel[1]);
  }
}

// All special caller state is installed in the launcher, never in GoogleTest.
void PrepareChild(const LaunchOptions& options, int report_fd, int error_fd) {
  RequireChildCall(dup2(report_fd, STDOUT_FILENO), "capture launcher report");
  RequireChildCall(dup2(error_fd, STDERR_FILENO), "capture launcher errors");
  PrepareTerminal(options.terminal);
  ConfigureSignals(options);
  BreakPipes(options.broken_pipes);

  for (const auto& variable : options.environment) {
    RequireChildCall(setenv(variable.first.c_str(), variable.second.c_str(), 1),
                     "set environment");
  }
  if (options.file_creation_mask >= 0) {
    umask(static_cast<mode_t>(options.file_creation_mask));
  }
  if (options.descriptor_limit != 0) {
    struct rlimit limit;
    RequireChildCall(getrlimit(RLIMIT_NOFILE, &limit), "get descriptor limit");
    limit.rlim_cur = options.descriptor_limit;
    RequireChildCall(setrlimit(RLIMIT_NOFILE, &limit), "set descriptor limit");
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
             errno == EINTR) {
      }
      return testing::AssertionFailure()
             << "detach did not return before the deadline";
    }
    // A short pause avoids spinning on waitpid(WNOHANG). Readiness still comes
    // from waitpid itself, rather than an assumed startup delay.
    poll(nullptr, 0, 5);
  }
  if (WIFEXITED(result.status)) {
    result.exit_code = WEXITSTATUS(result.status);
  }
  return testing::AssertionSuccess();
}

// Use the utility's public report, including its stderr fallback when stdout
// cannot be written. Tests then inspect the same PID and paths a user receives.
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

LaunchResult RunDetach(const std::vector<std::string>& arguments,
                       const std::string& directory,
                       const LaunchOptions& options) {
  std::string executable = options.executable;
  if (executable.empty()) {
    const char* selected = getenv("DETACH_TEST_EXECUTABLE");
    executable = selected == nullptr ? DETACH_PATH : selected;
  }
  const std::string report_path = directory + "/launcher.out";
  const std::string error_path = directory + "/launcher.err";
  LaunchResult result;
  // Regular files let us collect the report after the launcher exits without
  // waiting for EOF from its descendants. CLOEXEC keeps capture handles out of
  // the command being tested. Its actual stdout and stderr use separate files.
  Descriptor report(open(report_path.c_str(),
                         O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600));
  EXPECT_TRUE(CheckCall(report.get(), "open report"));
  if (report.get() < 0) {
    return result;
  }
  Descriptor errors(
      open(error_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600));
  EXPECT_TRUE(CheckCall(errors.get(), "open diagnostics"));
  if (errors.get() < 0) {
    return result;
  }

  // Own mutable argv storage before fork and keep it stable until exec.
  std::vector<std::string> command = {executable};
  command.insert(command.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& argument : command) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  result.launcher_pid = fork();
  EXPECT_TRUE(CheckCall(result.launcher_pid, "fork test launcher"));
  if (result.launcher_pid < 0) {
    return result;
  }
  if (result.launcher_pid == 0) {
    PrepareChild(options, report.get(), errors.get());
    if (chdir(directory.c_str()) < 0) {
      perror("test working directory");
      _exit(kLauncherFailure);
    }
    execv(executable.c_str(), argv.data());
    perror("exec detach");
    _exit(kLauncherFailure);
  }

  // The launcher has finished flushing its own report. The probe socket will
  // separately tell the test when the detached command has finished its I/O.
  EXPECT_TRUE(WaitForLauncher(result));
  result.output = ReadFile(report_path);
  result.error = ReadFile(error_path);
  ParseReport(result.output, result);
  ParseReport(result.error, result);
  EXPECT_NE(result.exit_code, kLauncherFailure) << result.error;
  return result;
}

}  // namespace detach_test
