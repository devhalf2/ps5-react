// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// pandorad: the app's `daemon` script in a host without a screen, input or sound, which keeps running
// after the app closes. The app starts it (Daemon.start) with "PDD1\n<token>\n<stateDir>\n<version>\n"
// on its standard input; the script calls Daemon.serve, and the daemon answers "ready" once it serves
// 127.0.0.1:9061, then lets go of the app's channel. See native/shared/daemon.hpp.
#include "daemon_platform.hpp"
#include "http_server.hpp"
#include "js_runtime.hpp"
#include "archives.hpp"
#include "daemon.hpp"
#include "host_api.hpp"
#include "network.hpp"
#include "packages.hpp"
#include "removals.hpp"
#include "system_state.hpp"
#include "thread_name.hpp"
#include "worker_thread.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
extern const char pandorad_bundle[];
extern const unsigned long pandorad_bundle_length;
}

namespace {
// The daemon shares the console with a game: its script gets the heap the spec budgets (docs/NATIVE-API.md).
constexpr std::size_t kHeapBytes = 32 * 1024 * 1024;
constexpr std::size_t kThreadStack = 2 * 1024 * 1024, kScriptStack = 1024 * 1024;
// A daemon that logs in a loop stops logging here rather than filling the drive.
constexpr std::uint64_t kMaxLogBytes = 16 * 1024 * 1024;

struct Args { std::string token, state_dir, version; } args;
// The app's end (payload loader socket or pipe) until the one answer line is written.
int channel = -1;
std::mutex log_mutex;
std::uint64_t logged = 0;
volatile std::sig_atomic_t stop_signal = 0;
bool exiting = false;
int exit_code = 0;
http::Server server;
JSValue serve_callback = JS_UNDEFINED;

void log_line(const char* text) {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  tm utc{};
  gmtime_r(&now.tv_sec, &utc);
  char stamp[32];
  const std::size_t length = std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &utc);
  std::snprintf(stamp + length, sizeof stamp - length, ".%03ldZ ", now.tv_nsec / 1'000'000);
  std::string line = std::string(stamp) + text + "\n";
  std::lock_guard lock(log_mutex);
  if (logged > kMaxLogBytes) return;
  logged += line.size();
  if (logged > kMaxLogBytes) line = std::string(stamp) + "log limit reached; later lines are dropped\n";
  // stdout is daemon.log once it is open, which native printf output shares.
  std::fflush(stdout);
  (void)!::write(STDOUT_FILENO, line.data(), line.size());
}

void log_format(const char* format, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(text, sizeof text, format, arguments);
  va_end(arguments);
  log_line(text);
}

// The one line the app reads back; after it the app's channel is let go, so the daemon outlives it.
void answer(const std::string& line) {
  if (channel < 0) return;
  const std::string text = line + "\n";
  (void)!::write(channel, text.data(), text.size());
  ::close(channel);
  channel = -1;
  const int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    if (null != STDIN_FILENO) ::close(null);
  }
}

// Four lines on standard input within the start timeout.
bool read_request(std::string& error) {
  std::string lines[4];
  int index = 0;
  timespec started{};
  clock_gettime(CLOCK_MONOTONIC, &started);
  while (index < 4) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long elapsed = (now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1'000'000;
    if (elapsed >= daemon_process::kReadyTimeoutMs) { error = "no request within 10 s"; return false; }
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, static_cast<int>(daemon_process::kReadyTimeoutMs - elapsed)) <= 0) continue;
    char byte = 0;
    const ssize_t count = ::read(STDIN_FILENO, &byte, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { error = "the request ended early"; return false; }
    if (byte == '\n') ++index;
    else if (lines[index].size() < 1024) lines[index] += byte;
    else { error = "a request line is longer than 1024 bytes"; return false; }
  }
  if (lines[0] != "PDD1") { error = "expected PDD1"; return false; }
  if (lines[1].empty() || lines[2].empty() || lines[2][0] != '/' || lines[3].empty()) {
    error = "expected a token, an absolute stateDir and a version";
    return false;
  }
  args = {lines[1], lines[2], lines[3]};
  return true;
}

