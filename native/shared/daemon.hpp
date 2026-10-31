// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

// The app's daemon, pandorad: the app's `daemon` entry in a headless host (native/daemon) that outlives
// the app. The app starts it with a request on its standard input (PS5: the payload loader's socket;
// desktop: a pipe) and reads one answer line back:
//   request  "PDD1\n<token>\n<stateDir>\n<version>\n"
//   answer   "ready" (serving on 127.0.0.1:9061), "busy" (the port is taken; it exits with code 3)
//            or "error <reason>"; other lines are log output and are skipped.
namespace daemon_process {
inline constexpr int kPort = 9061;
inline constexpr int kReadyTimeoutMs = 10'000;
inline constexpr const char* kHelloRequest = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

// kPort. The preview's tests run beside other programs on the same machine, so PS5_REACT_DAEMON_PORT
// moves the desktop daemon (and the preview's Daemon.start) elsewhere; the console always uses 9061.
inline int port() {
#ifndef PROSPERO
  if (const char* value = std::getenv("PS5_REACT_DAEMON_PORT")) {
    const int number = std::atoi(value);
    if (number >= 1024 && number <= 65535) return number;
  }
#endif
  return kPort;
}

enum class Answer { none, ready, busy, failed };

// What one line from the daemon means; `error` holds the reason of a failure.
inline Answer answer(const std::string& line, std::string& error) {
  if (line == "ready") return Answer::ready;
  if (line == "busy") return Answer::busy;
  if (line.rfind("error ", 0) == 0) { error = line.substr(6); return Answer::failed; }
  return Answer::none;
}

// Whether a response to kHelloRequest says a daemon serves.
inline bool hello_answered(const char* response) {
  return !std::strncmp(response, "HTTP/1.1 200 ", 13) || !std::strncmp(response, "HTTP/1.0 200 ", 13);
}

// Per platform (native/desktop/daemon_launcher.cpp, native/ps5/daemon_launcher.cpp): starts pandorad
// with `request`, then waits up to kReadyTimeoutMs for "ready", or, when one already holds the port,
// for it to answer GET /hello. Blocking, on a worker thread; false with `error`.
bool launch(const std::string& request, const std::atomic<bool>& cancelled, std::string& error);
} // namespace daemon_process
