// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// pandorad-spike: a measurement payload for spike 0B (background downloads). Sent to the payload loader
// on 127.0.0.1:9021 like pkg-installer.elf, it downloads one file with libcurl while it logs, every 5 s,
// whether it still runs, how fast, how much memory it holds and what the console is doing (system state,
// sleep/resume events, SIGCONT, which game runs). It keeps running after the loader's socket closes and
// serves its folder on 127.0.0.1:9061/i/ for the console installer (test T11). Not shipped in any app.
//
// Request lines: "DSP1", the URL ("-" to only serve and log, "stop" to stop a running copy), the
// destination folder (empty: /data/pandora-spike), flags separated by spaces: "keepawake",
// "probe-memory", "insecure", "name=<file>", "segments=<1-4>", "log-udp=<ip>:<port>", "install=<file>" (a PKG in the folder,
// installed by the console from http://127.0.0.1:9061/i/<file>, after the download when it is that file).
// Answers "ready", "busy" or "stopped", then mirrors every log line while the socket stays open.
//
// Every Sony library function other than libkernel's is looked up at run time (kernel_dynlib_dlsym), so a
// symbol a firmware lacks is logged instead of stopping the payload from loading, and every Sony call runs
// on a helper thread with a timeout so a call that does not return is logged and skipped until it does.
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef SPIKE_HOST
#include "host_shim.h"   // the macOS smoke build (build.py --host): every Sony call answers "unavailable"
#else
#include <pthread_np.h>
#include <sys/user.h>
#include <ps5/kernel.h>
#endif

#define VERSION "1"
#define PORT 9061
#define DEFAULT_DEST "/data/pandora-spike"
#define TICK_MS 5000
#define MINUTE_MS 60000
#define SEGMENTS 4
#define SMALL_STACK (512 * 1024)
#define BIG_STACK (1024 * 1024)
#define MAX_CONNECTIONS 16
#define SONY_TIMEOUT_MS 3000
#define MAX_FAILURES 60

int sceKernelLoadStartModule(const char*, size_t, const void*, unsigned, void*, int*);
int sceKernelOpenEventFlag(intptr_t* flag, const char* name);
int sceKernelPollEventFlag(intptr_t flag, uint64_t bits, unsigned mode, uint64_t* result);
int sceKernelAvailableFlexibleMemorySize(size_t* size);
typedef struct { char useless[45]; char message[3075]; } KernelNotification;
int sceKernelSendNotificationRequest(int, KernelNotification*, size_t, int);
typedef struct { uint32_t app_id; uint64_t unknown1; uint32_t app_type; char title_id[10]; char unknown2[0x3c]; } AppInfo;
int sceKernelGetAppInfo(pid_t pid, AppInfo* info);

// The Mozilla bundle the build embeds (tools/ca_bundle.py), appended to the console's store.
extern const char spike_cacert[];
extern const unsigned long spike_cacert_length;

// ---- configuration and shared state

static struct {
  char url[2048], dest[512], name[256], udp[80], install[256];
  bool keepawake, probe_memory, insecure;
  int segments;   // parallel ranges; some hosts answer 429 to more than two
} cfg = {.segments = SEGMENTS};

static atomic_int paused;          // set while the console goes to sleep; transfers stop and checkpoint
static atomic_int sigcont_count;
static atomic_int connections;
static char last_tick[1024];
static pthread_mutex_t tick_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t mono_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms) {
  struct timespec wait = {ms / 1000, (ms % 1000) * 1000000};
  while (nanosleep(&wait, &wait) && errno == EINTR) {}
}

// The payload SDK's libc has no memmem.
static char* find(const void* haystack, size_t size, const char* needle, size_t length) {
  for (const char* at = haystack; size >= length; ++at, --size)
    if (!memcmp(at, needle, length)) return (char*)at;
  return NULL;
}

// ---- logging: <dest>/log.txt (synced every line), the loader socket while it is open, UDP when asked

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_file = -1, udp_socket = -1;
static bool socket_open = true;
static struct sockaddr_in udp_target;

static void write_all(int fd, const char* data, size_t size) {
  while (size) {
    const ssize_t count = write(fd, data, size);
    if (count <= 0) { if (fd == STDOUT_FILENO) socket_open = false; return; }
    data += count; size -= (size_t)count;
  }
}

__attribute__((format(printf, 1, 2))) static void log_line(const char* format, ...) {
  char line[2048];
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  struct tm utc;
  gmtime_r(&now.tv_sec, &utc);
  int length = snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %lld ", utc.tm_year + 1900,
                        utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, now.tv_nsec / 1000000,
                        (long long)mono_ms());
  va_list args;
  va_start(args, format);
  length += vsnprintf(line + length, sizeof line - (size_t)length - 1, format, args);
  va_end(args);
  if (length > (int)sizeof line - 2) length = (int)sizeof line - 2;
  line[length++] = '\n';
  pthread_mutex_lock(&log_lock);
  if (log_file >= 0) { write_all(log_file, line, (size_t)length); fsync(log_file); }
  if (socket_open) write_all(STDOUT_FILENO, line, (size_t)length);
  if (udp_socket >= 0) sendto(udp_socket, line, (size_t)length, 0, (struct sockaddr*)&udp_target, sizeof udp_target);
  pthread_mutex_unlock(&log_lock);
}

static bool spawn(void* (*entry)(void*), void* argument, size_t stack, const char* name) {
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, stack);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  const int error = pthread_create(&thread, &attributes, entry, argument);
  pthread_attr_destroy(&attributes);
  if (error) { log_line("thread %s: pthread_create failed %d", name ? name : "sony", error); return false; }
  if (name) pthread_set_name_np(thread, name);   // helpers may already be gone, so they stay unnamed
  return true;
}

// ---- Sony calls on a helper thread with a timeout. A call shares its block with the helper. A call that
// times out is remembered and the same function is skipped until it returns: a game can starve this
// process for a minute, so a slow answer is not taken for a hang forever.

typedef struct Call {
  int64_t (*run)(struct Call*);
  void* function;
  int64_t argument;
  char buffer[4096];   // input on the way in, a printable result on the way out
  int64_t result;
  int done, references;
  pthread_mutex_t lock;
  pthread_cond_t ready;
} Call;

#define MAX_HUNG 32
static struct { const char* name; Call* call; int64_t since; } hung[MAX_HUNG];
static int hung_count;
static pthread_mutex_t hung_lock = PTHREAD_MUTEX_INITIALIZER;

static void release(Call* call) {
  pthread_mutex_lock(&call->lock);
  const bool last = --call->references == 0;
  pthread_mutex_unlock(&call->lock);
  if (last) { pthread_mutex_destroy(&call->lock); pthread_cond_destroy(&call->ready); free(call); }
}

static void* call_main(void* argument) {
  Call* call = argument;
  const int64_t result = call->run(call);
  pthread_mutex_lock(&call->lock);
  call->result = result;
  call->done = 1;
  pthread_cond_signal(&call->ready);
  pthread_mutex_unlock(&call->lock);
  release(call);
  return NULL;
}

// Runs `run` with `function`, `argument` and `input` (copied to the buffer). Returns false when the call
// timed out or hung before; otherwise *result and, when given, the buffer in `output`. `name` must be a
// string that outlives the call (it is remembered when the call hangs).
static bool sony(const char* name, int64_t (*run)(Call*), void* function, int64_t argument, const char* input,
                 int timeout_ms, int64_t* result, char* output, size_t output_size) {
  pthread_mutex_lock(&hung_lock);
  for (int i = 0; i < hung_count; ++i) {
    if (strcmp(hung[i].name, name)) continue;
    pthread_mutex_lock(&hung[i].call->lock);
    const bool returned = hung[i].call->done;
    pthread_mutex_unlock(&hung[i].call->lock);
    if (!returned) { pthread_mutex_unlock(&hung_lock); return false; }
    log_line("sony %s: returned after all, %lld ms after it was called", name, (long long)(mono_ms() - hung[i].since));
    release(hung[i].call);
    hung[i] = hung[--hung_count];
    break;
  }
  pthread_mutex_unlock(&hung_lock);
  Call* call = calloc(1, sizeof *call);
  if (!call) return false;
  call->run = run; call->function = function; call->argument = argument; call->references = 2;
  if (input) snprintf(call->buffer, sizeof call->buffer, "%s", input);
  pthread_mutex_init(&call->lock, NULL);
  pthread_cond_init(&call->ready, NULL);
  if (!spawn(call_main, call, SMALL_STACK, NULL)) { call->references = 1; release(call); return false; }
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
  if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
  pthread_mutex_lock(&call->lock);
  while (!call->done && pthread_cond_timedwait(&call->ready, &call->lock, &deadline) != ETIMEDOUT) {}
  const bool done = call->done;
  if (done) {
    *result = call->result;
    if (output) snprintf(output, output_size, "%s", call->buffer);
  }
  pthread_mutex_unlock(&call->lock);
  if (done) { release(call); return true; }
  // The caller's reference stays with the list until the call returns.
  pthread_mutex_lock(&hung_lock);
  if (hung_count < MAX_HUNG) hung[hung_count++] = (typeof(hung[0])){name, call, mono_ms() - timeout_ms};
  else release(call);
  pthread_mutex_unlock(&hung_lock);
  log_line("sony %s: no answer in %d ms; skipped until it returns", name, timeout_ms);
  return false;
}

