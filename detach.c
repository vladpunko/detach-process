// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2021-07-04

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Boolean macros for readability in this file.
#define TRUE 1
#define FALSE 0

#define OPTION_SEPARATOR "--"
#define OPTION_EQUALS_STR "="
#define STRING_TERMINATOR '\0'

#define DEV_NULL_PATH "/dev/null"
#define DEV_FD_PATH "/dev/fd"

static const struct option long_options[] = {
    {"help", no_argument, NULL, 'h'},
    {"stdin", required_argument, NULL, 'i'},
    {"stdout", required_argument, NULL, 'o'},
    {"stderr", required_argument, NULL, 'e'},
    {"pid", required_argument, NULL, 'p'},
    {NULL, 0, NULL, 0},
};

static void print_usage(FILE *stream) {
    const char *message =
        "Usage: detach [OPTIONS] -- program [ARGS...]\n"
        "\n"
        "Options:\n"
        "  --help         show this help message and exit\n"
        "  --stdin FILE   read the standard input from FILE\n"
        "  --stdout FILE  write the standard output to FILE\n"
        "  --stderr FILE  write the standard error to FILE\n"
        "  --pid FILE     write the child process ID to FILE\n"
        "\n"
        "Notes:\n"
        "  Use %s to separate detach options from the program arguments.\n"
        "  This is required when options are used.\n";
    fprintf(stream, message, OPTION_SEPARATOR);
}

static int argument_is_empty(const char *argument) {
    if (argument == NULL) {
        return TRUE;
    }

    if (argument[0] == STRING_TERMINATOR) {
        return TRUE;
    }

    return FALSE;
}

static int is_same_file(const struct stat *first, const struct stat *second) {
    // Device and inode identify a file even when its paths differ.
    return first->st_dev == second->st_dev &&
           first->st_ino == second->st_ino;
}

static int option_has_required_parameter(const char *option) {
    if (option == NULL) {
        return FALSE;
    }

    size_t separator_length = strlen(OPTION_SEPARATOR);

    if (strncmp(option, OPTION_SEPARATOR, separator_length) != 0) {
        return FALSE;
    }

    const char *option_name = option + separator_length;
    size_t option_name_length = strcspn(option_name, OPTION_EQUALS_STR);

    int option_index = 0;
    while (long_options[option_index].name != NULL) {
        const char *name = long_options[option_index].name;

        if (strlen(name) == option_name_length &&
            strncmp(name, option_name, option_name_length) == 0) {
            if (long_options[option_index].has_arg == required_argument) {
                return TRUE;
            }

            return FALSE;
        }
        option_index += 1;
    }

    return FALSE;
}

static int open_missing_standard_descriptors(void) {
    // Keep 0, 1, and 2 occupied so internal files cannot take their place.
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd += 1) {
        if (fcntl(fd, F_GETFD) >= 0) {
            continue;
        }
        if (errno != EBADF) {
            perror("standard descriptors");

            return FALSE;
        }
        if (open(DEV_NULL_PATH, O_RDWR) < 0) {
            perror(DEV_NULL_PATH);

            return FALSE;
        }
    }
    return TRUE;
}

