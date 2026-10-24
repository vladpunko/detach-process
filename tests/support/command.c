// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "tests/support/probe_state.h"

// Reports the operation before another call can overwrite the system error.
static int report_system_error(const char* operation) {
    const int error = errno;
    fprintf(stderr, "%s: %s\n", operation, strerror(error));
    return 0;
}

// Completes a binary write so partial transfers cannot corrupt probe data.
static int write_all(int fd, const void* buffer, size_t size) {
    const char* data = buffer;

    while (size > 0) {
        ssize_t length = write(fd, data, size);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            return report_system_error("cannot write command data");
        }
        if (length == 0) {
            fputs("cannot write command data: no bytes were written\n", stderr);
            return 0;
        }

        data += length;
        size -= (size_t)length;
    }

    return 1;
}

// Sends a complete text marker for deterministic stream assertions.
static int write_text(int fd, const char* text) {
    return write_all(fd, text, strlen(text));
}

// Copies input byte-for-byte and emits an error-stream marker to verify
// that all three command streams were redirected independently.
static int copy_stdin(void) {
    char buffer[4096];

    while (1) {
        ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            return report_system_error("cannot read command input");
        }
        if (length == 0) {
            return write_text(STDERR_FILENO, "probe stderr\n");
        }
        if (!write_all(STDOUT_FILENO, buffer, (size_t)length)) {
            return 0;
        }
    }
}

// Identifies `/dev/null` by file identity rather than by an assumed path.
static int is_null_input(void) {
    struct stat input;
    struct stat null_device;
    if (fstat(STDIN_FILENO, &input) < 0) {
        report_system_error("cannot inspect command input");
        return -1;
    }
    if (stat("/dev/null", &null_device) < 0) {
        report_system_error("cannot inspect /dev/null");
        return -1;
    }

    return input.st_dev == null_device.st_dev && input.st_ino == null_device.st_ino;
}

// Counts inherited handles outside the probe channel and standard streams
// so tests can detect descriptor leaks in the launched command.
static int extra_descriptors(int channel) {
    DIR* directory = opendir("/dev/fd");
    if (directory == NULL) {
        report_system_error("cannot list open descriptors");
        return -1;
    }

    int count = 0;
    while (1) {
        // Only a clean end of directory proves there were no extra handles.
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                report_system_error("cannot read descriptor directory");
                count = -1;
            }
            break;
        }

        char* end;
        errno = 0;
        intmax_t fd = strtoimax(entry->d_name, &end, 10);
        if (errno == 0 && end != entry->d_name && *end == '\0' && fd > STDERR_FILENO
            && fd != channel && fd != dirfd(directory)) {
            count += 1;
        }
    }

    if (closedir(directory) < 0) {
        report_system_error("cannot close descriptor directory");
        return -1;
    }

    return count;
}

// Reads a signal disposition so tests can verify the caller state survived.
static int signal_ignored(int signal_number) {
    struct sigaction action;
    if (sigaction(signal_number, NULL, &action) < 0) {
        report_system_error("cannot read signal disposition");
        return -1;
    }

    return action.sa_handler == SIG_IGN;
}

// Checks the inherited descriptor before the control socket can reuse its number.
static int check_descriptor(const char* number) {
    char* end;
    errno = 0;
    const intmax_t fd = strtoimax(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || fd < 0 || fd > INT_MAX) {
        fprintf(stderr, "invalid descriptor: %s\n", number);
        return -1;
    }

    while (1) {
        if (fcntl((int)fd, F_GETFD) >= 0) {
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EBADF) {
            return 0;
        }
        report_system_error("cannot check inherited descriptor");
        return -1;
    }
}