// ---- run-time lookup of Sony library functions

enum {
  RECEIVE_EVENT, SYSTEM_RUNNING_BIG_APP, SYSTEM_BIG_APP_LIST, LNC_RUNNING_BIG_APP, LNC_BIG_APP_LIST,
  LNC_APP_TITLE, SYSTEM_APP_STATUS, STATE_MGR_CURRENT, RESET_AUTO_POWER_DOWN, AUTO_POWER_DOWN_REMAINING,
  POWER_TICK, PLAYING_BIG_APP_TITLE, NOTIFICATION_SEND, USER_INITIALIZE, INSTALL_INITIALIZE, INSTALL_BY_PACKAGE,
  INSTALL_STATUS, SYMBOL_COUNT
};
static struct { const char *library, *name; void* address; } symbols[SYMBOL_COUNT] = {
  [RECEIVE_EVENT] = {"libSceSystemService.sprx", "sceSystemServiceReceiveEvent", NULL},
  [SYSTEM_RUNNING_BIG_APP] = {"libSceSystemService.sprx", "sceSystemServiceGetAppIdOfRunningBigApp", NULL},
  [SYSTEM_BIG_APP_LIST] = {"libSceSystemService.sprx", "sceSystemServiceGetAppIdListOfBigApp", NULL},
  [LNC_RUNNING_BIG_APP] = {"libSceSystemService.sprx", "sceLncUtilGetAppIdOfRunningBigApp", NULL},
  [LNC_BIG_APP_LIST] = {"libSceSystemService.sprx", "sceLncUtilGetAppIdListOfBigApp", NULL},
  [LNC_APP_TITLE] = {"libSceSystemService.sprx", "sceLncUtilGetAppTitleId", NULL},
  [SYSTEM_APP_STATUS] = {"libSceSystemService.sprx", "sceSystemServiceGetAppStatus", NULL},
  [STATE_MGR_CURRENT] = {"libSceSystemService.sprx", "sceSystemStateMgrGetCurrentState", NULL},
  [RESET_AUTO_POWER_DOWN] = {"libSceSystemService.sprx", "sceShellCoreUtilResetAutoPowerDownTimer", NULL},
  [AUTO_POWER_DOWN_REMAINING] = {"libSceSystemService.sprx", "sceShellCoreUtilGetAutoPowerDownRemainingSeconds", NULL},
  [POWER_TICK] = {"libSceSystemService.sprx", "sceSystemServicePowerTick", NULL},
  [PLAYING_BIG_APP_TITLE] = {"libSceUserService.sprx", "sceUserServiceGetPlayingBigAppTitleId", NULL},
  [NOTIFICATION_SEND] = {"libSceNotification.sprx", "sceNotificationSend", NULL},
  [USER_INITIALIZE] = {"libSceUserService.sprx", "sceUserServiceInitialize", NULL},
  [INSTALL_INITIALIZE] = {"libSceAppInstUtil.sprx", "sceAppInstUtilInitialize", NULL},
  [INSTALL_BY_PACKAGE] = {"libSceAppInstUtil.sprx", "sceAppInstUtilInstallByPackage", NULL},
  [INSTALL_STATUS] = {"libSceAppInstUtil.sprx", "sceAppInstUtilGetInstallStatus", NULL},
};

static int64_t run_load(Call* call) {
  // A module the process already holds answers by name; otherwise it is loaded from the firmware.
  uint32_t handle = 0;
  const char* name = strrchr(call->buffer, '/') + 1;
  if (!kernel_dynlib_handle(getpid(), name, &handle)) return handle;
  return sceKernelLoadStartModule(call->buffer, 0, NULL, 0, NULL, NULL);
}

static void resolve_symbols(void) {
  static const char* const folders[] = {"/system/common/lib/", "/system/priv/lib/"};
  static struct { const char* library; int64_t handle; } loaded[4];
  for (int i = 0; i < SYMBOL_COUNT; ++i) {
    size_t slot = 0;
    while (loaded[slot].library && strcmp(loaded[slot].library, symbols[i].library)) ++slot;
    if (!loaded[slot].library) {
      loaded[slot].library = symbols[i].library;
      loaded[slot].handle = -1;
      for (size_t f = 0; f < sizeof folders / sizeof *folders && loaded[slot].handle < 0; ++f) {
        char path[256];
        snprintf(path, sizeof path, "%s%s", folders[f], symbols[i].library);
        if (!sony(symbols[i].library, run_load, NULL, 0, path, 10000, &loaded[slot].handle, NULL, 0)) break;
        log_line("resolve: %s -> %s0x%08x", path, loaded[slot].handle < 0 ? "error " : "handle ", (unsigned)loaded[slot].handle);
      }
    }
    if (loaded[slot].handle < 0) { log_line("resolve %s: library unavailable", symbols[i].name); continue; }
    symbols[i].address = (void*)kernel_dynlib_dlsym(getpid(), (uint32_t)loaded[slot].handle, symbols[i].name);
    log_line("resolve %s: %s", symbols[i].name, symbols[i].address ? "found" : "MISSING");
  }
}

// ---- the calls themselves; each formats its result into the call's buffer

static void hex(char* out, size_t size, const uint8_t* data, size_t count) {
  for (size_t i = 0; i < count && (i + 1) * 2 < size; ++i) snprintf(out + i * 2, 3, "%02x", data[i]);
}

static int64_t run_int(Call* call) {
  const int result = ((int (*)(void))call->function)();
  snprintf(call->buffer, sizeof call->buffer, "0x%x", (unsigned)result);
  return result;
}

static int64_t run_null(Call* call) { return ((int (*)(void*))call->function)(NULL); }

static int64_t run_app_list(Call* call) {
  int32_t ids[16] = {0};
  uint32_t count = 0;
  const int result = ((int (*)(int32_t*, uint32_t, uint32_t*))call->function)(ids, 16, &count);
  int length = snprintf(call->buffer, sizeof call->buffer, "rc=0x%x n=%u", (unsigned)result, count);
  for (uint32_t i = 0; i < count && i < 16 && length < 200; ++i)
    length += snprintf(call->buffer + length, sizeof call->buffer - (size_t)length, "%s0x%x", i ? "," : " ", (unsigned)ids[i]);
  return result;
}

static int64_t run_app_title(Call* call) {
  char title[64] = {0};
  const int result = ((int (*)(int, char*))call->function)((int)call->argument, title);
  title[sizeof title - 1] = 0;
  snprintf(call->buffer, sizeof call->buffer, "rc=0x%x '%.16s'", (unsigned)result, title);
  return result;
}

static int64_t run_app_status(Call* call) {
  uint8_t status[256] = {0};
  const int result = ((int (*)(void*))call->function)(status);
  char dump[80];
  hex(dump, sizeof dump, status, 32);
  snprintf(call->buffer, sizeof call->buffer, "rc=0x%x %s", (unsigned)result, dump);
  return result;
}

static int64_t run_remaining(Call* call) {
  int seconds = -1;
  const int result = ((int (*)(int*))call->function)(&seconds);
  snprintf(call->buffer, sizeof call->buffer, "rc=0x%x %ds", (unsigned)result, seconds);
  return result;
}

// The event block is larger than any SDK's declaration (64 bytes in ezRemote, 8 KiB+ in the PS4 SDK).
static int64_t run_receive_event(Call* call) {
  uint8_t* event = calloc(1, 16384);
  if (!event) return -1;
  const int result = ((int (*)(void*))call->function)(event);
  uint32_t type;
  memcpy(&type, event, sizeof type);
  char dump[80];
  hex(dump, sizeof dump, event + 4, 24);
  snprintf(call->buffer, sizeof call->buffer, "type=0x%08x data=%s", type, dump);
  free(event);
  return result;
}

