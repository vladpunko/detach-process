// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2021-07-04

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define OPTION_SEPARATOR "--"
#define DEV_NULL_PATH "/dev/null"
#define DEV_FD_PATH "/dev/fd"
#define STDOUT_PATH_TEMPLATE "/tmp/stdout.XXXXXX"
#define STDERR_PATH_TEMPLATE "/tmp/stderr.XXXXXX"

static const unsigned char ASCII_DELETE = 0x7f;
static const char START_COMMAND = 1;
static const char STARTUP_FAILED = 1;

// Command arguments and selected paths borrow storage from `argv`.
struct options {
    const char* stdin_path;
    const char* stdout_path;
    const char* stderr_path;
    const char* pid_path;
    char** command;
};

// Paths borrow storage from arguments, static defaults or `launch_files`.
struct launch_file {
    const char* path;
    int fd;
    struct stat stats;
    bool is_temporary;
};

struct launch_files {
    struct launch_file input;
    struct launch_file output;
    struct launch_file errors;
    struct launch_file pid;
    char stdout_template[sizeof(STDOUT_PATH_TEMPLATE)];
    char stderr_template[sizeof(STDERR_PATH_TEMPLATE)];
};

struct sigpipe_state {
    struct sigaction original;
    struct sigaction ignored;
};

enum parse_result { PARSE_ERROR, PARSE_HELP, PARSE_COMMAND };

static const struct option long_options[] = {
    {.name = "help", .has_arg = no_argument, .val = 'h'},
    {.name = "stdin", .has_arg = required_argument, .val = 'i'},
    {.name = "stdout", .has_arg = required_argument, .val = 'o'},
    {.name = "stderr", .has_arg = required_argument, .val = 'e'},
    {.name = "pid", .has_arg = required_argument, .val = 'p'},
    {0},
};

// Writes the shared CLI reference used by help and argument diagnostics.
static bool print_usage(FILE* stream) {
    const char message[] =
        "Usage: detach [OPTIONS] -- program [ARGS...]\n"
        "\n"
        "Options:\n"
        "  --help         show this help message and exit\n"
        "  --stdin FILE   read the standard input from FILE\n"
        "  --stdout FILE  write the standard output to FILE\n"
        "  --stderr FILE  write the standard error to FILE\n"
        "  --pid FILE     write the program's process ID to FILE\n"
        "\n"
        "Notes:\n"
        "  Use %s to separate detach options from the program arguments.\n"
        "  This is required when options are used.\n"
        "  Reported paths escape backslashes as \\\\ and control bytes as "
        "\\xHH.\n";
    return fprintf(stream, message, OPTION_SEPARATOR) >= 0;
}

// Recognizes missing or empty values before they can become file paths.
static bool argument_is_empty(const char* argument) {
    return argument == NULL || argument[0] == '\0';
}

// Validates and stores one path option before startup can change any files.
static bool set_file_option(const char** path, const char* value, const char* name) {
    if (argument_is_empty(value)) {
        fprintf(stderr, "Missing an argument for the option: %s\n", name);
        print_usage(stderr);
        return false;
    }

    *path = value;
    return true;
}

// Escapes bytes that could forge report lines or control terminal output.
// Backslashes are doubled so literal escape sequences remain distinguishable.
static bool print_escaped_text(FILE* stream, const char* text) {
    const unsigned char* byte = (const unsigned char*)text;
    for (; *byte != '\0'; byte += 1) {
        int written;
        if (*byte == '\\') {
            written = fputs("\\\\", stream);
        } else if (*byte < ' ' || *byte == ASCII_DELETE) {
            written = fprintf(stream, "\\x%02x", (unsigned int)*byte);
        } else {
            written = fputc(*byte, stream);
        }
        if (written < 0) {
            return false;
        }
    }
    return true;
}

