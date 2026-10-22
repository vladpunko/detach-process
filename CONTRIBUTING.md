# Contributing

Build the utility and run the checks from the repository root:

```sh
make build
make test
make coverage
```

## Requirements

- Build: a C99 compiler, Make, and CMake 3.16+.
- Tests: a C++20 compiler. CMake uses an installed GoogleTest package or downloads GoogleTest 1.17.0 when needed.
- Coverage: Clang and matching `llvm-profdata` and `llvm-cov`. CMake looks beside the compiler and on `PATH`; on macOS it also checks the Xcode toolchain through `xcrun`. Set `LLVM_PROFDATA` and `LLVM_COV` through `CMAKE_FLAGS` if necessary.

The runtime needs `/dev/fd` and permission to create local Unix sockets. Tests also create pseudoterminals to check behavior after a real terminal closes.

## Build and test commands

| Command | Result |
| --- | --- |
| `make build` | Release binary at `build/release/detach`. |
| `make test` | Build and run the suite in `build/testing`. |
| `make test TEST_FILTER=OutputTest` | Run tests whose names match this regex. |
| `make coverage` | Build, test, report, and check coverage in `build/coverage`. |

`TEST_FILTER` selects tests by their CTest names. For example:

```sh
make test TEST_FILTER='ArgumentsTest|UnknownOptionTest'
make test TEST_FILTER=TerminalTest
```

Set `JOBS=4` to change build parallelism and the parallelism of `make test`. Build directories can be changed with `BUILD_DIR`, `TEST_BUILD_DIR`, and `COVERAGE_BUILD_DIR`. Additional CMake options go in `CMAKE_FLAGS`.

Coverage always runs the complete suite, regardless of `TEST_FILTER`. Each run removes old profiles and reports, runs tests against the instrumented `detach_faults` executable, and requires 100% line, function, and branch coverage of `detach.c`. Tool and test failures stop the target. Reports are saved as `build/coverage/coverage.profdata` and `build/coverage/coverage.lcov`.

The same coverage target is available directly through CMake:

```sh
cmake -S . -B build/coverage -DBUILD_TESTING=ON -DENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/coverage --target coverage --parallel 2
```

## Test layout

Tests describe observable behavior and run the actual CLI:

| File in `tests/` | Responsibility |
| --- | --- |
| `arguments_test.cc` | Options, separators, and argument preservation. |
| `files_test.cc` | Redirection, permissions, truncation, and invalid paths. |
| `file_aliases_test.cc` | Shared output, input protection, and PID file aliases. |
| `output_test.cc` | Help, launch report layout, and exit status. |
| `process_test.cc` | Sessions, environment, working directory, and descriptors. |
| `signals_test.cc` | Inherited signal dispositions and masks. |
| `terminal_test.cc` | Controlling terminal and terminal closure. |
| `failures_test.cc` | System call failures, retries, diagnostics, and cleanup. |

`DetachTest` in `test_support.h` provides a private directory and starts the [probe](tests/probe.c), a small command used to inspect the launched process. `test_launcher` sets up the caller and captures its report. `probe_client` coordinates readiness and shutdown over a socket. `test_files` owns file descriptors and provides file assertions.

`FaultTest` in `fault_test.h` adds fault plans, recording, and cleanup to the common fixture. It checks the exact injections that occurred after the probe has stopped. This keeps `failures_test.cc` focused on scenarios and expected behavior. Coverage configuration lives in `tests/coverage.cmake`; `tests/run_coverage.cmake` runs the checks and produces the report.

For a probe test, keep setup, execution, and assertions visible:

```cpp
TEST_F(ProcessTest, LauncherReturnsWhileTheCommandIsStillRunning) {
  const auto result = RunProbe();

  ASSERT_EQ(result.exit_code, 0) << result.error;
  ReadProbe();
  PingProbe();
}
```

- Give each test a behavioral name. Use named cases for a table of similar inputs so a failed run identifies the scenario rather than a numeric index.
- Call `ReadProbe()` before inspecting the command's output. It waits until the probe has finished its I/O. Use `PingProbe()` to prove it remains responsive.
- Use the fixture's `Path()` for files. Cleanup must work after a failed assertion.
- Synchronize through the probe socket or a bounded wait; avoid startup sleeps.
- Keep rare syscall failures in `failures_test.cc`. Only the test executable in `faults.c` injects failures; the production binary uses the real system calls.
- Check the public report against the running probe's PID and known paths. A permissive report parser alone cannot catch a changed output format.

## Style and CI

Keep functions focused on one stage of startup. Prefer explicit state and early error returns. Document what each function does and why it is needed; argument-by-argument descriptions are unnecessary. Use inline comments for ordering, ownership, or POSIX behavior that is not evident from the code. Keep the single-file C99 build working. When changing startup operations, review the fault-injection scenarios as well as the normal command paths.

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and the repository's `.clang-format` and `CPPLINT.cfg` files. With clang-format 23.1.3 and cpplint installed:

```sh
clang-format --dry-run --Werror detach.c tests/*.c tests/*.cc tests/*.h
cpplint detach.c tests/*.c tests/*.cc tests/*.h
python3 -m pre_commit run --all-files --config .githooks.yml
```

CI compiles the standalone C99 source, runs `make build` and `make test` on Linux and macOS, and runs `make coverage` on macOS. Keep examples in the README, help output, and [manual page](detach.1) consistent when changing the CLI.
