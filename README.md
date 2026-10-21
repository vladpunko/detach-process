# detach-process

![build](https://github.com/vladpunko/detach-process/actions/workflows/build.yml/badge.svg)

Run a command from your terminal and keep it running after the terminal closes. Works on Linux and macOS.

![usage-example](https://raw.githubusercontent.com/vladpunko/detach-process/master/detach.gif)

## Usage

```bash
detach -- your-command arg1 arg2
```

`detach` prints the command's PID and output paths, then returns to your shell. By default, stdin reads from `/dev/null`, and stdout and stderr go to separate temporary files in `/tmp`.

Choose the files yourself:

```bash
detach --pid app.pid --stdout app.out --stderr app.err -- your-command
detach --stdin input.txt --stdout output.txt -- cat
```

Use `--` to separate detach's options from the command. Run `detach --help` for all options.

Existing regular output and PID files are truncated. The PID file must differ from all streams, and regular stdin must differ from stdout and stderr. These checks include hard links and symlinks.

For portable script execution, include a shebang such as `#!/bin/sh` or pass the interpreter explicitly: `detach -- /bin/sh script.sh`.

## How it works

Two forks and `setsid()` create a separate session without making the command its leader. A local socket coordinates startup. Standard streams are redirected and inherited descriptors are closed before the command runs.

![Process flow](detach-workflow.png)

The exit status describes startup, not the command's eventual result. Abrupt termination before `exec` can close the startup socket without reporting an error. A reporting error can also return failure while the command is already running.

## Installation

Requires a C99 compiler and an accessible `/dev/fd` directory. On Linux, `/dev/fd` is usually provided through `/proc/self/fd`.

The installer also needs `curl`, `sudo`, `git`, `gcc`, and `install`:

```bash
install_script=$(curl -fsSL https://raw.githubusercontent.com/vladpunko/detach-process/master/install.sh) &&
  sudo sh -c "${install_script}"
```

Or build and install from source:

```bash
# Step -- 1.
git clone --depth=1 --branch=master https://github.com/vladpunko/detach-process.git

# Step -- 2.
cd detach-process

# Step -- 3.
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter detach.c -o detach

# Step -- 4.
sudo mkdir -p /usr/local/bin /usr/local/share/man/man1
sudo install -m 755 detach /usr/local/bin/detach
sudo install -m 644 detach.1 /usr/local/share/man/man1/detach.1
```

To uninstall:

```bash
sudo rm -f /usr/local/bin/detach /usr/local/share/man/man1/detach.1
```

## Development

Tests require CMake 3.16+ and a C++20 compiler. CMake uses an installed GoogleTest package or downloads it if needed. The standalone utility does not need these dependencies.

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
(cd build && ctest --output-on-failure --parallel 2)
```

### Coverage

Use Clang and matching LLVM tools. Each run starts with fresh profiles and checks for 100% line, function, and branch coverage in `detach.c`.

```bash
# Step -- 1. Configure.
cmake -S . -B build/coverage \
  -DBUILD_TESTING=ON \
  -DENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug

# Step -- 2. Build.
cmake --build build/coverage --parallel 2

# Step -- 3. Run tests with fresh profiles.
cd build/coverage
cmake -E remove_directory profiles
mkdir -p profiles
LLVM_PROFILE_FILE="${PWD}/profiles/%m-%p.profraw" \
  DETACH_TEST_EXECUTABLE="${PWD}/tests/detach_faults" \
  ctest --output-on-failure --parallel 2

# Step -- 4. Show coverage.
llvm-profdata merge -sparse profiles/*.profraw -o coverage.profdata
llvm-cov report tests/detach_faults -instr-profile=coverage.profdata ../../detach.c

# Step -- 5. Check full coverage.
llvm-cov export tests/detach_faults -instr-profile=coverage.profdata \
  -format=lcov ../../detach.c > coverage.lcov
cmake -DREPORT=coverage.lcov -P ../../tests/check_coverage.cmake
cd ../..
```

On macOS, prefix `llvm-profdata` and `llvm-cov` with `xcrun`.

Code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Format with clang-format 23.1.3 and the repository configuration. CI also runs cpplint and `.githooks.yml` checks.

## License

[MIT](LICENSE)
