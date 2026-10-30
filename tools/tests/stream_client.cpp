// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A 7z downloaded by the native downloader and extracted by rar-extract while it downloads, with the hosts'
// own wiring between them: usage: stream_client <url> <archive path> <output folder>. Prints when each ends,
// and what the extraction had written when the download did.
#include "archives.hpp"
#include "network.hpp"
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <thread>

void archives::trace(const char* step, const char* detail) { std::cerr << step << " " << detail << "\n"; }
std::size_t archives::pipeline_bytes() { return 32u << 20; }
bool archives::hold_memory(bool) { return true; }
bool archives::downloading() { return network::downloading(); }
bool archives::download_progress(const std::string& path, std::uint64_t& prefix, std::uint64_t& total) {
  return network::progress_of(path, prefix, total);
}
bool archives::fetch_range(const std::string& url, const std::vector<std::string>& headers, std::uint64_t offset, std::uint64_t length,
                           std::string& out, std::string& error, const std::atomic<bool>& cancelled) {
  return network::fetch_range(url, headers, offset, length, out, error, &cancelled);
}
bool archives::list_directory(const char* path, void (*visit)(const char*, void*), void* user) {
  DIR* d = opendir(path);
  if (!d) return false;
  while (auto* e = readdir(d)) visit(e->d_name, user);
  closedir(d);
  return true;
}

int main(int argc, char** argv) {
  if (argc != 4 || !network::start()) return 2;
  using clock = std::chrono::steady_clock;
  const auto started = clock::now();
  const auto ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count(); };
  network::Request download;
  download.url = argv[1];
  download.destination = argv[2];
  download.connections = 8;
  // Small ranges keep the bytes in flight well under the archive, as 16 MiB ranges are under a game.
  download.range_bytes = 1 << 20;
  download.adaptive = false;
  std::string error;
  const auto download_id = network::enqueue(download, error);
  if (!download_id) { std::cout << "download failed " << error << "\n"; return 1; }
  std::uint32_t extract_id = 0;
  bool downloaded = false;
  std::string extracted;
  std::uint64_t written = 0;
  while (extracted.empty() && ms() < 120000) {
    if (std::getenv("STREAM_TRACE")) {
      std::uint64_t prefix = 0, total = 0;
      if (network::progress_of(argv[2], prefix, total)) std::cerr << "t=" << ms() << " prefix=" << (prefix >> 20) << " written=" << (written >> 20) << "\n";
    }
    for (const auto& s : network::poll()) {
      // As the app does, once the partial file exists: here once the download knows its size.
      if (!extract_id && s.total_known && s.written > 0) {
        archives::Request request;
        request.sources = {argv[2]};
        request.destination = argv[3];
        request.stream = true;
        request.tail_url = argv[1];
        if (!(extract_id = archives::enqueue(request, error))) { std::cout << "extract failed " << error << "\n"; return 1; }
      }
      // What the extraction had written when the download ended.
      if (s.state == "completed" && !downloaded) { downloaded = true; std::cout << "downloaded " << ms() << " " << written << "\n"; }
      if (s.state == "failed" || s.state == "cancelled") { std::cout << "download " << s.state << " " << s.error << "\n"; return 1; }
    }
    for (const auto& s : archives::poll()) {
      written = s.written;
      if (s.state == "completed" || s.state == "failed" || s.state == "cancelled")
        extracted = s.state + " " + std::to_string(ms()) + " " + s.error;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "extracted " << extracted << "\n";
  archives::stop();
  network::stop();
  return extracted.rfind("completed", 0) == 0 ? 0 : 1;
}
