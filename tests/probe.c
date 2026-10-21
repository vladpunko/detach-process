// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#define _POSIX_C_SOURCE 200809L

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

#include "tests/probe_state.h"

static int write_all(int fd, const void* buffer, size_t size) {
  const char* data = buffer;
  while (size > 0) {
    ssize_t length = write(fd, data, size);
    if (length < 0 && errno == EINTR) {
      continue;
    }
    if (length <= 0) {
      return 0;
    }
    data += length;
    size -= (size_t)length;
  }
  return 1;
}

static int write_text(int fd, const char* text) {
  return write_all(fd, text, strlen(text));
}

static int copy_stdin(void) {
  char buffer[4096];
  while (1) {
    ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (length < 0 && errno == EINTR) {
      continue;
    }
    if (length < 0) {
      return 0;
    }
    if (length == 0) {
      return write_text(STDERR_FILENO, "probe stderr\n");
    }
    if (!write_all(STDOUT_FILENO, buffer, (size_t)length)) {
      return 0;
    }
  }
}

static int is_null_input(void) {
  struct stat input;
  struct stat null_device;
  if (fstat(STDIN_FILENO, &input) < 0 || stat("/dev/null", &null_device) < 0) {
    return -1;
  }
  return input.st_dev == null_device.st_dev &&
         input.st_ino == null_device.st_ino;
}

static int extra_descriptors(int channel) {
  DIR* directory = opendir("/dev/fd");
  if (directory == NULL) {
    return -1;
  }
  int count = 0;
  while (1) {
    // Only a clean end of directory proves there were no extra handles.
    errno = 0;
    struct dirent* entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0) {
        count = -1;
      }
      break;
    }
    char* end;
    errno = 0;
    intmax_t fd = strtoimax(entry->d_name, &end, 10);
    if (errno == 0 && end != entry->d_name && *end == '\0' &&
        fd > STDERR_FILENO && fd != channel && fd != dirfd(directory)) {
      count += 1;
    }
  }
  return closedir(directory) == 0 ? count : -1;
}

static int signal_ignored(int signal) {
  struct sigaction action;
  if (sigaction(signal, NULL, &action) < 0) {
    return -1;
  }
  return action.sa_handler == SIG_IGN;
}

// Complete the requested I/O before sending state to synchronize assertions.
static int run_mode(int argc, char* argv[]) {
  const char* mode = argv[1];
  if (strcmp(mode, "stdio") == 0) {
    return copy_stdin();
  }
  if (strcmp(mode, "streams") == 0) {
    return write_text(STDOUT_FILENO, "stdout one\n") &&
           write_text(STDERR_FILENO, "stderr one\n") &&
           write_text(STDOUT_FILENO, "stdout two\n") &&
           write_text(STDERR_FILENO, "stderr two\n");
  }
  if (strcmp(mode, "arguments") == 0) {
    for (int index = 3; index < argc; index += 1) {
      // Length prefixes preserve empty arguments and embedded newlines.
      if (printf("%zu:", strlen(argv[index])) < 0 ||
          fputs(argv[index], stdout) == EOF || putchar('\n') == EOF) {
        return 0;
      }
    }
    return 1;
  }
  if (strcmp(mode, "environment") == 0 && argc == 4) {
    const char* value = getenv(argv[3]);
    return value != NULL && fputs(value, stdout) >= 0;
  }
  if (strcmp(mode, "cwd") == 0) {
    char* directory = getcwd(NULL, 0);
    if (directory == NULL) {
      return 0;
    }
    int written = fputs(directory, stdout);
    free(directory);
    return written >= 0;
  }
  if (strcmp(mode, "terminal") == 0 && argc == 4) {
    // A second fork prevents an ordinary open from acquiring this terminal.
    int fd = open(argv[3], O_RDWR);
    if (fd < 0) {
      return 0;
    }
    close(fd);
    return 1;
  }
  return strcmp(mode, "state") == 0 || strcmp(mode, "descriptor") == 0 ||
         strcmp(mode, "pid-file") == 0;
}

static int connect_to_test(const char* path) {
  int channel = socket(AF_UNIX, SOCK_STREAM, 0);
  if (channel < 0) {
    return -1;
  }
  struct sockaddr_un address = {0};
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    close(channel);
    return -1;
  }
  memcpy(address.sun_path, path, strlen(path) + 1);
  if (connect(channel, (struct sockaddr*)&address, sizeof(address)) < 0) {
    close(channel);
    return -1;
  }
  return channel;
}

static int capture_state(int channel, int argc, char* argv[],
                         ProbeState* state) {
  int terminal = open("/dev/tty", O_RDWR | O_NOCTTY);
  int has_terminal = terminal >= 0;
  if (terminal >= 0) {
    close(terminal);
  }
  sigset_t mask;
  if (sigprocmask(SIG_BLOCK, NULL, &mask) < 0) {
    return 0;
  }
  // Reading umask requires changing it briefly, then restoring it.
  mode_t creation_mask = umask(0);
  umask(creation_mask);

  // Read the PID file before reporting readiness. This verifies the launcher
  // finished writing it before allowing the command to start.
  intmax_t pid_file = -1;
  if (strcmp(argv[1], "pid-file") == 0 && argc == 4) {
    FILE* file = fopen(argv[3], "r");
    if (file == NULL) {
      return 0;
    }
    int fields = fscanf(file, "%" SCNdMAX, &pid_file);
    fclose(file);
    if (fields != 1) {
      return 0;
    }
  }
  int fd_open = -1;
  if (strcmp(argv[1], "descriptor") == 0 && argc == 4) {
    char* end;
    errno = 0;
    intmax_t fd = strtoimax(argv[3], &end, 10);
    if (errno != 0 || end == argv[3] || *end != '\0' || fd < 0 ||
        fd > INT_MAX) {
      return 0;
    }
    // Test the inherited descriptor number itself. The directory scan below
    // also catches leaked handles whose numbers the test did not predict.
    fd_open = fcntl((int)fd, F_GETFD) >= 0;
  }

  state->pid = getpid();
  state->parent_pid = getppid();
  state->session_id = getsid(0);
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
  state->descriptor_open = fd_open;
  state->pid_file = pid_file;
  return state->session_id >= 0 && state->extra_descriptors >= 0 &&
         state->stdin_is_null >= 0 && state->sigpipe_ignored >= 0 &&
         state->sigchld_ignored >= 0 && state->sighup_ignored >= 0;
}

static int wait_for_test(int channel) {
  // Stay alive until the test closes the socket, answering liveness checks.
  char command;
  ssize_t length;
  while ((length = read(channel, &command, 1)) != 0) {
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 0;
    }
    if (!write_all(channel, &command, 1)) {
      return 0;
    }
  }
  return 1;
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return EXIT_FAILURE;
  }
  // Bound the helper's lifetime even if a regression blocks its scenario or
  // prevents it from reaching the control socket used for normal cleanup.
  alarm(15);
  if (!run_mode(argc, argv) || fflush(NULL) == EOF) {
    return EXIT_FAILURE;
  }

  int channel = connect_to_test(argv[2]);
  if (channel < 0) {
    return EXIT_FAILURE;
  }
  // The socket is the readiness barrier, then remains open for liveness.
  ProbeState state = {0};
  int success = capture_state(channel, argc, argv, &state) &&
                write_all(channel, &state, sizeof(state)) &&
                wait_for_test(channel);
  close(channel);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
