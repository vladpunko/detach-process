// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef DETACH_PROCESS_TESTS_PROBE_STATE_H_
#define DETACH_PROCESS_TESTS_PROBE_STATE_H_

#include <stdint.h>

// The probe and tests are built together and exchange this record locally.
// Named, fixed-width fields keep both C and C++ readers on the same layout.
typedef struct ProbeState {
  int64_t pid;
  int64_t parent_pid;
  int64_t session_id;
  int64_t process_group;
  int64_t has_terminal;
  // Standard streams and the control socket are excluded from this count.
  int64_t extra_descriptors;
  int64_t stdin_is_null;
  int64_t sigpipe_ignored;
  int64_t sigchld_ignored;
  int64_t sighup_ignored;
  int64_t sigpipe_blocked;
  int64_t sigusr1_blocked;
  int64_t creation_mask;
  // These two fields are -1 unless their respective probe mode is selected.
  int64_t descriptor_open;
  int64_t pid_file;
} ProbeState;

#endif  // DETACH_PROCESS_TESTS_PROBE_STATE_H_