// Validates launcher options before any files are opened or changed.
static enum parse_result parse_arguments(int argc,
                                         char* argv[],
                                         struct options* options) {
    bool options_present = false;
    int option_index = optind;

    // Stop at the command. The colon lets us diagnose missing option values.
    opterr = false;
    int option;
    while ((option = getopt_long(argc, argv, "+:", long_options, NULL)) != -1) {
        switch (option) {
            case 'i':
                if (!set_file_option(&options->stdin_path, optarg, "--stdin")) {
                    return PARSE_ERROR;
                }
                break;
            case 'o':
                if (!set_file_option(&options->stdout_path, optarg, "--stdout")) {
                    return PARSE_ERROR;
                }
                break;
            case 'e':
                if (!set_file_option(&options->stderr_path, optarg, "--stderr")) {
                    return PARSE_ERROR;
                }
                break;
            case 'p':
                if (!set_file_option(&options->pid_path, optarg, "--pid")) {
                    return PARSE_ERROR;
                }
                break;
            case 'h':
                return PARSE_HELP;
            case ':':
                fprintf(stderr, "Missing an argument for the option: %s\n",
                        argv[option_index]);
                print_usage(stderr);
                return PARSE_ERROR;
            case '?':
                // Grouped short options can leave `optind` on the current argument.
                fputs("Unknown option: ", stderr);
                print_escaped_text(stderr, argv[option_index]);
                fputc('\n', stderr);
                print_usage(stderr);
                return PARSE_ERROR;
            default:
                fputs("Unknown option.\n", stderr);
                print_usage(stderr);
                return PARSE_ERROR;
        }
        options_present = true;
        option_index = optind;
    }

    const bool separator_present =
        optind > option_index && strcmp(argv[option_index], OPTION_SEPARATOR) == 0;
    if (options_present && !separator_present) {
        fprintf(stderr,
                "Missing %s separator between detach options and the command.\n",
                OPTION_SEPARATOR);
        print_usage(stderr);
        return PARSE_ERROR;
    }

    if (optind >= argc) {
        fputs("Missing command to detach.\n", stderr);
        print_usage(stderr);
        return PARSE_ERROR;
    }

    options->command = &argv[optind];
    return PARSE_COMMAND;
}

// Prints help without starting a process and checks buffered output failures.
static bool show_help(void) {
    // `fclose` also checks errors from flushing buffered help text.
    if (!print_usage(stdout) || fclose(stdout) == EOF) {
        perror("help");
        return false;
    }

    return true;
}

// Detects aliases by file identity, including hard links and symbolic links.
static bool is_same_file(const struct stat* first, const struct stat* second) {
    return first->st_dev == second->st_dev && first->st_ino == second->st_ino;
}

// Clears regular files for new output while leaving pipes and devices usable.
static bool reset_regular_file(const struct launch_file* file, const char* label) {
    if (!S_ISREG(file->stats.st_mode)) {
        return true;
    }

    if (ftruncate(file->fd, 0) < 0) {
        perror(label);
        return false;
    }

    // Truncation keeps the offset. On macOS, `/dev/fd` can inherit a nonzero one.
    // Rewind explicitly so new output cannot start with a hole of zero bytes.
    if (lseek(file->fd, 0, SEEK_SET) < 0) {
        perror(label);
        return false;
    }

    return true;
}

// Opens a selected log or creates a private temporary one. Existing contents
// are preserved until all file aliases have been checked.
static bool open_log_file(struct launch_file* file,
                          char* temporary_path,
                          const char* label) {
    const bool is_temporary = file->path == NULL;
    if (is_temporary) {
        file->fd = mkstemp(temporary_path);
    } else {
        // Alias checks must finish before any existing file is truncated.
        file->fd = open(file->path, O_CREAT | O_WRONLY, 0600);
    }
    if (file->fd < 0) {
        perror(label);
        return false;
    }

    file->is_temporary = is_temporary;
    if (is_temporary) {
        file->path = temporary_path;
    }

    return true;
}

