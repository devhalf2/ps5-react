// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace network {
struct Piece {
  std::string url, sha1;
  std::uint64_t offset = 0, size = 0;
};
struct Request {
  std::string url, destination, storage_root, method = "GET", body, sha256;
  std::vector<std::string> headers;
  std::vector<Piece> pieces;
  // Other URLs serving the same bytes; ranges are spread across them and `url`.
  std::vector<std::string> mirrors;
  std::uint64_t expected_bytes = 0;
  std::uint64_t max_bytes = 1024 * 1024, range_bytes = 32 * 1024 * 1024;
  unsigned connections = 8;
  bool resume = true, adaptive = true, recover_completed = false;
  bool follow_redirects = true, reject_html = false;
};
struct Snapshot {
  std::uint32_t id = 0;
  std::string state = "queued", error, body, destination;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::uint64_t received = 0, written = 0, total = 0, buffered = 0;
  // Bytes of whole ranges, the ones a checkpoint keeps: never less after a pause, a resume or a retry.
  std::uint64_t completed_bytes = 0;
  // The file's first bytes, all on disk: what can be read in order while the rest downloads.
  std::uint64_t contiguous_bytes = 0;
  double bytes_per_second = 0;
  unsigned connections = 0, retries = 0;
  int status = 0;
  bool total_known = false;
};
// No JavaScript is accessed by either native worker. Only the caller polls JS.
bool start();
void stop();
std::uint32_t enqueue(Request request, std::string& error);
void cancel(std::uint32_t id);
std::vector<Snapshot> poll();
// True while a download to a file runs; image loading yields connections and memory to it.
bool downloading();
// How far the download to `destination` has come: its first `prefix` bytes are on disk (in
// `destination`.part), of `total` (0 until known). False when no unfinished download writes it. Any thread.
bool progress_of(const std::string& destination, std::uint64_t& prefix, std::uint64_t& total);
// One ranged GET of `length` bytes of `url` from `offset`, with `headers`, waited for and kept in `out`;
// `cancelled` (optional) stops it within a second. For an extraction that reads an archive's directory,
// kept at its end, before its download reaches it.
bool fetch_range(const std::string& url, const std::vector<std::string>& headers, std::uint64_t offset,
                 std::uint64_t length, std::string& out, std::string& error, const std::atomic<bool>* cancelled = nullptr);
const char* version();
// Applies the transport policy every request shares (protocols, redirects, TLS trust, timeouts,
// identity encoding and the PS5 socket options) to a libcurl easy handle.
bool configure_transport(void* curl, const std::string& url, bool follow_redirects);
// Platform owns libSceNet lifecycle and the trust-store path; desktop uses defaults.
bool platform_start(std::string& error);
void platform_stop();
const char* ca_path();
void platform_log(const char* line);
} // namespace network
