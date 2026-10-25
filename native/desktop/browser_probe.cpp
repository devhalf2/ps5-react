// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The in-app browser probe owns a PS5 WebKit page; the desktop preview has no
// such engine, so it only reports that the probe is console-only.
#include "browser_probe.hpp"
#include <mutex>

namespace browser_probe {
namespace {
std::mutex guard;
std::uint32_t next_id = 1;
Snapshot last;
bool has_last = false;
} // namespace

std::uint32_t start(std::string url, std::string&) {
  std::lock_guard lock(guard);
  (void)url;
  last = Snapshot{};
  last.id = next_id++;
  last.state = "unavailable";
  last.error = "browser probe runs only on the console (PS5 WebKit)";
  last.log = {"[browser-probe] unavailable on the desktop preview"};
  has_last = true;
  return last.id;
}

std::vector<Snapshot> poll() {
  std::lock_guard lock(guard);
  if (!has_last) return {};
  has_last = false;
  return {last};
}

void stop() {}

} // namespace browser_probe