// Captures the identity of the actual opened file for alias and type checks.
static bool inspect_file(struct launch_file* file, const char* label) {
    if (fstat(file->fd, &file->stats) < 0) {
        perror(label);
        return false;
    }

    return true;
}

// Opens the optional PID file without changing its existing contents.
static bool open_pid_file(struct launch_file* file) {
    if (file->path == NULL) {
        return true;
    }

    file->fd = open(file->path, O_CREAT | O_WRONLY, 0600);
    if (file->fd < 0) {
        perror("pid");
        return false;
    }

    return inspect_file(file, "pid");
}

// Rejects file aliases before truncation can destroy input or the PID file.
static bool validate_file_aliases(const struct launch_files* files) {
    const struct stat* input = &files->input.stats;
    const struct stat* output = &files->output.stats;
    const struct stat* errors = &files->errors.stats;
    const struct stat* pid = &files->pid.stats;

    if (files->pid.path != NULL &&
        (is_same_file(pid, input) || is_same_file(pid, output) ||
         is_same_file(pid, errors))) {
        fputs("pid file must differ from stdin, stdout, and stderr\n", stderr);
        return false;
    }

    // Reject regular input aliases while allowing shared devices like `/dev/null`.
    if (S_ISREG(input->st_mode) &&
        (is_same_file(input, output) || is_same_file(input, errors))) {
        fputs("stdin must differ from stdout and stderr for regular files\n", stderr);
        return false;
    }

    return true;
}

// Clears validated logs and shares the offset of aliased output streams.
static bool prepare_output_files(const struct launch_files* files) {
    if (!reset_regular_file(&files->output, "stdout")) {
        return false;
    }

    if (!is_same_file(&files->output.stats, &files->errors.stats)) {
        return reset_regular_file(&files->errors, "stderr");
    }

    // Share the offset so writes through one stream cannot overwrite the other.
    if (dup2(files->output.fd, files->errors.fd) < 0) {
        perror("stderr");
        return false;
    }

    return true;
}

// Opens launch files and records their identities before validation.
static bool open_launch_files(struct launch_files* files) {
    files->input.fd = open(files->input.path, O_RDONLY);
    if (files->input.fd < 0) {
        perror("stdin");
        return false;
    }

    if (!open_log_file(&files->output, files->stdout_template, "stdout") ||
        !open_log_file(&files->errors, files->stderr_template, "stderr")) {
        return false;
    }

    if (!inspect_file(&files->output, "stdout") ||
        !inspect_file(&files->errors, "stderr")) {
        return false;
    }

    if (!open_pid_file(&files->pid)) {
        return false;
    }

    return inspect_file(&files->input, "stdin");
}

// Removes generated logs without treating earlier cleanup as an error.
static void remove_temporary_file(const struct launch_file* file) {
    if (!file->is_temporary) {
        return;
    }

    if (unlink(file->path) < 0 && errno != ENOENT) {
        perror(file->path);
    }
}

// Cleans up an aborted launch while preserving user-selected files.
static int fail_startup(const struct launch_files* files) {
    remove_temporary_file(&files->output);
    remove_temporary_file(&files->errors);
    return EXIT_FAILURE;
}

// Saves the optional PID file before the command is allowed to execute.
static bool save_pid_file(const struct launch_file* file, pid_t pid) {
    if (file->path == NULL) {
        return true;
    }

    if (!reset_regular_file(file, "pid")) {
        return false;
    }

    if (dprintf(file->fd, "%jd\n", (intmax_t)pid) < 0) {
        perror("pid");
        close(file->fd);
        return false;
    }

    if (close(file->fd) < 0) {
        perror("pid");
        return false;
    }

    return true;
}

// Fills closed standard descriptor slots so internal files and sockets
// cannot accidentally take the place of `stdin`, `stdout`, or `stderr`.
static bool open_missing_standard_descriptors(void) {
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd += 1) {
        if (fcntl(fd, F_GETFD) >= 0) {
            continue;
        }
        if (errno != EBADF) {
            perror("standard descriptors");
            return false;
        }
        if (open(DEV_NULL_PATH, O_RDWR) < 0) {
            perror(DEV_NULL_PATH);
            return false;
        }
    }

    return true;
}

