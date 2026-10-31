// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// Installs one PKG through the console's app-install service. An app may not use that service, so the
// engine sends this program to the payload loader (127.0.0.1:9021), which hands it the rest of the
// connection as standard input and output.
//
// Request lines: "PKI1", the package path, the name the home screen shows while it installs.
// Answers: "ready", then "started <content ID>", then "p <installed bytes> <total bytes> <status>"
// once a second, then "ok" or "fail <code as eight hex digits> <reason>".
//
// "PKI2" is a console experiment (tools/spike/pkg-url): the lines are "PKI2", a package path or an
// http(s) URL the console downloads itself, the content ID (required for a URL), the name, an icon URL
// (may be empty) and space-separated flags: "dump" adds "raw <hex>" status lines, "detach" exits after
// "started", "more" reads another PKI2 request in the same process once this one ends. Same answers.
// PKI2 is built only with -DPKG_INSTALLER_PKI2 (tools/spike/pkg-url/build.sh): the shipped ELF keeps PKI1
// and its imports alone, since an extra import can keep a payload from starting on firmware 12.70+.
#ifdef PKG_INSTALLER_PKI2
#include <pthread.h>
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ps5/kernel.h>

int sceUserServiceInitialize(void*);
int sceKernelLoadStartModule(const char*, size_t, const void*, unsigned, void*, int*);

// The ABI as the console reads it. Sony reads eight bytes past icon_url: anything but zero there makes
// it treat the package as a patch and refuse it (0x80B2116F on 13.60), so the whole block is zeroed.
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

// The status layout is disputed between the scene's sources (etaHEN's above, ShadowMountPlus's 0x2C8
// bytes with the error at 0xA0), so the console writes into a zeroed 4 KiB block and PKI2 can dump it.
typedef union { InstallStatus fields; unsigned char raw[4096]; } StatusBlock;
_Static_assert(sizeof(InstallStatus) <= sizeof(StatusBlock), "the status block holds the known layout");

// A payload that imports the install library never starts on firmware 12.70 and later, so it is loaded
// by path and its functions are looked up.
static int (*initialize)(void);
static int (*install)(MetaInfo*, PkgInfo*, PlayGoInfo*);
static int (*install_status)(const char*, InstallStatus*);

static int load(void) {
  const int handle = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0, NULL, 0, NULL, NULL);
  if (handle < 0) return handle;
  initialize = (int (*)(void))kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilInitialize");
  install = (int (*)(MetaInfo*, PkgInfo*, PlayGoInfo*))kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilInstallByPackage");
  install_status = (int (*)(const char*, InstallStatus*))kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilGetInstallStatus");
  return initialize && install && install_status ? 0 : -1;
}

// Ten KiB of PlayGo state and the status block, kept off the payload's stack.
static PlayGoInfo playgo;
static StatusBlock status_block;

static void say(const char* line) {
  size_t size = strlen(line);
  while (size) {
    ssize_t count = write(STDOUT_FILENO, line, size);
    if (count <= 0) return;
    line += count; size -= (size_t)count;
  }
}

static int read_line(char* line, size_t capacity) {
  size_t length = 0;
  for (;;) {
    char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1) return 0;
    if (byte == '\n') { line[length] = 0; return 1; }
    if (length + 1 >= capacity) return 0;
    line[length++] = byte;
  }
}

static int fail(unsigned code, const char* reason) {
  char line[640];
  snprintf(line, sizeof line, "fail %08x %s\n", code, reason);
  say(line);
  return 1;
}

// This program runs with every right, so it installs only a .pkg or .fpkg named plainly under a drive's root.
static int allowed(const char* path) {
  const size_t length = strlen(path);
  return (!strncmp(path, "/data/", 6) || !strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8))
    && ((length > 4 && !strcmp(path + length - 4, ".pkg")) || (length > 5 && !strcmp(path + length - 5, ".fpkg")))
    && !strstr(path, "/../") && !strstr(path, "//");
}

#ifdef PKG_INSTALLER_PKI2
// The installer refuses a URI of 128 bytes or more (0x80A30003) and sends no headers, so a URL is checked
// for what it can carry: a plain http(s) address, nothing the request line could split on.
static int url_allowed(const char* uri) {
  const size_t length = strlen(uri);
  if (length > 127 || !((length > 7 && !strncmp(uri, "http://", 7)) || (length > 8 && !strncmp(uri, "https://", 8))))
    return 0;
  for (const unsigned char* c = (const unsigned char*)uri; *c; ++c)
    if (*c <= ' ' || *c >= 0x7F) return 0;
  return 1;
}