bool make_directories(const std::string& path) {
  for (std::size_t slash = path.find('/', 1); ; slash = path.find('/', slash + 1)) {
    const std::string part = path.substr(0, slash);
    if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (slash == std::string::npos) return true;
  }
}

// <FileSystem.dataDir>/daemon.log, the previous run's kept as daemon.prev.log.
bool open_log(std::string& error) {
  char directory[PATH_MAX];
  if (!host::resolve_path("/download0", directory, sizeof directory) || !make_directories(directory)) {
    error = std::string("cannot create the data folder: ") + std::strerror(errno);
    return false;
  }
  const std::string current = std::string(directory) + "/daemon.log", previous = std::string(directory) + "/daemon.prev.log";
  std::rename(current.c_str(), previous.c_str());
  const int fd = open(current.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (fd < 0) {
    error = current + ": " + std::strerror(errno);
    return false;
  }
  std::fflush(stdout);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > STDERR_FILENO) ::close(fd);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  return true;
}

void on_signal(int number) { stop_signal = number; }

std::string decode(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (text[i] == '+') out += ' ';
    else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
      out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
      i += 2;
    } else out += text[i];
  }
  return out;
}

void set_string(JSContext* ctx, JSValueConst object, const std::string& name, const std::string& value) {
  JS_SetPropertyStr(ctx, object, name.c_str(), JS_NewStringLen(ctx, value.data(), value.size()));
}

// The token guards everything but the handshake and the console installer's /i/ URLs, which cannot send it.
int check(const http::Request& request) {
  if ((request.method == "GET" && request.path == "/hello") || request.path.rfind("/i/", 0) == 0) return 0;
  const std::string* sent = request.header("x-pandora-token");
  if (!sent || sent->size() != args.token.size()) return 401;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < sent->size(); ++i) difference |= static_cast<unsigned char>((*sent)[i] ^ args.token[i]);
  return difference ? 401 : 0;
}

void answer_error(std::uint64_t ticket, const std::string& message) {
  log_format("Daemon.serve: %s", message.c_str());
  http::Response response;
  response.status = 500;
  response.headers.emplace_back("Content-Type", "application/json");
  response.body = "{\"error\":{\"message\":\"the daemon's handler gave an invalid response\"}}";
  server.respond(ticket, std::move(response));
}

