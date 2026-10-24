// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-22

#include "tests/support/error_fixture.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/support/io.h"

namespace detach_test {
namespace {

constexpr char kEventLogName[] = "events";
constexpr char kAppliedFaultLogName[] = "applied-faults";

// Maps process roles to the names read by the instrumented executable.
const char* RoleName(ProcessRole role) {
    switch (role) {
        case ProcessRole::kLauncher:
            return "launcher";
        case ProcessRole::kIntermediate:
            return "intermediate";
        case ProcessRole::kCommand:
            return "command";
    }

    ADD_FAILURE()
        << "unknown process role: "
        << static_cast<int>(role);
    return "";
}

// Formats a rule once for the launch plan and the verification log.
std::string FormatRule(const FaultRule& rule) {
    if (rule.operation == nullptr || rule.operation[0] == '\0') {
        ADD_FAILURE()
            << "missing fault operation";
        return {};
    }

    const std::string role = RoleName(rule.role);
    if (role.empty()) {
        return {};
    }

    const std::string call_number = std::to_string(rule.call_number);
    const std::string result = std::to_string(rule.result);

    return role + ":" + rule.operation + ":" + call_number + ":" + result;
}

}  // namespace

// Prepares isolated records before the instrumented program can write to them.
void ErrorTest::SetUp() {
    ASSERT_NO_FATAL_FAILURE(SetUpExecutable());
    ASSERT_NO_FATAL_FAILURE(DetachTest::SetUp());

    ASSERT_NO_FATAL_FAILURE(Write(Path(kEventLogName), ""));
    event_log_ready_ = true;

    ASSERT_NO_FATAL_FAILURE(Write(Path(kAppliedFaultLogName), ""));
    fault_log_ready_ = true;
}

// Rejects missing configuration before the base fixture creates test resources.
void ErrorTest::SetUpExecutable() {
    const char* path = getenv("DETACH_TEST_FAULT_EXECUTABLE");
    ASSERT_TRUE(path != nullptr && path[0] != '\0')
        << "set DETACH_TEST_FAULT_EXECUTABLE to the instrumented executable path";

    fault_executable_path_ = path;
}

// Keeps fault verification available after the base fixture removes its directory.
void ErrorTest::TearDown() {
    const auto temporary_logs = TemporaryLogPaths();

    // An open stream keeps the record readable after its path has been removed.
    std::ifstream applied;
    if (fault_log_ready_) {
        applied.open(Path(kAppliedFaultLogName));
        EXPECT_TRUE(applied.is_open())
            << "cannot open applied fault log: "
            << Path(kAppliedFaultLogName);
    }

    DetachTest::TearDown();

    if (applied.is_open()) {
        ExpectAppliedFaults(applied);
    }

    for (const auto& path : temporary_logs) {
        const int status = unlink(path.c_str());
        if (status < 0 && errno != ENOENT) {
            EXPECT_TRUE(CheckSysCall(status, "remove temporary log"))
                << path;
        }
    }
}

// Keeps execution and recording paths together for scenarios without fault rules.
LaunchOptions ErrorTest::FaultOptions() const {
    LaunchOptions options;
    options.executable_path = fault_executable_path_;
    options.environment["DETACH_TEST_EVENTS"] = Path(kEventLogName);
    options.environment["DETACH_TEST_APPLIED_FAULTS"] = Path(kAppliedFaultLogName);
    return options;
}

// Builds the launch plan and remembers the exact records required at teardown.
LaunchOptions ErrorTest::Inject(std::initializer_list<FaultRule> rules) {
    auto options = FaultOptions();
    std::string plan;

    for (const auto& rule : rules) {
        const std::string entry = FormatRule(rule);
        if (entry.empty()) {
            continue;
        }

        if (!plan.empty()) {
            plan += ';';
        }

        plan += entry;
        expected_faults_.insert(entry);
    }

    options.environment["DETACH_TEST_FAULTS"] = plan;
    return options;
}

// Reuses plan construction when only one fault is needed.
LaunchOptions ErrorTest::Inject(const FaultRule& rule) {
    return Inject({rule});
}

// Lists generated logs so cleanup also works after a simulated removal failure.
std::vector<std::string> ErrorTest::TemporaryLogPaths() const {
    std::vector<std::string> paths;
    if (!event_log_ready_) {
        return paths;
    }

    std::istringstream events(Read(Path(kEventLogName)));
    std::string path;
    while (std::getline(events, path)) {
        paths.push_back(path);
    }

    return paths;
}

// Requires every planned fault exactly as many times as it was requested.
void ErrorTest::ExpectAppliedFaults(std::istream& log) const {
    std::multiset<std::string> actual;
    std::string rule;
    while (std::getline(log, rule)) {
        actual.insert(rule);
    }

    EXPECT_TRUE(log.eof() && !log.bad())
        << "cannot read applied fault log";
    EXPECT_EQ(actual, expected_faults_)
        << "applied faults do not match the launch plan";
}

// Checks cleanup against the paths recorded by the instrumented executable.
void ErrorTest::ExpectTemporaryFilesRemoved() const {
    for (const auto& path : TemporaryLogPaths()) {
        EXPECT_FALSE(Exists(path))
            << path;
    }
}

}  // namespace detach_test
