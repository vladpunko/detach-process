// -*- coding: utf-8 -*-

// Copyright 2020 (c) Vladislav Punko <iam.vlad.punko@gmail.com>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        perror("insufficient number of arguments");

        return EXIT_FAILURE;
    }

    // Create a new process.
    pid_t pid = fork();
    if (pid > 0) {
        return EXIT_SUCCESS;
    } else if (pid < 0) {
        perror("fork");

        return EXIT_FAILURE;
    }

    // Create a new session.
    setsid();

    // New stdin.
    int fd_stdin = open("/dev/null", O_RDONLY);
    if (fd_stdin < 0) {
        perror("stdin");

        return EXIT_FAILURE;
    }
    dup2(fd_stdin, STDIN_FILENO);
    if (fd_stdin > STDIN_FILENO) {
        close(fd_stdin);
    }

    // New stdout.
    FILE *tmp_out = tmpfile();
    if (!tmp_out) {
        perror("stdout");

        return EXIT_FAILURE;
    }
    dup2(fileno(tmp_out), STDOUT_FILENO);

    // New stderr.
    FILE *tmp_err = tmpfile();
    if (!tmp_err) {
        perror("stderr");

        return EXIT_FAILURE;
    }
    dup2(fileno(tmp_err), STDERR_FILENO);

    // Start a program in a new session on the current operating system.
    execvp(argv[1], &argv[1]);

    return EXIT_FAILURE;
}
