// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#define _POSIX_C_SOURCE 200809L

// Run real detach code while failing selected system calls deterministically.
// DETACH_TEST_FAULTS contains role:operation:occurrence:result rules. For
// example, launcher:read:1:0 simulates EOF on the launcher's first read.
// Wrappers pass all other calls through to the OS. The final include below
// applies them only to this test executable, leaving the production build
// intact.

// Include the real declarations before replacing calls in the utility below.
#include "tests/faults.h"

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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef DETACH_TEST_COVERAGE
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char* name);
#endif

// The original parent runs detach, the first child creates the session, and
// the second child starts the user's command. Counters are local to each role.
enum process_role { LAUNCHER, INTERMEDIATE, COMMAND };

struct fault_rule {
  char role[16];
  char operation[32];
  unsigned int occurrence;
  int error;
  unsigned int calls;
};

static enum process_role current_role = LAUNCHER;
static struct fault_rule rules[8];
static size_t rule_count;

// Record actual injections separately from the plan. A successful retry test
// must prove that its selected failure happened, not just that detach worked.
static void record_fault(const struct fault_rule* rule) {
  const char* path = getenv("DETACH_TEST_APPLIED_FAULTS");
  if (path == NULL) {
    return;
  }
  // These are real calls, declared before the wrapper macros below. Appending
  // one short record keeps reports from the different startup roles together.
  int saved_error = errno;
  int fd = open(path, O_WRONLY | O_APPEND);
  if (fd < 0 ||
      dprintf(fd, "%s:%s:%u:%d\n", rule->role, rule->operation,
              rule->occurrence, rule->error) < 0 ||
      close(fd) < 0) {
    _exit(125);
  }
  errno = saved_error;
}

static void initialize_faults(void) {
  const char* plan = getenv("DETACH_TEST_FAULTS");
  if (plan == NULL) {
    return;
  }
  // Tokenization modifies its buffer. Keep the inherited environment string
  // intact so every forked role sees the original plan when inspecting it.
  char* copy = strdup(plan);
  if (copy == NULL) {
    exit(125);
  }
  char* saved;
  for (char* text = strtok_r(copy, ";", &saved); text != NULL;
       text = strtok_r(NULL, ";", &saved)) {
    if (rule_count == sizeof(rules) / sizeof(rules[0])) {
      exit(125);
    }
    struct fault_rule* rule = &rules[rule_count++];
    if (sscanf(text, "%15[^:]:%31[^:]:%u:%d", rule->role, rule->operation,
               &rule->occurrence, &rule->error) != 4 ||
        rule->occurrence == 0) {
      exit(125);
    }
  }
  free(copy);
}

// Each rule fires once at a numbered call within one process role.
// Positive errors return -1 with errno. Special results are named in faults.h.
static const struct fault_rule* find_fault(const char* operation) {
  const char* roles[] = {"launcher", "intermediate", "command"};
  const struct fault_rule* matched = NULL;
  for (size_t index = 0; index < rule_count; index += 1) {
    struct fault_rule* rule = &rules[index];
    if (strcmp(rule->role, roles[current_role]) == 0 &&
        strcmp(rule->operation, operation) == 0) {
      // Count only calls in the selected role. Unrelated operations and calls
      // made in another process cannot consume this rule's occurrence number.
      rule->calls += 1;
      if (rule->calls == rule->occurrence) {
        matched = rule;
      }
    }
  }
  if (matched != NULL) {
    record_fault(matched);
    if (matched->error > 0) {
      errno = matched->error;
    }
  }
  return matched;
}

// exec and _exit bypass the normal coverage shutdown handler. Flush before
// either operation so child setup and failure branches appear in the report.
static void write_profile(void) {
#ifdef DETACH_TEST_COVERAGE
  __llvm_profile_write_file();
#endif
}

