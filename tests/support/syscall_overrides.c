// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

// Simulates system call failures to test how `detach` handles them.
// Reads the failure rules from `DETACH_TEST_FAULTS`.

// Include the real declarations before replacing calls in the utility below.
#include "tests/support/syscall_overrides.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef DETACH_TEST_COVERAGE
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char* name);
#endif

// The original parent runs `detach`, the first child creates the session, and
// the second child starts the command. Counters are local to each role.
enum process_role { LAUNCHER, INTERMEDIATE, COMMAND };

enum { MAX_FAULT_RULES = 8 };

// Marks errors in the test helper itself.
enum { SETUP_ERROR_EXIT_CODE = 125 };

struct fault_rule {
    char role[16];
    char operation[32];
    unsigned int call_number;
    int result;
    unsigned int call_count;
};

static enum process_role current_role = LAUNCHER;
static struct fault_rule rules[MAX_FAULT_RULES];
static size_t rule_count;

// Test records.

// Records applied rules so a passing test also proves its planned faults occurred.
static void record_fault(const struct fault_rule* rule) {
    const char* path = getenv("DETACH_TEST_APPLIED_FAULTS");
    if (path == NULL) {
        return;
    }

    // Logging must preserve the system error seen by the tested code.
    const int saved_errno = errno;
    const int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    const int written = dprintf(fd, "%s:%s:%u:%d\n", rule->role, rule->operation,
                                rule->call_number, rule->result);
    if (written < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    if (close(fd) < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    errno = saved_errno;
}

// Records generated log paths so tests can clean up after a simulated unlink error.
static void record_temporary(const char* path) {
    const char* events = getenv("DETACH_TEST_EVENTS");
    if (events == NULL) {
        return;
    }

    const int saved_errno = errno;
    const int fd = open(events, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    if (dprintf(fd, "%s\n", path) < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    close(fd);
    errno = saved_errno;
}

// Fault rules.

// Reads one rule and rejects missing fields, invalid numbers and overflow.
static void parse_rule(const char* text, struct fault_rule* rule) {
    int number_offset = 0;
    const int fields =
        sscanf(text, "%15[^:]:%31[^:]:%n", rule->role, rule->operation, &number_offset);
    if (fields != 2 || number_offset == 0) {
        exit(SETUP_ERROR_EXIT_CODE);
    }

    // Parse numbers separately so values outside the supported range are rejected.
    const char* number_text = text + number_offset;
    char* end;
    errno = 0;
    const intmax_t call_number = strtoimax(number_text, &end, 10);
    if (errno != 0 || end == number_text || *end != ':' || call_number <= 0 ||
        call_number > UINT_MAX) {
        exit(SETUP_ERROR_EXIT_CODE);
    }

    const char* result_text = end + 1;
    errno = 0;
    const intmax_t result = strtoimax(result_text, &end, 10);
    if (errno != 0 || end == result_text || *end != '\0' || result < INT_MIN ||
        result > INT_MAX) {
        exit(SETUP_ERROR_EXIT_CODE);
    }

    rule->call_number = (unsigned int)call_number;
    rule->result = (int)result;
}

// Loads rules without modifying the environment string shared by forked processes.
static void initialize_faults(void) {
    const char* plan = getenv("DETACH_TEST_FAULTS");
    if (plan == NULL) {
        return;
    }

    char* plan_copy = strdup(plan);
    if (plan_copy == NULL) {
        exit(SETUP_ERROR_EXIT_CODE);
    }

    char* context = NULL;
    char* text = strtok_r(plan_copy, ";", &context);
    while (text != NULL) {
        if (rule_count == MAX_FAULT_RULES) {
            exit(SETUP_ERROR_EXIT_CODE);
        }

        parse_rule(text, &rules[rule_count]);
        rule_count += 1;

        text = strtok_r(NULL, ";", &context);
    }

    free(plan_copy);
}

// Counts this call and applies its scheduled result, including the selected `errno`.
static const struct fault_rule* apply_fault(const char* operation) {
    const char* roles[] = {"launcher", "intermediate", "command"};
    const struct fault_rule* selected = NULL;

    for (size_t index = 0; index < rule_count; index += 1) {
        struct fault_rule* rule = &rules[index];
        if (strcmp(rule->role, roles[current_role]) != 0 ||
            strcmp(rule->operation, operation) != 0) {
            continue;
        }

        // Advance every matching rule. The last rule scheduled for this call wins.
        rule->call_count += 1;
        if (rule->call_count == rule->call_number) {
            selected = rule;
        }
    }

    if (selected == NULL) {
        return NULL;
    }

    record_fault(selected);

    if (selected->result == FAULT_TERMINATE) {
        kill(getpid(), SIGKILL);
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    if (selected->result > 0) {
        errno = selected->result;
    }

    return selected;
}

// Processes and signals.

// `exec` and `_exit` bypass the normal coverage shutdown handler. Flush before
// either operation so child setup and failure branches appear in the report.
static void write_profile(void) {
#ifdef DETACH_TEST_COVERAGE
    __llvm_profile_write_file();
#endif
}

// Gives each child its own rule counters and coverage profile.
static void prepare_child(void) {
    current_role += 1;

    for (size_t index = 0; index < rule_count; index += 1) {
        rules[index].call_count = 0;
    }

#ifdef DETACH_TEST_COVERAGE
    // Expand the PID placeholder again after `fork` to keep child profiles separate.
    const char* path = getenv("LLVM_PROFILE_FILE");
    if (path != NULL) {
        __llvm_profile_set_filename(path);
    }
#endif
}

// Forks with optional failure injection and prepares the new child role.
static pid_t fault_fork(void) {
    if (apply_fault("fork") != NULL) {
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        prepare_child();
    }

    return pid;
}

static void fault_exit(int status) __attribute__((noreturn));

// Flushes child coverage before `_exit` bypasses normal shutdown handlers.
static void fault_exit(int status) {
    write_profile();
    _exit(status);
}

// Flushes setup coverage before a successful `exec` replaces the test code.
static int fault_execvp(const char* file, char* const argv[]) {
    write_profile();
    if (apply_fault("execvp") != NULL) {
        return -1;
    }
    return execvp(file, argv);
}

// Injects wait failures while still reaping real intermediate children
// so a failure scenario cannot leave zombies behind.
static pid_t fault_waitpid(pid_t pid, int* status, int options) {
    const struct fault_rule* rule = apply_fault("waitpid");
    if (rule == NULL) {
        return waitpid(pid, status, options);
    }

    // Keep real intermediate children reaped even when simulating wait errors.
    if (rule->result != EINTR) {
        while (waitpid(pid, status, options) < 0 && errno == EINTR) {
        }
    }

    errno = rule->result;
    return -1;
}

// Makes session creation fail in the intermediate child.
static pid_t fault_setsid(void) {
    return apply_fault("setsid") != NULL ? -1 : setsid();
}

// Makes initial signal-mask setup fail before files are opened.
static int fault_sigemptyset(sigset_t* mask) {
    return apply_fault("sigemptyset") != NULL ? -1 : sigemptyset(mask);
}

// Makes signal-disposition setup or restoration fail in a selected role.
static int fault_sigaction(int signal,
                           const struct sigaction* action,
                           struct sigaction* previous) {
    return apply_fault("sigaction") != NULL ? -1 : sigaction(signal, action, previous);
}

// Files and descriptors.

// Creates and records real temporary logs, with optional alias creation
// for cleanup tests that must survive removal of a pathname.
static int fault_mkstemp(char* path) {
    if (apply_fault("mkstemp") != NULL) {
        return -1;
    }

    const int fd = mkstemp(path);
    if (fd < 0) {
        return fd;
    }

    record_temporary(path);

    // Create the alias before `detach` opens the selected `stderr` path.
    const char* alias = getenv("DETACH_TEST_STDERR_ALIAS");
    if (alias == NULL || strncmp(path, "/tmp/stdout.", 12) != 0) {
        return fd;
    }

    if (link(path, alias) < 0) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    return fd;
}

// Injects file-open failures while forwarding normal calls with the
// correct optional creation mode.
static int fault_open(const char* path, int flags, ...) {
    if (apply_fault("open") != NULL) {
        return -1;
    }

    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode_t mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);

        return open(path, flags, mode);
    }

    return open(path, flags);
}

// Injects descriptor-flag failures while preserving the real calling
// convention for flag queries and updates.
static int fault_fcntl(int fd, int command, ...) {
    if (apply_fault("fcntl") != NULL) {
        return -1;
    }

    if (command == F_GETFD) {
        return fcntl(fd, command);
    }

    va_list arguments;
    va_start(arguments, command);
    int flags = va_arg(arguments, int);
    va_end(arguments);

    return fcntl(fd, command, flags);
}

// Releases the descriptor once, then simulates a delayed close error.
static int fault_close(int fd) {
    const struct fault_rule* rule = apply_fault("close");
    const int status = close(fd);
    if (rule != NULL) {
        errno = rule->result;
        return -1;
    }

    return status;
}

// Simulates failure to allocate shared startup status before any fork.
static void* fault_mmap(void* address,
                        size_t length,
                        int protection,
                        int flags,
                        int fd,
                        off_t offset) {
    if (apply_fault("mmap") != NULL) {
        return MAP_FAILED;
    }

    return mmap(address, length, protection, flags, fd, offset);
}

// Makes stream redirection or shared-offset setup fail at a chosen call.
static int fault_dup2(int source, int target) {
    return apply_fault("dup2") != NULL ? -1 : dup2(source, target);
}

// Makes file-identity inspection fail before any existing file is truncated.
static int fault_fstat(int fd, struct stat* status) {
    return apply_fault("fstat") != NULL ? -1 : fstat(fd, status);
}

// Makes truncation fail so tests can verify startup stops safely.
static int fault_ftruncate(int fd, off_t length) {
    return apply_fault("ftruncate") != NULL ? -1 : ftruncate(fd, length);
}

// Makes temporary-log removal fail so cleanup diagnostics can be checked.
static int fault_unlink(const char* path) {
    return apply_fault("unlink") != NULL ? -1 : unlink(path);
}

// Makes output rewinding or diagnostic-log seeking fail at a chosen call.
static off_t fault_lseek(int fd, off_t offset, int origin) {
    return apply_fault("lseek") != NULL ? -1 : lseek(fd, offset, origin);
}

// Closes the actual stream but can report a delayed close error to test
// help and report flushing without leaking the stream.
static int fault_fclose(FILE* stream) {
    const struct fault_rule* rule = apply_fault("fclose");
    const int status = fclose(stream);

    if (rule != NULL) {
        errno = rule->result;
        return EOF;
    }

    return status;
}

// Stream transfers.

// Makes startup-channel creation fail before a child can be launched.
static int fault_socketpair(int domain, int type, int protocol, int channel[2]) {
    if (apply_fault("socketpair") != NULL) {
        return -1;
    }

    return socketpair(domain, type, protocol, channel);
}

// Simulates read errors, short reads and damaged startup permissions.
static ssize_t fault_read(int fd, void* data, size_t size) {
    const struct fault_rule* rule = apply_fault("read");
    if (rule == NULL) {
        return read(fd, data, size);
    }

    if (rule->result == FAULT_BAD_PERMISSION) {
        const ssize_t length = read(fd, data, size);
        if (length > 0) {
            memset(data, 0, (size_t)length);
        }
        return length;
    }

    if (rule->result >= 0) {
        return rule->result == FAULT_EOF ? 0 : -1;
    }

    size = size > 1 ? 1 : size;
    return read(fd, data, size);
}

// Simulates failed or short sends to check startup communication handling.
static ssize_t fault_send(int fd, const void* data, size_t size, int flags) {
    const struct fault_rule* rule = apply_fault("send");
    if (rule == NULL) {
        return send(fd, data, size, flags);
    }

    if (rule->result >= 0) {
        return rule->result == FAULT_EOF ? 0 : -1;
    }

    size = size > 1 ? 1 : size;
    return send(fd, data, size, flags);
}

// Makes copying startup diagnostics fail without interrupting cleanup.
static size_t fault_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    return apply_fault("fwrite") != NULL ? 0 : fwrite(data, size, count, stream);
}

// Directory scanning.

// Makes inherited-descriptor enumeration fail before exec.
static DIR* fault_opendir(const char* path) {
    return apply_fault("opendir") != NULL ? NULL : opendir(path);
}

// Simulates directory read errors or supplies an entry for descriptor checks.
static struct dirent* fault_readdir(DIR* directory) {
    const struct fault_rule* rule = apply_fault("readdir");
    if (rule == NULL) {
        return readdir(directory);
    }

    if (rule->result != FAULT_DIRECTORY_ENTRY) {
        return NULL;
    }

    static struct dirent entry;
    const char* name = getenv("DETACH_TEST_ENTRY");
    if (name == NULL) {
        _exit(SETUP_ERROR_EXIT_CODE);
    }

    snprintf(entry.d_name, sizeof(entry.d_name), "%s", name);
    return &entry;
}

// Closes the real directory but reports a selected close error so the
// test exercises diagnostics without leaking the enumeration handle.
static int fault_closedir(DIR* directory) {
    const struct fault_rule* rule = apply_fault("closedir");
    const int status = closedir(directory);

    if (rule != NULL) {
        errno = rule->result;
        return -1;
    }

    return status;
}

// Overrides conversion errno to exercise directory-entry parsing edges
// without inventing a descriptor or replacing the actual conversion.
static intmax_t fault_strtoimax(const char* text, char** end, int base) {
    const struct fault_rule* rule = apply_fault("strtoimax");
    const intmax_t value = strtoimax(text, end, base);

    if (rule != NULL) {
        errno = rule->result;
    }

    return value;
}

// Formatted output.

// Makes a path byte or its final newline fail while forwarding other output.
static int fault_fputc(int byte, FILE* stream) {
    return apply_fault("fputc") != NULL ? EOF : fputc(byte, stream);
}

// Makes formatted help output fail while forwarding unrelated writes.
static int fault_fprintf(FILE* stream, const char* format, ...) {
    if (apply_fault("fprintf") != NULL) {
        return -1;
    }

    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);

    return result;
}

// Makes the normal launch report fail after the command may have started.
static int fault_printf(const char* format, ...) {
    if (apply_fault("printf") != NULL) {
        return -1;
    }

    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);

    return result;
}

