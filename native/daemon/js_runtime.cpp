// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "js_runtime.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <vector>

namespace js {
namespace {
JSRuntime* runtime = nullptr;
JSContext* ctx = nullptr;
void (*sink)(const char*) = nullptr;
std::int64_t started_us = 0;

struct Timer {
  JSValue callback = JS_UNDEFINED;
  std::vector<JSValue> args;
  std::int64_t due_ms = 0, interval_ms = 0;
};
std::map<std::uint32_t, Timer> timers;
std::uint32_t next_timer = 1;

std::int64_t clock_us(clockid_t id) {
  timespec now{};
  clock_gettime(id, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

std::int64_t monotonic_ms() { return (clock_us(CLOCK_MONOTONIC) - started_us) / 1000; }

void emit(const std::string& line) { if (sink) sink(line.c_str()); }

void free_timer(Timer& timer) {
  JS_FreeValue(ctx, timer.callback);
  for (JSValue arg : timer.args) JS_FreeValue(ctx, arg);
  timer.args.clear();
}

// console.log/info/debug/warn/error: the arguments joined with spaces, as the app's console writes them.
JSValue console_line(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int level) {
  static const char* const prefixes[] = {"", "warn: ", "error: "};
  std::string line = prefixes[level];
  for (int i = 0; i < argc; ++i) {
    if (i) line += ' ';
    if (const char* text = JS_ToCString(context, argv[i])) {
      line += text;
      JS_FreeCString(context, text);
    }
  }
  emit(line);
  return JS_UNDEFINED;
}

JSValue add_timer(JSContext* context, int argc, JSValueConst* argv, bool repeat) {
  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    return JS_ThrowTypeError(context, "%s: expected a function", repeat ? "setInterval" : "setTimeout");
  double delay = 0;
  if (argc > 1 && JS_ToFloat64(context, &delay, argv[1])) return JS_EXCEPTION;
  delay = std::isfinite(delay) && delay > 0 ? std::floor(delay) : 0;
  while (!next_timer || timers.count(next_timer)) ++next_timer;
  const std::uint32_t id = next_timer++;
  Timer& timer = timers[id];
  timer.callback = JS_DupValue(context, argv[0]);
  for (int i = 2; i < argc; ++i) timer.args.push_back(JS_DupValue(context, argv[i]));
  // An interval of 0 would fire on every turn of the loop.
  timer.interval_ms = repeat ? (delay < 1 ? 1 : static_cast<std::int64_t>(delay)) : 0;
  timer.due_ms = monotonic_ms() + static_cast<std::int64_t>(delay);
  return JS_NewUint32(context, id);
}

JSValue set_timeout(JSContext* context, JSValueConst, int argc, JSValueConst* argv) { return add_timer(context, argc, argv, false); }
JSValue set_interval(JSContext* context, JSValueConst, int argc, JSValueConst* argv) { return add_timer(context, argc, argv, true); }

JSValue clear_timer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  std::uint32_t id = 0;
  if (argc < 1 || !JS_IsNumber(argv[0]) || JS_ToUint32(context, &id, argv[0])) return JS_UNDEFINED;
  if (auto found = timers.find(id); found != timers.end()) {
    free_timer(found->second);
    timers.erase(found);
  }
  return JS_UNDEFINED;
}

JSValue performance_now(JSContext* context, JSValueConst, int, JSValueConst*) {
  return JS_NewFloat64(context, static_cast<double>(clock_us(CLOCK_MONOTONIC) - started_us) / 1000.0);
}

JSValue date_now(JSContext* context, JSValueConst, int, JSValueConst*) {
  return JS_NewInt64(context, clock_us(CLOCK_REALTIME) / 1000);
}

JSValue date_constructor(JSContext* context, JSValueConst, int, JSValueConst*) {
  return JS_ThrowTypeError(context, "Date objects need ER_JS_INTRINSIC_DATE; only Date.now() is built in");
}

// Rejections without a handler, kept until the microtask queue empties: one a later then() adopts (an
// async function's promise awaited by its caller) is reported here first and handled a moment later.
std::vector<std::pair<JSValue, JSValue>> unhandled;

void rejection(JSContext* context, JSValueConst promise, JSValueConst reason, bool handled, void*) {
  if (handled) {
    for (auto item = unhandled.begin(); item != unhandled.end(); ++item)
      if (JS_VALUE_GET_PTR(item->first) == JS_VALUE_GET_PTR(promise)) {
        JS_FreeValue(context, item->first);
        JS_FreeValue(context, item->second);
        unhandled.erase(item);
        return;
      }
    return;
  }
  unhandled.emplace_back(JS_DupValue(context, promise), JS_DupValue(context, reason));
}

void report_unhandled() {
  for (auto& [promise, reason] : unhandled) {
    std::string line = "Unhandled promise rejection: ";
    if (const char* text = JS_ToCString(ctx, reason)) { line += text; JS_FreeCString(ctx, text); }
    JSValue stack = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
    if (const char* text = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr) {
      line += '\n';
      line += text;
      JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, stack);
    emit(line);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, reason);
  }
  unhandled.clear();
}

void set_function(JSValueConst object, const char* name, JSCFunction* function, int length) {
  JS_SetPropertyStr(ctx, object, name, JS_NewCFunction(ctx, function, name, length));
}
} // namespace

bool start(std::size_t heap_bytes, std::size_t stack_bytes, void (*log)(const char*)) {
  sink = log;
  started_us = clock_us(CLOCK_MONOTONIC);
  runtime = JS_NewRuntime();
  if (!runtime) return false;
  JS_SetMemoryLimit(runtime, heap_bytes);
  JS_SetMaxStackSize(runtime, stack_bytes);
  JS_UpdateStackTop(runtime);
  JS_SetHostPromiseRejectionTracker(runtime, rejection, nullptr);
  ctx = JS_NewContextRaw(runtime);
  if (!ctx) return false;
  // The lite profile of er_js_new_context, and the Eval hook JS_Eval needs to load a source bundle.
  JS_AddIntrinsicBaseObjects(ctx);
  JS_AddIntrinsicRegExp(ctx);
  JS_AddIntrinsicJSON(ctx);
  JS_AddIntrinsicMapSet(ctx);
  JS_AddIntrinsicPromise(ctx);
  JS_AddIntrinsicEval(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  constexpr struct { const char* name; int level; } levels[] = {{"log", 0}, {"info", 0}, {"debug", 0}, {"warn", 1}, {"error", 2}};
  for (const auto& level : levels)
    JS_SetPropertyStr(ctx, console, level.name, JS_NewCFunctionMagic(ctx, console_line, level.name, 1, JS_CFUNC_generic_magic, level.level));
  JS_SetPropertyStr(ctx, global, "console", console);
  set_function(global, "setTimeout", set_timeout, 2);
  set_function(global, "setInterval", set_interval, 2);
  set_function(global, "clearTimeout", clear_timer, 1);
  set_function(global, "clearInterval", clear_timer, 1);
  JSValue performance = JS_NewObject(ctx);
  set_function(performance, "now", performance_now, 0);
  JS_SetPropertyStr(ctx, global, "performance", performance);
  JSValue date = JS_NewCFunction2(ctx, date_constructor, "Date", 7, JS_CFUNC_constructor_or_func, 0);
  JSValue prototype = JS_NewObject(ctx);
  JS_SetConstructor(ctx, date, prototype);
  JS_FreeValue(ctx, prototype);
  set_function(date, "now", date_now, 0);
  JS_SetPropertyStr(ctx, global, "Date", date);
  JS_FreeValue(ctx, global);
  return true;
}

JSContext* context() { return ctx; }

std::string take_exception() {
  JSValue error = JS_GetException(ctx);
  std::string text;
  if (const char* message = JS_ToCString(ctx, error)) { text = message; JS_FreeCString(ctx, message); }
  JSValue stack = JS_IsObject(error) ? JS_GetPropertyStr(ctx, error, "stack") : JS_UNDEFINED;
  if (const char* trace = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr) {
    text += '\n';
    text += trace;
    JS_FreeCString(ctx, trace);
  }
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, error);
  return text;
}

void report(const char* where) { emit(std::string("Uncaught in ") + where + ": " + take_exception()); }

void drain() {
  for (JSContext* job = nullptr;;) {
    const int ran = JS_ExecutePendingJob(runtime, &job);
    if (ran == 0) break;
    if (ran < 0) report("a promise job");
  }
  report_unhandled();
}

bool evaluate(const char* source, std::size_t length, const char* name, std::string& error) {
  JSValue result = JS_Eval(ctx, source, length, name, JS_EVAL_TYPE_GLOBAL);
  const bool ok = !JS_IsException(result);
  if (!ok) error = take_exception();
  JS_FreeValue(ctx, result);
  drain();
  return ok;
}

int run_timers() {
  const std::int64_t now = monotonic_ms();
  // The due timers in order; one may clear another, so each is looked up again before it runs.
  std::vector<std::pair<std::int64_t, std::uint32_t>> due;
  for (const auto& [id, timer] : timers) if (timer.due_ms <= now) due.emplace_back(timer.due_ms, id);
  std::sort(due.begin(), due.end());
  for (const auto& [_, id] : due) {
    auto found = timers.find(id);
    if (found == timers.end()) continue;
    Timer& timer = found->second;
    JSValue callback = JS_DupValue(ctx, timer.callback);
    std::vector<JSValue> args;
    for (JSValue arg : timer.args) args.push_back(JS_DupValue(ctx, arg));
    if (timer.interval_ms) {
      // Late intervals skip the turns they missed rather than firing in a burst.
      timer.due_ms += timer.interval_ms;
      if (timer.due_ms <= now) timer.due_ms = now + timer.interval_ms;
    } else {
      free_timer(timer);
      timers.erase(found);
    }
    JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, static_cast<int>(args.size()), args.data());
    if (JS_IsException(result)) report("a timer");
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, callback);
    for (JSValue arg : args) JS_FreeValue(ctx, arg);
    drain();
  }
  if (timers.empty()) return -1;
  std::int64_t next = INT64_MAX;
  for (const auto& [id, timer] : timers) if (timer.due_ms < next) next = timer.due_ms;
  const std::int64_t wait = next - monotonic_ms();
  return wait <= 0 ? 0 : wait > 1'000'000 ? 1'000'000 : static_cast<int>(wait);
}

void stop() {
  if (!ctx) return;
  report_unhandled();
  for (auto& [id, timer] : timers) free_timer(timer);
  timers.clear();
  JS_FreeContext(ctx);
  JS_FreeRuntime(runtime);
  ctx = nullptr;
  runtime = nullptr;
}
} // namespace js
