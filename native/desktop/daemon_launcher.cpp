// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Daemon.start in the preview: pandorad runs as its own session, so closing the preview (or the
// terminal that ran it) leaves it running, as the payload outlives the app on the console.
#include "daemon.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace {
// PS5_REACT_PANDORAD, or pandorad beside this executable, where the build puts it.
std::string daemon_path() {
  if (const char* path = std::getenv("PS5_REACT_PANDORAD")) return path;
  char self[PATH_MAX] = {};
#ifdef __APPLE__
  std::uint32_t size = sizeof self;
  if (_NSGetExecutablePath(self, &size)) return {};
#else
  if (readlink("/proc/self/exe", self, sizeof self - 1) < 0) return {};
#endif
  std::string path(self);
  return path.substr(0, path.rfind('/') + 1) + "pandorad";
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Whether something on the daemon's port answers GET /hello with 200.
bool hello() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(daemon_process::port()));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  char response[32] = {};
  std::size_t received = 0;
  const std::size_t length = std::strlen(daemon_process::kHelloRequest);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
      send(fd, daemon_process::kHelloRequest, length, 0) == static_cast<ssize_t>(length))
    for (ssize_t count; received < sizeof response - 1 &&
                        (count = recv(fd, response + received, sizeof response - 1 - received, 0)) > 0;)
      received += static_cast<std::size_t>(count);
  close(fd);
  return daemon_process::hello_answered(response);
}

// The daemon runs on after the start returns; reaping it here keeps it from lingering as a zombie
// of the preview once it exits.
void reap(pid_t pid) {
  std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}
} // namespace

bool daemon_process::launch(const std::string& request, const std::atomic<bool>& cancelled, std::string& error) {
  const std::string path = daemon_path();
  if (path.empty() || access(path.c_str(), X_OK)) {
    error = "pandorad is missing beside the preview (" + path + "); the build makes it for an app with a daemon entry";
    return false;
  }
  int input[2] = {-1, -1}, output[2] = {-1, -1};
  if (pipe(input) || pipe(output)) {
    error = std::string("pipe: ") + std::strerror(errno);
    for (int fd : {input[0], input[1], output[0], output[1]}) if (fd >= 0) close(fd);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  // Its log is daemon.log; an inherited stderr would hold the preview's output pipe open for its lifetime.
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  short flags = 0;
#ifdef POSIX_SPAWN_SETSID
  flags |= POSIX_SPAWN_SETSID;
#else
  flags |= POSIX_SPAWN_SETPGROUP;
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // Only the three descriptors above reach it.
#endif
  posix_spawnattr_setflags(&attributes, flags);
  char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv, environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  close(input[0]);
  close(output[1]);
  if (spawned) {
    close(input[1]);
    close(output[0]);
    error = path + ": " + std::strerror(spawned);
    return false;
  }
  // A daemon that dies before reading its request must fail the start, not end the preview.
  signal(SIGPIPE, SIG_IGN);
  const bool sent = write(input[1], request.data(), request.size()) == static_cast<ssize_t>(request.size());
  close(input[1]);

  Answer result = sent ? Answer::none : Answer::failed;
  if (!sent) error = "pandorad did not take its request";
  std::string line;
  const std::int64_t deadline = now_ms() + kReadyTimeoutMs;
  bool ended = false;
  while (result == Answer::none && !ended && !cancelled) {
    const std::int64_t left = deadline - now_ms();
    if (left <= 0) break;
    pollfd ready{output[0], POLLIN, 0};
    if (poll(&ready, 1, static_cast<int>(left < 250 ? left : 250)) <= 0) continue;
    char buffer[256];
    const ssize_t count = read(output[0], buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    ended = count <= 0;
    for (ssize_t i = 0; i < count && result == Answer::none; ++i) {
      if (buffer[i] != '\n') { if (line.size() < 1024) line.push_back(buffer[i]); continue; }
      result = answer(line, error);
      line.clear();
    }
  }
  close(output[0]);
  if (result == Answer::ready) {
    reap(pid);
    return true;
  }
  if (result == Answer::none && !ended) {
    // Still starting, or stuck: it keeps the port if it gets there, and a later start finds it.
    reap(pid);
    error = cancelled ? "cancelled" : "pandorad was not ready within 10 s; see daemon.log";
    return false;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (result == Answer::busy || (WIFEXITED(status) && WEXITSTATUS(status) == 3)) {
    if (hello()) return true;
    error = "port " + std::to_string(port()) + " is taken by a program that does not answer GET /hello";
  } else if (result == Answer::none) {
    error = "pandorad exited before it was ready (" +
            (WIFEXITED(status) ? "code " + std::to_string(WEXITSTATUS(status)) : "signal " + std::to_string(WTERMSIG(status))) +
            "); see daemon.log";
  }
  return false;
}
