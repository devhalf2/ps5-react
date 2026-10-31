// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The host side of archives.hpp in the daemon. The RAR/7z worker is started per platform (desktop: a
// child process, rar_worker_process.cpp; PS5: through the payload loader, ps5.cpp).
#include "archives.hpp"
#include "host_api.hpp"
#include "network.hpp"
#ifdef PROSPERO
#include "async_log.hpp"
#endif

void archives::trace(const char* step, const char* detail) {
#ifdef PROSPERO
  async_log::write("[PANDORAD] archive %s %s", step, detail);
#else
  (void)step; (void)detail;
#endif
}
// No image caches to make room from: one fixed write ring, within the daemon's memory budget.
std::size_t archives::pipeline_bytes() { return 16 * 1024 * 1024; }
bool archives::hold_memory(bool) { return true; }
bool archives::downloading() { return network::downloading(); }
bool archives::download_progress(const std::string& path, std::uint64_t& prefix, std::uint64_t& total) {
  return network::progress_of(path, prefix, total);
}
bool archives::fetch_range(const std::string& url, const std::vector<std::string>& headers, std::uint64_t offset, std::uint64_t length,
                           std::string& out, std::string& error, const std::atomic<bool>& cancelled) {
  return network::fetch_range(url, headers, offset, length, out, error, &cancelled);
}
bool archives::list_directory(const char* path, void (*visit)(const char*, void*), void* user) { return host::read_dir(path, visit, user); }
