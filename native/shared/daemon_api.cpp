// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The app's side of `__ps5ReactNative.daemon` (host_api.hpp): whether the app has a daemon, and starting
// it. pandorad defines its own (native/daemon/daemon_host.cpp), which serves instead.
#include "host_api.hpp"
#include "daemon.hpp"
#include "app_config.hpp"
#include "worker_thread.hpp"
#include <memory>
#include <mutex>

namespace {
struct Start {
  std::uint32_t id = 0;
  std::string state = "starting", error;
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  WorkerThread worker;
};
std::mutex guard;
std::shared_ptr<Start> active;
std::uint32_t next_id = 1;

bool terminal(const std::string& state) { return state == "completed" || state == "failed" || state == "cancelled"; }

// A request value may hold no line break or NUL: the daemon reads one value per line.
bool line_value(JSContext* ctx, JSValueConst value, std::size_t limit, std::string& out) {
  std::size_t length = 0;
  const char* text = JS_IsString(value) ? JS_ToCStringLen(ctx, &length, value) : nullptr;
  if (!text) return false;
  out.assign(text, length);
  JS_FreeCString(ctx, text);
  return !out.empty() && out.size() <= limit && out.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}

JSValue start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (!PS5_REACT_DAEMON) return JS_ThrowPlainError(ctx, "Daemon.start: the app declares no daemon entry (app.json \"daemon\")");
  std::string token, state_dir;
  if (argc < 2 || !line_value(ctx, argv[0], 128, token))
    return JS_ThrowTypeError(ctx, "Daemon.start: token must be one line of 1 to 128 characters");
  if (!line_value(ctx, argv[1], 1024, state_dir) || state_dir[0] != '/')
    return JS_ThrowTypeError(ctx, "Daemon.start: stateDir must be an absolute path");
  std::lock_guard lock(guard);
  if (active) return JS_ThrowPlainError(ctx, "Daemon.start: a start is already waiting for the daemon");
  auto job = std::make_shared<Start>();
  job->id = next_id++;
  const std::string request = "PDD1\n" + token + "\n" + state_dir + "\n" PS5_REACT_VERSION "\n";
  active = job;
  if (!job->worker.start([job, request] {
        std::string error;
        const bool ok = daemon_process::launch(request, job->cancelled, error);
        std::lock_guard job_lock(job->mutex);
        job->state = job->cancelled ? "cancelled" : ok ? "completed" : "failed";
        job->error = ok ? "" : error;
      }, 512 * 1024)) {
    active.reset();
    return JS_ThrowPlainError(ctx, "Daemon.start: cannot start its thread");
  }
  return JS_NewUint32(ctx, job->id);
}

JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::uint32_t id = 0;
  if (!argc || JS_ToUint32(ctx, &id, argv[0])) return JS_ThrowTypeError(ctx, "daemon.cancel: expected task id");
  std::lock_guard lock(guard);
  if (active && active->id == id) active->cancelled = true;
  return JS_UNDEFINED;
}

JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  JSValue out = JS_NewArray(ctx);
  std::lock_guard lock(guard);
  if (!active) return out;
  std::string state, error;
  { std::lock_guard job_lock(active->mutex); state = active->state; error = active->error; }
  JSValue item = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, active->id));
  JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state.c_str()));
  JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, error.c_str()));
  JS_SetPropertyUint32(ctx, out, 0, item);
  if (terminal(state)) {
    active->worker.join();
    active.reset();
  }
  return out;
}
} // namespace

JSValue ps5_react_daemon_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "supported", JS_NewBool(ctx, PS5_REACT_DAEMON));
  JS_SetPropertyStr(ctx, api, "isDaemon", JS_FALSE);
  JS_SetPropertyStr(ctx, api, "args", JS_NULL);
  JS_SetPropertyStr(ctx, api, "port", JS_NewInt32(ctx, daemon_process::port()));
  JS_SetPropertyStr(ctx, api, "start", JS_NewCFunction(ctx, start, "start", 2));
  JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
  return api;
}
