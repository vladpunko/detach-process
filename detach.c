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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Boolean macros for readability in this file.
#define TRUE 1
#define FALSE 0

#define OPTION_SEPARATOR "--"
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

static int print_usage(FILE* stream) {
  const char* message =
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
      "  This is required when options are used.\n";
  return fprintf(stream, message, OPTION_SEPARATOR) >= 0;
}

static int argument_is_empty(const char* argument) {
  if (argument == NULL) {
    return TRUE;
  }

  if (argument[0] == STRING_TERMINATOR) {
    return TRUE;
  }

  return FALSE;
}

static int is_same_file(const struct stat* first, const struct stat* second) {
  // Device and inode identify a file even when its paths differ.
  return first->st_dev == second->st_dev && first->st_ino == second->st_ino;
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

static int redirect_standard_descriptor(int source_fd, int target_fd,
                                        const char* label) {
  if (dup2(source_fd, target_fd) < 0) {
    perror(label);

    return FALSE;
  }
  // After duplication, only the standard descriptor is needed.
  if (source_fd != target_fd) {
    close(source_fd);
  }
  return TRUE;
}

static int close_inherited_descriptors(int startup_fd) {
  // Enumerate open descriptors, including those above a lowered limit.
  DIR* directory = opendir(DEV_FD_PATH);
  if (directory == NULL) {
    perror(DEV_FD_PATH);

    return FALSE;
  }

  while (TRUE) {
    // If readdir returns NULL, errno tells us whether reading failed.
    // Clear it first so an earlier error cannot look like a read failure.
    errno = 0;
    struct dirent* entry = readdir(directory);
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
    char* end;
    errno = 0;
    intmax_t fd = strtoimax(entry->d_name, &end, 10);
    if (errno != 0 || end == entry->d_name || *end != STRING_TERMINATOR ||
        fd <= STDERR_FILENO || fd > INT_MAX) {
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

static int reset_regular_file(int fd, const struct stat* file_stat,
                              const char* label) {
  // Pipes and devices must remain usable without truncation or seeking.
  if (!S_ISREG(file_stat->st_mode)) {
    return TRUE;
  }
  if (ftruncate(fd, 0) < 0) {
    perror(label);

    return FALSE;
  }
  // Truncation keeps the offset. On macOS, /dev/fd can inherit a nonzero one.
  // Rewind explicitly so new output cannot start with a hole of zero bytes.
  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror(label);

    return FALSE;
  }
  return TRUE;
}

static int save_pid_file(int fd, pid_t pid, const struct stat* file_stat) {
  // Reuse the checked descriptor instead of reopening its path.
  if (!reset_regular_file(fd, file_stat, "pid")) {
    return FALSE;
  }
  if (dprintf(fd, "%jd\n", (intmax_t)pid) < 0) {
    perror("pid");
    close(fd);

    return FALSE;
  }
  if (close(fd) < 0) {
    perror("pid");

    return FALSE;
  }
  return TRUE;
}

static int send_process_id(int fd, pid_t pid) {
  // A stream socket may transfer the PID in several sends.
  // MSG_NOSIGNAL lets a closed channel return an error without SIGPIPE.
  const char* data = (const char*)&pid;
  size_t sent = 0;
  while (sent < sizeof(pid)) {
    ssize_t length = send(fd, data + sent, sizeof(pid) - sent, MSG_NOSIGNAL);
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("startup channel");

      return FALSE;
    }
    if (length == 0) {
      return FALSE;
    }
    sent += (size_t)length;
  }
  return TRUE;
}

static int receive_process_id(int fd, pid_t* pid) {
  // Read the complete PID before interpreting the following status byte.
  char* data = (char*)pid;
  size_t received = 0;
  while (received < sizeof(*pid)) {
    ssize_t length = read(fd, data + received, sizeof(*pid) - received);
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("startup channel");

      return FALSE;
    }
    if (length == 0) {
      return FALSE;
    }
    received += (size_t)length;
  }
  return TRUE;
}

static void report_startup_failure(int startup_fd) {
  // Notify the parent even if it may already have closed the channel.
  char failed = TRUE;
  ssize_t written;
  do {
    written = send(startup_fd, &failed, sizeof(failed), MSG_NOSIGNAL);
  } while (written < 0 && errno == EINTR);
}

static void print_error_log(int fd) {
  // Read diagnostics through the descriptor, even if the path was removed.
  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror("stderr");

    return;
  }

  char buffer[BUFSIZ];
  while (TRUE) {
    ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length == 0) {
      return;
    }
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("stderr");

      return;
    }
    if (fwrite(buffer, 1, (size_t)length, stderr) != (size_t)length) {
      return;
    }
  }
}