// UP0001-CUSA04286_00-XXXXXXXXXXXXXXXX: A an uppercase letter, 0 a digit, X either.
static int content_id_valid(const char* id) {
  static const char pattern[] = "AA0000-AAAA00000_00-XXXXXXXXXXXXXXXX";
  if (strlen(id) != sizeof pattern - 1) return 0;
  for (size_t i = 0; pattern[i]; ++i) {
    const char c = id[i];
    const int letter = c >= 'A' && c <= 'Z', digit = c >= '0' && c <= '9';
    if (pattern[i] == 'A' ? !letter : pattern[i] == '0' ? !digit : pattern[i] == 'X' ? !(letter || digit) : c != pattern[i])
      return 0;
  }
  return 1;
}
#endif

// A PS4 PKG starts with \x7FCNT and names its content ID at 0x40. A PS5 PKG starts with \x7FFIH and keeps
// such a header at the offset its 64-bit little-endian field at 0x58 gives (0x80000 in most, the end of
// some); a PS5 PKG cut short of that header is incomplete. Returns the reason it failed, or NULL.
static const char* read_content_id(const char* path, char content_id[37]) {
  char header[0x40 + 36];
  const int file = open(path, O_RDONLY);
  if (file < 0) return "The package could not be opened.";
  ssize_t got = pread(file, header, sizeof header, 0);
  const int ps5 = got == (ssize_t)sizeof header && !memcmp(header, "\x7F" "FIH", 4);
  if (ps5) {
    uint64_t offset;
    memcpy(&offset, header + 0x58, sizeof offset);
    got = offset <= INT64_MAX ? pread(file, header, sizeof header, (off_t)offset) : -1;
  }
  close(file);
  if (got != (ssize_t)sizeof header || memcmp(header, "\x7F" "CNT", 4))
    return ps5 ? "The PS5 package is incomplete." : "The file is not a PKG.";
  memcpy(content_id, header + 0x40, 36);
  content_id[36] = 0;
  return NULL;
}

#ifdef PKG_INSTALLER_PKI2
// sceAppInstUtilInitialize has been seen to never return (ps5upload bounds it at 10 s), so PKI2 runs it
// on a thread and gives up rather than leave the caller waiting on a silent payload.
static atomic_int initialize_done;
static int initialize_result;
static void* run_initialize(void* unused) {
  (void)unused;
  initialize_result = initialize();
  atomic_store(&initialize_done, 1);
  return NULL;
}
// 1 with *result once it returned, 0 when it did not within `seconds`, -1 when no thread could run it
// (calling it here instead could hang this payload with nobody told).
static int initialize_within(unsigned seconds, int* result) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, run_initialize, NULL)) return -1;
  for (unsigned tenths = 0; tenths < seconds * 10; ++tenths) {
    if (atomic_load(&initialize_done)) { pthread_join(thread, NULL); *result = initialize_result; return 1; }
    usleep(100000);
  }
  return 0;
}
#endif

static int start(const char* path_or_url, const char* content_id, const char* name, const char* icon_url,
                 char installing[48]) {
  // The service reads internal-drive packages through its /user view of /data.
  char uri[1100];
  // memcmp, not strncmp: the caller's buffers are 1 KiB, and the shipped ELF keeps main's imports.
  snprintf(uri, sizeof uri, memcmp(path_or_url, "/data/", 6) ? "%s" : "/user%s", path_or_url);
  MetaInfo meta;
  memset(&meta, 0, sizeof meta);
  meta.uri = uri; meta.ex_uri = ""; meta.playgo_scenario_id = ""; meta.content_id = content_id;
  meta.content_name = name; meta.icon_url = icon_url;
  PkgInfo info;
  memset(&info, 0, sizeof info);
  const int result = install(&meta, &info, &playgo);
  if (result) return fail((unsigned)result, "The console refused the package.");
  info.content_id[sizeof info.content_id - 1] = 0;
  snprintf(installing, 48, "%s", info.content_id[0] ? info.content_id : content_id);
  char line[96];
  snprintf(line, sizeof line, "started %s\n", installing);
  say(line);
  return 0;
}

static void dump(const unsigned char* bytes, size_t size) {
  static const char digits[] = "0123456789abcdef";
  char line[4 + 2 * 0x300 + 2] = "raw ";
  for (size_t i = 0; i < size; ++i) {
    line[4 + 2 * i] = digits[bytes[i] >> 4];
    line[5 + 2 * i] = digits[bytes[i] & 15];
  }
  line[4 + 2 * size] = '\n';
  line[5 + 2 * size] = 0;
  say(line);
}

