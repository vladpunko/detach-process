// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_PROBE_STATE_H_
#define TESTS_SUPPORT_PROBE_STATE_H_

#include <stdint.h>

// Shares process state between the C probe and C++ tests on the same host.
// Receiving the full record confirms the probe has flushed its output.
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

#endif  // TESTS_SUPPORT_PROBE_STATE_H_