static pid_t fault_fork(void) {
  if (find_fault("fork") != NULL) {
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    // Each ordered fork advances only the child to the next startup role.
    // The parent retains its own role and counters in its separate memory.
    current_role += 1;
    for (size_t index = 0; index < rule_count; index += 1) {
      rules[index].calls = 0;
    }
#ifdef DETACH_TEST_COVERAGE
    // Expand the profile filename again so its PID placeholder identifies this
    // child instead of keeping the filename chosen before fork.
    // Separate files preserve coverage from children that later exec or _exit.
    const char* path = getenv("LLVM_PROFILE_FILE");
    if (path != NULL) {
      __llvm_profile_set_filename(path);
    }
#endif
  }
  return pid;
}

static void fault_exit(int status) __attribute__((noreturn));

static void fault_exit(int status) {
  write_profile();
  _exit(status);
}

static int fault_execvp(const char* file, char* const argv[]) {
  write_profile();
  return execvp(file, argv);
}

static pid_t fault_waitpid(pid_t pid, int* status, int options) {
  const struct fault_rule* rule = find_fault("waitpid");
  if (rule == NULL) {
    return waitpid(pid, status, options);
  }
  // Keep real intermediate children reaped even when simulating wait errors.
  if (rule->error != EINTR) {
    while (waitpid(pid, status, options) < 0 && errno == EINTR) {
    }
  }
  errno = rule->error;
  return -1;
}

static ssize_t fault_read(int fd, void* data, size_t size) {
  const struct fault_rule* rule = find_fault("read");
  if (rule != NULL) {
    if (rule->error == FAULT_BAD_PERMISSION) {
      ssize_t length = read(fd, data, size);
      if (length > 0) {
        memset(data, 0, (size_t)length);
      }
      return length;
    }
    if (rule->error >= 0) {
      return rule->error == 0 ? 0 : -1;
    }
    size = size > 1 ? 1 : size;
  }
  return read(fd, data, size);
}

static ssize_t fault_send(int fd, const void* data, size_t size, int flags) {
  const struct fault_rule* rule = find_fault("send");
  if (rule != NULL) {
    if (rule->error >= 0) {
      return rule->error == 0 ? 0 : -1;
    }
    size = size > 1 ? 1 : size;
  }
  return send(fd, data, size, flags);
}