// Makes writing the PID file fail before execution is permitted.
static int fault_dprintf(int fd, const char* format, ...) {
    if (apply_fault("dprintf") != NULL) {
        return -1;
    }

    va_list arguments;
    va_start(arguments, format);
    int result = vdprintf(fd, format, arguments);
    va_end(arguments);

    return result;
}

// Command arguments.

// Supplies parser outcomes that normal command arguments cannot produce.
static int fault_getopt_long(int argc,
                             char* const argv[],
                             const char* options,
                             const struct option* long_options,
                             int* index) {
    const struct fault_rule* rule = apply_fault("getopt_long");
    if (rule == NULL) {
        return getopt_long(argc, argv, options, long_options, index);
    }

    if (rule->result == FAULT_PARSER_ADVANCE) {
        optind += 1;
        return -1;
    }

    return 'X';
}

// Only this translation unit redirects calls made by `detach.c`.
// Production builds compile that source directly.
#define main utility_main
#define fork fault_fork
#define _exit fault_exit
#define execvp fault_execvp
#define waitpid fault_waitpid
#define read fault_read
#define send fault_send
#define mkstemp fault_mkstemp
#define open fault_open
#define fcntl fault_fcntl
#define close fault_close
#define mmap fault_mmap
#define dup2 fault_dup2
#define fstat fault_fstat
#define strtoimax fault_strtoimax
#define ftruncate fault_ftruncate
#define socketpair fault_socketpair
#define setsid fault_setsid
#undef sigemptyset
#define sigemptyset fault_sigemptyset
#define sigaction(...) fault_sigaction(__VA_ARGS__)
#define opendir fault_opendir
#define readdir fault_readdir
#define closedir fault_closedir
#define unlink fault_unlink
#define lseek fault_lseek
#define fclose fault_fclose
#define fputc fault_fputc
#define fprintf fault_fprintf
#define printf fault_printf
#define dprintf fault_dprintf
#define fwrite fault_fwrite
#define getopt_long fault_getopt_long
#include "detach.c"
#undef main

// Loads injection rules and runs the production CLI, or a small helper
// for static-function edge cases that have no public CLI representation.
int main(int argc, char* argv[]) {
    initialize_faults();

    const char* helper = getenv("DETACH_TEST_HELPER");
    if (helper != NULL && strcmp(helper, "empty-null") == 0) {
        return argument_is_empty(NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (helper != NULL && strcmp(helper, "same-descriptor") == 0) {
        if (redirect_standard_descriptor(STDOUT_FILENO, STDOUT_FILENO, "stdout")) {
            return EXIT_SUCCESS;
        }

        return EXIT_FAILURE;
    }

    return utility_main(argc, argv);
}
