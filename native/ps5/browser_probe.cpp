// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// Stage 1 of the in-app browser. Owns a WebKit page in-process like Pegasus DL+:
// loads libSceNKWebKit, resolves the WK* C API by NID with sceKernelDlsym,
// builds a context + page + view, loads a URL and paints one frame into an
// offscreen buffer. It never presents and never steers navigation; it only logs
// what resolved and what the engine could build, so a console tester can confirm
// the approach before stage 2 wires a real view. Nothing here runs unless the
// build defines PS5_REACT_BROWSER_PROBE.
//
// The NID map was recovered from the Pegasus binary: every WK symbol is exported
// only under its 11-char Sony NID, so the names below are the obfuscated NIDs,
// not the readable WK* names they decode to (kept in comments). None of this has
// been verified on hardware.

#include "browser_probe.hpp"
#include "async_log.hpp"
#include "worker_thread.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace browser_probe {
namespace {

struct Job {
  std::string url;
  Snapshot snapshot;
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  WorkerThread worker;
};

std::mutex guard;
std::shared_ptr<Job> current;
std::uint32_t next_id = 1;

bool terminal(const std::string& state) {
  return state == "completed" || state == "failed" || state == "unavailable";
}

void note(const std::shared_ptr<Job>& job, const char* line) {
  async_log::write("[browser-probe] task=%u %s", job->snapshot.id, line);
  std::lock_guard lock(job->mutex);
  job->snapshot.log.emplace_back(line);
}

[[maybe_unused]] void note(const std::shared_ptr<Job>& job, const std::string& line) {
  note(job, line.c_str());
}

#if PS5_REACT_BROWSER_PROBE

extern "C" {
int sceSysmoduleLoadModuleInternal(std::uint32_t id);
int sceSysmoduleGetModuleHandleInternal(std::uint32_t id);
int sceKernelDlsym(int handle, const char* name, void** symbol);
int sceUserServiceGetInitialUser(int* user);
}

// libSceNKWebKit; Pegasus loads the same sysmodule (0x8000007a).
constexpr std::uint32_t kWebKitModule = 0x8000007a;

// WebKit paints the page into a software buffer at this fixed size; two BGRA
// frames of 1280x720 are 7,372,800 bytes, the size Pegasus allocates.
constexpr int kPaintWidth = 1280;
constexpr int kPaintHeight = 720;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kPaintWidth) * kPaintHeight * 4;

struct WKSize { double width, height; };

// Every WK import the probe needs, keyed by its exported NID. The table is the
// recovered map; a tester's log tells us which the console actually exports.
struct Resolver {
  int handle = -1;
  int resolved = 0, missing = 0;
  void* get(const std::shared_ptr<Job>& job, const char* nid, const char* name) {
    void* symbol = nullptr;
    const int rc = sceKernelDlsym(handle, nid, &symbol);
    char line[160];
    std::snprintf(line, sizeof line, "resolve %-11s %-44s rc=0x%08x %s", nid, name,
                  static_cast<unsigned>(rc), symbol ? "ok" : "MISSING");
    note(job, line);
    if (symbol) ++resolved; else ++missing;
    return symbol;
  }
};

template <typename Fn>
Fn as(void* symbol) { return reinterpret_cast<Fn>(symbol); }

void sleep_ms(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1000000};
  nanosleep(&ts, nullptr);
}

