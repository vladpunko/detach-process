// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2021-07-04

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Boolean macros for readability in this file.
#define TRUE 1
#define FALSE 0

#define OPTION_SEPARATOR "--"
#define OPTION_EQUALS_STR "="
#define OPTION_EQUALS_CHAR '='
#define STRING_TERMINATOR '\0'

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

static int option_is_consuming_next_argument(const char *option) {
    if (!option_has_required_parameter(option)) {
        return FALSE;
    }

    if (strchr(option, OPTION_EQUALS_CHAR) != NULL) {
        return FALSE;
    }

    return TRUE;
}

int main(int argc, char *argv[]) {
    const char *stdin_path = "/dev/null";
    const char *stdout_path = NULL;
    const char *stderr_path = NULL;
    const char *pid_path = NULL;

    int opt;
    int options_present = FALSE;
    int separator_present = FALSE;

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
    }

    const char *separator_marker = NULL;
    if (optind > 1) {
        separator_marker = argv[optind - 1];
    }

    const char *option_candidate = NULL;
    if (optind > 2) {
        option_candidate = argv[optind - 2];
    }

    if (options_present) {
        separator_present = FALSE;
        // Treat the separator as valid only when it is not an option argument.
        if (separator_marker != NULL &&
            strcmp(separator_marker, OPTION_SEPARATOR) == 0) {
            if (!option_is_consuming_next_argument(option_candidate)) {
                separator_present = TRUE;
            }
        }
    } else {
        separator_present = TRUE;
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
        fd_stdout = open(stdout_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
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
        fd_stderr = open(stderr_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd_stderr < 0) {
            perror("stderr");

            return EXIT_FAILURE;
        }
    }

    // Create a new process.
    pid_t pid = fork();
    if (pid > 0) {
        printf("PID: %d\n", pid);
        printf("stdout: %s\n", stdout_path);
        printf("stderr: %s\n", stderr_path);

        if (pid_path != NULL) {
            int fd_pid = open(pid_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (fd_pid < 0) {
                perror("pid");

                return EXIT_FAILURE;
            }
            dprintf(fd_pid, "%d\n", pid);
            close(fd_pid);
        }

        close(fd_stdout);
        close(fd_stderr);

        return EXIT_SUCCESS;
    } else if (pid < 0) {
        perror("fork");

        return EXIT_FAILURE;
    }

    // Create a new session.
    if (setsid() < 0) {
        perror("setsid");

        return EXIT_FAILURE;
    }

    // New stdin.
    int fd_stdin = open(stdin_path, O_RDONLY);
    if (fd_stdin < 0) {
        perror("stdin");

        return EXIT_FAILURE;
    }
    dup2(fd_stdin, STDIN_FILENO);
    if (fd_stdin > STDIN_FILENO) {
        close(fd_stdin);
    }

    // Redirect stdout.
    dup2(fd_stdout, STDOUT_FILENO);
    if (fd_stdout > STDOUT_FILENO) {
        close(fd_stdout);
    }

    // Redirect stderr.
    dup2(fd_stderr, STDERR_FILENO);
    if (fd_stderr > STDERR_FILENO) {
        close(fd_stderr);
    }

    // Start a program in a new session on the current operating system.
    execvp(argv[optind], &argv[optind]);
    perror("exec");

    return EXIT_FAILURE;
}