// Installs a command stream and releases its original descriptor while
// keeping an already matching standard descriptor open.
static bool redirect_standard_descriptor(int source_fd,
                                         int target_fd,
                                         const char* label) {
    if (dup2(source_fd, target_fd) < 0) {
        perror(label);
        return false;
    }

    if (source_fd != target_fd && close(source_fd) < 0) {
        perror(label);
        return false;
    }

    return true;
}

// Converts `/dev/fd` entry names to inherited descriptor numbers, returning -1
// for invalid entries and standard streams so they are never closed.
static int parse_descriptor_name(const char* name) {
    char* end;
    errno = 0;
    const intmax_t descriptor = strtoimax(name, &end, 10);
    if (errno != 0 || end == name || *end != '\0' || descriptor <= STDERR_FILENO ||
        descriptor > INT_MAX) {
        return -1;
    }

    return (int)descriptor;
}

// Closes inherited handles before `exec`, preserving redirected streams and
// the startup socket. Enumeration also finds handles above a lowered limit.
static bool close_inherited_descriptors(int startup_fd) {
    DIR* directory = opendir(DEV_FD_PATH);
    if (directory == NULL) {
        perror(DEV_FD_PATH);
        return false;
    }

    while (true) {
        // Clear `errno` to distinguish end of directory from a read error.
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                perror(DEV_FD_PATH);
                closedir(directory);
                return false;
            }
            break;
        }

        const int fd = parse_descriptor_name(entry->d_name);
        if (fd < 0) {
            continue;
        }

        // Keep enumeration and startup communication alive until they complete.
        if (fd == dirfd(directory) || fd == startup_fd) {
            continue;
        }
        if (close(fd) < 0) {
            perror("close inherited descriptor");
            closedir(directory);
            return false;
        }
    }

    if (closedir(directory) < 0) {
        perror(DEV_FD_PATH);
        return false;
    }

    return true;
}

// Redirects all command streams, then closes unrelated inherited handles
// without losing the channel needed to report startup failures.
static bool prepare_command_streams(const struct launch_files* files, int startup_fd) {
    if (!redirect_standard_descriptor(files->input.fd, STDIN_FILENO, "stdin") ||
        !redirect_standard_descriptor(files->output.fd, STDOUT_FILENO, "stdout") ||
        !redirect_standard_descriptor(files->errors.fd, STDERR_FILENO, "stderr")) {
        return false;
    }

    return close_inherited_descriptors(startup_fd);
}

// Temporarily ignores `SIGPIPE` so broken diagnostic pipes cannot interrupt
// cleanup, saving the original disposition for the command.
static bool ignore_sigpipe(struct sigpipe_state* sigpipe) {
    sigpipe->ignored.sa_handler = SIG_IGN;
    if (sigemptyset(&sigpipe->ignored.sa_mask) < 0 ||
        sigaction(SIGPIPE, &sigpipe->ignored, &sigpipe->original) < 0) {
        perror("sigpipe");
        return false;
    }

    return true;
}

// Sends a complete command PID before status bytes enter the startup socket,
// retrying interruptions and partial transfers.
static bool send_process_id(int fd, pid_t pid) {
    // A stream socket may transfer the PID in several sends.
    // `MSG_NOSIGNAL` lets a closed channel return an error without `SIGPIPE`.
    const char* data = (const char*)&pid;
    size_t sent = 0;

    while (sent < sizeof(pid)) {
        const ssize_t length = send(fd, data + sent, sizeof(pid) - sent, MSG_NOSIGNAL);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            perror("startup channel");
            return false;
        }
        if (length == 0) {
            return false;
        }

        sent += (size_t)length;
    }

    return true;
}