// respond({status, headers?, body?}), bound to one request's ticket.
JSValue respond(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
  double number = 0;
  JS_ToFloat64(ctx, &number, data[0]);
  const auto ticket = static_cast<std::uint64_t>(number);
  JSValueConst value = argc ? argv[0] : JS_UNDEFINED;
  if (!JS_IsObject(value)) { answer_error(ticket, "the response is not an object"); return JS_UNDEFINED; }
  http::Response response;
  JSValue status = JS_GetPropertyStr(ctx, value, "status");
  std::int32_t code = 0;
  const bool status_ok = JS_IsNumber(status) && !JS_ToInt32(ctx, &code, status) && code >= 200 && code <= 599;
  JS_FreeValue(ctx, status);
  if (!status_ok) { answer_error(ticket, "status must be a number from 200 to 599"); return JS_UNDEFINED; }
  response.status = code;
  JSValue body = JS_GetPropertyStr(ctx, value, "body");
  if (JS_IsString(body)) {
    std::size_t length = 0;
    if (const char* text = JS_ToCStringLen(ctx, &length, body)) { response.body.assign(text, length); JS_FreeCString(ctx, text); }
  }
  const bool body_ok = JS_IsString(body) || JS_IsUndefined(body) || JS_IsNull(body);
  JS_FreeValue(ctx, body);
  if (!body_ok) { answer_error(ticket, "body must be a string"); return JS_UNDEFINED; }
  JSValue headers = JS_GetPropertyStr(ctx, value, "headers");
  bool headers_ok = JS_IsUndefined(headers) || JS_IsNull(headers);
  JSPropertyEnum* names = nullptr;
  std::uint32_t count = 0;
  if (JS_IsObject(headers) && !JS_GetOwnPropertyNames(ctx, &names, &count, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
    headers_ok = true;
    for (std::uint32_t i = 0; i < count; ++i) {
      const char* name = JS_AtomToCString(ctx, names[i].atom);
      JSValue item = JS_GetProperty(ctx, headers, names[i].atom);
      const char* text = JS_IsString(item) ? JS_ToCString(ctx, item) : nullptr;
      const bool valid = name && text && *name && !std::strpbrk(name, " \t\r\n:") && !std::strpbrk(text, "\r\n");
      if (valid) response.headers.emplace_back(name, text);
      headers_ok = headers_ok && valid;
      if (text) JS_FreeCString(ctx, text);
      if (name) JS_FreeCString(ctx, name);
      JS_FreeValue(ctx, item);
    }
    JS_FreePropertyEnum(ctx, names, count);
  }
  JS_FreeValue(ctx, headers);
  if (!headers_ok) { answer_error(ticket, "headers must map names to one-line strings"); return JS_UNDEFINED; }
  server.respond(ticket, std::move(response));
  return JS_UNDEFINED;
}

void handle(std::uint64_t ticket, http::Request&& request) {
  JSContext* ctx = js::context();
  JSValue object = JS_NewObject(ctx);
  set_string(ctx, object, "method", request.method);
  set_string(ctx, object, "path", request.path);
  JSValue query = JS_NewObject(ctx);
  for (std::size_t start = 0; start < request.query.size();) {
    const auto end = std::min(request.query.find('&', start), request.query.size());
    const std::string pair = request.query.substr(start, end - start);
    const auto equals = pair.find('=');
    if (!pair.empty()) set_string(ctx, query, decode(pair.substr(0, equals)), equals == std::string::npos ? "" : decode(pair.substr(equals + 1)));
    start = end + 1;
  }
  JS_SetPropertyStr(ctx, object, "query", query);
  JSValue headers = JS_NewObject(ctx);
  for (const auto& [name, value] : request.headers) set_string(ctx, headers, name, value);
  JS_SetPropertyStr(ctx, object, "headers", headers);
  set_string(ctx, object, "body", request.body);
  JSValue ticket_value = JS_NewFloat64(ctx, static_cast<double>(ticket));
  JSValue arguments[] = {object, JS_NewCFunctionData(ctx, respond, 1, 0, 1, &ticket_value)};
  JSValue result = JS_Call(ctx, serve_callback, JS_UNDEFINED, 2, arguments);
  if (JS_IsException(result)) {
    answer_error(ticket, "the handler threw: " + js::take_exception());
  }
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, arguments[0]);
  JS_FreeValue(ctx, arguments[1]);
  JS_FreeValue(ctx, ticket_value);
  js::drain();
}

JSValue serve(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "daemon.serve: expected a function");
  if (server.serving() || exiting) return JS_ThrowPlainError(ctx, "Daemon.serve: already serving");
  serve_callback = JS_DupValue(ctx, argv[0]);
  server.serve(check, handle);
  log_format("serving 127.0.0.1:%d", daemon_process::port());
  answer("ready");
  return JS_UNDEFINED;
}

JSValue exit_daemon(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::int32_t code = 0;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &code, argv[0])) return JS_EXCEPTION;
  if (!exiting) log_format("Daemon.exit(%d)", code);
  exiting = true;
  exit_code = code;
  return JS_UNDEFINED;
}

// Inside the daemon `__ps5ReactNative` keeps the modules that need no screen or input, and serves.
void install_api(JSContext* ctx) {
  ps5_react_install_host_api(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue api = JS_GetPropertyStr(ctx, global, "__ps5ReactNative");
  for (const char* name : {"image", "sound", "shader", "browser", "pad", "text", "openURL", "exit"}) {
    const JSAtom atom = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, api, atom, 0);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, api);
  JS_FreeValue(ctx, global);
}

