// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Stage 1 of the in-app browser: a developer-only probe that owns a WebKit page
// in-process the way Pegasus DL+ does, instead of launching the system browser.
// It loads libSceNKWebKit, resolves the WK* C API by NID through sceKernelDlsym,
// creates a context + page + view, loads a URL and paints one frame into an
// offscreen buffer, logging every step to the klog (app.log). It never presents
// to the screen and never changes navigation; it only reports what resolved and
// what the engine could build. Compiled inert unless PS5_REACT_BROWSER_PROBE is
// defined, and inert on desktop. A tester reads the "[browser-probe]" lines.
namespace browser_probe {
struct Snapshot {
  std::uint32_t id = 0;
  std::string state = "queued"; // queued | running | completed | failed | unavailable
  std::string error;
  std::vector<std::string> log; // step lines, also written to the klog
};
// Starts the probe on a worker thread. Returns a task id, or 0 with error set.
std::uint32_t start(std::string url, std::string& error);
// Terminal snapshots are consumed once read, like browser_capture::poll.
std::vector<Snapshot> poll();
void stop();
} // namespace browser_probe