// Receives the complete PID record so subsequent status bytes cannot be
// misinterpreted as part of the process identity.
static bool receive_process_id(int fd, pid_t* pid) {
    char* data = (char*)pid;
    size_t received = 0;

    while (received < sizeof(*pid)) {
        const ssize_t length = read(fd, data + received, sizeof(*pid) - received);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            perror("startup channel");
            return false;
        }
        if (length == 0) {
            return false;
        }

        received += (size_t)length;
    }

    return true;
}

// Sends one startup marker without letting `SIGPIPE` interrupt cleanup.
static ssize_t send_startup_byte(int fd, char value) {
    while (true) {
        const ssize_t sent = send(fd, &value, sizeof(value), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return sent;
    }
}

// Notifies the launcher after cleanup, ignoring a channel it has already closed.
static void report_startup_failure(int startup_fd) {
    (void)send_startup_byte(startup_fd, STARTUP_FAILED);
}

// Releases the final child only after the launcher has reaped the intermediate
// process and saved the optional PID file.
static bool allow_command_start(int fd) {
    const ssize_t sent = send_startup_byte(fd, START_COMMAND);
    if (sent != sizeof(char)) {
        perror("startup channel");
        return false;
    }

    return true;
}

// Reads one startup permission or status byte, retrying interruptions while
// keeping `EOF` distinct from a communication error.
static ssize_t read_startup_byte(int fd, char* value) {
    while (true) {
        const ssize_t received = read(fd, value, sizeof(*value));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            perror("startup channel");
        }
        return received;
    }
}

// Waits for the session-creating child so it cannot remain a zombie,
// including when the caller ignores `SIGCHLD`.
static bool reap_intermediate_process(pid_t pid) {
    while (waitpid(pid, NULL, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        // A caller that ignores `SIGCHLD` can have its children reaped by the OS.
        if (errno == ECHILD) {
            break;
        }
        perror("waitpid");
        return false;
    }
    return true;
}

// Copies startup diagnostics through an open descriptor so a removed or
// aliased temporary pathname does not hide the failure.
static void print_error_log(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("stderr");
        return;
    }

    char buffer[BUFSIZ];
    while (true) {
        const ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length == 0) {
            return;
        }
        if (length < 0) {
            perror("stderr");
            return;
        }
        if (fwrite(buffer, 1, (size_t)length, stderr) != (size_t)length) {
            return;
        }
    }
}

// Keeps a labeled path on one line in reports and startup diagnostics.
static bool print_path_line(FILE* stream, const char* label, const char* path) {
    return fprintf(stream, "%s: ", label) >= 0 && print_escaped_text(stream, path) &&
           fputc('\n', stream) != EOF;
}

// Uses the same path representation and field order for every launch report.
static bool print_log_paths(FILE* stream, const struct launch_files* files) {
    return print_path_line(stream, "stdout", files->output.path) &&
           print_path_line(stream, "stderr", files->errors.path);
}

// Reports a failed command setup and exposes its diagnostics, either by
// reading a temporary log or naming the selected `stderr` file.
static void print_startup_error(const struct launch_files* files) {
    fputs("failed to start the command\n", stderr);
    if (files->errors.is_temporary) {
        print_error_log(files->errors.fd);
    } else if (files->output.is_temporary &&
               is_same_file(&files->output.stats, &files->errors.stats)) {
        // Selected `stderr` may alias an unlinked temporary `stdout`.
        print_error_log(files->output.fd);
    } else {
        print_path_line(stderr, "check stderr", files->errors.path);
    }
}

// Attempts to preserve launch details on `stderr` when the normal report
// fails, because the command may already be running.
static void report_output_failure(pid_t pid, const struct launch_files* files) {
    perror("report");
    fprintf(stderr, "the command may already be running\nPID: %jd\n", (intmax_t)pid);
    print_log_paths(stderr, files);
}