int run() {
  name_thread("pandorad");
  if (!network::start()) log_line("network: the transport did not start; Downloads and Http will fail");
  // Rest mode, resume and a running game (System.onStateChange); installing the API also starts it.
  if (!system_state::start()) log_line("system: the state watcher thread did not start");
  if (!js::start(kHeapBytes, kScriptStack, log_line)) {
    answer("error the script runtime did not start");
    return 1;
  }
  install_api(js::context());
  std::string error;
  if (!js::evaluate(pandorad_bundle, pandorad_bundle_length, "daemon.bundle.js", error)) {
    log_format("the daemon script failed: %s", error.c_str());
    answer("error the daemon script failed: " + error.substr(0, error.find('\n')));
    if (!exiting) exit_code = 1;
    exiting = true;
  } else if (!server.serving() && !exiting) {
    log_line("the daemon script has not called Daemon.serve yet");
  }
  while (!exiting) {
    const int next = js::run_timers();
    if (exiting) break;
    if (stop_signal) {
      log_format("stopping on signal %d", static_cast<int>(stop_signal));
      break;
    }
    server.run(next < 0 || next > 500 ? 500 : next);
    js::drain();
    daemon_platform::tick();
  }
  // Answers already given still go out; then each service stops and saves what it can resume from.
  server.close(2000);
  archives::stop();
  packages::stop();
  removals::stop();
  network::stop();
  system_state::stop();
  JS_FreeValue(js::context(), serve_callback);
  serve_callback = JS_UNDEFINED;
  js::stop();
  return exit_code;
}
} // namespace

JSValue ps5_react_daemon_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "supported", JS_TRUE);
  JS_SetPropertyStr(ctx, api, "isDaemon", JS_TRUE);
  JSValue values = JS_NewObject(ctx);
  set_string(ctx, values, "token", args.token);
  set_string(ctx, values, "stateDir", args.state_dir);
  set_string(ctx, values, "version", args.version);
  JS_SetPropertyStr(ctx, api, "args", values);
  JS_SetPropertyStr(ctx, api, "pid", JS_NewInt32(ctx, static_cast<int32_t>(getpid())));
  JS_SetPropertyStr(ctx, api, "port", JS_NewInt32(ctx, daemon_process::port()));
  JS_SetPropertyStr(ctx, api, "serve", JS_NewCFunction(ctx, serve, "serve", 1));
  JS_SetPropertyStr(ctx, api, "exit", JS_NewCFunction(ctx, exit_daemon, "exit", 1));
  return api;
}

int main() {
  // The app's channel closes while the daemon runs on; a write to it must not end the daemon.
  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  struct sigaction action{};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);
  channel = dup(STDOUT_FILENO);
  std::string error;
  if (!read_request(error)) {
    answer("error bad request: " + error);
    return 2;
  }
  // The port first: a second daemon leaves before it touches the log or runs any of the script, so two
  // never run the queue at once. Daemon.serve then starts answering on it.
  const int port = daemon_process::port();
  if (const int failure = server.listen(port)) {
    answer(failure == EADDRINUSE ? std::string("busy")
                                 : "error cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(failure));
    return failure == EADDRINUSE ? 3 : 1;
  }
  if (!daemon_platform::start(error) || !open_log(error)) {
    answer("error " + error);
    return 1;
  }
  log_format("pandorad %s starting, pid %ld, stateDir %s", args.version.c_str(), static_cast<long>(getpid()), args.state_dir.c_str());
  int code = 1;
  WorkerThread loop;
  // The script runs on a thread with a known stack: the payload's main thread stack is the loader's choice.
  if (loop.start([&code] { code = run(); }, kThreadStack)) loop.join();
  else log_line("cannot start the script thread");
  if (channel >= 0) answer("error the daemon exited before it served; see daemon.log");
  log_format("exit %d", code);
  std::fflush(stdout);
  fsync(STDOUT_FILENO);
  return code;
}
