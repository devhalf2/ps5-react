// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The console-state watcher (system_state.hpp). One thread, once a second, asks the platform for
// sleep and resume signals, notices SIGCONT and gaps in its own schedule, and every few seconds
// which game runs. Events wait in a bounded queue until JavaScript polls them on its thread.
#include "system_state.hpp"
#include "thread_name.hpp"
#include "worker_thread.hpp"

#include <signal.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>

namespace {
// A pass of the watcher this long after the previous one means the process did not run: rest mode,
// or a game in front throttling background threads. Matches network.cpp's suspension_gap.
constexpr std::int64_t kGapUs = 10 * 1000000;
constexpr std::int64_t kPassUs = 1000000;
constexpr std::int64_t kGamePollUs = 3 * 1000000;
// Several sources report the same resume (the system event, the state flag, SIGCONT, the gap):
// a resume within this long of the previous one is the same one.
constexpr std::int64_t kResumeWindowUs = 15 * 1000000;

std::int64_t clock_us(clockid_t clock) {
  timespec now{};
  clock_gettime(clock, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

std::mutex mutex;
std::condition_variable wake;
bool stopping = false;
bool running = false;
WorkerThread worker;
std::deque<system_state::Event> queue;
std::string game;

std::atomic<bool> asleep{false};
std::atomic<bool> game_flag{false};
std::atomic<std::int64_t> resumed_us{0};
std::atomic<bool> keeping_awake{false};
std::atomic<bool> continued{false};
struct sigaction previous_sigcont {};

void sigcont(int) { continued.store(true, std::memory_order_relaxed); }

// Holds the mutex.
void push(const char* state, const char* source, const std::string& title = {}, double paused_ms = -1) {
  if (queue.size() == system_state::kMaxQueued) queue.pop_front();
  queue.push_back({state, static_cast<double>(clock_us(CLOCK_REALTIME) / 1000), source, title, paused_ms});
}

void report_locked(bool sleeping, const char* source, double paused_ms = -1) {
  const std::int64_t now = clock_us(CLOCK_MONOTONIC);
  if (sleeping) {
    if (!asleep.exchange(true)) push("sleeping", source);
    return;
  }
  const std::int64_t last = resumed_us.load();
  if (!asleep.exchange(false) && last && now - last < kResumeWindowUs) return;
  resumed_us = now;
  push("resumed", source, {}, paused_ms);
}

void report(bool sleeping, const char* source) {
  std::lock_guard lock(mutex);
  report_locked(sleeping, source);
}

void set_game_locked(const std::string& title, const char* source) {
  if (title == game) return;
  if (!game.empty()) push("game-ended", source, game);
  game = title;
  game_flag = !game.empty();
  if (!game.empty()) push("game-started", source, game);
}

void watch() {
  name_thread("system-state");
  system_platform::open();
  std::int64_t previous_mono = clock_us(CLOCK_MONOTONIC), previous_wall = clock_us(CLOCK_REALTIME);
  std::int64_t next_game = 0, next_power = 0;
  bool was_keeping = false;
  std::unique_lock lock(mutex);
  while (!stopping) {
    lock.unlock();
    system_platform::poll(report);
    const std::int64_t mono = clock_us(CLOCK_MONOTONIC), wall = clock_us(CLOCK_REALTIME);
    // Rest mode may stop the monotonic clock, so the wall clock is compared as well.
    const std::int64_t gap = std::max(mono - previous_mono, wall - previous_wall);
    previous_mono = mono;
    previous_wall = wall;
    char title[16] = "";
    const int found = mono >= next_game ? system_platform::running_game(title, sizeof title) : -1;
    if (mono >= next_game) next_game = mono + kGamePollUs;
    const bool keeping = keeping_awake.load();
    if (keeping && (!was_keeping || mono >= next_power)) {
      system_platform::reset_power_timer();
      next_power = mono + system_state::kPowerTimerResetUs;
    }
    was_keeping = keeping;
    lock.lock();
    if (continued.exchange(false)) report_locked(false, "sigcont");
    if (gap > kGapUs) report_locked(false, "gap", static_cast<double>((gap - kPassUs) / 1000));
    if (found >= 0) set_game_locked(found ? title : "", "game-poll");
    wake.wait_for(lock, std::chrono::microseconds(kPassUs), [] { return stopping; });
  }
  lock.unlock();
  system_platform::close();
}
} // namespace

namespace system_state {
bool start() {
  std::lock_guard lock(mutex);
  if (running) return true;
  stopping = false;
  // SIGCONT is how a process learns it was resumed from standby (PKG Manager relies on it); the
  // handler only sets a flag. Crash signals stay with crash_log.cpp.
  struct sigaction action {};
  action.sa_handler = sigcont;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGCONT, &action, &previous_sigcont);
  running = worker.start(watch, 512 * 1024);
  if (!running) sigaction(SIGCONT, &previous_sigcont, nullptr);
  return running;
}

void stop() {
  {
    std::lock_guard lock(mutex);
    if (!running) return;
    stopping = true;
  }
  wake.notify_all();
  worker.join();
  sigaction(SIGCONT, &previous_sigcont, nullptr);
  std::lock_guard lock(mutex);
  running = false;
  queue.clear();
}

std::vector<Event> poll() {
  std::lock_guard lock(mutex);
  std::vector<Event> events(queue.begin(), queue.end());
  queue.clear();
  return events;
}

bool suspended() { return asleep.load(std::memory_order_relaxed); }
bool game_running() { return game_flag.load(std::memory_order_relaxed); }
std::int64_t resumed_at_us() { return resumed_us.load(std::memory_order_relaxed); }

std::string running_game() {
  std::lock_guard lock(mutex);
  return game;
}

// The watcher resets the timer on its next pass, within a second.
void keep_awake(bool enabled) { keeping_awake = enabled; }

bool simulate(const char* state, const char* title_id) {
  std::lock_guard lock(mutex);
  if (!std::strcmp(state, "sleeping")) report_locked(true, "simulated");
  else if (!std::strcmp(state, "resumed")) {
    // A simulated resume always counts, so a test can repeat it without waiting out the window.
    resumed_us = 0;
    report_locked(false, "simulated");
  } else if (!std::strcmp(state, "game-started")) set_game_locked(*title_id ? title_id : "SIMULATED", "simulated");
  else if (!std::strcmp(state, "game-ended")) set_game_locked("", "simulated");
  else return false;
  return true;
}
} // namespace system_state

namespace {
JSValue system_poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  JSValue list = JS_NewArray(ctx);
  std::uint32_t index = 0;
  for (const auto& event : system_state::poll()) {
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, event.state.c_str()));
    JS_SetPropertyStr(ctx, item, "at", JS_NewFloat64(ctx, event.at));
    JS_SetPropertyStr(ctx, item, "source", JS_NewString(ctx, event.source.c_str()));
    if (!event.title_id.empty()) JS_SetPropertyStr(ctx, item, "titleId", JS_NewString(ctx, event.title_id.c_str()));
    if (event.paused_ms >= 0) JS_SetPropertyStr(ctx, item, "pausedMs", JS_NewFloat64(ctx, event.paused_ms));
    JS_SetPropertyUint32(ctx, list, index++, item);
  }
  return list;
}

JSValue system_running_game(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  const std::string title = system_state::running_game();
  if (title.empty()) return JS_NULL;
  JSValue object = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, object, "titleId", JS_NewString(ctx, title.c_str()));
  return object;
}

JSValue system_simulate(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
  const char* state = JS_ToCString(ctx, argv[0]);
  if (!state) return JS_EXCEPTION;
  const char* title = JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const bool ok = system_state::simulate(state, title ? title : "");
  if (title) JS_FreeCString(ctx, title);
  const JSValue result = ok ? JS_TRUE : JS_ThrowTypeError(ctx, "system.simulate %s: unknown state", state);
  JS_FreeCString(ctx, state);
  return result;
}

} // namespace

JSValue ps5_react_system_api(JSContext* ctx) {
  if (!system_state::start()) system_platform::log("system: the state watcher thread did not start");
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, system_poll, "poll", 0));
  JS_SetPropertyStr(ctx, api, "runningGame", JS_NewCFunction(ctx, system_running_game, "runningGame", 0));
  JS_SetPropertyStr(ctx, api, "simulate", JS_NewCFunction(ctx, system_simulate, "simulate", 2));
  return api;
}
