// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <sys/resource.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

std::string escaped(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 32) { char text[7]; std::snprintf(text, sizeof text, "\\u%04x", c); out += text; }
    else out += static_cast<char>(c);
  }
  return out+'"';
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  if (argc > 8 && std::strtoull(argv[8], nullptr, 10)) {
    const rlim_t bytes = std::strtoull(argv[8], nullptr, 10);
    const rlimit limit{bytes, bytes};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return 5;
    std::signal(SIGXFSZ, SIG_IGN);
  }
  if (!network::start()) return 3;
  // NETWORK_TEST_FETCH_RANGE=<offset>:<length>: one ranged GET of the URL, its size and first bytes printed.
  if (const char* range = std::getenv("NETWORK_TEST_FETCH_RANGE")) {
    std::string out, error;
    const std::uint64_t offset = std::strtoull(range, nullptr, 10), length = std::strtoull(std::strchr(range, ':')+1, nullptr, 10);
    const bool ok = network::fetch_range(argv[1], {}, offset, length, out, error);
    std::string head;
    for (std::size_t i = 0; i < std::min<std::size_t>(out.size(), 16); ++i) {
      char hex[3]; std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned char>(out[i])); head += hex;
    }
    std::cout << "{\"state\":" << escaped(ok ? "completed" : "failed") << ",\"error\":" << escaped(error) << ",\"size\":" << out.size()
      << ",\"head\":" << escaped(head) << ",\"peakBuffer\":0,\"peakConnections\":0}\n";
    network::stop(); return ok ? 0 : 1;
  }
  network::Request request;
  request.url = argv[1];
  if (std::string(argv[2]) != "-") request.destination = argv[2];
  request.adaptive = std::getenv("NETWORK_TEST_ADAPTIVE") != nullptr;
  request.reject_html = std::getenv("NETWORK_TEST_REJECT_HTML") != nullptr;
  request.follow_redirects = std::getenv("NETWORK_TEST_NO_REDIRECT") == nullptr;
  request.recover_completed = std::getenv("NETWORK_TEST_RECOVER") != nullptr;
  if (const char* root = std::getenv("NETWORK_TEST_STORAGE_ROOT")) request.storage_root = root;
  if (const char* mirrors = std::getenv("NETWORK_TEST_MIRRORS")) {
    for (std::string list = mirrors; !list.empty();) {
      const auto comma = list.find(',');
      request.mirrors.push_back(list.substr(0, comma));
      list = comma == std::string::npos ? "" : list.substr(comma+1);
    }
  }
  request.connections = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 4;
  request.range_bytes = 1024*1024;
  if (const char* bytes = std::getenv("NETWORK_TEST_RANGE_BYTES")) request.range_bytes = std::strtoull(bytes, nullptr, 10);
  request.max_bytes = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1024*1024;
  const unsigned cancel_ms = argc > 5 ? static_cast<unsigned>(std::atoi(argv[5])) : 0;
  const unsigned stop_ms = argc > 9 ? static_cast<unsigned>(std::atoi(argv[9])) : 0;
  if (argc > 6) request.sha256 = argv[6];
  if (argc > 7) request.method = argv[7];
  if (std::getenv("NETWORK_TEST_RANGE_PROBE")) request.headers.emplace_back("Range: bytes=0-0");
  if (argc > 10 && std::string(argv[10]) == "pieces") {
    const std::uint64_t sizes[] = {3*1024*1024+17, 5*1024*1024+31, 7*1024*1024+9};
    std::uint64_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
      request.pieces.push_back({request.url+"/piece"+std::to_string(i), argc > 11+static_cast<int>(i) ? argv[11+i] : "", offset, sizes[i]});
      offset += sizes[i];
    }
    request.expected_bytes = offset;
  }
  std::string error;
  const std::string destination = request.destination;
  const auto id = network::enqueue(std::move(request), error);
  if (!id) { std::cerr << error << '\n'; network::stop(); return 4; }
  const char* deadline = std::getenv("NETWORK_TEST_DEADLINE_MS");
  const long long deadline_ms = deadline ? std::atoll(deadline) : 15000;
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t peak_buffer = 0;
  unsigned peak_connections = 0;
  // Resume progress: what the first "downloading" snapshot holds, and how often whole-range bytes went down.
  std::int64_t first_completed = -1;
  std::uint64_t last_completed = 0;
  unsigned completed_drops = 0;
  // What network::progress_of reported for the destination: how often its prefix went down or past the total.
  std::uint64_t last_prefix = 0;
  unsigned prefix_drops = 0, prefix_over = 0, prefix_reads = 0;
  // NETWORK_TEST_BESIDE: a request answered in memory, queued once the download is under way, and the
  // download's state when that request ended.
  const char* beside_url = std::getenv("NETWORK_TEST_BESIDE");
  std::uint32_t beside_id = 0;
  long long beside_queued = 0, beside_ms = -1;
  std::string state, beside_state, beside_body, beside_during;
  for (;;) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    if (stop_ms && elapsed >= stop_ms) {
      network::stop();
      std::cout << "{\"state\":\"stopped\",\"peakBuffer\":0,\"peakConnections\":0}\n";
      return 0;
    }
    if ((cancel_ms && elapsed >= cancel_ms) || elapsed > deadline_ms) network::cancel(id);
    if (beside_url && !beside_id && state == "downloading") {
      network::Request beside;
      beside.url = beside_url;
      beside_id = network::enqueue(std::move(beside), error);
      if (!beside_id) { std::cerr << error << '\n'; network::stop(); return 4; }
      beside_queued = elapsed;
    }
    if (std::uint64_t prefix = 0, total = 0; !destination.empty() && network::progress_of(destination, prefix, total)) {
      ++prefix_reads;
      if (prefix < last_prefix) ++prefix_drops;
      if (total && prefix > total) ++prefix_over;
      last_prefix = prefix;
    }
    for (const auto& result : network::poll()) {
      if (beside_id && result.id == beside_id) {
        if (result.state == "completed" || result.state == "failed" || result.state == "cancelled") {
          beside_state = result.state; beside_body = result.body; beside_during = state; beside_ms = elapsed-beside_queued;
        }
        continue;
      }
      state = result.state;
      peak_buffer = std::max(peak_buffer, result.buffered);
      peak_connections = std::max(peak_connections, result.connections);
      if (result.state == "downloading" && first_completed < 0) first_completed = static_cast<std::int64_t>(result.completed_bytes);
      if (result.state != "queued" && result.state != "connecting") {
        if (result.completed_bytes < last_completed) ++completed_drops;
        last_completed = result.completed_bytes;
      }
      if (result.state == "completed" || result.state == "failed" || result.state == "cancelled") {
        std::cout << "{\"state\":" << escaped(result.state) << ",\"error\":" << escaped(result.error)
          << ",\"body\":" << escaped(result.body) << ",\"written\":" << result.written
          << ",\"completedBytes\":" << result.completed_bytes << ",\"firstCompleted\":" << first_completed
          << ",\"completedDrops\":" << completed_drops << ",\"contiguous\":" << result.contiguous_bytes
          << ",\"prefixReads\":" << prefix_reads << ",\"prefixDrops\":" << prefix_drops << ",\"prefixOver\":" << prefix_over
          << ",\"url\":" << escaped(result.url) << ",\"headers\":{";
        bool first = true;
        for (const auto& [name, value] : result.headers) {
          if (!first) std::cout << ",";
          first = false; std::cout << escaped(name) << ":" << escaped(value);
        }
        std::cout << "},\"retries\":" << result.retries
          << ",\"received\":" << result.received << ",\"status\":" << result.status
          << ",\"peakBuffer\":" << peak_buffer << ",\"peakConnections\":" << peak_connections
          << ",\"milliseconds\":" << elapsed << ",\"besideId\":" << beside_id
          << ",\"besideState\":" << escaped(beside_state) << ",\"besideBody\":" << escaped(beside_body)
          << ",\"besideDuring\":" << escaped(beside_during) << ",\"besideMilliseconds\":" << beside_ms << "}" << '\n';
        network::stop(); return result.state == "completed" ? 0 : 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
