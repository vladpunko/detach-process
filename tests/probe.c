// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t size) {
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
            return write_all(STDERR_FILENO, "probe stderr\n", 13);
        }
        if (!write_all(STDOUT_FILENO, buffer, (size_t)length)) {
            return 0;
        }
    }
}

static int is_null_input(void) {
    struct stat input;
    struct stat null_device;
    if (fstat(STDIN_FILENO, &input) < 0 ||
        stat("/dev/null", &null_device) < 0) {
        return -1;
    }
    return input.st_dev == null_device.st_dev &&
           input.st_ino == null_device.st_ino;
}

static int extra_descriptors(int channel) {
    DIR *directory = opendir("/dev/fd");
    if (directory == NULL) {
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        intmax_t fd = strtoimax(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' &&
            fd > STDERR_FILENO && fd != channel && fd != dirfd(directory)) {
            count += 1;
        }
    }
    closedir(directory);
    return count;
}

static int signal_ignored(int signal) {
    struct sigaction action;
    if (sigaction(signal, NULL, &action) < 0) {
        return -1;
    }
    return action.sa_handler == SIG_IGN;
}

static int run_mode(int argc, char *argv[]) {
    const char *mode = argv[1];
    if (strcmp(mode, "stdio") == 0) {
        return copy_stdin();
    }
    if (strcmp(mode, "streams") == 0) {
        return write_all(STDOUT_FILENO, "stdout one\n", 11) &&
               write_all(STDERR_FILENO, "stderr one\n", 11) &&
               write_all(STDOUT_FILENO, "stdout two\n", 11) &&
               write_all(STDERR_FILENO, "stderr two\n", 11);
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
        const char *value = getenv(argv[3]);
        return value != NULL && fputs(value, stdout) >= 0;
    }
    if (strcmp(mode, "cwd") == 0) {
        char *directory = getcwd(NULL, 0);
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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return EXIT_FAILURE;
    }
    // A failed test cannot leave this detached helper alive indefinitely.
    alarm(15);
    if (!run_mode(argc, argv) || fflush(NULL) == EOF) {
        return EXIT_FAILURE;
    }

    int channel = socket(AF_UNIX, SOCK_STREAM, 0);
    if (channel < 0) {
        return EXIT_FAILURE;
    }
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (strlen(argv[2]) >= sizeof(address.sun_path)) {
        return EXIT_FAILURE;
    }
    memcpy(address.sun_path, argv[2], strlen(argv[2]) + 1);
    if (connect(channel, (struct sockaddr *)&address, sizeof(address)) < 0) {
        return EXIT_FAILURE;
    }

    int terminal = open("/dev/tty", O_RDWR | O_NOCTTY);
    int has_terminal = terminal >= 0;
    if (terminal >= 0) {
        close(terminal);
    }
    sigset_t mask;
    if (sigprocmask(SIG_BLOCK, NULL, &mask) < 0) {
        return EXIT_FAILURE;
    }
    mode_t creation_mask = umask(0);
    umask(creation_mask);

    intmax_t pid_file = -1;
    if (strcmp(argv[1], "pid-file") == 0 && argc == 4) {
        FILE *file = fopen(argv[3], "r");
        if (file == NULL) {
            return EXIT_FAILURE;
        }
        int fields = fscanf(file, "%" SCNdMAX, &pid_file);
        fclose(file);
        if (fields != 1) {
            return EXIT_FAILURE;
        }
    }
    int fd_open = -1;
    if (strcmp(argv[1], "descriptor") == 0 && argc == 4) {
        fd_open = fcntl(atoi(argv[3]), F_GETFD) >= 0;
    }
    if (dprintf(channel,
                "pid %jd\nppid %jd\nsid %jd\npgrp %jd\n"
                "terminal %d\nextra_descriptors %d\nstdin_is_null %d\n"
                "sigpipe_ignored %d\nsigchld_ignored %d\nsighup_ignored %d\n"
                "sigpipe_blocked %d\nsigusr1_blocked %d\numask %u\n"
                "fd_open %d\npid_file %jd\n\n",
                (intmax_t)getpid(), (intmax_t)getppid(), (intmax_t)getsid(0),
                (intmax_t)getpgrp(), has_terminal, extra_descriptors(channel),
                is_null_input(), signal_ignored(SIGPIPE),
                signal_ignored(SIGCHLD), signal_ignored(SIGHUP),
                sigismember(&mask, SIGPIPE), sigismember(&mask, SIGUSR1),
                (unsigned int)creation_mask, fd_open, pid_file) < 0) {
        return EXIT_FAILURE;
    }

    // Stay alive until the test closes the socket, answering liveness checks.
    char command;
    ssize_t length;
    while ((length = read(channel, &command, 1)) != 0) {
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            return EXIT_FAILURE;
        }
        if (!write_all(channel, &command, 1)) {
            return EXIT_FAILURE;
        }
    }
    close(channel);
    return EXIT_SUCCESS;
}