void run_probe(std::shared_ptr<Job> job) {
  note(job, "stage 1 probe start (dev build)");
  note(job, std::string("url=") + job->url);

  const int load_rc = sceSysmoduleLoadModuleInternal(kWebKitModule);
  char line[160];
  std::snprintf(line, sizeof line, "sceSysmoduleLoadModuleInternal(0x%08x) rc=0x%08x",
                kWebKitModule, static_cast<unsigned>(load_rc));
  note(job, line);

  Resolver r;
  r.handle = sceSysmoduleGetModuleHandleInternal(kWebKitModule);
  std::snprintf(line, sizeof line, "module handle=%d", r.handle);
  note(job, line);
  if (r.handle < 0) {
    std::lock_guard lock(job->mutex);
    job->snapshot.state = "failed";
    job->snapshot.error = "libSceNKWebKit did not load; the system browser remains the fallback";
    return;
  }

  // Resolve the full set first so the log reports the whole NID map at once.
  auto* StringCreate = as<void* (*)(const char*)>(r.get(job, "kOwDs0cPXSI", "WKStringCreateWithUTF8CString"));
  auto* URLCreate = as<void* (*)(const char*)>(r.get(job, "DR1TVxyTPKc", "WKURLCreateWithUTF8CString"));
  auto* Release = as<void (*)(void*)>(r.get(job, "lwLheZe67v0", "WKRelease"));
  auto* RunLoopInit = as<void (*)()>(r.get(job, "L8wKvGYzNDU", "WKRunLoopInitializeMain"));
  auto* RunLoopCycle = as<void (*)(unsigned)>(r.get(job, "CKJOqWLNaIA", "WTF::RunLoop::cycle"));
  auto* CfgCreate = as<void* (*)()>(r.get(job, "gElXE+OeQvA", "WKContextConfigurationCreate"));
  auto* CfgSetWebPath = as<void (*)(void*, void*)>(r.get(job, "ATJaPHE0IC0", "WKContextConfigurationSetWebProcessPath"));
  auto* CfgSetNetPath = as<void (*)(void*, void*)>(r.get(job, "m4VaXZE7J6U", "WKContextConfigurationSetNetworkProcessPath"));
  auto* CfgSetUserId = as<void (*)(void*, int)>(r.get(job, "6xWOV1fjt24", "WKContextConfigurationSetUserId"));
  auto* ContextCreate = as<void* (*)(void*)>(r.get(job, "XgehYXFKMDs", "WKContextCreateWithConfiguration"));
  auto* DataStoreCreate = as<void* (*)()>(r.get(job, "hzBAQGt4hMw", "WKWebsiteDataStoreCreateNonPersistentDataStore"));
  auto* PrefsCreate = as<void* (*)()>(r.get(job, "tAuQSNscAqg", "WKPreferencesCreate"));
  auto* PrefsSetAccelComposite = as<void (*)(void*, bool)>(r.get(job, "GIjv95F1IfU", "WKPreferencesSetAcceleratedCompositingEnabled"));
  auto* PrefsSetAccelDraw = as<void (*)(void*, bool)>(r.get(job, "s+C+abjNOWc", "WKPreferencesSetAcceleratedDrawingEnabled"));
  auto* PrefsSetCanvasAccel = as<void (*)(void*, bool)>(r.get(job, "+FvUhrQaiJQ", "WKPreferencesSetCanvasUsesAcceleratedDrawing"));
  auto* PrefsSetThreadedScroll = as<void (*)(void*, bool)>(r.get(job, "VehaKjlHG2c", "WKPreferencesSetThreadedScrollingEnabled"));
  auto* PageCfgCreate = as<void* (*)()>(r.get(job, "nadjzO4Rufs", "WKPageConfigurationCreate"));
  auto* PageCfgSetContext = as<void (*)(void*, void*)>(r.get(job, "BKgXHG1tMP0", "WKPageConfigurationSetContext"));
  auto* PageCfgSetPrefs = as<void (*)(void*, void*)>(r.get(job, "h-JNdoOhUY8", "WKPageConfigurationSetPreferences"));
  auto* PageCfgSetDataStore = as<void (*)(void*, void*)>(r.get(job, "pkP97nU1QO4", "WKPageConfigurationSetWebsiteDataStore"));
  auto* ViewCreate = as<void* (*)(void*)>(r.get(job, "HvpAPSloiLA", "WKViewCreate"));
  auto* ViewGetPage = as<void* (*)(void*)>(r.get(job, "JHuFHzY5Ov0", "WKViewGetPage"));
  auto* PageGetPid = as<int (*)(void*)>(r.get(job, "llISO4CgRb8", "WKPageGetProcessIdentifier"));
  auto* PageSetScale = as<void (*)(void*, double)>(r.get(job, "iot3o+WKLVI", "WKPageSetCustomBackingScaleFactor"));
  auto* PageGetScale = as<double (*)(void*)>(r.get(job, "e0jjfsQdveM", "WKPageGetBackingScaleFactor"));
  auto* ViewSetSize = as<void (*)(void*, WKSize)>(r.get(job, "1DkPR0np8Xc", "WKViewSetSize"));
  auto* ViewSetVisible = as<void (*)(void*, bool)>(r.get(job, "8j9ZB-vGetc", "WKViewSetVisible"));
  auto* ViewSetActive = as<void (*)(void*, bool)>(r.get(job, "mZQNSovP058", "WKViewSetActive"));
  auto* ViewSetFocus = as<void (*)(void*, bool)>(r.get(job, "eYcl-oTV7Ng", "WKViewSetFocus"));
  auto* PageLoadURL = as<void (*)(void*, void*)>(r.get(job, "8nRcqN+Rt-Q", "WKPageLoadURL"));
  auto* ViewPaint = as<void (*)(void*, void*)>(r.get(job, "SAB+QaBUXA8", "WKViewPaint"));
  auto* PagePaint = as<void (*)(void*, void*)>(r.get(job, "sjOtr8kW5Ik", "WKPagePaint"));

  std::snprintf(line, sizeof line, "resolution summary resolved=%d missing=%d", r.resolved, r.missing);
  note(job, line);

  const bool core_ready = StringCreate && URLCreate && Release && CfgCreate && ContextCreate &&
                          DataStoreCreate && PrefsCreate && PageCfgCreate && ViewCreate &&
                          ViewGetPage && PageLoadURL;
  if (!core_ready) {
    std::lock_guard lock(job->mutex);
    job->snapshot.state = "failed";
    job->snapshot.error = "core WK bindings missing; the system browser remains the fallback";
    return;
  }

  if (RunLoopInit) { RunLoopInit(); note(job, "WKRunLoopInitializeMain done"); }

  void* cfg = CfgCreate();
  note(job, cfg ? "context configuration created" : "context configuration null");
  if (cfg) {
    if (CfgSetWebPath && StringCreate) {
      void* s = StringCreate("/system_ex/common_ex/lib/NKWebProcess.self");
      if (s) { CfgSetWebPath(cfg, s); Release(s); }
    }
    if (CfgSetNetPath && StringCreate) {
      void* s = StringCreate("/system_ex/common_ex/lib/NKNetworkProcess.self");
      if (s) { CfgSetNetPath(cfg, s); Release(s); }
    }
    if (CfgSetUserId) {
      int user = -1;
      sceUserServiceGetInitialUser(&user);
      CfgSetUserId(cfg, user);
      std::snprintf(line, sizeof line, "context user id=%d", user);
      note(job, line);
    }
  }

  void* context = cfg ? ContextCreate(cfg) : nullptr;
  if (cfg) Release(cfg);
  note(job, context ? "context created" : "context null");
  if (!context) {
    std::lock_guard lock(job->mutex);
    job->snapshot.state = "failed";
    job->snapshot.error = "WKContextCreateWithConfiguration returned null";
    return;
  }

  void* datastore = DataStoreCreate();
  void* prefs = PrefsCreate();
  if (prefs) {
    // Software painting: keep every accelerated path off, as Pegasus does.
    if (PrefsSetAccelComposite) PrefsSetAccelComposite(prefs, false);
    if (PrefsSetAccelDraw) PrefsSetAccelDraw(prefs, false);
    if (PrefsSetCanvasAccel) PrefsSetCanvasAccel(prefs, false);
    if (PrefsSetThreadedScroll) PrefsSetThreadedScroll(prefs, false);
  }
  std::snprintf(line, sizeof line, "session context=%d datastore=%d preferences=%d",
                context != nullptr, datastore != nullptr, prefs != nullptr);
  note(job, line);

  void* pagecfg = PageCfgCreate();
  if (pagecfg) {
    if (PageCfgSetContext) PageCfgSetContext(pagecfg, context);
    if (prefs && PageCfgSetPrefs) PageCfgSetPrefs(pagecfg, prefs);
    if (datastore && PageCfgSetDataStore) PageCfgSetDataStore(pagecfg, datastore);
  }
  note(job, pagecfg ? "page configuration created" : "page configuration null");

  void* view = pagecfg ? ViewCreate(pagecfg) : nullptr;
  if (pagecfg) Release(pagecfg);
  void* page = view ? ViewGetPage(view) : nullptr;
  std::snprintf(line, sizeof line, "view=%d page=%d", view != nullptr, page != nullptr);
  note(job, line);
  if (!view || !page) {
    std::lock_guard lock(job->mutex);
    job->snapshot.state = "failed";
    job->snapshot.error = "WKViewCreate / WKViewGetPage returned null";
    return;
  }

  if (PageSetScale) PageSetScale(page, 1.0);
  if (PageGetScale) {
    const double scale = PageGetScale(page);
    std::snprintf(line, sizeof line, "backing scale=%.3f", scale);
    note(job, line);
  }
  if (ViewSetSize) ViewSetSize(view, WKSize{static_cast<double>(kPaintWidth), static_cast<double>(kPaintHeight)});
  if (ViewSetVisible) ViewSetVisible(view, true);
  if (ViewSetActive) ViewSetActive(view, true);
  if (ViewSetFocus) ViewSetFocus(view, true);
  if (PageGetPid) {
    std::snprintf(line, sizeof line, "WebProcess pid=%d", PageGetPid(page));
    note(job, line);
  }

  auto* frame = static_cast<std::uint8_t*>(std::malloc(kFrameBytes));
  std::snprintf(line, sizeof line, "paint buffer %zu bytes alloc=%d", kFrameBytes, frame != nullptr);
  note(job, line);
  if (frame) std::memset(frame, 0, kFrameBytes);

  void* url = URLCreate(job->url.c_str());
  if (url) {
    PageLoadURL(page, url);
    Release(url);
    note(job, "WKPageLoadURL submitted");
  } else {
    note(job, "WKURLCreateWithUTF8CString null; skipping load");
  }

  // Pump the WebKit run loop and paint once content has had a chance to arrive.
  // The probe never presents the frame; it only reports that paint ran and
  // whether the buffer changed, which is what a tester confirms.
  void (*paint)(void*, void*) = ViewPaint ? ViewPaint : PagePaint;
  const bool paint_takes_view = ViewPaint != nullptr;
  note(job, ViewPaint ? "paint path=WKViewPaint" : PagePaint ? "paint path=WKPagePaint" : "paint path=none");

  bool painted = false;
  for (int cycle = 0; cycle < 120 && !job->cancelled; ++cycle) {
    if (RunLoopCycle) RunLoopCycle(0);
    sleep_ms(50);
    if (frame && paint && cycle == 60) {
      note(job, "calling paint");
      paint(paint_takes_view ? view : page, frame);
      std::uint64_t checksum = 0;
      for (std::size_t i = 0; i < kFrameBytes; i += 997) checksum += frame[i];
      std::snprintf(line, sizeof line, "paint returned; sampled checksum=%llu %s",
                    static_cast<unsigned long long>(checksum), checksum ? "(non-blank)" : "(blank)");
      note(job, line);
      painted = true;
    }
  }

  std::free(frame);
  if (ViewSetVisible) ViewSetVisible(view, false);
  if (Release) Release(view);

  {
    std::lock_guard lock(job->mutex);
    job->snapshot.state = job->cancelled ? "failed" : "completed";
    if (job->cancelled) job->snapshot.error = "probe cancelled";
    else if (!painted) job->snapshot.error = "probe built the page but did not reach paint";
  }
  note(job, "stage 1 probe end");
}

