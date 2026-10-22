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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define OPTION_SEPARATOR "--"
#define DEV_NULL_PATH "/dev/null"
#define DEV_FD_PATH "/dev/fd"

struct options {
  const char* stdin_path;
  const char* stdout_path;
  const char* stderr_path;
  const char* pid_path;
  char** command;
};

struct opened_file {
  const char* path;
  int fd;
  struct stat info;
  bool temporary;
};

struct launch_files {
  struct opened_file input;
  struct opened_file output;
  struct opened_file errors;
  struct opened_file pid;
  char stdout_template[sizeof("/tmp/stdout.XXXXXX")];
  char stderr_template[sizeof("/tmp/stderr.XXXXXX")];
};

struct signal_state {
  struct sigaction original_sigpipe;
  struct sigaction ignored_sigpipe;
};

enum parse_result { PARSE_ERROR, PARSE_HELP, PARSE_COMMAND };

static const struct option long_options[] = {
    {"help", no_argument, NULL, 'h'},
    {"stdin", required_argument, NULL, 'i'},
    {"stdout", required_argument, NULL, 'o'},
    {"stderr", required_argument, NULL, 'e'},
    {"pid", required_argument, NULL, 'p'},
    {NULL, 0, NULL, 0},
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
      "  This is required when options are used.\n";
  return fprintf(stream, message, OPTION_SEPARATOR) >= 0;
}

// Recognizes missing or empty values before they can become file paths.
static bool argument_is_empty(const char* argument) {
  return argument == NULL || argument[0] == '\0';
}

// Validates and stores one path option before startup can change any files.
static bool set_file_option(const char** path, const char* value,
                            const char* name) {
  if (argument_is_empty(value)) {
    fprintf(stderr, "Missing an argument for the option: %s\n", name);
    print_usage(stderr);
    return false;
  }
  *path = value;
  return true;
}

// Separates launcher options from the command while preserving its arguments
// for direct execution. Invalid input is rejected before opening files.
static enum parse_result parse_arguments(int argc, char* argv[],
                                         struct options* options) {
  bool options_present = false;
  int next_argument = optind;

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
                argv[next_argument]);
        print_usage(stderr);
        return PARSE_ERROR;
      case '?':
        // Grouped short options can leave optind on the current argument.
        fprintf(stderr, "Unknown option: %s\n", argv[next_argument]);
        print_usage(stderr);
        return PARSE_ERROR;
      default:
        fputs("Unknown option.\n", stderr);
        print_usage(stderr);
        return PARSE_ERROR;
    }
    options_present = true;
    next_argument = optind;
  }

  bool separator_present = optind > next_argument &&
                           strcmp(argv[next_argument], OPTION_SEPARATOR) == 0;
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
  // fclose also checks errors from flushing buffered help text.
  if (!print_usage(stdout) || fclose(stdout) == EOF) {
    perror("help");
    return false;
  }
  return true;
}

// Compares opened file identities so different paths, hard links, and symlinks
// cannot hide a conflicting stream or PID file.
static bool is_same_file(const struct stat* first, const struct stat* second) {
  return first->st_dev == second->st_dev && first->st_ino == second->st_ino;
}

// Truncates and rewinds a regular file for fresh output while leaving pipes
// and devices usable.
static bool reset_regular_file(int fd, const struct stat* file_stat,
                               const char* label) {
  if (!S_ISREG(file_stat->st_mode)) {
    return true;
  }
  if (ftruncate(fd, 0) < 0) {
    perror(label);
    return false;
  }
  // Truncation keeps the offset. On macOS, /dev/fd can inherit a nonzero one.
  // Rewind explicitly so new output cannot start with a hole of zero bytes.
  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror(label);
    return false;
  }
  return true;
}

