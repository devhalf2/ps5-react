// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// pandorad.elf: a payload the loader on 9021 runs, a child of the loader rather than of the app, with
// the console's whole filesystem. The app's sandbox mounts (/app0, its download0) go away when the app
// closes, so the app paths map into the title's folder on /data instead, the one the app keeps its
// cache and logs in after elevation (native/ps5/filesystem_access.cpp).
#include "daemon_platform.hpp"
#include "app_config.hpp"
#include "archives.hpp"
#include "filesystem_access.hpp"
#include "host_api.hpp"
#include "host_platform.hpp"
#include "network.hpp"
#include "payload_loader.hpp"
#include "platform/ps5/system.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr const char* kTitleData = "/data/ps5-react/" PS5_REACT_TITLE;
}

// What filesystem_access.hpp provides the app, for host_platform.cpp. No elevation: a payload already
// sees the console's filesystem.
void initialize_filesystem_access() {}
bool console_filesystem_accessible() { return true; }

bool resolve_filesystem_path(const char* path, char* out, std::size_t size) {
  static const struct { const char* logical; const char* physical; } roots[] = {
    {"/download0", kTitleData}, {"/temp0", "/data/ps5-react/" PS5_REACT_TITLE "/tmp"},
    {"/cache0", "/data/ps5-react/" PS5_REACT_TITLE "/cache"},
  };
  const char* base = path;
  const char* suffix = "";
  for (const auto& root : roots) {
    const std::size_t length = std::strlen(root.logical);
    if (!std::strncmp(path, root.logical, length) && (!path[length] || path[length] == '/')) {
      base = root.physical;
      suffix = path + length;
      break;
    }
  }
  // The app folder is gone once the app closes; the daemon's copies of its workers are in /download0/bin.
  if (base == path && !std::strncmp(path, "/app0", 5) && (!path[5] || path[5] == '/')) return false;
  const int length = std::snprintf(out, size, "%s%s", base, suffix);
  return length >= 0 && static_cast<std::size_t>(length) < size;
}

// host_api.cpp's BackHandler.exitApp support, which the daemon removes from its API: there is no app to reopen.
bool host::arrange_relaunch(char* error, std::size_t size) {
  std::snprintf(error, size, "the daemon has no app to reopen");
  return false;
}

namespace daemon_platform {
bool start(std::string& error) {
  for (const char* directory : {"/data/ps5-react", kTitleData, "/data/ps5-react/" PS5_REACT_TITLE "/tmp",
                                "/data/ps5-react/" PS5_REACT_TITLE "/cache"})
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
      error = std::string("cannot create ") + directory + ": " + std::strerror(errno);
      return false;
    }
  // Daemon.start copied rar-extract.elf and pkg-installer.elf there (native/ps5/daemon_launcher.cpp).
  payload_loader::set_directory("/download0/bin");
  return true;
}

void tick() { host_platform_tick(hui::sys::monotonic_us()); }
} // namespace daemon_platform

#if PS5_REACT_NETWORKING
extern "C" {
int sceNetInit(void);
int sceNetTerm(void);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
}
namespace network {
namespace {
int pool = -1;
bool initialized = false;
char store[256] = "";
}
// The app merges Mozilla's roots with the console's store into its cache (native/ps5/network_platform.cpp),
// which is this folder's cache/: the daemon reads that file, or the console's store before the app wrote it.
bool platform_start(std::string& error) {
  std::snprintf(store, sizeof store, "%s/cache/ca-bundle.pem", kTitleData);
  if (access(store, R_OK) != 0) std::snprintf(store, sizeof store, "/system/common/cert/CA_LIST.cer");
  if (access(store, R_OK) != 0) {
    error = "native networking cannot read a certificate store";
    return false;
  }
  // The loader's process may have initialized libSceNet already; a payload's sockets do not need the pool.
  initialized = sceNetInit() == 0;
  pool = sceNetPoolCreate("pandorad-http", 1024 * 1024, 0);
  hui::sys::log("[PANDORAD] network: certificates %s, sceNetInit %s, pool %d", store, initialized ? "ok" : "already", pool);
  return true;
}
void platform_stop() {
  if (pool >= 0) sceNetPoolDestroy(pool);
  if (initialized) sceNetTerm();
  pool = -1;
  initialized = false;
}
const char* ca_path() { return store; }
void platform_log(const char* line) { hui::sys::log("[PANDORAD] %s", line); }
} // namespace network
#endif

// rar-extract.elf goes to the payload loader as the app sends it (native/ps5/native_host.cpp), from the
// daemon's copy.
int archives::start_rar_worker(const std::string& request, std::string& error) {
  const int socket = payload_loader::send("rar-extract.elf", request, 5'000'000, "Archives.extract", error);
  if (socket >= 0) payload_loader::receive_timeout(socket, 250'000);
  return socket;
}
long archives::read_rar_worker(int stream, char* data, std::size_t size) {
  const int count = payload_loader::receive(stream, data, size);
  return count < 0 ? -1 : count;
}
void archives::cancel_rar_worker(int stream) { payload_loader::write(stream, "cancel\n", 7); }
bool archives::tell_rar_worker(int stream, const std::string& line) { return payload_loader::write(stream, line.data(), line.size()); }
void archives::close_rar_worker(int stream) { payload_loader::close(stream); }
