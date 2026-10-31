// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "quickjs.h"
}

// The daemon's JavaScript: QuickJS without Embedded React. The globals match what the app's script sees
// (the same intrinsics as er_js_new_context, console, timers, performance.now and Date.now), so the
// app's modules run unchanged, but timers follow the system clock instead of rendered frames.
namespace js {
// `log` receives each console line and uncaught error, without a newline.
bool start(std::size_t heap_bytes, std::size_t stack_bytes, void (*log)(const char* line));
JSContext* context();
// Evaluates a classic script and runs the microtasks it queued; false with the error's message and stack.
bool evaluate(const char* source, std::size_t length, const char* name, std::string& error);
// Runs the timers that are due and the microtasks they queue. Milliseconds until the next one, or -1.
int run_timers();
// Runs queued microtasks; call after any native call into JS.
void drain();
// Logs and clears the pending exception, naming where it was thrown.
void report(const char* where);
// The message and stack of the pending exception, which it clears.
std::string take_exception();
// Frees every timer, the context and the runtime.
void stop();
} // namespace js