// Opens a selected log or creates a private temporary one. Existing contents
// are preserved until all file aliases have been checked.
static bool open_log_file(struct opened_file* file, char* temporary_path,
                          const char* label) {
  bool temporary = file->path == NULL;
  if (temporary) {
    file->fd = mkstemp(temporary_path);
  } else {
    // Alias checks must finish before any existing file is truncated.
    file->fd = open(file->path, O_CREAT | O_WRONLY, 0600);
  }
  if (file->fd < 0) {
    perror(label);
    return false;
  }
  file->temporary = temporary;
  if (temporary) {
    file->path = temporary_path;
  }
  return true;
}

// Captures the identity of the actual opened file for alias and type checks.
static bool inspect_file(struct opened_file* file, const char* label) {
  if (fstat(file->fd, &file->info) < 0) {
    perror(label);
    return false;
  }
  return true;
}

// Rejects stream and PID file conflicts before truncation could destroy
// unread input or overwrite launch metadata.
static bool validate_file_aliases(const struct launch_files* files) {
  const struct stat* input = &files->input.info;
  const struct stat* output = &files->output.info;
  const struct stat* errors = &files->errors.info;
  const struct stat* pid = &files->pid.info;

  if (files->pid.path != NULL &&
      (is_same_file(pid, input) || is_same_file(pid, output) ||
       is_same_file(pid, errors))) {
    fputs("PID file must differ from stdin, stdout, and stderr.\n", stderr);
    return false;
  }
  // Reject regular input aliases while allowing shared devices like /dev/null.
  if (S_ISREG(input->st_mode) &&
      (is_same_file(input, output) || is_same_file(input, errors))) {
    fputs("stdin must differ from stdout and stderr for regular files.\n",
          stderr);
    return false;
  }
  return true;
}

// Resets validated logs and shares their offset when stdout and stderr
// refer to the same file, preventing overlapping writes.
static bool prepare_output_files(const struct launch_files* files) {
  if (!reset_regular_file(files->output.fd, &files->output.info, "stdout") ||
      !reset_regular_file(files->errors.fd, &files->errors.info, "stderr")) {
    return false;
  }
  // Share the offset so writes through one stream cannot overwrite the other.
  if (is_same_file(&files->output.info, &files->errors.info) &&
      dup2(files->output.fd, files->errors.fd) < 0) {
    perror("stderr");
    return false;
  }
  return true;
}

// Opens the input, logs, and optional PID file and records their identities.
// It leaves existing contents intact for validation.
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
  if (files->pid.path != NULL) {
    files->pid.fd = open(files->pid.path, O_CREAT | O_WRONLY, 0600);
    if (files->pid.fd < 0) {
      perror("pid");
      return false;
    }
    if (!inspect_file(&files->pid, "pid")) {
      return false;
    }
  }
  if (!inspect_file(&files->input, "stdin")) {
    return false;
  }
  return true;
}

// Removes only a log created by this launch, tolerating cleanup already
// performed by another startup process.
static void remove_temporary_file(const struct opened_file* file) {
  if (!file->temporary) {
    return;
  }
  // Another startup process may already have removed the same temporary log.
  if (unlink(file->path) < 0 && errno != ENOENT) {
    perror(file->path);
  }
}

// Cleans up generated logs and returns failure for an aborted startup.
// User-selected files are kept for inspection.
static int fail_startup(const struct launch_files* files) {
  remove_temporary_file(&files->output);
  remove_temporary_file(&files->errors);
  return EXIT_FAILURE;
}

// Writes the final command PID through the already validated descriptor
// before the launcher permits the command to execute.
static bool save_pid_file(int fd, pid_t pid, const struct stat* file_stat) {
  if (!reset_regular_file(fd, file_stat, "pid")) {
    return false;
  }
  if (dprintf(fd, "%jd\n", (intmax_t)pid) < 0) {
    perror("pid");
    close(fd);
    return false;
  }
  if (close(fd) < 0) {
    perror("pid");
    return false;
  }
  return true;
}