// Flushes the launch report without removing logs if writing fails.
static bool print_launch_report(pid_t pid, const struct launch_files* files) {
    // `fclose` checks the final buffered write as well as closing the stream.
    if (printf("PID: %jd\n", (intmax_t)pid) < 0 || !print_log_paths(stdout, files) ||
        fclose(stdout) == EOF) {
        report_output_failure(pid, files);
        return false;
    }
    return true;
}

// Waits for command startup and preserves logs if its final state is uncertain.
static int run_launcher(pid_t intermediate_pid,
                        int startup_fd,
                        int command_fd,
                        const struct launch_files* files,
                        const int* startup_failed) {
    bool endpoints_closed = true;
    if (close(files->input.fd) < 0) {
        perror("stdin");
        endpoints_closed = false;
    }
    // Close the command endpoint so `exec` can produce `EOF` on the channel.
    if (close(command_fd) < 0) {
        perror("startup channel");
        endpoints_closed = false;
    }
    if (!endpoints_closed) {
        // Release the command before waiting for its parent to finish.
        close(startup_fd);
        reap_intermediate_process(intermediate_pid);
        return fail_startup(files);
    }

    pid_t command_pid = -1;
    const bool pid_received = receive_process_id(startup_fd, &command_pid);
    if (!reap_intermediate_process(intermediate_pid)) {
        return fail_startup(files);
    }
    if (!pid_received || command_pid <= 0) {
        fputs("failed to start the command\n", stderr);
        return fail_startup(files);
    }

    if (!save_pid_file(&files->pid, command_pid)) {
        return fail_startup(files);
    }
    if (!allow_command_start(startup_fd)) {
        return fail_startup(files);
    }

    // The shared flag detects unfinished setup even without a failure byte.
    // An unreported death after attempting `execvp` remains uncertain.
    char failure_marker;
    const ssize_t status_length = read_startup_byte(startup_fd, &failure_marker);
    if (status_length < 0) {
        fprintf(stderr, "PID: %jd\n", (intmax_t)command_pid);
        print_log_paths(stderr, files);
        // The command may be running, so keep its logs.
        return EXIT_FAILURE;
    }
    close(startup_fd);
    if (status_length > 0 || __atomic_load_n(startup_failed, __ATOMIC_ACQUIRE)) {
        print_startup_error(files);
        return fail_startup(files);
    }

    if (!print_launch_report(command_pid, files)) {
        // A report error does not stop the running command.
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

// Prepares the detached process and reports failures before returning.
static int run_detached_command(char* const command[],
                                int startup_fd,
                                const struct launch_files* files,
                                const struct sigpipe_state* sigpipe,
                                int* startup_failed) {
    if (!send_process_id(startup_fd, getpid())) {
        __atomic_store_n(startup_failed, true, __ATOMIC_RELEASE);
        return fail_startup(files);
    }

    char start_permission;
    const ssize_t permission_length = read_startup_byte(startup_fd, &start_permission);
    if (permission_length < 0) {
        goto failed;
    }
    // `EOF` or an invalid byte means the launcher did not permit execution.
    if (permission_length != sizeof(start_permission) ||
        start_permission != START_COMMAND) {
        goto failed;
    }

    if (!prepare_command_streams(files, startup_fd)) {
        goto failed;
    }
    if (sigaction(SIGPIPE, &sigpipe->original, NULL) < 0) {
        perror("sigpipe");
        goto failed;
    }

    // Preparation is complete, but an abrupt death during `execvp` remains uncertain.
    __atomic_store_n(startup_failed, false, __ATOMIC_RELEASE);
    execvp(command[0], command);

    // Failed `exec` returns here. Protect diagnostics from `SIGPIPE` again.
    const int exec_error = errno;
    __atomic_store_n(startup_failed, true, __ATOMIC_RELEASE);
    if (sigaction(SIGPIPE, &sigpipe->ignored, NULL) < 0) {
        goto failed;
    }
    errno = exec_error;
    perror("exec");

failed:
    // Publish the failure before cleanup or channel errors can hide it.
    __atomic_store_n(startup_failed, true, __ATOMIC_RELEASE);
    // Finish cleanup diagnostics before the launcher starts reading its log.
    fail_startup(files);
    report_startup_failure(startup_fd);
    return EXIT_FAILURE;
}

// Creates a new session and forks again to prevent terminal ownership.
static int run_session_child(char* const command[],
                             int launcher_fd,
                             int startup_fd,
                             const struct launch_files* files,
                             const struct sigpipe_state* sigpipe,
                             int* startup_failed) {
    // Retaining the parent's endpoint would hide its exit from the command.
    if (close(launcher_fd) < 0) {
        perror("startup channel");
        send_process_id(startup_fd, 0);
        return fail_startup(files);
    }
    if (files->pid.path != NULL) {
        close(files->pid.fd);
    }

    if (setsid() < 0) {
        perror("setsid");
        send_process_id(startup_fd, 0);
        return fail_startup(files);
    }

    // Fork again so `open` cannot give the command a controlling terminal.
    const pid_t command_pid = fork();
    if (command_pid < 0) {
        perror("fork");
        send_process_id(startup_fd, 0);
        return fail_startup(files);
    }
    if (command_pid > 0) {
        // Do not flush inherited stream buffers.
        _exit(EXIT_SUCCESS);
    }

    return run_detached_command(command, startup_fd, files, sigpipe, startup_failed);
}

// Creates a startup channel and separates the launcher from its session child.
static int launch_command(char* const command[],
                          const struct launch_files* files,
                          const struct sigpipe_state* sigpipe) {
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0) {
        perror("startup channel");
        return fail_startup(files);
    }
    const int launcher_fd = channel[0];
    const int command_fd = channel[1];
    if (fcntl(command_fd, F_SETFD, FD_CLOEXEC) < 0) {
        perror("startup channel");
        return fail_startup(files);
    }

    // The shared mapping lives until each process exits or executes the command.
    // Atomic accesses keep failures visible if the socket send fails.
    int* startup_failed = mmap(NULL, sizeof(*startup_failed), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (startup_failed == MAP_FAILED) {
        perror("startup state");
        return fail_startup(files);
    }
    *startup_failed = true;

    const pid_t intermediate_pid = fork();
    if (intermediate_pid < 0) {
        perror("fork");
        return fail_startup(files);
    }
    if (intermediate_pid == 0) {
        return run_session_child(command, launcher_fd, command_fd, files, sigpipe,
                                 startup_failed);
    }

    return run_launcher(intermediate_pid, launcher_fd, command_fd, files,
                        startup_failed);
}

// Runs the startup stages in order: parse options, protect diagnostics,
// prepare and validate files, then launch the detached command.
int main(int argc, char* argv[]) {
    struct options options = {.stdin_path = DEV_NULL_PATH};
    const enum parse_result parsed = parse_arguments(argc, argv, &options);
    if (parsed == PARSE_HELP) {
        return show_help() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (parsed == PARSE_ERROR) {
        return EXIT_FAILURE;
    }

    struct sigpipe_state sigpipe = {0};
    if (!ignore_sigpipe(&sigpipe) || !open_missing_standard_descriptors()) {
        return EXIT_FAILURE;
    }

    struct launch_files files = {
        .input = {.path = options.stdin_path, .fd = -1},
        .output = {.path = options.stdout_path, .fd = -1},
        .errors = {.path = options.stderr_path, .fd = -1},
        .pid = {.path = options.pid_path, .fd = -1},
        .stdout_template = STDOUT_PATH_TEMPLATE,
        .stderr_template = STDERR_PATH_TEMPLATE,
    };
    if (!open_launch_files(&files) || !validate_file_aliases(&files) ||
        !prepare_output_files(&files)) {
        return fail_startup(&files);
    }

    return launch_command(options.command, &files, &sigpipe);
}
