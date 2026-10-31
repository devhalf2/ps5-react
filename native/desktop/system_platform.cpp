// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Desktop probes for system_state.hpp. A Mac reports no console states: the shared watcher still sees
// SIGCONT (`kill -STOP`, then `kill -CONT`) and gaps such as the Mac's own sleep, and system.simulate
// (or the control channel's `system` command) feeds the rest.
#include "system_state.hpp"

#include <cstdio>

namespace system_platform {
void open() {}
void close() {}
void poll(Report) {}
// Unknown, so a simulated game stays until a simulated game-ended.
int running_game(char*, std::size_t) { return -1; }
void reset_power_timer() {}
// Notifications.send falls back to host::notify, which prints the toast as Notifications.show does.
bool send_notification(const char*) { return false; }
void log(const char* line) {
  std::printf("[system] %s\n", line);
  std::fflush(stdout);
}
} // namespace system_platform
