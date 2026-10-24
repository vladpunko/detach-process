// -*- coding: utf-8 -*-

// Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
// Created date: 2026-10-21

#ifndef TESTS_SUPPORT_SYSCALL_OVERRIDES_H_
#define TESTS_SUPPORT_SYSCALL_OVERRIDES_H_

// Positive values are `errno` codes. These values simulate other call results.
enum fault_result {
    FAULT_EOF = 0,
    FAULT_SHORT_IO = -1,
    FAULT_BAD_PERMISSION = -2,
    FAULT_PARSER_ADVANCE = -3,
    FAULT_DIRECTORY_ENTRY = -4,
    FAULT_TERMINATE = -5,
};

#endif  // TESTS_SUPPORT_SYSCALL_OVERRIDES_H_
