// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// pandorad on the desktop: the preview's platform files (paths in PS5_REACT_SANDBOX, the system's TLS
// store, rar-extract beside the executable) serve it as they are; nothing else to prepare.
#include "daemon_platform.hpp"

namespace daemon_platform {
bool start(std::string&) { return true; }
void tick() {}
}
