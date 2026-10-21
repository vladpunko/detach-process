// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#include "tests/test_support.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"

#include "tests/probe_state.h"
#include "tests/test_launcher.h"

namespace detach_test {

void DetachTest::SetUp() {
  char directory[] = "/tmp/detach-test.XXXXXX";
  char* created = mkdtemp(directory);
  ASSERT_NE(created, nullptr) << strerror(errno);

  directory_ = created;
  probe_.Listen(SocketPath());
}

void DetachTest::TearDown() {
  // Finish the probe before removing files it may still be using.
  probe_.Close();

  for (const auto& path : temporary_logs_) {
    EXPECT_EQ(unlink(path.c_str()), 0) << path << ": " << strerror(errno);
  }
  if (!directory_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error) << error.message();
  }
}

std::string DetachTest::Path(const std::string& name) const {
  return directory_ + "/" + name;
}

std::string DetachTest::SocketPath() const { return Path("probe.socket"); }

std::vector<std::string> DetachTest::ProbeCommand(
    const std::string& mode, const std::vector<std::string>& arguments) const {
  std::vector<std::string> command = {PROBE_PATH, mode, SocketPath()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return command;
}

LaunchResult DetachTest::Run(const std::vector<std::string>& arguments,
                             const LaunchOptions& options) {
  auto result = RunDetach(arguments, directory_, options);

  // User-selected files belong to the test directory. Only default logs need
  // separate cleanup because detach creates them directly in /tmp.
  for (const auto& path : {result.stdout_path, result.stderr_path}) {
    if (path.rfind("/tmp/stdout.", 0) == 0 ||
        path.rfind("/tmp/stderr.", 0) == 0) {
      temporary_logs_.insert(path);
    }
  }
  return result;
}

LaunchResult DetachTest::RunProbe(const std::vector<std::string>& options,
                                  const std::string& mode,
                                  const std::vector<std::string>& arguments,
                                  const LaunchOptions& launch_options) {
  auto command = options;
  command.push_back("--");

  const auto probe_command = ProbeCommand(mode, arguments);
  command.insert(command.end(), probe_command.begin(), probe_command.end());

  auto result = Run(command, launch_options);
  probe_.ExpectProcess(result.program_pid);
  return result;
}

ProbeState DetachTest::ReadProbe() { return probe_.ReadState(); }

void DetachTest::PingProbe() { probe_.Ping(); }

}  // namespace detach_test