static int close_inherited_descriptors(int startup_fd) {
    // Enumerate open descriptors, including those above a lowered limit.
    DIR *directory = opendir(DEV_FD_PATH);
    if (directory == NULL) {
        perror(DEV_FD_PATH);

        return FALSE;
    }

    while (TRUE) {
        // If readdir returns NULL, errno tells us whether reading failed.
        // Clear it first so an earlier error cannot look like a read failure.
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                perror(DEV_FD_PATH);
                closedir(directory);

                return FALSE;
            }
            break;
        }

        // Descriptor entries have numeric names. Ignore "." and "..".
        // Check the range before converting the number to an int.
        // Preserve stdin, stdout, and stderr, which are already redirected.
        char *end;
        errno = 0;
        intmax_t fd = strtoimax(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name ||
            *end != STRING_TERMINATOR || fd <= STDERR_FILENO || fd > INT_MAX) {
            continue;
        }

        // Keep the directory itself open until enumeration is complete.
        if (fd == dirfd(directory)) {
            continue;
        }
        // Preserve the startup channel until exec closes it automatically.
        if (fd == startup_fd) {
            continue;
        }
        // This function runs only in the child. Closing its copies does
        // not close the corresponding descriptors in the parent process.
        if (close((int)fd) < 0) {
            perror("close inherited descriptor");
            closedir(directory);

            return FALSE;
        }
    }

    // Close the directory and its file descriptor after reading all entries.
    if (closedir(directory) < 0) {
        perror(DEV_FD_PATH);

        return FALSE;
    }
    return TRUE;
}

static int report_startup_failure(int startup_fd) {
    // Notify the parent before exiting after a setup or exec failure.
    char failed = TRUE;
    ssize_t written;
    do {
        written = write(startup_fd, &failed, sizeof(failed));
    } while (written < 0 && errno == EINTR);

    return EXIT_FAILURE;
}

