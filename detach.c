// Copyright 2020 (c) Vladislav Punko <iam.vlad.punko@gmail.com>

#include <unistd.h>

int main(int argc, char **argv) {
  // Create a new process.
  if (fork()) return 0;

  close(0);
  close(1);
  close(2);

  // Create a new session.
  setsid();

  // Start a program in a new session on the current operating system.
  execvp(argv[1], &argv[1]);

  return -1;
}