static intptr_t state_flag = -1;
static int64_t run_state_flag(Call* call) {
  if (state_flag < 0) {
    const int opened = sceKernelOpenEventFlag(&state_flag, "SceSystemStateMgrInfo");
    if (opened < 0) { state_flag = -1; snprintf(call->buffer, sizeof call->buffer, "open rc=0x%x", (unsigned)opened); return opened; }
  }
  uint64_t pattern = 0;
  const int result = sceKernelPollEventFlag(state_flag, UINT64_MAX, 2 /* OR, no clear */, &pattern);
  snprintf(call->buffer, sizeof call->buffer, "%llu", (unsigned long long)pattern);
  return result < 0 ? result : (int64_t)pattern;
}

static int64_t run_notification(Call* call) {
  return ((int (*)(int, bool, const char*))call->function)(0xFE, true, call->buffer);
}

static int64_t run_kernel_notification(Call* call) {
  KernelNotification request;
  memset(&request, 0, sizeof request);
  snprintf(request.message, sizeof request.message, "%s", call->buffer);
  return sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

static int64_t run_tick(Call* call) { ((void (*)(void))call->function)(); return 0; }

// Calls symbol `index` with `run`, formatting "name=<result>" (or why it did not run) into `out`.
static int64_t probe(int index, int64_t (*run)(Call*), int64_t argument, char* out, size_t size) {
  int64_t result = -1;
  char text[512] = "";
  if (!symbols[index].address) snprintf(text, sizeof text, "missing");
  else if (!sony(symbols[index].name, run, symbols[index].address, argument, NULL, SONY_TIMEOUT_MS, &result, text, sizeof text))
    snprintf(text, sizeof text, "hung");
  snprintf(out, size, "%s", text);
  return result;
}

// ---- notifications

static void toast(const char* bundle, const char* overwrite, const char* message, const char* detail) {
  char json[2048];
  snprintf(json, sizeof json,
           "{\"rawData\":{\"viewTemplateType\":\"ToastTemplateA\",\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
           "\"bundleName\":\"%s\",\"toastOverwriteType\":\"%s\",\"isImmediate\":true,\"priority\":100,"
           "\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
           "\"message\":{\"body\":\"%s\"},\"subMessage\":{\"body\":\"%s\"}}},"
           "\"createdDateTime\":\"2026-10-30T00:00:00.000Z\",\"localNotificationId\":\"%ld\"}",
           bundle, overwrite, message, detail, (long)(mono_ms() % 1000000000));
  int64_t result = -1;
  if (!symbols[NOTIFICATION_SEND].address) { log_line("notify %s '%s': sceNotificationSend missing", bundle, message); return; }
  if (sony("sceNotificationSend", run_notification, symbols[NOTIFICATION_SEND].address, 0, json, SONY_TIMEOUT_MS, &result, NULL, 0))
    log_line("notify %s (%s) '%s': rc=0x%x", bundle, overwrite, message, (unsigned)result);
}

static void kernel_toast(const char* message) {
  int64_t result = -1;
  if (sony("sceKernelSendNotificationRequest", run_kernel_notification, NULL, 0, message, SONY_TIMEOUT_MS, &result, NULL, 0))
    log_line("notify kernel '%s': rc=0x%x", message, (unsigned)result);
}

// One at start, three replacing ones 2 s apart, the kernel's plain variant once.
static void* notifications_main(void* argument) {
  (void)argument;
  toast("pandorad-spike-start", "No", "Pandora spike running", cfg.name[0] ? cfg.name : "serving only");
  kernel_toast("pandorad-spike: kernel notification");
  for (int i = 1; i <= 3; ++i) {
    sleep_ms(2000);
    char message[64];
    snprintf(message, sizeof message, "Spike update %d of 3", i);
    toast("pandorad-spike-progress", "InQueue", message, "should replace the previous update");
  }
  return NULL;
}

// ---- download: one file, SEGMENTS ranges in parallel, resumable from <name>.part and <name>.part.state

typedef struct {
  int index;
  int64_t start, end;      // end is exclusive; INT64_MAX when the size is unknown
  _Atomic int64_t done;
  int failures;
} Segment;

static struct {
  char part[700], final_path[700], state_path[700];
  int64_t size;            // -1 when the server does not say
  bool ranges;
  int count, fd;
  Segment segments[SEGMENTS];
  _Atomic int64_t received;  // bytes written during this run, for the rate
  atomic_int finished, active, state;  // state: 0 idle, 1 running, 2 done, 3 failed
  int64_t started_ms, base;
  struct curl_blob ca;
} download = {.fd = -1, .size = -1};

static void configure(CURL* curl, const char* url) {
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "pandorad-spike/" VERSION);
  if (download.ca.len) curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &download.ca);
  if (cfg.insecure) { curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L); }
}

// The console's store (PEM on every firmware seen) plus the bundle the build embeds.
static void load_certificates(void) {
  char* system = NULL;
  size_t system_size = 0;
  const int file = open("/system/common/cert/CA_LIST.cer", O_RDONLY);
  struct stat info;
  if (file >= 0 && !fstat(file, &info) && info.st_size > 0 && info.st_size < 4 * 1024 * 1024) {
    system = malloc((size_t)info.st_size + 1);
    if (system && read(file, system, (size_t)info.st_size) == info.st_size) system_size = (size_t)info.st_size;
  }
  if (file >= 0) close(file);
  const bool pem = system_size && find(system, system_size, "-----BEGIN CERTIFICATE-----", 27);
  char* merged = malloc((pem ? system_size + 1 : 0) + spike_cacert_length);
  if (!merged) { free(system); return; }
  size_t size = 0;
  if (pem) { memcpy(merged, system, system_size); size = system_size; merged[size++] = '\n'; }
  memcpy(merged + size, spike_cacert, spike_cacert_length);
  size += spike_cacert_length;
  free(system);
  download.ca = (struct curl_blob){merged, size, CURL_BLOB_NOCOPY};
  log_line("certificates: system store %zu bytes (%s) + bundle %lu bytes", system_size, pem ? "PEM" : "not used", spike_cacert_length);
}

static pthread_mutex_t checkpoint_lock = PTHREAD_MUTEX_INITIALIZER;

static void checkpoint(const char* why) {
  pthread_mutex_lock(&checkpoint_lock);
  if (download.fd < 0 || !download.ranges || download.state != 1) { pthread_mutex_unlock(&checkpoint_lock); return; }
  fsync(download.fd);
  char temporary[720], text[1024];
  snprintf(temporary, sizeof temporary, "%s.tmp", download.state_path);
  int length = snprintf(text, sizeof text, "DSP1\n%lld\n%d\n", (long long)download.size, download.count);
  for (int i = 0; i < download.count; ++i)
    length += snprintf(text + length, sizeof text - (size_t)length, "%lld %lld %lld\n", (long long)download.segments[i].start,
                       (long long)download.segments[i].end, (long long)download.segments[i].done);
  const int file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0) {
    log_line("checkpoint (%s): cannot write %s: %s", why, temporary, strerror(errno));
    pthread_mutex_unlock(&checkpoint_lock);
    return;
  }
  write_all(file, text, (size_t)length);
  fsync(file);
  close(file);
  if (rename(temporary, download.state_path)) log_line("checkpoint (%s): rename failed: %s", why, strerror(errno));
  else if (strcmp(why, "tick")) log_line("checkpoint (%s): %lld bytes on disk", why, (long long)atomic_load(&download.received) + download.base);
  pthread_mutex_unlock(&checkpoint_lock);
}

// Restores the segments of an earlier run of the same file, when its size still matches.
static int64_t restore(void) {
  FILE* file = fopen(download.state_path, "r");
  if (!file) return 0;
  long long size = 0, start, end, done;
  int count = 0;
  int64_t total = 0;
  char magic[8] = "";
  if (fscanf(file, "%7s %lld %d", magic, &size, &count) == 3 && !strcmp(magic, "DSP1") && size == download.size && count == download.count) {
    for (int i = 0; i < count; ++i) {
      if (fscanf(file, "%lld %lld %lld", &start, &end, &done) != 3 || start != download.segments[i].start ||
          end != download.segments[i].end || done < 0 || done > end - start) { total = -1; break; }
      download.segments[i].done = done;
      total += done;
    }
  } else total = -1;
  fclose(file);
  if (total < 0) {
    for (int i = 0; i < download.count; ++i) download.segments[i].done = 0;
    log_line("download: checkpoint does not match this file; starting over");
    return 0;
  }
  return total;
}

typedef struct { CURL* curl; char headers[512]; long status; } Probe;