// Record only files made by mkstemp so failed cleanup can be tested safely.
static void record_temporary(const char* path) {
  const char* events = getenv("DETACH_TEST_EVENTS");
  if (events == NULL) {
    return;
  }
  // Event logging is test bookkeeping. Preserve the errno that the real
  // mkstemp call left behind so bookkeeping cannot change utility behavior.
  int saved_error = errno;
  int fd = open(events, O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (fd < 0 || dprintf(fd, "%s\n", path) < 0) {
    _exit(125);
  }
  close(fd);
  errno = saved_error;
}

static int fault_mkstemp(char* path) {
  if (find_fault("mkstemp") != NULL) {
    return -1;
  }
  int fd = mkstemp(path);
  if (fd >= 0) {
    record_temporary(path);
    // Create a real alias before the utility opens its selected stderr.
    const char* alias = getenv("DETACH_TEST_STDERR_ALIAS");
    if (alias != NULL && strncmp(path, "/tmp/stdout.", 12) == 0 &&
        link(path, alias) < 0) {
      _exit(125);
    }
  }
  return fd;
}

static int fault_open(const char* path, int flags, ...) {
  if (find_fault("open") != NULL) {
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

static int fault_fcntl(int fd, int command, ...) {
  if (find_fault("fcntl") != NULL) {
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

static int fault_close(int fd) {
  return find_fault("close") != NULL ? -1 : close(fd);
}

static int fault_dup2(int source, int target) {
  return find_fault("dup2") != NULL ? -1 : dup2(source, target);
}

static int fault_fstat(int fd, struct stat* status) {
  return find_fault("fstat") != NULL ? -1 : fstat(fd, status);
}

static intmax_t fault_strtoimax(const char* text, char** end, int base) {
  const struct fault_rule* rule = find_fault("strtoimax");
  intmax_t result = strtoimax(text, end, base);
  if (rule != NULL) {
    errno = rule->error;
  }
  return result;
}

static int fault_ftruncate(int fd, off_t length) {
  return find_fault("ftruncate") != NULL ? -1 : ftruncate(fd, length);
}

static int fault_socketpair(int domain, int type, int protocol,
                            int channel[2]) {
  return find_fault("socketpair") != NULL
             ? -1
             : socketpair(domain, type, protocol, channel);
}

static pid_t fault_setsid(void) {
  return find_fault("setsid") != NULL ? -1 : setsid();
}

static int fault_sigemptyset(sigset_t* mask) {
  return find_fault("sigemptyset") != NULL ? -1 : sigemptyset(mask);
}

static int fault_sigaction(int signal, const struct sigaction* action,
                           struct sigaction* previous) {
  return find_fault("sigaction") != NULL ? -1
                                         : sigaction(signal, action, previous);
}

static DIR* fault_opendir(const char* path) {
  return find_fault("opendir") != NULL ? NULL : opendir(path);
}

static struct dirent* fault_readdir(DIR* directory) {
  const struct fault_rule* rule = find_fault("readdir");
  if (rule == NULL) {
    return readdir(directory);
  }
  if (rule->error == FAULT_DIRECTORY_ENTRY) {
    static struct dirent entry;
    const char* name = getenv("DETACH_TEST_ENTRY");
    if (name == NULL) {
      _exit(125);
    }
    snprintf(entry.d_name, sizeof(entry.d_name), "%s", name);
    return &entry;
  }
  return NULL;
}

static int fault_closedir(DIR* directory) {
  const struct fault_rule* rule = find_fault("closedir");
  int result = closedir(directory);
  if (rule != NULL) {
    errno = rule->error;
    return -1;
  }
  return result;
}

static int fault_unlink(const char* path) {
  return find_fault("unlink") != NULL ? -1 : unlink(path);
}

static off_t fault_lseek(int fd, off_t offset, int origin) {
  return find_fault("lseek") != NULL ? -1 : lseek(fd, offset, origin);
}

static int fault_fclose(FILE* stream) {
  const struct fault_rule* rule = find_fault("fclose");
  int result = fclose(stream);
  if (rule != NULL) {
    errno = rule->error;
    return EOF;
  }
  return result;
}

static int fault_fprintf(FILE* stream, const char* format, ...) {
  if (find_fault("fprintf") != NULL) {
    return -1;
  }
  va_list arguments;
  va_start(arguments, format);
  int result = vfprintf(stream, format, arguments);
  va_end(arguments);
  return result;
}

static int fault_printf(const char* format, ...) {
  if (find_fault("printf") != NULL) {
    return -1;
  }
  va_list arguments;
  va_start(arguments, format);
  int result = vprintf(format, arguments);
  va_end(arguments);
  return result;
}

static int fault_dprintf(int fd, const char* format, ...) {
  if (find_fault("dprintf") != NULL) {
    return -1;
  }
  va_list arguments;
  va_start(arguments, format);
  int result = vdprintf(fd, format, arguments);
  va_end(arguments);
  return result;
}

static size_t fault_fwrite(const void* data, size_t size, size_t count,
                           FILE* stream) {
  return find_fault("fwrite") != NULL ? 0 : fwrite(data, size, count, stream);
}

static int fault_getopt_long(int argc, char* const argv[], const char* options,
                             const struct option* long_options, int* index) {
  const struct fault_rule* rule = find_fault("getopt_long");
  if (rule != NULL) {
    if (rule->error == FAULT_PARSER_ADVANCE) {
      optind += 1;
      return -1;
    }
    return 'X';
  }
  return getopt_long(argc, argv, options, long_options, index);
}

// This translation unit alone redirects calls in the unchanged C source.
// Production builds continue to compile detach.c directly.
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
#define fprintf fault_fprintf
#define printf fault_printf
#define dprintf fault_dprintf
#define fwrite fault_fwrite
#define getopt_long fault_getopt_long
#include "detach.c"  // NOLINT(build/include)
#undef main

int main(int argc, char* argv[]) {
  initialize_faults();
  const char* helper = getenv("DETACH_TEST_HELPER");
  if (helper != NULL && strcmp(helper, "empty-null") == 0) {
    return argument_is_empty(NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (helper != NULL && strcmp(helper, "same-descriptor") == 0) {
    return redirect_standard_descriptor(STDOUT_FILENO, STDOUT_FILENO, "stdout")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }
  return utility_main(argc, argv);
}
