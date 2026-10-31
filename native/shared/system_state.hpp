// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Console state (ABI v11): rest mode, resume and the game running beside the app, watched on a
// thread of its own and read by JavaScript (`system`) and by native services. Self-contained: a host
// installs `ps5_react_system_api` and calls system_state::stop() on exit; the platform supplies the
// probes declared in system_platform below (native/ps5/system_platform.cpp, native/desktop/...).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace system_state {
struct Event {
  std::string state;    // 'sleeping' | 'resumed' | 'game-started' | 'game-ended'
  double at = 0;        // wall clock, ms since the Unix epoch
  std::string source;   // 'event' | 'state-flag' | 'sigcont' | 'gap' | 'game-poll' | 'simulated'
  std::string title_id; // game-started/game-ended
  double paused_ms = -1; // resumed after a gap: how long the process did not run
};

// Starts the watcher thread; repeated calls do nothing. False when the thread cannot start.
bool start();
// Stops and joins the watcher; start() may run it again.
void stop();
// Events since the last poll, oldest first; at most kMaxQueued are kept.
std::vector<Event> poll();
constexpr std::size_t kMaxQueued = 32;

// For the network and archive services; any thread, lock-free.
// Between a sleep signal (BEFORE_SLEEP, SUSPEND_ON_GOING, MAIN_ON_STANDBY) and the next resume.
bool suspended();
// A game other than this app is running (best effort; see system_platform::running_game).
bool game_running();
// Monotonic µs of the last resume, or 0: connections that crossed it may be dead.
std::int64_t resumed_at_us();

// The running game's title ID ("PPSA01234"), or empty.
std::string running_game();
// While on, the watcher resets the auto power-down timer every kPowerTimerResetUs.
void keep_awake(bool enabled);
constexpr std::int64_t kPowerTimerResetUs = 60 * 1000000;

// Test hook: feeds a state as if the platform had reported it (desktop preview, self-tests).
// title_id names the game for 'game-started'. False for an unknown state.
bool simulate(const char* state, const char* title_id);
} // namespace system_state

// Rich system toasts (notifications_api.cpp).
namespace toast {
// The sceNotificationSend JSON for a Downloads-channel toast. bundleName comes from id, so a later
// toast with the same id replaces this one; icon_url is a console path or http(s) URL, or empty for
// the kind's predefined icon. kind: 'info' | 'done' | 'failed' | 'attention'. created_ms: wall clock.
std::string json(const char* id, const char* title, const char* body, const char* icon_url, const char* kind,
                 double created_ms);
// Whether kind is one of the kinds above.
bool known_kind(const char* kind);
} // namespace toast

// Per platform; called on the watcher thread only (send_notification: the render thread).
namespace system_platform {
// Reports a power transition found by poll(): sleeping true for rest mode, false for resume.
using Report = void (*)(bool sleeping, const char* source);
// Resolves the platform calls and opens what poll() reads; close() releases them.
void open();
void close();
void poll(Report report);
// 1 with title_id filled when a game runs, 0 when none does, -1 when it cannot be told.
int running_game(char* title_id, std::size_t size);
// Restarts the console's auto power-down (rest mode) timer.
void reset_power_timer();
// Sends a system toast from notification JSON; false when the platform has no such call.
bool send_notification(const char* json);
// The host's log (PS5: the kernel log through async_log; desktop: stdout). Any thread.
void log(const char* line);
} // namespace system_platform

// ABI v11: system.poll() → [{state, at, source, titleId?, pausedMs?}], system.runningGame() →
// {titleId} | null, system.simulate(state, titleId) → boolean.
JSValue ps5_react_system_api(JSContext* ctx);
// ABI v11: notifications.send(id, title, body, icon, kind) → boolean.
JSValue ps5_react_notifications_api(JSContext* ctx);
