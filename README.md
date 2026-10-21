# detach-process

![build](https://github.com/vladpunko/detach-process/actions/workflows/build.yml/badge.svg)

Spawn a new process and detach it from the current interactive session.

![usage-example](https://raw.githubusercontent.com/vladpunko/detach-process/master/detach.gif)

## Motivation

Detaching a long-running command usually means juggling `nohup`, backgrounding, `disown`, and redirections. This tool simplifies that workflow into one predictable command and reports the PID and output paths for easy scripting.

## Installation

Supported platforms are Linux and macOS. The utility requires an accessible `/dev/fd` directory to enumerate and close inherited descriptors. Minimal Linux containers must provide it, usually through `/proc/self/fd` with procfs mounted.

Building from source requires [git](https://git-scm.com/downloads), a C99 compiler such as GCC or Clang, and `install` (provided by macOS and Linux coreutils). The installer invokes `gcc`. For the installer command below you also need [curl](https://curl.se/download.html) and `sudo`.

To install this package as a standalone application with the command-line interface you are to run the following command:

```bash
install_script=$(curl -fsSL https://raw.githubusercontent.com/vladpunko/detach-process/master/install.sh) &&
  sudo sh -c "${install_script}"
```

The install script defaults to `/usr/local/bin` for the binary and `/usr/local/share/man` for the man page.

You can also build and install from a local checkout:

```bash
# Step -- 1.
git clone --depth=1 --branch=master https://github.com/vladpunko/detach-process.git

# Step -- 2.
cd ./detach-process/

# Step -- 3.
gcc -Wall -Wextra -Werror -Wno-unused-parameter detach.c -o detach

# Step -- 4.
sudo mkdir -p /usr/local/bin /usr/local/share/man/man1
sudo install -m 755 detach /usr/local/bin/detach
sudo install -m 644 detach.1 /usr/local/share/man/man1/detach.1
```

## Uninstall

If you installed to the default paths, remove the binary and the man page:

```bash
sudo rm -f /usr/local/bin/detach /usr/local/share/man/man1/detach.1
```

If you installed to a different location, remove those files instead.

## Basic usage

Using this program allows you to create a new detached process avoiding complex shell pipelines in the current interactive session. It protects you from the trouble of closing an application which is the parent process of a different program.

```bash
detach ping -c 10 google.com  # pgrep ping | xargs ps -p
```

Use `--` to separate detach's options from the command you want to run. This is required when options are used.

The exit status describes startup, not the command's eventual exit status. Setup and exec errors return a nonzero status when reported through the startup channel. An abrupt termination before exec may close that channel without an error report.

Executable scripts should include an interpreter line such as `#!/bin/sh`, or be launched explicitly with their interpreter, for example `detach -- /bin/sh script.sh`. Automatic shell execution of scripts without an interpreter line depends on the host system and is unavailable with musl.

You can write the PID and the standard output and error to specific files:

```bash
detach --pid ping.pid --stdout ping.out --stderr ping.err -- ping -c 10 google.com
```

Use the standard input from a file:

```bash
detach --stdin README.md --stdout cat.out -- cat
```

Regular input files must differ from stdout and stderr files. The PID file must differ from all three streams. These checks include hard links and symbolic links and run before existing file contents are changed. Devices such as `/dev/null` may be shared by the input and output streams.

Show usage:

```bash
detach --help
```

## Contributing

### Tests

The integration suite uses GoogleTest and runs the compiled utility on Linux and macOS. Building tests requires CMake 3.16 or newer and a C++20 compiler. CMake uses an installed GoogleTest package when available, otherwise it downloads version 1.17.0 and verifies the archive checksum.

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

Tests are grouped by behavior:

- `tests/arguments_test.cc`: options, separators, errors, and argument forwarding.
- `tests/files_test.cc`: redirections, PID files, permissions, and file aliases.
- `tests/process_test.cc`: sessions, terminals, signals, inherited descriptors, environment, and startup status.
- `tests/failures_test.cc`: system-call failures, interrupted I/O, partial transfers, diagnostic errors, and cleanup.

Start reading the test infrastructure at `tests/test_support.h`. The fixture prepares a test directory, runs a command and releases its probe during cleanup. Its helpers have separate responsibilities:

| Module | Responsibility |
| --- | --- |
| `tests/test_support.cc` | Test setup, command construction and cleanup of temporary logs. |
| `tests/test_launcher.cc` | Caller environment, process launch, bounded wait and captured reports. |
| `tests/probe_client.cc` | Probe readiness, process state, liveness checks and shutdown. |
| `tests/test_files.cc` | Descriptor ownership and basic file operations. |

A state check starts with `RunProbe()`, then calls `ReadProbe()` before inspecting the process. Output scenarios use `RunProbeMode("streams")` or `RunProbeMode("stdio")`. Launcher settings such as ignored signals are passed as the second argument to `RunProbe`.

`tests/probe.c` reports the detached command's state over a local socket and stays alive until the test releases it. This lets tests verify that `detach` returns while the command is still running. Terminal tests use a real pseudoterminal and check that closing it leaves the detached command responsive. Each test has its own temporary directory, bounded waits, and cleanup for reported temporary logs.

`tests/faults.c` compiles the unchanged utility into a separate test executable with controlled system calls. Rules select the process, operation, call number, and result. Each test checks that every planned injection actually occurred, including its occurrence count. These checks exercise failures that are difficult to reproduce reliably through OS resource limits. The production executable uses the original system calls.

Tests are disabled by default. Building the standalone utility does not require C++, GoogleTest, or a network connection.

### Coverage

Coverage requires Clang and its matching LLVM tools. The coverage run uses the test executable without active faults for the integration checks, then injects faults for the failure checks. Forked children write separate profiles before exec or immediate exit. No lines in `detach.c` are excluded. CI requires 100% of lines, functions, and branches.

```bash
cmake -S . -B build/coverage -DBUILD_TESTING=ON -DENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/coverage --parallel 2
cmake -E remove_directory build/coverage/profiles
mkdir -p build/coverage/profiles
LLVM_PROFILE_FILE="${PWD}/build/coverage/profiles/%m-%p.profraw" \
  DETACH_TEST_EXECUTABLE="${PWD}/build/coverage/tests/detach_faults" \
  ctest --test-dir build/coverage --output-on-failure --parallel 2
llvm-profdata merge -sparse build/coverage/profiles/*.profraw -o build/coverage/coverage.profdata
llvm-cov report build/coverage/tests/detach_faults -instr-profile=build/coverage/coverage.profdata detach.c
llvm-cov export build/coverage/tests/detach_faults -instr-profile=build/coverage/coverage.profdata -format=lcov detach.c > build/coverage/coverage.lcov
cmake -DREPORT=build/coverage/coverage.lcov -P tests/check_coverage.cmake
```

On macOS, prefix the `llvm-profdata` and `llvm-cov` commands with `xcrun`.

### Development

Pull requests are welcome. Please open an issue first to discuss what should be changed.

Code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Use clang-format 23.1.3 with the repository configuration. C++ test helpers report errors through GoogleTest assertions and compile with exceptions disabled. The utility and C helpers retain C naming conventions.

Please make sure this system package works without errors.

```bash
# Step -- 1.
python3 -m venv .venv && source ./.venv/bin/activate && pip install cpplint pre-commit clang-format==23.1.3

# Step -- 2.
pre-commit install --config .githooks.yml

# Step -- 3.
clang-format --dry-run --Werror detach.c tests/*.c tests/*.cc tests/*.h
cpplint detach.c tests/*.c tests/*.cc tests/*.h
```

## License

[MIT](https://choosealicense.com/licenses/mit)
