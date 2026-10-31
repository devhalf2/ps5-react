// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// The macOS smoke build of daemon_spike.c: the download, checkpoint, range server and logging run as on
// the console; every Sony library lookup fails and libkernel's calls answer an error.
#pragma once
#include <stdint.h>
#include <sys/types.h>

#define pthread_set_name_np(thread, name) ((void)(thread), (void)(name))
static inline int kernel_dynlib_handle(pid_t pid, const char* name, uint32_t* handle) { (void)pid; (void)name; (void)handle; return -1; }
static inline intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char* name) { (void)pid; (void)handle; (void)name; return 0; }
static inline uint32_t kernel_get_fw_version(void) { return 0; }
#define PAGE_SIZE 4096
