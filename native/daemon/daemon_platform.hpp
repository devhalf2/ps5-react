// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <string>

// What differs between pandorad on the console (native/daemon/ps5.cpp) and on the desktop (desktop.cpp).
namespace daemon_platform {
// Prepares the folders the app paths map to (PS5: the title's folder on /data, since the app's sandbox
// mounts go away with it), before the log opens there. False with `error`.
bool start(std::string& error);
// Once per turn of the loop (PS5: keeps the console awake while Power.keepAwake is on).
void tick();
}