static size_t probe_header(char* data, size_t size, size_t count, void* user) {
  Probe* probe = user;
  const size_t length = size * count;
  if (length > 14 && !strncasecmp(data, "content-range:", 14)) {
    const char* value = data + 14;
    size_t count = length - 14;
    while (count && *value == ' ') { ++value; --count; }
    while (count && (value[count - 1] == '\r' || value[count - 1] == '\n')) --count;
    snprintf(probe->headers, sizeof probe->headers, "%.*s", (int)(count > 200 ? 200 : count), value);
  }
  return length;
}

static size_t discard(char* data, size_t size, size_t count, void* user) { (void)data; (void)user; return size * count < 2 ? size * count : 0; }

// A one-byte range request: 206 with Content-Range names the size and proves ranges work.
static bool probe_size(void) {
  Probe probe = {curl_easy_init(), "", 0};
  if (!probe.curl) return false;
  configure(probe.curl, cfg.url);
  curl_easy_setopt(probe.curl, CURLOPT_RANGE, "0-0");
  curl_easy_setopt(probe.curl, CURLOPT_HEADERFUNCTION, probe_header);
  curl_easy_setopt(probe.curl, CURLOPT_HEADERDATA, &probe);
  curl_easy_setopt(probe.curl, CURLOPT_WRITEFUNCTION, discard);
  const int64_t began = mono_ms();
  const CURLcode code = curl_easy_perform(probe.curl);
  curl_easy_getinfo(probe.curl, CURLINFO_RESPONSE_CODE, &probe.status);
  curl_off_t length = -1;
  curl_easy_getinfo(probe.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
  char* effective = NULL;
  curl_easy_getinfo(probe.curl, CURLINFO_EFFECTIVE_URL, &effective);
  const char* slash = strchr(probe.headers, '/');
  download.ranges = probe.status == 206 && slash && slash[1] != '*';
  download.size = download.ranges ? strtoll(slash + 1, NULL, 10) : (probe.status == 200 ? (int64_t)length : -1);
  log_line("download: probe %s status=%ld curl=%d (%s) range=[%s] size=%lld ranges=%d in %lld ms, final url %.300s",
       cfg.url, probe.status, code, curl_easy_strerror(code), probe.headers, (long long)download.size, download.ranges,
       (long long)(mono_ms() - began), effective ? effective : "?");
  curl_easy_cleanup(probe.curl);
  return probe.status == 206 || probe.status == 200;
}

typedef struct { Segment* segment; CURL* curl; bool checked; } Transfer;

static size_t write_segment(char* data, size_t size, size_t count, void* user) {
  Transfer* transfer = user;
  Segment* segment = transfer->segment;
  size_t length = size * count;
  if (!transfer->checked) {
    long status = 0;
    curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &status);
    // A server that ignores the range would write the file's start at this segment's offset.
    if (download.ranges && status != 206) { log_line("segment %d: status %ld instead of 206", segment->index, status); return 0; }
    transfer->checked = true;
  }
  if (atomic_load(&paused)) return 0;
  const int64_t offset = segment->start + segment->done;
  if (segment->end != INT64_MAX && (int64_t)length > segment->end - offset) length = (size_t)(segment->end - offset);
  for (size_t written = 0; written < length;) {
    const ssize_t count_written = pwrite(download.fd, data + written, length - written, offset + (int64_t)written);
    if (count_written <= 0) { log_line("segment %d: write failed at %lld: %s", segment->index, (long long)offset, strerror(errno)); return 0; }
    written += (size_t)count_written;
  }
  segment->done += (int64_t)length;
  download.received += (int64_t)length;
  return length == size * count ? length : 0;
}

static int abort_when_paused(void* user, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)user; (void)a; (void)b; (void)c; (void)d;
  return atomic_load(&paused);
}

static void* segment_main(void* argument) {
  Segment* segment = argument;
  CURL* curl = curl_easy_init();
  char range[64];
  while (curl && (segment->end == INT64_MAX || segment->done < segment->end - segment->start)) {
    if (atomic_load(&paused)) {
      log_line("segment %d: paused at %lld", segment->index, (long long)segment->done);
      while (atomic_load(&paused)) sleep_ms(500);
      log_line("segment %d: resuming at %lld", segment->index, (long long)segment->done);
    }
    if (!download.ranges) {   // no ranges: every attempt starts the file over
      download.received -= segment->done;
      segment->done = 0;
      ftruncate(download.fd, 0);
    }
    curl_easy_reset(curl);
    configure(curl, cfg.url);
    Transfer transfer = {segment, curl, false};
    if (download.ranges) {
      snprintf(range, sizeof range, "%lld-%lld", (long long)(segment->start + segment->done), (long long)(segment->end - 1));
      curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_segment);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_paused);
    const int64_t began = mono_ms(), before = segment->done;
    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK && (segment->end == INT64_MAX || segment->done >= segment->end - segment->start)) break;
    if (atomic_load(&paused)) continue;
    if (++segment->failures >= MAX_FAILURES) {
      log_line("segment %d: giving up after %d failures", segment->index, segment->failures);
      download.state = 3;
      break;
    }
    const int64_t backoff = segment->failures < 5 ? 2000 << segment->failures : 30000;
    log_line("segment %d: curl=%d (%s) after %lld bytes in %lld ms; failure %d, retry in %lld ms", segment->index, code,
         curl_easy_strerror(code), (long long)(segment->done - before), (long long)(mono_ms() - began), segment->failures,
         (long long)(backoff > 30000 ? 30000 : backoff));
    sleep_ms(backoff > 30000 ? 30000 : backoff);
    if (segment->done > before) segment->failures = 0;
  }
  if (curl) curl_easy_cleanup(curl);
  log_line("segment %d: ended with %lld bytes", segment->index, (long long)segment->done);
  download.finished++;
  return NULL;
}

static void* download_main(void* argument) {
  (void)argument;
  download.state = 1;
  download.started_ms = mono_ms();
  while (!probe_size()) {
    log_line("download: probe failed; retrying in 30 s");
    sleep_ms(30000);
  }
  download.count = download.ranges && download.size >= SEGMENTS * 1024 * 1024 ? cfg.segments : 1;
  const int64_t size = download.size;
  for (int i = 0; i < download.count; ++i) {
    Segment* segment = &download.segments[i];
    segment->index = i;
    segment->start = size > 0 ? size / download.count * i : 0;
    segment->end = !download.ranges || size <= 0 ? INT64_MAX : i == download.count - 1 ? size : size / download.count * (i + 1);
  }
  download.fd = open(download.part, O_RDWR | O_CREAT, 0644);
  if (download.fd < 0) { log_line("download: cannot open %s: %s", download.part, strerror(errno)); download.state = 3; return NULL; }
  struct stat info;
  if (download.ranges && !fstat(download.fd, &info) && info.st_size > 0) download.base = restore();
  if (download.ranges && size > 0) ftruncate(download.fd, size);
  struct statfs disk;
  if (!statfs(cfg.dest, &disk))
    log_line("download: %s free %llu MiB", cfg.dest, (unsigned long long)(disk.f_bavail * disk.f_bsize >> 20));
  log_line("download: %d segment(s), resuming from %lld of %lld bytes into %s", download.count, (long long)download.base,
       (long long)size, download.part);
  for (int i = 0; i < download.count; ++i) {
    char name[32];
    snprintf(name, sizeof name, "pdspike-seg%d", i);
    if (!spawn(segment_main, &download.segments[i], BIG_STACK, name)) download.finished++;
  }
  while (download.finished < download.count) sleep_ms(500);
  const int64_t elapsed = mono_ms() - download.started_ms;
  int64_t total = 0;
  for (int i = 0; i < download.count; ++i) total += download.segments[i].done;
  if (download.state == 3 || (size > 0 && total != size)) {
    download.state = 1;   // checkpoint() writes only while running
    checkpoint("failed");
    download.state = 3;
    log_line("download: FAILED with %lld of %lld bytes after %lld s", (long long)total, (long long)size, (long long)(elapsed / 1000));
    toast("pandorad-spike-end", "No", "Spike download failed", cfg.name);
    return NULL;
  }
  fsync(download.fd);
  close(download.fd);
  download.fd = -1;
  if (rename(download.part, download.final_path)) log_line("download: rename failed: %s", strerror(errno));
  unlink(download.state_path);
  download.state = 2;
  log_line("download: DONE %lld bytes in %lld s, %.2f MB/s over this run", (long long)total, (long long)(elapsed / 1000),
       elapsed ? (double)download.received / 1e6 / ((double)elapsed / 1000) : 0.0);
  toast("pandorad-spike-end", "No", "Spike download finished", cfg.name);
  return NULL;
}

