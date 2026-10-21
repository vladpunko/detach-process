# detach-process

![build](https://github.com/vladpunko/detach-process/actions/workflows/build.yml/badge.svg)

Spawn a new process and detach it from the current interactive session.

![usage-example](https://raw.githubusercontent.com/vladpunko/detach-process/master/detach.gif)

## Motivation

Detaching a long-running command usually means juggling `nohup`, backgrounding, `disown`, and redirections. This tool simplifies that workflow into one predictable command and reports the PID and output paths for easy scripting.

## Installation

Requirements: a Unix-like OS with [git](https://git-scm.com/downloads), a C compiler such as [gcc](https://gcc.gnu.org/install), and `install` (from [coreutils](https://www.gnu.org/software/coreutils/)). For the installer you also need [curl](https://curl.se/download.html) and `sudo`.

To install this package as a standalone application with the command-line interface you are to run the following command:

```bash
install_script=$(curl -fsSL https://raw.githubusercontent.com/vladpunko/detach-process/master/install.sh) &&
  sudo sh -c "${install_script}"
```

The install script defaults to `/usr/local/bin` for the binary and `/usr/local/share/man` for the man page.

You can also install this system package on your working machine (works for unix-like operating systems) from source code to a system location:

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

You can write the PID and the standard output and error to specific files:

```bash
detach --pid ping.pid --stdout ping.out --stderr ping.err -- ping -c 10 google.com
```

Use the standard input from a file:

```bash
detach --stdin README.md --stdout cat.out -- cat
```

Show usage:

```bash
detach --help
```

## Contributing

### Tests

The integration suite uses GoogleTest and runs the compiled utility on Linux and macOS. Building tests requires CMake 3.16 or newer and a C++17 compiler. CMake uses an installed GoogleTest package when available, otherwise it downloads version 1.17.0 and verifies the archive checksum.

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

Tests are grouped by behavior:

- `tests/arguments_test.cpp`: options, separators, errors, and argument forwarding.
- `tests/files_test.cpp`: redirections, PID files, permissions, and file aliases.
- `tests/process_test.cpp`: sessions, terminals, signals, inherited descriptors, environment, and startup status.
- `tests/failures_test.cpp`: system-call failures, interrupted I/O, partial transfers, diagnostic errors, and cleanup.

`tests/probe.c` reports the detached command's state over a local socket and stays alive until the test releases it. This lets tests verify that `detach` returns while the command is still running. Terminal tests use a real pseudoterminal and check that closing it leaves the detached command responsive. Each test has its own temporary directory, bounded waits, and cleanup for reported temporary logs.

`tests/faults.c` compiles the unchanged utility into a separate test executable with controlled system calls. Rules select the process, operation, call number, and result. These checks exercise failures that are difficult to reproduce reliably through OS resource limits. The production executable uses the original system calls.

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

Please make sure this system package works without errors.

```bash
# Step -- 1.
python3 -m venv .venv && source ./.venv/bin/activate && pip install cpplint pre-commit

# Step -- 2.
pre-commit install --config .githooks.yml

# Step -- 3.
cpplint detach.c tests/*.c tests/*.cpp tests/*.h
```

## License

[MIT](https://choosealicense.com/licenses/mit)
