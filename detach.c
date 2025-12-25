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

    // New stdout.
    char stdout_path[] = "/tmp/stdout.XXXXXX";
    int fd_stdout = mkstemp(stdout_path);
    if (fd_stdout < 0) {
        perror("stdout");

        return EXIT_FAILURE;
    }

    // New stderr.
    char stderr_path[] = "/tmp/stderr.XXXXXX";
    int fd_stderr = mkstemp(stderr_path);
    if (fd_stderr < 0) {
        perror("stderr");

        return EXIT_FAILURE;
    }

    // Create a new process.
    pid_t pid = fork();
    if (pid > 0) {
        printf("PID: %d\n", pid);
        printf("stdout: %s\n", stdout_path);
        printf("stderr: %s\n", stderr_path);

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
    int fd_stdin = open("/dev/null", O_RDONLY);
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
    execvp(argv[1], &argv[1]);
    perror("exec");

    return EXIT_FAILURE;
}