// ---- console state: system events, the SceSystemStateMgrInfo flag, SIGCONT

static const char* event_name(uint32_t type) {
  switch (type) {
    case 0x10000000: return "ON_RESUME";
    case 0x10000001: return "BEFORE_SLEEP (ezRemote's value)";
    default: return "?";
  }
}

static atomic_int last_flag = -1;

static void set_paused(bool sleeping, const char* why) {
  if (atomic_exchange(&paused, sleeping) == sleeping) return;
  log_line("%s: %s", sleeping ? "PAUSE" : "RESUME", why);
  if (sleeping) { sleep_ms(200); checkpoint("going to sleep"); }
}

#ifdef SPIKE_HOST
// The smoke build has no console states: SIGUSR1 plays "going to sleep", SIGUSR2 "back to WORKING".
static atomic_int host_sleep = -1;
static void on_host_signal(int number) { atomic_store(&host_sleep, number == SIGUSR1); }
#endif

static void* state_main(void* argument) {
  (void)argument;
  int64_t previous = mono_ms();
  int last_receive = 1, last_sigcont = 0;
  char text[512];
  for (;;) {
    sleep_ms(500);
    const int64_t now = mono_ms();
    if (now - previous > 10000) log_line("state: this thread did not run for %lld ms", (long long)(now - previous));
    previous = now;
    for (int i = 0; i < 8 && symbols[RECEIVE_EVENT].address; ++i) {
      const int result = (int)probe(RECEIVE_EVENT, run_receive_event, 0, text, sizeof text);
      if (result != last_receive) { log_line("event: receive rc=0x%x", (unsigned)result); last_receive = result; }
      if (result) break;
      uint32_t type = (uint32_t)strtoul(text + 5, NULL, 16);
      log_line("event: %s %s", text, event_name(type));
      if (type == 0x10000001) set_paused(true, "BEFORE_SLEEP event");
      if (type == 0x10000000) set_paused(false, "ON_RESUME event");
    }
    int64_t pattern = -1;
    char flag[64] = "";
    if (!sony("SceSystemStateMgrInfo", run_state_flag, NULL, 0, NULL, SONY_TIMEOUT_MS, &pattern, flag, sizeof flag)) pattern = -2;
    const int value = pattern < 0 ? (int)pattern : (int)(pattern & 0xffff);
    if (value != atomic_exchange(&last_flag, value)) {
      log_line("state: SceSystemStateMgrInfo %s (%s)", flag, value == 1000 ? "WORKING" : value == 300 ? "SUSPEND_ON_GOING" :
           value == 500 ? "MAIN_ON_STANDBY" : value == 100 ? "SHUTDOWN_ON_GOING" : "?");
      if (value == 300 || value == 500 || value == 100) set_paused(true, "SceSystemStateMgrInfo leaves WORKING");
      if (value == 1000) set_paused(false, "SceSystemStateMgrInfo back to WORKING");
    }
#ifdef SPIKE_HOST
    const int host = atomic_exchange(&host_sleep, -1);
    if (host >= 0) set_paused(host, host ? "SIGUSR1 (host: sleep)" : "SIGUSR2 (host: wake)");
#endif
    const int sigcont = atomic_load(&sigcont_count);
    if (sigcont != last_sigcont) {
      log_line("signal: SIGCONT x%d", sigcont - last_sigcont);
      last_sigcont = sigcont;
      if (atomic_load(&last_flag) == 1000 || atomic_load(&last_flag) < 0) set_paused(false, "SIGCONT");
    }
  }
  return NULL;
}

static void on_sigcont(int number) { (void)number; atomic_fetch_add(&sigcont_count, 1); }


// ---- memory: the process's resident size, the flexible memory left, and the periodic allocation probe

static void memory_text(char* out, size_t size) {
  // The kernel's kinfo_proc may be larger than the SDK's, so the answer gets room to spare.
  long long pages = -1;
#ifndef SPIKE_HOST
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  static _Thread_local char info[4096];
  size_t length = sizeof info;
  if (!sysctl(mib, 4, info, &length, NULL, 0) && length >= offsetof(struct kinfo_proc, ki_rssize) + sizeof(segsz_t))
    pages = (long long)((struct kinfo_proc*)info)->ki_rssize;
#endif
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  size_t flexible = 0;
  const int flexible_rc = sceKernelAvailableFlexibleMemorySize(&flexible);
  char flexible_text[32];
  if (flexible_rc) snprintf(flexible_text, sizeof flexible_text, "err:0x%x", (unsigned)flexible_rc);
  else snprintf(flexible_text, sizeof flexible_text, "%zuMiB", flexible >> 20);
  snprintf(out, size, "rss=%lldMiB(%lld pages) maxrss=%ldMiB flex_free=%s", pages < 0 ? -1 : pages * PAGE_SIZE >> 20, pages,
           usage.ru_maxrss >> 10, flexible_text);
}

static void memory_probe(void) {
  static const int sizes[] = {64, 128, 256, 384};
  char memory[160];
  memory_text(memory, sizeof memory);
  log_line("memprobe: before %s", memory);
  for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
    const size_t bytes = (size_t)sizes[i] << 20;
    log_line("memprobe: %d MiB start", sizes[i]);
    const int64_t began = mono_ms();
    char* block = malloc(bytes);
    const int64_t allocated = mono_ms();
    if (!block) { log_line("memprobe: %d MiB FAIL malloc in %lld ms", sizes[i], (long long)(allocated - began)); break; }
    // Volatile stores: the compiler must not drop writes to memory that is freed right after.
    volatile char* pages = block;
    for (size_t page = 0; page < bytes; page += 4096) pages[page] = (char)page;
    const int64_t touched = mono_ms();
    free(block);
    log_line("memprobe: %d MiB ok malloc=%lldms touch=%lldms free=%lldms", sizes[i], (long long)(allocated - began),
         (long long)(touched - allocated), (long long)(mono_ms() - touched));
  }
}

// Everything that runs once a minute: auto-power-down state, the keep-awake reset, the memory probe.
static void* minute_main(void* argument) {
  (void)argument;
  char text[256];
  for (;;) {
    probe(AUTO_POWER_DOWN_REMAINING, run_remaining, 0, text, sizeof text);
    if (cfg.keepawake) {
      char reset[64];
      probe(RESET_AUTO_POWER_DOWN, run_int, 0, reset, sizeof reset);
      char after[64];
      probe(AUTO_POWER_DOWN_REMAINING, run_remaining, 0, after, sizeof after);
      log_line("keepawake: remaining before %s, reset %s, after %s", text, reset, after);
    } else log_line("autopowerdown: remaining %s (keepawake off)", text);
    if (cfg.probe_memory) memory_probe();
    sleep_ms(MINUTE_MS);
  }
  return NULL;
}

// ---- running-game detection: every candidate the SDK names, plus the process list

static void game_text(char* out, size_t size) {
  char system_running[64], lnc_running[64], system_list[256], lnc_list[256], title[96] = "-", status[128], playing[16];
  const int64_t app = probe(SYSTEM_RUNNING_BIG_APP, run_int, 0, system_running, sizeof system_running);
  const int64_t lnc = probe(LNC_RUNNING_BIG_APP, run_int, 0, lnc_running, sizeof lnc_running);
  probe(SYSTEM_BIG_APP_LIST, run_app_list, 0, system_list, sizeof system_list);
  probe(LNC_BIG_APP_LIST, run_app_list, 0, lnc_list, sizeof lnc_list);
  const int64_t id = app > 0 && app != 0xffffffff ? app : lnc > 0 && lnc != 0xffffffff ? lnc : 0;
  if (id) probe(LNC_APP_TITLE, run_app_title, id, title, sizeof title);
  probe(SYSTEM_APP_STATUS, run_app_status, 0, status, sizeof status);
  snprintf(playing, sizeof playing, "%s", symbols[PLAYING_BIG_APP_TITLE].address ? "resolved" : "missing");
  // Processes that carry a title ID, with their resident size; a game shows up here with its own.
  char processes[512] = "";
#ifndef SPIKE_HOST
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t length = 0, used = 0;
  if (!sysctl(mib, 4, NULL, &length, NULL, 0)) {
    length += length / 4;
    char* list = malloc(length);
    if (list && !sysctl(mib, 4, list, &length, NULL, 0)) {
      for (char* item = list; item < list + length && used < sizeof processes - 64;) {
        struct kinfo_proc* info = (struct kinfo_proc*)item;
        if (info->ki_structsize <= 0) break;
        item += info->ki_structsize;
        AppInfo app_info;
        memset(&app_info, 0, sizeof app_info);
        if (sceKernelGetAppInfo(info->ki_pid, &app_info) || !app_info.title_id[0]) continue;
        app_info.title_id[9] = 0;
        used += (size_t)snprintf(processes + used, sizeof processes - used, "%s%s:%.12s/0x%x/%lldMiB", used ? "," : "",
                                 app_info.title_id, info->ki_comm, app_info.app_id,
                                 (long long)info->ki_rssize * PAGE_SIZE >> 20);
      }
    }
    free(list);
  }
#endif
  snprintf(out, size, "game[sysRunBig=%s lncRunBig=%s sysList=%s lncList=%s title=%s appStatus=%s playingTitle=%s] procs[%s]",
           system_running, lnc_running, system_list, lnc_list, title, status, playing, processes);
}