#else // PS5_REACT_BROWSER_PROBE

void run_probe(std::shared_ptr<Job> job) {
  note(job, "browser probe not compiled into this build (PS5_REACT_BROWSER_PROBE off)");
  std::lock_guard lock(job->mutex);
  job->snapshot.state = "unavailable";
  job->snapshot.error = "browser probe disabled; rebuild with PS5_REACT_BROWSER_PROBE";
}

#endif // PS5_REACT_BROWSER_PROBE

} // namespace

std::uint32_t start(std::string url, std::string& error) {
  std::lock_guard lock(guard);
  if (current) { error = "A browser probe is already running; wait for it to finish."; return 0; }
  auto job = std::make_shared<Job>();
  job->url = std::move(url);
  job->snapshot.id = next_id++;
  job->snapshot.state = "running";
  current = job;
  if (!job->worker.start([job] { run_probe(job); }, 2 * 1024 * 1024)) {
    current.reset();
    error = "Cannot start the probe thread.";
    return 0;
  }
  return job->snapshot.id;
}

std::vector<Snapshot> poll() {
  std::lock_guard lock(guard);
  if (!current) return {};
  Snapshot snapshot;
  { std::lock_guard lock(current->mutex); snapshot = current->snapshot; }
  if (terminal(snapshot.state)) { current->worker.join(); current.reset(); }
  return {snapshot};
}

void stop() {
  std::shared_ptr<Job> job;
  { std::lock_guard lock(guard); job = current; }
  if (!job) return;
  job->cancelled = true;
  job->worker.join();
  std::lock_guard lock(guard);
  current.reset();
}

} // namespace browser_probe
