// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// PS5 probes for system_state.hpp. None of them has been verified on hardware yet; each logs its
// first result, and every later change between success and failure.
//
// Imported (game-facing calls the title already depends on modules for): the event flag calls from
// libkernel and sceSystemServiceReceiveEvent. Resolved at run time instead, so a firmware or process
// type without them disables one feature instead of keeping the title from starting:
// sceShellCoreUtilResetAutoPowerDownTimer, sceSystemServiceGetAppIdOfRunningBigApp and
// sceSystemServiceGetAppTitleId (libSceSystemService), sceNotificationSend (libSceNotification,
// which the title does not load otherwise).
#include "system_state.hpp"
#include "async_log.hpp"

#if __has_include("app_config.hpp")
#include "app_config.hpp"
#endif
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int sceKernelOpenEventFlag(std::intptr_t* flag, const char* name);
int sceKernelPollEventFlag(std::intptr_t flag, std::uint64_t pattern, unsigned mode, std::uint64_t* result);
int sceKernelCloseEventFlag(std::intptr_t flag);
int sceSystemServiceReceiveEvent(void* event);
}

namespace {
// The state manager's flag, as ShadowMountPlus reads it: the low 16 bits hold the state.
constexpr unsigned kWaitOr = 2;
constexpr int kBusy = static_cast<int>(0x80020010u); // nothing set: the flag is zero
constexpr unsigned kSuspendOnGoing = 300, kMainOnStandby = 500, kWorking = 1000;
// SceSystemServiceEvent types, as ezRemote Server handles them.
constexpr std::uint32_t kOnResume = 0x10000000u, kBeforeSleep = 0x10000001u;

// Logs a call's first result and each later switch between success and failure.
struct Outcome {
  const char* call;
  int state = -1; // -1 never logged, 0 failed, 1 succeeded
  void note(int result) {
    const int now = result >= 0 ? 1 : 0;
    if (now == state) return;
    state = now;
    async_log::write("[PS5-REACT] system: %s=0x%08x", call, static_cast<unsigned>(result));
  }
};

// Opens a system module by name, then by its path; the handle stays open for the process lifetime.
void* symbol(const char* module, const char* name) {
  char path[96];
  std::snprintf(path, sizeof path, "/system/common/lib/%s", module);
  void* handle = dlopen(module, RTLD_LAZY);
  if (!handle) handle = dlopen(path, RTLD_LAZY);
  void* found = handle ? dlsym(handle, name) : nullptr;
  async_log::write("[PS5-REACT] system: %s %s %s", name, found ? "from" : "unavailable from", module);
  return found;
}

template <typename Function>
struct Late {
  const char* module;
  const char* name;
  bool resolved = false;
  Function function = nullptr;
  Function get() {
    if (!resolved) {
      resolved = true;
      function = reinterpret_cast<Function>(symbol(module, name));
    }
    return function;
  }
};

Late<int (*)()> reset_timer{"libSceSystemService.sprx", "sceShellCoreUtilResetAutoPowerDownTimer"};
Late<int (*)()> running_big_app{"libSceSystemService.sprx", "sceSystemServiceGetAppIdOfRunningBigApp"};
Late<int (*)(int, char*)> app_title_id{"libSceSystemService.sprx", "sceSystemServiceGetAppTitleId"};
Late<int (*)(int, bool, const char*)> notification_send{"libSceNotification.sprx", "sceNotificationSend"};

Outcome open_outcome{"sceKernelOpenEventFlag(SceSystemStateMgrInfo)"}, poll_outcome{"sceKernelPollEventFlag"},
  event_outcome{"sceSystemServiceReceiveEvent"}, reset_outcome{"sceShellCoreUtilResetAutoPowerDownTimer"},
  big_app_outcome{"sceSystemServiceGetAppIdOfRunningBigApp"}, title_outcome{"sceSystemServiceGetAppTitleId"},
  send_outcome{"sceNotificationSend"};

std::intptr_t state_flag = -1;
bool flag_open = false;
unsigned previous_state = 0;
// The event's union is 8 KiB in the PS4 SDK; ezRemote passes 64 bytes. The larger, zeroed, is safe.
alignas(8) unsigned char event_buffer[8192 + 64];
} // namespace

namespace system_platform {
void open() {
  const int result = sceKernelOpenEventFlag(&state_flag, "SceSystemStateMgrInfo");
  open_outcome.note(result);
  flag_open = result >= 0;
  previous_state = 0;
}

void close() {
  if (flag_open) sceKernelCloseEventFlag(state_flag);
  flag_open = false;
}

void poll(Report report) {
  // Received events are consumed; the title's host reads none of them itself.
  for (int i = 0; i < 8; ++i) {
    std::memset(event_buffer, 0, sizeof event_buffer);
    const int result = sceSystemServiceReceiveEvent(event_buffer);
    event_outcome.note(result);
    if (result != 0) break;
    std::uint32_t type = 0;
    std::memcpy(&type, event_buffer, sizeof type);
    if (type == kBeforeSleep) report(true, "event");
    else if (type == kOnResume) report(false, "event");
  }
  if (!flag_open) return;
  std::uint64_t pattern = 0;
  int result = sceKernelPollEventFlag(state_flag, ~0ull, kWaitOr, &pattern);
  if (result == kBusy) {
    pattern = 0;
    result = 0;
  }
  poll_outcome.note(result);
  if (result < 0) return;
  const unsigned state = static_cast<unsigned>(pattern & 0xffffu);
  if (state != previous_state) {
    async_log::write("[PS5-REACT] system: SceSystemStateMgrInfo state %u -> %u", previous_state, state);
    if (state == kSuspendOnGoing || state == kMainOnStandby) report(true, "state-flag");
    else if (state == kWorking && previous_state != 0) report(false, "state-flag");
  }
  previous_state = state;
}

// The running big app other than this one. Unverified: the calls are the ones etaHEN uses from
// ShellUI; whether a title or a payload may make them, and what they answer while a game is suspended
// in the background, needs a console. System apps (NPXS) do not count as games.
int running_game(char* title_id, std::size_t size) {
  const auto app_id = running_big_app.get();
  const auto title = app_title_id.get();
  if (!app_id || !title) return -1;
  const int id = app_id();
  big_app_outcome.note(id);
  if (id < 0) return 0; // no big app
  char found[64] = {};
  const int result = title(id, found);
  title_outcome.note(result);
  if (result != 0) return -1;
  found[sizeof found - 1] = '\0';
  if (!*found || !std::strncmp(found, "NPXS", 4)) return 0;
#ifdef PS5_REACT_TITLE
  if (!std::strcmp(found, PS5_REACT_TITLE)) return 0;
#endif
  std::snprintf(title_id, size, "%s", found);
  return 1;
}

void reset_power_timer() {
  if (const auto reset = reset_timer.get()) reset_outcome.note(reset());
}

bool send_notification(const char* json) {
  const auto send = notification_send.get();
  if (!send) return false;
  // 0xFE: the system user, as the payload SDK's notify sample sends.
  const int result = send(0xFE, true, json);
  send_outcome.note(result);
  return result == 0;
}

void log(const char* line) { async_log::write("[PS5-REACT] %s", line); }
} // namespace system_platform
