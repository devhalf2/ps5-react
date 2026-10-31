// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Daemon.start on the console: pandorad.elf goes to the payload loader like the other payloads, and,
// as a child of the loader, outlives the app. It starts its workers from copies on /data, since the
// app's /app0 goes away when the app closes.
#include "daemon.hpp"
#include "app_config.hpp"
#include "digest.hpp"
#include "filesystem_access.hpp"
#include "payload_loader.hpp"
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// The daemon's FileSystem.dataDir (native/daemon/ps5.cpp).
constexpr const char* kDataRoot = "/data/ps5-react/" PS5_REACT_TITLE;

std::int64_t now_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1'000'000;
}

// The file's SHA-256, or empty when it cannot be read.
std::string file_digest(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  integrity::Hash hash;
  char buffer[65536];
  ssize_t count = 0;
  while ((count = read(fd, buffer, sizeof buffer)) > 0) hash.update(buffer, static_cast<std::size_t>(count));
  close(fd);
  return count < 0 ? std::string() : hash.finish();
}

bool make_directory(const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }

// /app0/<name> into <bin>/<name>, unless an identical copy is there. The copy is checked against the
// app's file before it replaces the old one, so a short write never leaves a broken worker in place.
bool install_worker(const std::string& name, const std::string& bin, std::string& error) {
  char source[PATH_MAX];
  if (!resolve_filesystem_path(("/app0/" + name).c_str(), source, sizeof source)) { error = name + ": path too long"; return false; }
  const std::string expected = file_digest(source);
  if (expected.empty()) { error = name + " is missing from the app folder"; return false; }
  const std::string target = bin + "/" + name, temporary = target + ".tmp";
  if (file_digest(target) == expected) return true;
  const int from = open(source, O_RDONLY), to = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
  bool ok = from >= 0 && to >= 0;
  char buffer[65536];
  for (ssize_t count; ok && (count = read(from, buffer, sizeof buffer)) != 0;)
    ok = count > 0 && write(to, buffer, static_cast<std::size_t>(count)) == count;
  if (to >= 0) ok = fsync(to) == 0 && close(to) == 0 && ok;
  if (from >= 0) close(from);
  if (ok && file_digest(temporary) != expected) { errno = EIO; ok = false; }
  if (ok) ok = rename(temporary.c_str(), target.c_str()) == 0;
  if (!ok) {
    error = "cannot copy " + name + " to " + bin + ": " + std::strerror(errno);
    unlink(temporary.c_str());
  }
  return ok;
}

// Whether a daemon answers GET /hello on 127.0.0.1:9061.
bool hello() {
  const int socket = payload_loader::connect(daemon_process::kPort, 2'000'000);
  if (socket < 0) return false;
  char response[32] = {};
  int received = 0;
  if (payload_loader::write(socket, daemon_process::kHelloRequest, std::strlen(daemon_process::kHelloRequest)))
    for (int count; received < static_cast<int>(sizeof response) - 1 &&
                    (count = payload_loader::receive(socket, response + received, sizeof response - 1 - received)) > 0;)
      received += count;
  payload_loader::close(socket);
  return daemon_process::hello_answered(response);
}
} // namespace

bool daemon_process::launch(const std::string& request, const std::atomic<bool>& cancelled, std::string& error) {
  if (!console_filesystem_accessible()) {
    error = "the daemon needs console filesystem access (app.json filesystemAccess: \"console\")";
    return false;
  }
  const std::string bin = std::string(kDataRoot) + "/bin";
  if (!make_directory("/data/ps5-react") || !make_directory(kDataRoot) || !make_directory(bin)) {
    error = "cannot create " + bin + ": " + std::strerror(errno);
    return false;
  }
  for (const char* name : {"rar-extract.elf", "pkg-installer.elf"})
    if (!install_worker(name, bin, error)) return false;
  // Receives give up each second so the deadline and a cancel are seen.
  const int socket = payload_loader::send("pandorad.elf", request, 1'000'000, "pandorad", error);
  if (socket < 0) return false;
  Answer result = Answer::none;
  bool ended = false;
  std::string line;
  for (const std::int64_t deadline = now_ms() + kReadyTimeoutMs; result == Answer::none && !ended && !cancelled && now_ms() < deadline;) {
    char buffer[256];
    const int count = payload_loader::receive(socket, buffer, sizeof buffer);
    if (count < 0) continue;
    ended = count == 0;
    for (int i = 0; i < count && result == Answer::none; ++i) {
      if (buffer[i] != '\n') { if (line.size() < 1024) line.push_back(buffer[i]); continue; }
      result = answer(line, error);
      line.clear();
    }
  }
  payload_loader::close(socket);
  if (result == Answer::ready) return true;
  if (result == Answer::busy) {
    if (hello()) return true;
    error = "port 9061 is taken by a program that does not answer GET /hello";
  } else if (result == Answer::none) {
    error = cancelled ? "cancelled" : ended ? "pandorad exited before it was ready; see daemon.log"
                                            : "pandorad was not ready within 10 s; see daemon.log";
  }
  return false;
}