// Fills closed standard descriptor slots so internal files and sockets
// cannot accidentally take the place of stdin, stdout, or stderr.
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
static bool redirect_standard_descriptor(int source_fd, int target_fd,
                                         const char* label) {
  if (dup2(source_fd, target_fd) < 0) {
    perror(label);
    return false;
  }
  if (source_fd != target_fd) {
    close(source_fd);
  }
  return true;
}

// Converts /dev/fd entry names to inherited descriptor numbers, returning -1
// for invalid entries and standard streams so they are never closed.
static int parse_descriptor_name(const char* name) {
  char* end;
  errno = 0;
  intmax_t descriptor = strtoimax(name, &end, 10);
  if (errno != 0 || end == name || *end != '\0' ||
      descriptor <= STDERR_FILENO || descriptor > INT_MAX) {
    return -1;
  }
  return (int)descriptor;
}

// Closes inherited handles before exec, preserving redirected streams and
// the startup socket. Enumeration also finds handles above a lowered limit.
static bool close_inherited_descriptors(int startup_fd) {
  DIR* directory = opendir(DEV_FD_PATH);
  if (directory == NULL) {
    perror(DEV_FD_PATH);
    return false;
  }

  while (true) {
    // If readdir returns NULL, errno tells us whether reading failed.
    // Clear it first so an earlier error cannot look like a read failure.
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

    int fd = parse_descriptor_name(entry->d_name);
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
static bool prepare_command_streams(const struct launch_files* files,
                                    int startup_fd) {
  return redirect_standard_descriptor(files->input.fd, STDIN_FILENO, "stdin") &&
         redirect_standard_descriptor(files->output.fd, STDOUT_FILENO,
                                      "stdout") &&
         redirect_standard_descriptor(files->errors.fd, STDERR_FILENO,
                                      "stderr") &&
         close_inherited_descriptors(startup_fd);
}

// Temporarily ignores SIGPIPE so broken diagnostic pipes cannot interrupt
// cleanup, saving the original disposition for the command.
static bool ignore_sigpipe(struct signal_state* signals) {
  signals->ignored_sigpipe.sa_handler = SIG_IGN;
  if (sigemptyset(&signals->ignored_sigpipe.sa_mask) < 0 ||
      sigaction(SIGPIPE, &signals->ignored_sigpipe,
                &signals->original_sigpipe) < 0) {
    perror("SIGPIPE");
    return false;
  }
  return true;
}

// Sends a complete command PID before status bytes enter the startup socket,
// retrying interruptions and partial transfers.
static bool send_process_id(int fd, pid_t pid) {
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
    ssize_t length = read(fd, data + received, sizeof(*pid) - received);
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
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

// Notifies the launcher after child diagnostics and cleanup are complete,
// allowing it to distinguish a reported failure from exec closing the socket.
static void report_startup_failure(int startup_fd) {
  char failed = true;
  ssize_t written;
  do {
    written = send(startup_fd, &failed, sizeof(failed), MSG_NOSIGNAL);
  } while (written < 0 && errno == EINTR);
}

// Releases the final child only after the launcher has reaped the intermediate
// process and saved the optional PID file.
static bool allow_command_start(int fd) {
  char ready = true;
  ssize_t written;
  do {
    written = send(fd, &ready, sizeof(ready), MSG_NOSIGNAL);
  } while (written < 0 && errno == EINTR);
  if (written != sizeof(ready)) {
    perror("startup channel");
    return false;
  }
  return true;
}

// Reads one startup permission or status byte, retrying interruptions while
// keeping EOF distinct from a communication error.
static ssize_t read_startup_byte(int fd, char* value) {
  ssize_t received;
  do {
    received = read(fd, value, sizeof(*value));
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    perror("startup channel");
  }
  return received;
}

// Waits for the session-creating child so it cannot remain a zombie,
// including when the caller ignores SIGCHLD.
static bool reap_intermediate_process(pid_t pid) {
  while (waitpid(pid, NULL, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    // A caller that ignores SIGCHLD can have its children reaped by the OS.
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

// Reports a failed command setup and exposes its diagnostics, either by
// reading a temporary log or naming the selected stderr file.
static void print_startup_error(const struct launch_files* files) {
  fputs("Failed to start the command.\n", stderr);
  if (files->errors.temporary) {
    print_error_log(files->errors.fd);
  } else if (files->output.temporary &&
             is_same_file(&files->output.info, &files->errors.info)) {
    // The explicitly selected stderr may alias an unlinked temporary stdout.
    print_error_log(files->output.fd);
  } else {
    fprintf(stderr, "Check stderr: %s\n", files->errors.path);
  }
}

// Attempts to preserve launch details on stderr when the normal report
// fails, because the command may already be running.
static void report_output_failure(pid_t pid, const struct launch_files* files) {
  perror("report");
  fprintf(stderr,
          "The command may already be running.\n"
          "PID: %jd\nstdout: %s\nstderr: %s\n",
          (intmax_t)pid, files->output.path, files->errors.path);
}

// Writes and flushes the public PID and log-path report, detecting output
// errors without deleting logs from an already started job.
static bool print_launch_report(pid_t pid, const struct launch_files* files) {
  // fclose checks the final buffered write as well as closing the stream.
  if (printf("PID: %jd\nstdout: %s\nstderr: %s\n", (intmax_t)pid,
             files->output.path, files->errors.path) < 0 ||
      fclose(stdout) == EOF) {
    report_output_failure(pid, files);
    return false;
  }
  return true;
}

// Coordinates startup from the original process: receives the PID, grants
// execution, and reports the outcome while preserving possibly active logs.
static int run_launcher(pid_t intermediate_pid, int startup_fd,
                        int child_startup_fd,
                        const struct launch_files* files) {
  close(files->input.fd);
  // Our copy must close so exec in the command can produce EOF on this socket.
  close(child_startup_fd);

  pid_t command_pid;
  bool pid_received = receive_process_id(startup_fd, &command_pid);
  if (!reap_intermediate_process(intermediate_pid)) {
    return fail_startup(files);
  }
  if (!pid_received || command_pid <= 0) {
    fputs("Failed to start the command.\n", stderr);
    return fail_startup(files);
  }
  if (files->pid.path != NULL &&
      !save_pid_file(files->pid.fd, command_pid, &files->pid.info)) {
    return fail_startup(files);
  }
  if (!allow_command_start(startup_fd)) {
    return fail_startup(files);
  }

  // EOF normally follows exec, but can also mean the child terminated early.
  // Only a received failure byte proves a reported setup or exec failure.
  char failed;
  ssize_t received = read_startup_byte(startup_fd, &failed);
  if (received < 0) {
    fprintf(stderr, "PID: %jd\nstdout: %s\nstderr: %s\n", (intmax_t)command_pid,
            files->output.path, files->errors.path);
    return EXIT_FAILURE;  // The command may be running: preserve its logs.
  }
  close(startup_fd);
  if (received > 0) {
    print_startup_error(files);
    return fail_startup(files);
  }
  if (!print_launch_report(command_pid, files)) {
    return EXIT_FAILURE;  // Reporting failure must not remove the job's logs.
  }
  close(files->output.fd);
  close(files->errors.fd);
  return EXIT_SUCCESS;
}

// Runs the final child handshake, prepares its execution state, and replaces
// it with the command. Reported failures finish cleanup before notifying
// the launcher.
static int run_detached_command(char* const command[], int startup_fd,
                                const struct launch_files* files,
                                const struct signal_state* signals) {
  if (!send_process_id(startup_fd, getpid())) {
    return fail_startup(files);
  }

  char ready;
  ssize_t received = read_startup_byte(startup_fd, &ready);
  if (received < 0) {
    goto failed;
  }
  // EOF or an invalid byte means the parent never permitted execution.
  if (received != sizeof(ready) || ready != true) {
    return fail_startup(files);
  }
  if (!prepare_command_streams(files, startup_fd)) {
    goto failed;
  }
  if (sigaction(SIGPIPE, &signals->original_sigpipe, NULL) < 0) {
    perror("SIGPIPE");
    goto failed;
  }

  execvp(command[0], command);

  // Failed exec returns here. Protect diagnostics from SIGPIPE again.
  int exec_error = errno;
  if (sigaction(SIGPIPE, &signals->ignored_sigpipe, NULL) < 0) {
    goto failed;
  }
  errno = exec_error;
  perror("exec");

failed:
  // Finish cleanup diagnostics before the launcher starts reading its log.
  fail_startup(files);
  report_startup_failure(startup_fd);
  return EXIT_FAILURE;
}

// Creates a terminal-independent session and forks the final command so
// it does not become a session leader or acquire a controlling terminal.
static int run_session_child(char* const command[], int parent_startup_fd,
                             int startup_fd, const struct launch_files* files,
                             const struct signal_state* signals) {
  // Retaining the parent's endpoint would hide its exit from the command.
  close(parent_startup_fd);
  if (files->pid.path != NULL) {
    close(files->pid.fd);
  }
  if (setsid() < 0) {
    perror("setsid");
    send_process_id(startup_fd, 0);
    return fail_startup(files);
  }

  // Fork again so the command cannot acquire a controlling terminal by open.
  pid_t command_pid = fork();
  if (command_pid < 0) {
    perror("fork");
    send_process_id(startup_fd, 0);
    return fail_startup(files);
  }
  if (command_pid > 0) {
    _exit(EXIT_SUCCESS);  // Do not flush inherited stdio buffers.
  }
  return run_detached_command(command, startup_fd, files, signals);
}

// Creates the close-on-exec startup channel and dispatches the original
// launcher and session-creating child into their separate process roles.
static int launch_command(char* const command[],
                          const struct launch_files* files,
                          const struct signal_state* signals) {
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0) {
    perror("startup channel");
    return fail_startup(files);
  }
  int parent_fd = channel[0];
  int child_fd = channel[1];
  if (fcntl(child_fd, F_SETFD, FD_CLOEXEC) < 0) {
    perror("startup channel");
    return fail_startup(files);
  }

  pid_t intermediate_pid = fork();
  if (intermediate_pid < 0) {
    perror("fork");
    return fail_startup(files);
  }
  if (intermediate_pid == 0) {
    return run_session_child(command, parent_fd, child_fd, files, signals);
  }
  return run_launcher(intermediate_pid, parent_fd, child_fd, files);
}

// Runs the startup stages in order: parse options, protect diagnostics,
// prepare and validate files, then launch the detached command.
int main(int argc, char* argv[]) {
  struct options options = {.stdin_path = DEV_NULL_PATH};
  enum parse_result parsed = parse_arguments(argc, argv, &options);
  if (parsed == PARSE_HELP) {
    return show_help() ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (parsed == PARSE_ERROR) {
    return EXIT_FAILURE;
  }

  struct signal_state signals = {0};
  if (!ignore_sigpipe(&signals) || !open_missing_standard_descriptors()) {
    return EXIT_FAILURE;
  }
  struct launch_files files = {
      .input = {.path = options.stdin_path, .fd = -1},
      .output = {.path = options.stdout_path, .fd = -1},
      .errors = {.path = options.stderr_path, .fd = -1},
      .pid = {.path = options.pid_path, .fd = -1},
      .stdout_template = "/tmp/stdout.XXXXXX",
      .stderr_template = "/tmp/stderr.XXXXXX",
  };
  if (!open_launch_files(&files) || !validate_file_aliases(&files) ||
      !prepare_output_files(&files)) {
    return fail_startup(&files);
  }
  return launch_command(options.command, &files, &signals);
}
