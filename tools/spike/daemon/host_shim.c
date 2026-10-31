// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// libkernel's functions for the macOS smoke build (host_shim.h): each answers an error.
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define UNAVAILABLE ((int)0x80020002)
int sceKernelLoadStartModule(const char* path, size_t a, const void* b, unsigned c, void* d, int* e) {
  (void)path; (void)a; (void)b; (void)c; (void)d; (void)e; return UNAVAILABLE;
}
int sceKernelOpenEventFlag(intptr_t* flag, const char* name) { (void)flag; (void)name; return UNAVAILABLE; }
int sceKernelPollEventFlag(intptr_t flag, uint64_t bits, unsigned mode, uint64_t* result) {
  (void)flag; (void)bits; (void)mode; (void)result; return UNAVAILABLE;
}
int sceKernelAvailableFlexibleMemorySize(size_t* size) { (void)size; return UNAVAILABLE; }
int sceKernelSendNotificationRequest(int a, void* request, size_t size, int b) { (void)a; (void)request; (void)size; (void)b; return UNAVAILABLE; }
int sceKernelGetAppInfo(pid_t pid, void* info) { (void)pid; (void)info; return UNAVAILABLE; }