// Reads the PID file before reporting readiness so tests see completed output.
static int read_pid_file(const char* path, int64_t* pid) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return report_system_error("cannot open pid file");
    }

    // Reject oversized PID files rather than accepting a truncated number.
    char text[64] = {0};
    const size_t length = fread(text, 1, sizeof(text) - 1, file);
    if (ferror(file)) {
        report_system_error("cannot read pid file");
        fclose(file);
        return 0;
    }

    if (fclose(file) == EOF) {
        return report_system_error("cannot close pid file");
    }

    char* end;
    errno = 0;
    const intmax_t value = strtoimax(text, &end, 10);
    const int invalid_number
        = errno != 0 || end == text || value <= 0 || value > INT64_MAX;

    while (end < text + length && isspace((unsigned char)*end)) {
        end += 1;
    }
    if (invalid_number || end != text + length || length == sizeof(text) - 1) {
        fprintf(stderr, "invalid pid file: %s\n", path);
        return 0;
    }

    *pid = (int64_t)value;
    return 1;
}

// Rejects missing or extra arguments before a mode reads its required value.
static int require_mode_argument(int argc, const char* mode) {
    if (argc == 4) {
        return 1;
    }

    fprintf(stderr, "expected one argument for mode: %s\n", mode);
    return 0;
}

// Prints each argument with its length to preserve empty values and newlines.
static int print_arguments(int argc, char* argv[]) {
    for (int index = 3; index < argc; index += 1) {
        if (printf("%zu:", strlen(argv[index])) < 0 || fputs(argv[index], stdout) == EOF
            || putchar('\n') == EOF) {
            return report_system_error("cannot write command arguments");
        }
    }

    return 1;
}

// Prints one inherited environment value so tests can check it unchanged.
static int print_environment(const char* name) {
    const char* value = getenv(name);
    if (value == NULL) {
        fprintf(stderr, "environment variable is not set: %s\n", name);
        return 0;
    }

    if (fputs(value, stdout) == EOF) {
        return report_system_error("cannot write environment value");
    }

    return 1;
}

// Prints the current directory so tests can compare it with the caller directory.
static int print_working_directory(void) {
    char* directory = getcwd(NULL, 0);
    if (directory == NULL) {
        return report_system_error("cannot read working directory");
    }

    const int status = fputs(directory, stdout);
    if (status == EOF) {
        report_system_error("cannot write working directory");
    }

    free(directory);
    return status >= 0;
}

// Opens a terminal so state capture can detect accidental terminal ownership.
static int check_terminal(const char* path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return report_system_error("cannot open command terminal");
    }

    if (close(fd) < 0) {
        return report_system_error("cannot close command terminal");
    }

    return 1;
}

// Finishes the requested reads and writes before tests receive the process state.
static int run_mode(int argc, char* argv[], ProbeState* state) {
    const char* mode = argv[1];
    if (strcmp(mode, "stdio") == 0) {
        return copy_stdin();
    }

    if (strcmp(mode, "streams") == 0) {
        return write_text(STDOUT_FILENO, "stdout one\n")
               && write_text(STDERR_FILENO, "stderr one\n")
               && write_text(STDOUT_FILENO, "stdout two\n")
               && write_text(STDERR_FILENO, "stderr two\n");
    }

    if (strcmp(mode, "arguments") == 0) {
        return print_arguments(argc, argv);
    }

    if (strcmp(mode, "environment") == 0) {
        if (!require_mode_argument(argc, mode)) {
            return 0;
        }

        return print_environment(argv[3]);
    }

    if (strcmp(mode, "cwd") == 0) {
        return print_working_directory();
    }

    if (strcmp(mode, "terminal") == 0) {
        if (!require_mode_argument(argc, mode)) {
            return 0;
        }

        return check_terminal(argv[3]);
    }

    if (strcmp(mode, "descriptor") == 0) {
        if (!require_mode_argument(argc, mode)) {
            return 0;
        }

        state->descriptor_open = check_descriptor(argv[3]);
        return state->descriptor_open >= 0;
    }

    if (strcmp(mode, "pid-file") == 0) {
        if (!require_mode_argument(argc, mode)) {
            return 0;
        }

        return read_pid_file(argv[3], &state->pid_file);
    }

    if (strcmp(mode, "state") == 0) {
        return 1;
    }

    fprintf(stderr, "unknown command mode: %s\n", mode);
    return 0;
}