int main(int argc, char *argv[]) {
    const char *stdin_path = DEV_NULL_PATH;
    const char *stdout_path = NULL;
    const char *stderr_path = NULL;
    const char *pid_path = NULL;

    int opt;
    int options_present = FALSE;
    int separator_present = FALSE;
    int next_argument = optind;

    // Suppress getopt output so we print our own errors.
    opterr = FALSE;
    while ((opt = getopt_long(argc, argv, "+", long_options, NULL)) != -1) {
        switch (opt) {
        case 'i': {
            stdin_path = optarg;

            if (argument_is_empty(optarg)) {
                fputs("Missing an argument for the option: --stdin\n",
                      stderr);
                print_usage(stderr);

                return EXIT_FAILURE;
            }

            break;
        }

        case 'o': {
            stdout_path = optarg;

            if (argument_is_empty(optarg)) {
                fputs("Missing an argument for the option: --stdout\n",
                      stderr);
                print_usage(stderr);

                return EXIT_FAILURE;
            }

            break;
        }

        case 'e': {
            stderr_path = optarg;

            if (argument_is_empty(optarg)) {
                fputs("Missing an argument for the option: --stderr\n",
                      stderr);
                print_usage(stderr);

                return EXIT_FAILURE;
            }

            break;
        }

        case 'p': {
            pid_path = optarg;

            if (argument_is_empty(optarg)) {
                fputs("Missing an argument for the option: --pid\n", stderr);
                print_usage(stderr);

                return EXIT_FAILURE;
            }

            break;
        }

        case 'h':
            print_usage(stdout);

            return EXIT_SUCCESS;

        case '?': {
            const char *option_name = NULL;

            if (optind > 0 && argv[optind - 1] != NULL) {
                option_name = argv[optind - 1];
            }

            if (option_has_required_parameter(option_name)) {
                fprintf(stderr, "Missing an argument for the option: %s\n",
                        option_name);
            } else if (option_name != NULL) {
                fprintf(stderr, "Unknown option: %s\n", option_name);
            } else {
                fputs("Unknown option or missing argument.\n", stderr);
            }
            print_usage(stderr);

            return EXIT_FAILURE;
        }

        default:
            fputs("Unknown option.\n", stderr);
            print_usage(stderr);

            return EXIT_FAILURE;
        }

        // Track option usage for the required separator rule.
        if (opt == 'i' || opt == 'o' || opt == 'e' || opt == 'p') {
            options_present = TRUE;
        }
        next_argument = optind;
    }

    if (optind > next_argument) {
        if (strcmp(argv[next_argument], OPTION_SEPARATOR) == 0) {
            separator_present = TRUE;
        }
    }

    if (options_present && !separator_present) {
        const char *message =
            "Missing %s separator between detach options and the command.\n";
        fprintf(stderr, message, OPTION_SEPARATOR);
        print_usage(stderr);

        return EXIT_FAILURE;
    }

    if (optind >= argc) {
        fputs("Missing command to detach.\n", stderr);
        print_usage(stderr);

        return EXIT_FAILURE;
    }

    if (!open_missing_standard_descriptors()) {
        return EXIT_FAILURE;
    }

    // Open stdin now so the PID check uses the file the child will read.
    int fd_stdin = open(stdin_path, O_RDONLY);
    if (fd_stdin < 0) {
        perror("stdin");

        return EXIT_FAILURE;
    }

    // Keep existing contents until the PID file has been checked.
    // New stdout.
    char stdout_template[] = "/tmp/stdout.XXXXXX";
    int fd_stdout;
    if (stdout_path == NULL) {
        fd_stdout = mkstemp(stdout_template);
        if (fd_stdout < 0) {
            perror("stdout");

            return EXIT_FAILURE;
        }
        stdout_path = stdout_template;
    } else {
        fd_stdout = open(stdout_path, O_CREAT | O_WRONLY, 0600);
        if (fd_stdout < 0) {
            perror("stdout");

            return EXIT_FAILURE;
        }
    }

    // New stderr.
    char stderr_template[] = "/tmp/stderr.XXXXXX";
    int fd_stderr;
    if (stderr_path == NULL) {
        fd_stderr = mkstemp(stderr_template);
        if (fd_stderr < 0) {
            perror("stderr");

            return EXIT_FAILURE;
        }
        stderr_path = stderr_template;
    } else {
        fd_stderr = open(stderr_path, O_CREAT | O_WRONLY, 0600);
        if (fd_stderr < 0) {
            perror("stderr");

            return EXIT_FAILURE;
        }
    }

    struct stat stdout_stat;
    if (fstat(fd_stdout, &stdout_stat) < 0) {
        perror("stdout");

        return EXIT_FAILURE;
    }

    struct stat stderr_stat;
    if (fstat(fd_stderr, &stderr_stat) < 0) {
        perror("stderr");

        return EXIT_FAILURE;
    }

    int fd_pid = -1;
    struct stat pid_stat;
    if (pid_path != NULL) {
        fd_pid = open(pid_path, O_CREAT | O_WRONLY, 0600);
        if (fd_pid < 0) {
            perror("pid");

            return EXIT_FAILURE;
        }
        if (fstat(fd_pid, &pid_stat) < 0) {
            perror("pid");

            return EXIT_FAILURE;
        }

        struct stat stdin_stat;
        if (fstat(fd_stdin, &stdin_stat) < 0) {
            perror("stdin");

            return EXIT_FAILURE;
        }

        // Reject aliases before truncating or writing any of these files.
        if (is_same_file(&pid_stat, &stdin_stat) ||
            is_same_file(&pid_stat, &stdout_stat) ||
            is_same_file(&pid_stat, &stderr_stat)) {
            fputs("PID file must differ from stdin, stdout, and stderr.\n",
                  stderr);

            return EXIT_FAILURE;
        }
    }

    // Truncate regular output files only after checking for PID conflicts.
    // Devices and pipes do not need truncation.
    if (S_ISREG(stdout_stat.st_mode) && ftruncate(fd_stdout, 0) < 0) {
        perror("stdout");

        return EXIT_FAILURE;
    }
    if (S_ISREG(stderr_stat.st_mode) && ftruncate(fd_stderr, 0) < 0) {
        perror("stderr");

        return EXIT_FAILURE;
    }

    // Share the file offset when both streams refer to the same file.
    if (is_same_file(&stdout_stat, &stderr_stat)) {
        if (dup2(fd_stdout, fd_stderr) < 0) {
            perror("stderr");

            return EXIT_FAILURE;
        }
    }

    // The parent grants startup permission after saving the PID.
    // The child uses the same channel to report startup failures.
    int startup_channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, startup_channel) < 0) {
        perror("startup channel");

        return EXIT_FAILURE;
    }

    // Successful exec closes the child's endpoint and wakes the parent.
    if (fcntl(startup_channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        perror("startup channel");

        return EXIT_FAILURE;
    }

    // Create a new process.
    pid_t pid = fork();
    if (pid > 0) {
        close(fd_stdin);
        close(startup_channel[1]);

        if (pid_path != NULL) {
            // Reuse the checked descriptor instead of reopening its path.
            if (S_ISREG(pid_stat.st_mode) && ftruncate(fd_pid, 0) < 0) {
                perror("pid");

                return EXIT_FAILURE;
            }
            if (dprintf(fd_pid, "%jd\n", (intmax_t)pid) < 0) {
                perror("pid");
                close(fd_pid);

                return EXIT_FAILURE;
            }
            if (close(fd_pid) < 0) {
                perror("pid");

                return EXIT_FAILURE;
            }

            // Permit startup only after writing and closing the PID file.
            // On failure, process exit closes the channel without permission.
            char ready = TRUE;
            ssize_t written;
            do {
                written = write(startup_channel[0], &ready, sizeof(ready));
            } while (written < 0 && errno == EINTR);
            if (written != sizeof(ready)) {
                perror("startup channel");

                return EXIT_FAILURE;
            }
        }

        // Wait for exec to close the channel or for a failure byte.
        char failed;
        ssize_t received;
        do {
            received = read(startup_channel[0], &failed, sizeof(failed));
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            perror("startup channel");

            return EXIT_FAILURE;
        }
        close(startup_channel[0]);
        if (received > 0) {
            fprintf(stderr, "Failed to start the command. Check stderr: %s\n",
                    stderr_path);

            return EXIT_FAILURE;
        }

        printf("PID: %jd\n", (intmax_t)pid);
        printf("stdout: %s\n", stdout_path);
        printf("stderr: %s\n", stderr_path);

        close(fd_stdout);
        close(fd_stderr);

        return EXIT_SUCCESS;
    } else if (pid < 0) {
        perror("fork");

        return EXIT_FAILURE;
    }

    close(startup_channel[0]);

    if (pid_path != NULL) {
        close(fd_pid);

        // EOF means the parent failed before granting startup permission.
        char ready;
        ssize_t received;
        do {
            received = read(startup_channel[1], &ready, sizeof(ready));
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            perror("startup channel");

            return report_startup_failure(startup_channel[1]);
        }
        if (received != sizeof(ready) || ready != TRUE) {
            return EXIT_FAILURE;
        }
    }

    // Create a new session.
    if (setsid() < 0) {
        perror("setsid");

        return report_startup_failure(startup_channel[1]);
    }

    // Redirect stdin.
    if (dup2(fd_stdin, STDIN_FILENO) < 0) {
        perror("stdin");

        return report_startup_failure(startup_channel[1]);
    }
    if (fd_stdin > STDIN_FILENO) {
        close(fd_stdin);
    }

    // Redirect stdout.
    if (dup2(fd_stdout, STDOUT_FILENO) < 0) {
        perror("stdout");

        return report_startup_failure(startup_channel[1]);
    }
    if (fd_stdout > STDOUT_FILENO) {
        close(fd_stdout);
    }

    // Redirect stderr.
    if (dup2(fd_stderr, STDERR_FILENO) < 0) {
        perror("stderr");

        return report_startup_failure(startup_channel[1]);
    }
    if (fd_stderr > STDERR_FILENO) {
        close(fd_stderr);
    }

    // Close inherited files, pipes, and sockets before starting the program.
    if (!close_inherited_descriptors(startup_channel[1])) {
        return report_startup_failure(startup_channel[1]);
    }

    // Start a program in a new session on the current operating system.
    execvp(argv[optind], &argv[optind]);
    perror("exec");

    return report_startup_failure(startup_channel[1]);
}