// The console installs in the background; its status names the stage until the game is playable.
// A status that stays unreadable once the bytes are all in means the install finished and left the queue.
// "patience" is how many unreadable seconds in a row end the watch.
static int watch(const char* installing, int patience, int dumping) {
  static unsigned char shown[0x300];
  char line[640];
  int unreadable = 0, result;
  uint64_t done = 0, total = 0;
  for (unsigned second = 1;; ++second) {
    sleep(1);
    memset(&status_block, 0, sizeof status_block);
    InstallStatus* const status = &status_block.fields;
    if ((result = install_status(installing, status))) {
      if (dumping) {
        snprintf(line, sizeof line, "raw error %08x\n", (unsigned)result);
        say(line);
      }
      if (++unreadable >= patience) {
        if (total && done >= total) { say("ok\n"); return 0; }
        return fail((unsigned)result, "The install status could not be read.");
      }
      continue;
    }
    unreadable = 0;
    if (dumping && (second <= 30 || memcmp(shown, status_block.raw, sizeof shown))) {
      memcpy(shown, status_block.raw, sizeof shown);
      dump(shown, sizeof shown);
    }
    status->status[sizeof status->status - 1] = 0;
    done = status->downloaded; total = status->total;
    snprintf(line, sizeof line, "p %llu %llu %s\n", (unsigned long long)done, (unsigned long long)total, status->status);
    say(line);
    if (status->error.code) {
      status->error.description[sizeof status->error.description - 1] = 0;
      return fail((unsigned)status->error.code, status->error.description[0] ? status->error.description : "The install failed.");
    }
    if (!strcmp(status->status, "playable") || !strcmp(status->status, "completed")) { say("ok\n"); return 0; }
    if (!strcmp(status->status, "error")) return fail(0, "The install failed.");
  }
}

static int pki1(void) {
  char path[1024], name[256], content_id[37], installing[48];
  if (!read_line(path, sizeof path) || !read_line(name, sizeof name)) return 1;
  if (!allowed(path)) return fail(0, "The package path is not allowed.");
  const char* problem = read_content_id(path, content_id);
  if (problem) return fail(0, problem);
  say("ready\n");

  (void)sceUserServiceInitialize(NULL);
  int result = load();
  if (result) return fail((unsigned)result, "The install library could not be loaded.");
  if ((result = initialize())) return fail((unsigned)result, "The install service did not start.");
  if (start(path, content_id, name, "", installing)) return 1;
  return watch(installing, 10, 0);
}

#ifdef PKG_INSTALLER_PKI2
// One PKI2 request, after its magic line. *more says whether another request follows in this process.
static int pki2(int* more) {
  static int loaded;
  char uri[1024], wanted[64], name[256], icon_url[1024], flags[128], content_id[37], installing[48];
  *more = 0;
  if (!read_line(uri, sizeof uri) || !read_line(wanted, sizeof wanted) || !read_line(name, sizeof name) ||
      !read_line(icon_url, sizeof icon_url) || !read_line(flags, sizeof flags)) return 1;
  int dumping = 0, detach = 0;
  for (char *flag = strtok(flags, " "); flag; flag = strtok(NULL, " ")) {
    if (!strcmp(flag, "dump")) dumping = 1;
    else if (!strcmp(flag, "detach")) detach = 1;
    else if (!strcmp(flag, "more")) *more = 1;
    else return fail(0, "Unknown flag; use dump, detach or more.");
  }
  if (!strncmp(uri, "http://", 7) || !strncmp(uri, "https://", 8)) {
    if (!url_allowed(uri)) return fail(0, "The URL must be http(s), at most 127 bytes, without spaces or control characters.");
    if (!content_id_valid(wanted)) return fail(0, "A URL needs a content ID like UP0001-CUSA04286_00-XXXXXXXXXXXXXXXX.");
    memcpy(content_id, wanted, sizeof content_id);
  } else {
    if (!allowed(uri)) return fail(0, "The package path is not allowed.");
    const char* problem = read_content_id(uri, content_id);
    if (problem) return fail(0, problem);
    if (wanted[0] && strcmp(wanted, content_id)) return fail(0, "The content ID does not match the package.");
  }
  say("ready\n");

  // Load and initialize once per process: "more" exists to see whether a second install in one
  // process fails, as PKG Manager reports on firmware 9.60 and later.
  if (!loaded) {
    (void)sceUserServiceInitialize(NULL);
    int result = load();
    if (result) return fail((unsigned)result, "The install library could not be loaded.");
    const int answered = initialize_within(10, &result);
    if (answered < 0) return fail(0, "The install service could not be started on a thread of its own.");
    if (!answered) {
      fail(0, "The install service did not answer within 10 s.");
      _exit(1);  // The stuck thread must not keep this process alive.
    }
    if (result) return fail((unsigned)result, "The install service did not start.");
    loaded = 1;
  }
  if (start(uri, content_id, name, icon_url, installing)) return 1;
  if (detach) { *more = 0; return 0; }
  // A URL install may wait for its first bytes before the status reads, so it gets a minute.
  return watch(installing, 60, dumping);
}
#endif

int main(void) {
  char magic[8];
  if (!read_line(magic, sizeof magic)) return 1;
  if (!strcmp(magic, "PKI1")) return pki1();
#ifdef PKG_INSTALLER_PKI2
  if (strcmp(magic, "PKI2")) return 1;
  int result, more;
  do result = pki2(&more);
  while (more && read_line(magic, sizeof magic) && !strcmp(magic, "PKI2"));
  return result;
#else
  return 1;
#endif
}
