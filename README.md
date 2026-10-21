# detach-process

[![Build](https://github.com/vladpunko/detach-process/actions/workflows/build.yml/badge.svg)](https://github.com/vladpunko/detach-process/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Close the terminal. Keep the command running.**

`detach` launches a command in a separate session on Linux and macOS, redirects its output to files, and returns control to your shell. Use it for long downloads, scripts, and background jobs that need to survive a closed terminal.

```sh
detach -- your-command arg1 arg2
```

[Installation](#installation) · [Usage](#usage) · [How it works](#how-it-works) · [Development](#development)

![Launching a detached command from the terminal](detach.gif)

## Installation

The utility requires a C99 compiler and an accessible `/dev/fd` directory. On Linux, `/dev/fd` is usually provided through `/proc/self/fd`.

### Build from source

With Make and CMake 3.16+ installed:

```sh
git clone --depth=1 https://github.com/vladpunko/detach-process.git
cd detach-process
make build
./build/release/detach --help
```

To install the binary and manual page system-wide:

```sh
sudo mkdir -p /usr/local/bin /usr/local/share/man/man1
sudo install -m 755 build/release/detach /usr/local/bin/detach
sudo install -m 644 detach.1 /usr/local/share/man/man1/detach.1
```

<details>
<summary>Build directly with a C compiler</summary>

The utility is a single C source file. Make, CMake, and the test dependencies are optional for this build:

```sh
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter detach.c -o detach
./detach --help
```

Use `detach` instead of `build/release/detach` as the source path in the installation command above.

</details>

<details>
<summary>Install with the provided script</summary>

The installer downloads the source, builds it, and installs the binary and manual page under `/usr/local`. It requires `curl`, `sudo`, `git`, `gcc`, and `install`.

```sh
curl -fsSL https://raw.githubusercontent.com/vladpunko/detach-process/master/install.sh -o /tmp/install-detach.sh
sudo sh /tmp/install-detach.sh
```

</details>

To uninstall:

```sh
sudo rm -f /usr/local/bin/detach /usr/local/share/man/man1/detach.1
```

## Usage

```text
detach [OPTIONS] -- program [ARGS...]
```

`detach` prints the launched command's PID and the paths of its output files, then returns to the shell:

```sh
detach -- sleep 3600
```

Example output (the PID and file names vary):

```text
PID: 12345
stdout: /tmp/stdout.aB3xYz
stderr: /tmp/stderr.cD4wUv
```

Use the printed paths to inspect output with `cat` or follow it with `tail -f`. The log files remain after the command exits; remove them when you no longer need them.

### Options

| Option | Purpose | Default |
| --- | --- | --- |
| `--stdin FILE` | Read standard input from a file | `/dev/null` |
| `--stdout FILE` | Write standard output to a file | A unique `/tmp/stdout.XXXXXX` file |
| `--stderr FILE` | Write standard error to a file | A unique `/tmp/stderr.XXXXXX` file |
| `--pid FILE` | Write the command's PID to a file before it starts | No PID file |
| `--help` | Show help and exit | — |
| `--` | Separate detach options from the command and its arguments | Required when file options are used |

### Examples

**Choose log and PID files:**

```sh
detach --pid job.pid --stdout job.out --stderr job.err -- /bin/sh job.sh
tail -f job.out job.err
```

**Combine standard output and standard error:**

```sh
detach --stdout job.log --stderr job.log -- /bin/sh job.sh
```

**Read from a file:**

```sh
detach --stdin input.txt --stdout output.txt -- sort
```

**Run a pipeline or another shell expression:**

```sh
detach -- /bin/sh -c 'printf "hello\n" | tr a-z A-Z'
```

`detach` passes arguments to the program directly. Use an explicit shell for pipelines, redirections, and other shell syntax. Executable scripts should include a shebang such as `#!/bin/sh`, or be passed to their interpreter as in the examples above.

### Files and exit status

- Existing regular output and PID files are **truncated**, so each launch starts with fresh files. There is no append option.
- New output and PID files are created with permissions `0600`, subject to the caller's umask. Existing files keep their permissions.
- Standard output and standard error may share a file. A regular input file must differ from both outputs, and the PID file must differ from all three streams. These checks include hard links and symlinks and happen before truncation.
- The command inherits the current working directory and environment. Relative paths resolve from that directory.
- Exit code `0` means no startup error was reported; a nonzero code means an error occurred. The launcher's exit code does not report the command's eventual result.

On a reported startup failure, diagnostics are printed to the terminal or directed to the selected stderr file. A reporting error can return failure after the command has already started. An abrupt termination before `exec` can also close the startup channel without reporting an error.

For the full command reference, run `detach --help` or `man detach` after installation.

## How it works

1. Open the input, output, and optional PID files, then check for conflicting file identities.
2. Fork, call `setsid()` to create a new session, and fork again so the command is not the session leader and has no controlling terminal.
3. Coordinate startup over a local socket and write the PID file, if requested.
4. Redirect standard streams, close inherited file descriptors, and execute the command. Report launch details to the original shell.

![Process creation and startup coordination](detach-workflow.png)

`detach` performs a single launch. Automatic restarts, log rotation, and starting jobs at boot require a service manager or other tooling.

## Development

| Command | Result | Build directory |
| --- | --- | --- |
| `make build` | Build the standalone utility in Release mode | `build/release` |
| `make test` | Build and run the integration and failure tests | `build/testing` |
| `make coverage` | Run tests, print a coverage report, and require 100% coverage | `build/coverage` |

Each command configures and builds what it needs. Running `make` without a target is equivalent to `make build`. Set `JOBS` to change build parallelism, for example `make build JOBS=4`.

Tests require CMake 3.16+ and a C++20 compiler. CMake uses an installed GoogleTest package or downloads it if needed. The standalone utility does not need GoogleTest or C++.

### Coverage

Requires Clang and matching `llvm-profdata` and `llvm-cov` tools. On macOS, CMake also looks for tools through `xcrun`.

```sh
make coverage
```

Each run clears old profiles, runs the tests against the instrumented executable, and checks **100% line, function, and branch coverage** of `detach.c`. A failed test, tool command, or coverage check fails the target. The merged profile and LCOV report are saved to `build/coverage/coverage.profdata` and `build/coverage/coverage.lcov`.

<details>
<summary>Use CMake directly or select LLVM tools</summary>

```sh
cmake -S . -B build/coverage -DBUILD_TESTING=ON -DENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/coverage --target coverage --parallel 2
```

If the LLVM tools are outside `PATH`, pass `-DLLVM_PROFDATA=/path/to/llvm-profdata` and `-DLLVM_COV=/path/to/llvm-cov` when configuring CMake, or through `make coverage CMAKE_FLAGS="..."`.

</details>

Code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Format with clang-format 23.1.3 and the repository configuration. CI builds and tests on Linux and macOS, checks coverage on macOS, and runs clang-format, cpplint, and the hooks in [`.githooks.yml`](.githooks.yml).

## License

[MIT](LICENSE) · Vladislav Punko