// ---- 127.0.0.1:9061: /i/<file> with Range for the console installer, /status, /quit

static bool send_text(int fd, const char* text, size_t size) {
  while (size) {
    const ssize_t count = send(fd, text, size, 0);
    if (count <= 0) return false;
    text += count; size -= (size_t)count;
  }
  return true;
}

static void header_value(const char* headers, const char* name, char* out, size_t size) {
  out[0] = 0;
  const size_t length = strlen(name);
  for (const char* line = strstr(headers, "\r\n"); line; line = strstr(line, "\r\n")) {
    line += 2;
    if (strncasecmp(line, name, length) || line[length] != ':') continue;
    const char* value = line + length + 1;
    while (*value == ' ') ++value;
    const char* end = strstr(value, "\r\n");
    snprintf(out, size, "%.*s", (int)(end ? end - value : (long)strlen(value)), value);
    return;
  }
}

static bool plain_name(const char* name) {
  if (!*name || strlen(name) > 200 || name[0] == '.') return false;
  for (const char* c = name; *c; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_'))
      return false;
  return true;
}

// Answers one request; false closes the connection.
static bool serve(int fd, int id, const char* request, char* buffer, size_t buffer_size) {
  const int64_t began = mono_ms();
  char method[8] = "", target[512] = "", range[128], agent[160], connection[32], response[512];
  sscanf(request, "%7s %511s", method, target);
  header_value(request, "Range", range, sizeof range);
  header_value(request, "User-Agent", agent, sizeof agent);
  header_value(request, "Connection", connection, sizeof connection);
  const bool head = !strcmp(method, "HEAD"), keep = strcasecmp(connection, "close") != 0;
  char* query = strchr(target, '?');
  if (query) *query++ = 0;
  int status = 404;
  long long sent = 0, first = 0, last = -1, size = -1;
  if (strcmp(method, "GET") && !head) status = 405;
  else if (!strcmp(target, "/quit")) {
    static const char bye[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbye\n";
    send_text(fd, bye, sizeof bye - 1);
    log_line("http #%d %s /quit: stopping", id, method);
    set_paused(true, "quit requested");
    log_line("=== exit (quit)");
    _exit(0);
  } else if (!strcmp(target, "/status")) {
    pthread_mutex_lock(&tick_lock);
    const int length = snprintf(buffer, buffer_size, "%s\n", last_tick);
    pthread_mutex_unlock(&tick_lock);
    const int header = snprintf(response, sizeof response, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n", length);
    status = 200;
    if (!send_text(fd, response, (size_t)header) || (!head && !send_text(fd, buffer, (size_t)length))) return false;
    log_line("http #%d %s %s -> 200", id, method, target);
    return keep;
  } else if (!strncmp(target, "/i/", 3) && plain_name(target + 3)) {
    const char* name = target + 3;
    const size_t name_length = strlen(name);
    // The installer asks for <content ID>.crc, which must not exist.
    if (!(name_length > 4 && !strcmp(name + name_length - 4, ".crc"))) {
      char path[800];
      snprintf(path, sizeof path, "%s/%s", cfg.dest, name);
      const int file = open(path, O_RDONLY);
      struct stat info;
      if (file >= 0 && !fstat(file, &info) && S_ISREG(info.st_mode)) {
        size = info.st_size;
        status = 200;
        last = size - 1;
        if (range[0]) {
          long long a = -1, b = -1;
          if (!strncmp(range, "bytes=-", 7)) { const long long n = atoll(range + 7); a = size - (n < size ? n : size); b = size - 1; }
          else if (sscanf(range, "bytes=%lld-%lld", &a, &b) < 1) a = -1;
          if (b < 0 || b >= size) b = size - 1;
          if (a < 0 || a >= size || a > b) status = 416;
          else { status = 206; first = a; last = b; }
        }
        char content_range[96] = "";
        if (status == 206) snprintf(content_range, sizeof content_range, "Content-Range: bytes %lld-%lld/%lld\r\n", first, last, size);
        const int header = status == 416
          ? snprintf(response, sizeof response, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\n\r\n", size)
          : snprintf(response, sizeof response, "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                     "Content-Length: %lld\r\n%s\r\n", status, status == 206 ? "Partial Content" : "OK", last - first + 1, content_range);
        bool ok = send_text(fd, response, (size_t)header);
        if (ok && !head && status != 416) {
          for (long long offset = first; offset <= last && ok;) {
            const size_t want = (size_t)(last - offset + 1 < (long long)buffer_size ? last - offset + 1 : (long long)buffer_size);
            const ssize_t got = pread(file, buffer, want, offset);
            if (got <= 0) { ok = false; break; }
            ok = send_text(fd, buffer, (size_t)got);
            if (ok) { offset += got; sent += got; }
          }
        }
        close(file);
        log_line("http #%d %s /i/%s?%s range=[%s] ua=[%s] -> %d bytes %lld-%lld/%lld sent=%lld in %lld ms%s", id, method, name,
             query ? query : "", range, agent, status, first, last, size, sent, (long long)(mono_ms() - began), ok ? "" : " (connection lost)");
        return ok && keep;
      }
      if (file >= 0) close(file);
    }
  }
  const int header = snprintf(response, sizeof response, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n\r\n", status,
                              status == 405 ? "Method Not Allowed" : "Not Found");
  log_line("http #%d %s %s?%s range=[%s] ua=[%s] -> %d", id, method, target, query ? query : "", range, agent, status);
  return send_text(fd, response, (size_t)header) && keep;
}

typedef struct { int fd, id; } Client;

static void* client_main(void* argument) {
  Client client = *(Client*)argument;
  free(argument);
  char* request = malloc(8192);
  char* buffer = malloc(256 * 1024);
  size_t filled = 0;
  struct timeval timeout = {60, 0};
  setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  while (request && buffer) {
    char* end = filled ? find(request, filled, "\r\n\r\n", 4) : NULL;
    if (!end) {
      if (filled >= 8191) break;
      const ssize_t count = recv(client.fd, request + filled, 8191 - filled, 0);
      if (count <= 0) break;
      filled += (size_t)count;
      continue;
    }
    *end = 0;   // requests to this server carry no body
    const size_t used = (size_t)(end - request) + 4;
    if (!serve(client.fd, client.id, request, buffer, 256 * 1024)) break;
    memmove(request, request + used, filled - used);
    filled -= used;
  }
  free(request);
  free(buffer);
  close(client.fd);
  connections--;
  return NULL;
}

static void* server_main(void* argument) {
  const int listener = (int)(intptr_t)argument;
  for (int id = 1;; ++id) {
    const int fd = accept(listener, NULL, NULL);
    if (fd < 0) { log_line("http: accept failed: %s", strerror(errno)); sleep_ms(1000); continue; }
    if (connections >= MAX_CONNECTIONS) {
      static const char busy[] = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n";
      send_text(fd, busy, sizeof busy - 1);
      close(fd);
      log_line("http #%d: refused, %d connections open", id, MAX_CONNECTIONS);
      continue;
    }
    Client* client = malloc(sizeof *client);
    if (!client) { close(fd); continue; }
    *client = (Client){fd, id};
    connections++;
    if (!spawn(client_main, client, SMALL_STACK, "pdspike-http")) { free(client); close(fd); connections--; }
  }
  return NULL;
}

// Asks the copy that holds the port to quit (url "stop").
static void stop_other(void) {
  static const char quit[] = "GET /quit HTTP/1.1\r\n\r\n";
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  char reply[128] = "";
  if (fd >= 0 && !connect(fd, (struct sockaddr*)&address, sizeof address) && send_text(fd, quit, sizeof quit - 1)) {
    const ssize_t count = recv(fd, reply, sizeof reply - 1, 0);
    if (count > 0) reply[count] = 0;
  }
  if (fd >= 0) close(fd);
  char* newline = strchr(reply, '\r');
  if (newline) *newline = 0;
  log_line("stop: the running copy answered '%s'", reply);
}

// ---- T11: the console's installer pulls a PKG from this payload's own server (Route U)

// The layouts pkg_installer.c uses (native/ps5/payloads). The status block is read into 4 KiB and dumped,
// since ShadowMountPlus describes it as 0x2C8 bytes with the error at 0xA0 (spec E5).
typedef struct {
  const char *uri, *ex_uri, *playgo_scenario_id, *content_id, *content_name, *icon_url;
  uint64_t tail[8];
} MetaInfo;
typedef struct { char content_id[48]; int type; int platform; } PkgInfo;
typedef struct { char languages[30][8]; char scenario_ids[64][3]; char content_ids[64][48]; char unknown[6480]; } PlayGoInfo;
typedef struct { int32_t code, version; char description[512]; char type[9]; } ErrorInfo;
typedef struct {
  char status[16]; char src_type[8]; uint32_t remain_time;
  uint64_t downloaded, initial_chunk, total; uint32_t promote_progress;
  ErrorInfo error; int32_t local_copy_percent; _Bool copy_only;
  char reserved[256];
} InstallStatus;

// Buffer in: "<uri>\n<content ID>\n<name>". Out: the content ID the console reports.
static int64_t run_install(Call* call) {
  char* uri = call->buffer;
  char* content_id = strchr(uri, '\n');
  if (!content_id) return -1;
  *content_id++ = 0;
  char* name = strchr(content_id, '\n');
  if (!name) return -1;
  *name++ = 0;
  MetaInfo meta;
  memset(&meta, 0, sizeof meta);   // a nonzero tail makes the console treat the package as a patch
  meta.uri = uri; meta.ex_uri = ""; meta.playgo_scenario_id = ""; meta.content_id = content_id;
  meta.content_name = name; meta.icon_url = "";
  PkgInfo info;
  memset(&info, 0, sizeof info);
  PlayGoInfo* playgo = calloc(1, sizeof *playgo);
  if (!playgo) return -1;
  const int result = ((int (*)(MetaInfo*, PkgInfo*, PlayGoInfo*))call->function)(&meta, &info, playgo);
  free(playgo);
  info.content_id[sizeof info.content_id - 1] = 0;
  snprintf(call->buffer, sizeof call->buffer, "%s", info.content_id);
  return result;
}

// Buffer in: the content ID. Out: the parsed fields, then the first 0x2C8 bytes in hex.
static int64_t run_install_status(Call* call) {
  uint8_t* raw = calloc(1, 4096);
  if (!raw) return -1;
  char content_id[64];
  snprintf(content_id, sizeof content_id, "%s", call->buffer);
  const int result = ((int (*)(const char*, void*))call->function)(content_id, raw);
  InstallStatus status;
  memcpy(&status, raw, sizeof status);
  status.status[sizeof status.status - 1] = 0;
  status.error.description[sizeof status.error.description - 1] = 0;
  int32_t error_a0;
  memcpy(&error_a0, raw + 0xA0, sizeof error_a0);
  int length = snprintf(call->buffer, sizeof call->buffer, "status=%s downloaded=%llu total=%llu error=0x%x@%zu 0x%x@0xA0 '%.100s' raw=",
                        status.status, (unsigned long long)status.downloaded, (unsigned long long)status.total,
                        (unsigned)status.error.code, offsetof(InstallStatus, error), (unsigned)error_a0, status.error.description);
  hex(call->buffer + length, sizeof call->buffer - (size_t)length, raw, 0x2C8);
  free(raw);
  return result;
}

// The content ID: a PS4 PKG (\x7FCNT) names it at 0x40; a PS5 PKG (\x7FFIH) keeps such a header at the
// offset its field at 0x58 gives.
static bool content_id_of(const char* path, char* out) {
  char header[0x40 + 36];
  const int file = open(path, O_RDONLY);
  if (file < 0) return false;
  ssize_t got = pread(file, header, sizeof header, 0);
  if (got == (ssize_t)sizeof header && !memcmp(header, "\x7F" "FIH", 4)) {
    uint64_t offset;
    memcpy(&offset, header + 0x58, sizeof offset);
    got = offset <= INT64_MAX ? pread(file, header, sizeof header, (off_t)offset) : -1;
  }
  close(file);
  if (got != (ssize_t)sizeof header || memcmp(header, "\x7F" "CNT", 4)) return false;
  memcpy(out, header + 0x40, 36);
  out[36] = 0;
  return true;
}

static void* install_main(void* argument) {
  (void)argument;
  if (!plain_name(cfg.install)) { log_line("install: '%s' is not a plain file name", cfg.install); return NULL; }
  // The download's own file installs once it is complete; any other file already is.
  if (!strcmp(cfg.install, cfg.name)) {
    log_line("install: waiting for the download to finish");
    while (download.state != 2) {
      if (download.state == 3) { log_line("install: the download failed; nothing to install"); return NULL; }
      sleep_ms(1000);
    }
  }
  char path[800], uri[160], content_id[40], text[4096];
  snprintf(path, sizeof path, "%s/%s", cfg.dest, cfg.install);
  if (!content_id_of(path, content_id)) { log_line("install: %s is missing or not a PKG", path); return NULL; }
  const int uri_length = snprintf(uri, sizeof uri, "http://127.0.0.1:%d/i/%s", PORT, cfg.install);
  if (uri_length > 127) { log_line("install: %s is %d bytes; the installer takes 127 at most", uri, uri_length); return NULL; }
  for (int i = USER_INITIALIZE; i <= INSTALL_STATUS; ++i)
    if (!symbols[i].address) { log_line("install: %s is missing", symbols[i].name); return NULL; }
  int64_t result = -1;
  if (sony(symbols[USER_INITIALIZE].name, run_null, symbols[USER_INITIALIZE].address, 0, NULL, 10000, &result, NULL, 0))
    log_line("install: sceUserServiceInitialize rc=0x%x", (unsigned)result);
  if (!sony(symbols[INSTALL_INITIALIZE].name, run_int, symbols[INSTALL_INITIALIZE].address, 0, NULL, 10000, &result, NULL, 0)) return NULL;
  log_line("install: sceAppInstUtilInitialize rc=0x%x", (unsigned)result);
  char request[400];
  snprintf(request, sizeof request, "%s\n%s\npandorad-spike T11", uri, content_id);
  if (!sony(symbols[INSTALL_BY_PACKAGE].name, run_install, symbols[INSTALL_BY_PACKAGE].address, 0, request, 30000, &result, text, sizeof text))
    return NULL;
  log_line("install: InstallByPackage %s (%s) rc=0x%x reported content ID '%s'", uri, content_id, (unsigned)result, text);
  if (result) { toast("pandorad-spike-end", "No", "Spike install refused", cfg.install); return NULL; }
  const char* installing = text[0] ? text : content_id;
  char id[48];
  snprintf(id, sizeof id, "%s", installing);
  // The status every 5 s, with the raw block the first time and whenever the stage changes.
  char stage[32] = "";
  for (int unreadable = 0;;) {
    sleep_ms(5000);
    if (!sony(symbols[INSTALL_STATUS].name, run_install_status, symbols[INSTALL_STATUS].address, 0, id, SONY_TIMEOUT_MS, &result, text, sizeof text))
      return NULL;
    char* raw = strstr(text, " raw=");
    char current[32] = "";
    sscanf(text, "status=%31s", current);
    if (result) {
      log_line("install: status rc=0x%x", (unsigned)result);
      if (++unreadable >= 12) { log_line("install: status unreadable for a minute; stopping the watch"); return NULL; }
      continue;
    }
    unreadable = 0;
    const bool changed = strcmp(current, stage) != 0;
    if (!changed && raw) *raw = 0;
    log_line("install: %s", text);
    snprintf(stage, sizeof stage, "%s", current);
    if (!strcmp(current, "playable") || !strcmp(current, "completed") || !strcmp(current, "error")) {
      toast("pandorad-spike-end", "No", strcmp(current, "error") ? "Spike install finished" : "Spike install failed", cfg.install);
      return NULL;
    }
  }
}

// ---- request and start-up

static int read_line(char* line, size_t capacity) {
  size_t length = 0;
  for (;;) {
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 15000) <= 0) { line[length] = 0; return length > 0; }
    char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1) { line[length] = 0; return length > 0 ? 1 : -1; }
    if (byte == '\r') continue;
    if (byte == '\n') { line[length] = 0; return 1; }
    if (length + 1 >= capacity) return 0;
    line[length++] = byte;
  }
}

static void say(const char* text) { write_all(STDOUT_FILENO, text, strlen(text)); }

static void parse_flags(char* flags) {
  for (char* flag = strtok(flags, " "); flag; flag = strtok(NULL, " ")) {
    if (!strcmp(flag, "keepawake")) cfg.keepawake = true;
    else if (!strcmp(flag, "probe-memory")) cfg.probe_memory = true;
    else if (!strcmp(flag, "insecure")) cfg.insecure = true;
    else if (!strncmp(flag, "name=", 5)) snprintf(cfg.name, sizeof cfg.name, "%s", flag + 5);
    else if (!strncmp(flag, "segments=", 9) && atoi(flag + 9) >= 1 && atoi(flag + 9) <= SEGMENTS) cfg.segments = atoi(flag + 9);
    else if (!strncmp(flag, "install=", 8)) snprintf(cfg.install, sizeof cfg.install, "%s", flag + 8);
    else if (!strncmp(flag, "log-udp=", 8)) snprintf(cfg.udp, sizeof cfg.udp, "%s", flag + 8);
    else log_line("request: unknown flag '%s' ignored", flag);
  }
}

// The file name: name=, else the URL's last path segment, else download.bin.
static void pick_name(void) {
  if (!cfg.name[0]) {
    char path[2048];
    snprintf(path, sizeof path, "%s", cfg.url);
    path[strcspn(path, "?#")] = 0;
    const char* scheme = strstr(path, "://");
    const char* slash = strrchr(path, '/');
    snprintf(cfg.name, sizeof cfg.name, "%s", scheme && slash > scheme + 2 ? slash + 1 : "");
  }
  for (char* c = cfg.name; *c; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_')) *c = '_';
  if (!plain_name(cfg.name)) snprintf(cfg.name, sizeof cfg.name, "download.bin");
}

static void make_folders(const char* path) {
  char partial[512];
  for (const char* slash = strchr(path + 1, '/');; slash = strchr(slash + 1, '/')) {
    snprintf(partial, sizeof partial, "%.*s", (int)(slash ? slash - path : (long)strlen(path)), path);
    mkdir(partial, 0777);
    if (!slash) return;
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_handler = on_sigcont;
  sigaction(SIGCONT, &action, NULL);
#ifdef SPIKE_HOST
  signal(SIGUSR1, on_host_signal);
  signal(SIGUSR2, on_host_signal);
#endif
  pthread_set_name_np(pthread_self(), "pandorad-spike");
  struct timeval send_timeout = {5, 0};
  setsockopt(STDOUT_FILENO, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof send_timeout);

  char magic[8], dest[512] = "", flags[512] = "";
  if (read_line(magic, sizeof magic) != 1 || strcmp(magic, "DSP1") || read_line(cfg.url, sizeof cfg.url) != 1) {
    say("fail expected DSP1, url, dest, flags\n");
    return 1;
  }
  read_line(dest, sizeof dest);
  read_line(flags, sizeof flags);
  snprintf(cfg.dest, sizeof cfg.dest, "%s", dest[0] ? dest : DEFAULT_DEST);
  if (cfg.dest[0] != '/' || strstr(cfg.dest, "..")) { say("fail dest must be an absolute path\n"); return 1; }
  parse_flags(flags);
  if (cfg.udp[0]) {
    char host[64];
    int port = 0;
    if (sscanf(cfg.udp, "%63[^:]:%d", host, &port) == 2 && inet_pton(AF_INET, host, &udp_target.sin_addr) == 1) {
      udp_target.sin_family = AF_INET;
      udp_target.sin_port = htons((uint16_t)port);
      udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    } else say("warning log-udp needs <ip>:<port>\n");
  }

  // Singleton: whoever holds 127.0.0.1:9061 is the running copy.
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  const int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  const bool bound = listener >= 0 && !bind(listener, (struct sockaddr*)&address, sizeof address) && !listen(listener, 16);
  if (!bound) {
    const int error = errno;
    log_line("busy: 127.0.0.1:%d is taken (%s); another pandorad-spike is running", PORT, strerror(error));
    if (!strcmp(cfg.url, "stop")) { stop_other(); say("stopped\n"); }
    else say("busy\n");
    return 0;
  }
  if (!strcmp(cfg.url, "stop")) { log_line("stop: no copy was running"); say("busy nothing to stop\n"); return 0; }

  make_folders(cfg.dest);
  char path[700];
  snprintf(path, sizeof path, "%s/log.txt", cfg.dest);
  log_file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  say("ready\n");
  if (strcmp(cfg.url, "-")) pick_name();
  log_line("=== pandorad-spike %s pid=%d fw=0x%08x url=%s dest=%s flags=[%s%s%s%s segments=%d name=%s install=%s] log=%s%s", VERSION, getpid(),
       kernel_get_fw_version(), cfg.url, cfg.dest, cfg.keepawake ? " keepawake" : "", cfg.probe_memory ? " probe-memory" : "",
       cfg.insecure ? " insecure" : "", cfg.udp[0] ? " log-udp" : "", cfg.segments, cfg.name, cfg.install, path, log_file < 0 ? " (cannot open!)" : "");

  resolve_symbols();
  curl_global_init(CURL_GLOBAL_ALL);
  log_line("curl: %s", curl_version());
  load_certificates();
  spawn(state_main, NULL, SMALL_STACK, "pdspike-state");
  spawn(server_main, (void*)(intptr_t)listener, SMALL_STACK, "pdspike-server");
  log_line("http: serving %s on 127.0.0.1:%d/i/", cfg.dest, PORT);
  if (strcmp(cfg.url, "-")) {
    snprintf(download.part, sizeof download.part, "%s/%s.part", cfg.dest, cfg.name);
    snprintf(download.final_path, sizeof download.final_path, "%s/%s", cfg.dest, cfg.name);
    snprintf(download.state_path, sizeof download.state_path, "%s/%s.part.state", cfg.dest, cfg.name);
    spawn(download_main, NULL, BIG_STACK, "pdspike-download");
  }
  spawn(minute_main, NULL, SMALL_STACK, "pdspike-minute");
  if (cfg.install[0]) spawn(install_main, NULL, BIG_STACK, "pdspike-install");

  spawn(notifications_main, NULL, SMALL_STACK, "pdspike-notify");

  // The tick: one line every 5 s for as long as the process lives.
  int64_t previous = mono_ms(), previous_bytes = download.received;
  for (int64_t next = previous + TICK_MS;; next += TICK_MS) {
    const int64_t now = mono_ms();
    if (next > now) sleep_ms(next - now);
    else next = now;   // after a long gap, tick from now instead of catching up
    const int64_t tick = mono_ms(), gap = tick - previous, bytes = download.received;
    char memory[160], game[1400], state[64];
    memory_text(memory, sizeof memory);
    game_text(game, sizeof game);
    probe(STATE_MGR_CURRENT, run_int, 0, state, sizeof state);
    if (cfg.keepawake && symbols[POWER_TICK].address && tick / MINUTE_MS != previous / MINUTE_MS) {
      char ignored[16];
      int64_t result;
      sony("sceSystemServicePowerTick", run_tick, symbols[POWER_TICK].address, 0, NULL, SONY_TIMEOUT_MS, &result, ignored, sizeof ignored);
    }
    int64_t total = 0;
    for (int i = 0; i < download.count; ++i) total += download.segments[i].done;
    char line[2048];
    snprintf(line, sizeof line, "tick gap=%lldms%s bytes=%lld/%lld rate=%.2fMB/s avg=%.2fMB/s dl=%s %s flag=%d mgr=%s paused=%d http=%d %s",
             (long long)gap, gap > 10000 ? " (SUSPENDED?)" : "", (long long)total, (long long)download.size,
             gap ? (double)(bytes - previous_bytes) / 1e3 / (double)gap : 0.0,
             download.started_ms && tick > download.started_ms ? (double)bytes / 1e3 / (double)(tick - download.started_ms) : 0.0,
             (const char*[]){"idle", "running", "done", "failed"}[download.state], memory, (int)last_flag, state,
             (int)paused, (int)connections, game);
    log_line("%s", line);
    pthread_mutex_lock(&tick_lock);
    snprintf(last_tick, sizeof last_tick, "%s", line);
    pthread_mutex_unlock(&tick_lock);
    if (!paused) checkpoint("tick");
    previous = tick;
    previous_bytes = bytes;
  }
}