// Opens a private control channel independent of the streams under test.
static int connect_to_test(const char* path) {
    int channel = socket(AF_UNIX, SOCK_STREAM, 0);
    if (channel < 0) {
        return -1;
    }

    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    const size_t path_length = strlen(path);
    if (path_length >= sizeof(address.sun_path)) {
        close(channel);
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(address.sun_path, path, path_length + 1);
    if (connect(channel, (struct sockaddr*)&address, sizeof(address)) < 0) {
        const int error = errno;
        close(channel);
        errno = error;
        return -1;
    }
    return channel;
}

// Captures process state after the command finishes reading and writing.
// Tests use this state to check the launch report against the running process.
static int capture_state(int channel, ProbeState* state) {
    int terminal = open("/dev/tty", O_RDWR | O_NOCTTY);
    int has_terminal = terminal >= 0;
    if (terminal >= 0) {
        close(terminal);
    }

    sigset_t mask;
    if (sigprocmask(SIG_BLOCK, NULL, &mask) < 0) {
        return report_system_error("cannot read blocked signals");
    }

    // Reading `umask` requires changing it briefly, then restoring it.
    mode_t creation_mask = umask(0);
    umask(creation_mask);

    state->pid = getpid();
    state->parent_pid = getppid();
    state->session_id = getsid(0);
    if (state->session_id < 0) {
        return report_system_error("cannot read process session");
    }
    state->process_group = getpgrp();

    state->has_terminal = has_terminal;
    state->extra_descriptors = extra_descriptors(channel);
    state->stdin_is_null = is_null_input();

    state->sigpipe_ignored = signal_ignored(SIGPIPE);
    state->sigchld_ignored = signal_ignored(SIGCHLD);
    state->sighup_ignored = signal_ignored(SIGHUP);
    state->sigpipe_blocked = sigismember(&mask, SIGPIPE);
    state->sigusr1_blocked = sigismember(&mask, SIGUSR1);
    state->creation_mask = creation_mask;

    return state->extra_descriptors >= 0 && state->stdin_is_null >= 0
           && state->sigpipe_ignored >= 0 && state->sigchld_ignored >= 0
           && state->sighup_ignored >= 0;
}

// Keeps the probe alive for assertions and answers liveness checks until
// the fixture releases it.
static int wait_for_test(int channel) {
    char command;
    ssize_t length;

    while ((length = read(channel, &command, 1)) != 0) {
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            return report_system_error("cannot read test request");
        }
        if (!write_all(channel, &command, 1)) {
            return 0;
        }
    }
    return 1;
}

// Runs the selected probe behavior, reports readiness, and waits for cleanup
// with a bounded lifetime if a regression prevents normal shutdown.
int main(int argc, char* argv[]) {
    if (argc < 3) {
        fputs("usage: command mode socket-path [arguments]\n", stderr);
        return EXIT_FAILURE;
    }

    // Bounds the command lifetime if its scenario or control connection stalls.
    alarm(15);
    ProbeState state = {.descriptor_open = -1, .pid_file = -1};
    if (!run_mode(argc, argv, &state)) {
        return EXIT_FAILURE;
    }
    if (fflush(NULL) == EOF) {
        report_system_error("cannot flush command output");
        return EXIT_FAILURE;
    }

    int channel = connect_to_test(argv[2]);
    if (channel < 0) {
        report_system_error("cannot connect to test socket");
        return EXIT_FAILURE;
    }

    // The socket is the readiness barrier, then remains open for liveness.
    int success = capture_state(channel, &state)
                  && write_all(channel, &state, sizeof(state))
                  && wait_for_test(channel);
    close(channel);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