static void report_output_failure(pid_t pid, const char* stdout_path,
                                  const char* stderr_path) {
  perror("report");
  // Keep the launch details available when stdout cannot carry the report.
  fprintf(stderr,
          "The command may already be running.\n"
          "PID: %jd\n"
          "stdout: %s\n"
          "stderr: %s\n",
          (intmax_t)pid, stdout_path, stderr_path);
}

int main(int argc, char* argv[]) {
  const char* stdin_path = DEV_NULL_PATH;
  const char* stdout_path = NULL;
  const char* stderr_path = NULL;
  const char* pid_path = NULL;

  int opt;
  int options_present = FALSE;
  int separator_present = FALSE;
  int next_argument = optind;

  // Suppress getopt output so we print our own errors.
  // The colon makes a missing value return ':' instead of '?'.
  opterr = FALSE;
  while ((opt = getopt_long(argc, argv, "+:", long_options, NULL)) != -1) {
    switch (opt) {
      case 'i': {
        stdin_path = optarg;

        if (argument_is_empty(optarg)) {
          fputs("Missing an argument for the option: --stdin\n", stderr);
          print_usage(stderr);

          return EXIT_FAILURE;
        }

        break;
      }

      case 'o': {
        stdout_path = optarg;

        if (argument_is_empty(optarg)) {
          fputs("Missing an argument for the option: --stdout\n", stderr);
          print_usage(stderr);

          return EXIT_FAILURE;
        }

        break;
      }

      case 'e': {
        stderr_path = optarg;

        if (argument_is_empty(optarg)) {
          fputs("Missing an argument for the option: --stderr\n", stderr);
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
        if (!print_usage(stdout)) {
          perror("help");

          return EXIT_FAILURE;
        }
        // Check buffered output and the final close for errors.
        if (fclose(stdout) == EOF) {
          perror("help");

          return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;

      case ':':
        fprintf(stderr, "Missing an argument for the option: %s\n",
                argv[next_argument]);
        print_usage(stderr);

        return EXIT_FAILURE;

      case '?':
        // Grouped short options may leave optind on the current argument.
        // Use the index saved before getopt handled that argument.
        fprintf(stderr, "Unknown option: %s\n", argv[next_argument]);
        print_usage(stderr);

        return EXIT_FAILURE;

      default:
        fputs("Unknown option.\n", stderr);
        print_usage(stderr);

        return EXIT_FAILURE;
    }

    // Track option usage for the required separator rule.
    options_present = TRUE;
    next_argument = optind;
  }

  if (optind > next_argument) {
    if (strcmp(argv[next_argument], OPTION_SEPARATOR) == 0) {
      separator_present = TRUE;
    }
  }

  if (options_present && !separator_present) {
    const char* message =
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

  // Failed diagnostic writes must not interrupt temporary-file cleanup.
  // Restore the caller's disposition before starting the program.
  struct sigaction ignored_sigpipe = {0};
  struct sigaction original_sigpipe;
  ignored_sigpipe.sa_handler = SIG_IGN;
  if (sigemptyset(&ignored_sigpipe.sa_mask) < 0 ||
      sigaction(SIGPIPE, &ignored_sigpipe, &original_sigpipe) < 0) {
    perror("SIGPIPE");

    return EXIT_FAILURE;
  }

  if (!open_missing_standard_descriptors()) {
    return EXIT_FAILURE;
  }

  // Open stdin now so alias checks use the file the child will read.
  int fd_stdin = open(stdin_path, O_RDONLY);
  if (fd_stdin < 0) {
    perror("stdin");

    return EXIT_FAILURE;
  }

  char stdout_template[] = "/tmp/stdout.XXXXXX";
  char stderr_template[] = "/tmp/stderr.XXXXXX";
  int stdout_is_temporary = FALSE;
  int stderr_is_temporary = FALSE;
  int failure_report_fd = -1;

  // Preserve existing contents until all stream and PID aliases are checked.
  // New stdout.
  int fd_stdout;
  if (stdout_path == NULL) {
    fd_stdout = mkstemp(stdout_template);
    if (fd_stdout < 0) {
      perror("stdout");

      goto startup_failed;
    }
    stdout_path = stdout_template;
    stdout_is_temporary = TRUE;
  } else {
    fd_stdout = open(stdout_path, O_CREAT | O_WRONLY, 0600);
    if (fd_stdout < 0) {
      perror("stdout");

      goto startup_failed;
    }
  }

  // New stderr.
  int fd_stderr;
  if (stderr_path == NULL) {
    fd_stderr = mkstemp(stderr_template);
    if (fd_stderr < 0) {
      perror("stderr");

      goto startup_failed;
    }
    stderr_path = stderr_template;
    stderr_is_temporary = TRUE;
  } else {
    fd_stderr = open(stderr_path, O_CREAT | O_WRONLY, 0600);
    if (fd_stderr < 0) {
      perror("stderr");

      goto startup_failed;
    }
  }

  struct stat stdout_stat;
  if (fstat(fd_stdout, &stdout_stat) < 0) {
    perror("stdout");

    goto startup_failed;
  }

  struct stat stderr_stat;
  if (fstat(fd_stderr, &stderr_stat) < 0) {
    perror("stderr");

    goto startup_failed;
  }

  int fd_pid = -1;
  struct stat pid_stat;
  if (pid_path != NULL) {
    fd_pid = open(pid_path, O_CREAT | O_WRONLY, 0600);
    if (fd_pid < 0) {
      perror("pid");

      goto startup_failed;
    }
    if (fstat(fd_pid, &pid_stat) < 0) {
      perror("pid");

      goto startup_failed;
    }
  }

  struct stat stdin_stat;
  if (fstat(fd_stdin, &stdin_stat) < 0) {
    perror("stdin");

    goto startup_failed;
  }

  // Compare open files so hard links and symlinks cannot bypass the checks.
  // No file has been truncated yet, including the optional PID file.
  if (pid_path != NULL) {
    if (is_same_file(&pid_stat, &stdin_stat) ||
        is_same_file(&pid_stat, &stdout_stat) ||
        is_same_file(&pid_stat, &stderr_stat)) {
      fputs("PID file must differ from stdin, stdout, and stderr.\n", stderr);

      goto startup_failed;
    }
  }
  // Writing to the input file would destroy unread data before exec.
  // Devices such as /dev/null may still serve more than one stream.
  if (S_ISREG(stdin_stat.st_mode) &&
      (is_same_file(&stdin_stat, &stdout_stat) ||
       is_same_file(&stdin_stat, &stderr_stat))) {
    fputs("stdin must differ from stdout and stderr for regular files.\n",
          stderr);

    goto startup_failed;
  }

  // Reset outputs only after all file identities have been checked.
  if (!reset_regular_file(fd_stdout, &stdout_stat, "stdout")) {
    goto startup_failed;
  }
  if (!reset_regular_file(fd_stderr, &stderr_stat, "stderr")) {
    goto startup_failed;
  }

  // Share the file offset when both streams refer to the same file.
  if (is_same_file(&stdout_stat, &stderr_stat)) {
    if (dup2(fd_stdout, fd_stderr) < 0) {
      perror("stderr");

      goto startup_failed;
    }
  }

  // The final child sends its PID and waits for startup permission.
  // After permission, it reports failure or closes the channel through exec.
  int startup_channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, startup_channel) < 0) {
    perror("startup channel");

    goto startup_failed;
  }

  const int parent_startup_fd = startup_channel[0];
  const int child_startup_fd = startup_channel[1];

  // Successful exec closes the child's endpoint and wakes the parent.
  if (fcntl(child_startup_fd, F_SETFD, FD_CLOEXEC) < 0) {
    perror("startup channel");

    goto startup_failed;
  }

  // Create the intermediate process that will become the session leader.
  pid_t intermediate_pid = fork();
  if (intermediate_pid > 0) {
    close(fd_stdin);
    close(child_startup_fd);

    pid_t program_pid;
    int pid_received = receive_process_id(parent_startup_fd, &program_pid);

    // Reap the intermediate process before allowing the final child to run.
    // ECHILD also covers callers that ignore SIGCHLD.
    while (waitpid(intermediate_pid, NULL, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        break;
      }
      perror("waitpid");

      goto startup_failed;
    }
    if (!pid_received || program_pid <= 0) {
      fputs("Failed to start the command.\n", stderr);

      goto startup_failed;
    }

    if (pid_path != NULL) {
      if (!save_pid_file(fd_pid, program_pid, &pid_stat)) {
        goto startup_failed;
      }
    }

    // Allow startup after saving the PID file, if requested.
    // On failure, process exit closes the channel without permission.
    char ready = TRUE;
    ssize_t written;
    do {
      written = send(parent_startup_fd, &ready, sizeof(ready), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != sizeof(ready)) {
      perror("startup channel");

      goto startup_failed;
    }

    // EOF normally follows exec, but also occurs if the child dies.
    // A failure byte explicitly reports a setup or exec error.
    char failed;
    ssize_t received;
    do {
      received = read(parent_startup_fd, &failed, sizeof(failed));
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
      perror("startup channel");

      // The command may be running, so keep its logs and show the paths.
      fprintf(stderr, "PID: %jd\nstdout: %s\nstderr: %s\n",
              (intmax_t)program_pid, stdout_path, stderr_path);

      return EXIT_FAILURE;
    }
    close(parent_startup_fd);
    if (received > 0) {
      fputs("Failed to start the command.\n", stderr);
      if (stderr_is_temporary) {
        print_error_log(fd_stderr);
      } else if (stdout_is_temporary &&
                 is_same_file(&stdout_stat, &stderr_stat)) {
        print_error_log(fd_stdout);
      } else {
        fprintf(stderr, "Check stderr: %s\n", stderr_path);
      }

      goto startup_failed;
    }

    // Keep the logs on report errors because the command may be running.
    if (printf("PID: %jd\n"
               "stdout: %s\n"
               "stderr: %s\n",
               (intmax_t)program_pid, stdout_path, stderr_path) < 0) {
      report_output_failure(program_pid, stdout_path, stderr_path);

      return EXIT_FAILURE;
    }

    // Flush buffered report data and check the final close for errors.
    if (fclose(stdout) == EOF) {
      report_output_failure(program_pid, stdout_path, stderr_path);

      return EXIT_FAILURE;
    }

    close(fd_stdout);
    close(fd_stderr);

    return EXIT_SUCCESS;
  } else if (intermediate_pid < 0) {
    perror("fork");

    goto startup_failed;
  }

  close(parent_startup_fd);

  if (pid_path != NULL) {
    close(fd_pid);
  }

  // Create a new session before the second fork.
  if (setsid() < 0) {
    perror("setsid");
    send_process_id(child_startup_fd, 0);

    goto startup_failed;
  }

  // The final child inherits the session without becoming its leader.
  pid_t program_pid = fork();
  if (program_pid < 0) {
    perror("fork");
    send_process_id(child_startup_fd, 0);

    goto startup_failed;
  }
  if (program_pid > 0) {
    // Exit without flushing the inherited stdio buffers.
    _exit(EXIT_SUCCESS);
  }

  // The final child sends its PID before any startup status byte.
  if (!send_process_id(child_startup_fd, getpid())) {
    goto startup_failed;
  }

  // EOF means the parent failed before granting startup permission.
  char ready;
  ssize_t received;
  do {
    received = read(child_startup_fd, &ready, sizeof(ready));
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    perror("startup channel");

    goto child_startup_failed;
  }
  if (received != sizeof(ready) || ready != TRUE) {
    goto startup_failed;
  }

  // Redirect stdin.
  if (!redirect_standard_descriptor(fd_stdin, STDIN_FILENO, "stdin")) {
    goto child_startup_failed;
  }

  // Redirect stdout.
  if (!redirect_standard_descriptor(fd_stdout, STDOUT_FILENO, "stdout")) {
    goto child_startup_failed;
  }

  // Redirect stderr.
  if (!redirect_standard_descriptor(fd_stderr, STDERR_FILENO, "stderr")) {
    goto child_startup_failed;
  }

  // Close inherited files, pipes, and sockets before starting the program.
  if (!close_inherited_descriptors(child_startup_fd)) {
    goto child_startup_failed;
  }

  // Preserve the signal behavior the program would have without detach.
  if (sigaction(SIGPIPE, &original_sigpipe, NULL) < 0) {
    perror("SIGPIPE");

    goto child_startup_failed;
  }

  // Start a program in a new session on the current operating system.
  execvp(argv[optind], &argv[optind]);

  // If exec returns, protect diagnostics and cleanup from SIGPIPE again.
  int exec_error = errno;
  if (sigaction(SIGPIPE, &ignored_sigpipe, NULL) < 0) {
    goto child_startup_failed;
  }
  errno = exec_error;
  perror("exec");

child_startup_failed:
  // Finish cleanup before the parent starts reading the diagnostic log.
  failure_report_fd = child_startup_fd;

startup_failed:
  // Remove only files created by mkstemp, never user-selected paths.
  // ENOENT is harmless if the other process has already removed them.
  if (stdout_is_temporary) {
    if (unlink(stdout_template) < 0 && errno != ENOENT) {
      perror(stdout_template);
    }
  }
  if (stderr_is_temporary) {
    if (unlink(stderr_template) < 0 && errno != ENOENT) {
      perror(stderr_template);
    }
  }
  // Cleanup diagnostics are complete before the parent receives the failure.
  if (failure_report_fd >= 0) {
    report_startup_failure(failure_report_fd);
  }
  return EXIT_FAILURE;
}
