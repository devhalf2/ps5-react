# pandorad-spike (spike 0B: background downloads)

A measurement payload, not part of any app. It answers whether a resident payload
sent to the ELF loader can keep downloading with the app closed, during a game,
across auto-rest and rest mode, how fast it goes, and how much memory it can get
while a game runs. The results decide the daemon design (headless JS or C) for
Pandora's background downloads.

`pandorad-spike.elf` is sent to John Törnblom's ELF loader on port 9021, like
`pkg-installer.elf`. It:

- downloads one file with libcurl (PacBrew curl, OpenSSL, zlib, linked statically),
  in 4 parallel ranges, into `<dest>/<name>.part`. It resumes from a checkpoint
  (`<name>.part.state`), and renames the file to `<name>` when done;
- logs one `tick` line every 5 s: wall clock, monotonic ms, gap since the last tick,
  bytes, MB/s, resident memory, system state, and what every running-game API answers;
- with `probe-memory`, allocates and touches 64, 128, 256 and 384 MiB once a minute;
- watches `sceSystemServiceReceiveEvent`, the `SceSystemStateMgrInfo` event flag and
  SIGCONT. When the console goes to sleep it pauses and checkpoints; on wake it resumes;
- with `keepawake`, resets the auto-power-down timer once a minute and logs the
  remaining seconds before and after;
- sends notifications: one at start, the plain kernel one, three updates that should
  replace each other (`bundleName` with `toastOverwriteType: InQueue`), and one at the end;
- holds 127.0.0.1:9061 as a singleton and serves `<dest>` there as `/i/<file>`
  (HEAD, Range → 206, `.crc` → 404). With `install=<file>` it asks the console's
  installer to install `http://127.0.0.1:9061/i/<file>` (T11);
- logs to `<dest>/log.txt` (synced every line), to the loader socket while it is open,
  and over UDP to the Mac with `log-udp`.

Every Sony function outside libkernel is looked up at run time, by loading its
module by path and calling `kernel_dynlib_dlsym`. A symbol a firmware lacks is
logged as `MISSING` and the payload still loads (a payload that imported
libSceAppInstUtil never started on 12.70+). Every Sony call runs on a helper
thread with a timeout. A call that times out is logged and skipped until it returns
(a game can starve the payload's threads, so a slow answer is not a hang).

## Build

```sh
export PATH="$HOME/.homebrew/bin:$PWD/.venv/bin:$PATH"   # this repo's venv; adjust to your setup
export PS5_LLVM=$HOME/.homebrew/opt/llvm@18/bin
python3 tools/spike/daemon/build.py          # .build/spike/daemon/pandorad-spike.elf
python3 tools/spike/daemon/build.py --host   # .build/spike/daemon/pandorad-spike-host (Mac smoke build)
```

The payload SDK must be in `.deps/sdk/ps5-payload-sdk`; any `npm run build` fetches it.
The build embeds Mozilla's CA bundle (`tools/ca_bundle.py`), and the payload appends
it to the console's `/system/common/cert/CA_LIST.cer`, as the app does.

The host build runs the same download, checkpoint, range server and logging on a Mac.
Every Sony call answers "unavailable" there. `kill -USR1` plays "going to sleep" and
`kill -USR2` plays "awake".

## Run

You need: the ELF loader listening on 9021, the Mac and the console on the same LAN,
about 12 GB free on `/data`, and a large file served with Range support. A public
speed-test file works, for example `https://ash-speed.hetzner.com/10GB.bin`
(10 GiB, Range supported; speed depends on your internet connection, so note it).
That host answers 429 to a third connection at once, so use `segments=2` with it;
the tests below already do.

Terminal 1, the log listener (keeps every line even after the loader socket closes):

```sh
python3 tools/spike/daemon/listen.py --port 9062 --out spike-B1.log
```

Terminal 2, send the payload:

```sh
python3 tools/spike/daemon/send.py <console-ip> <url> --flags "log-udp=<mac-ip>:9062 segments=2 [keepawake] [probe-memory]"
```

- Request fields: the URL (`-` to only serve and log; `stop` to stop the running copy),
  `--dest` (default `/data/pandora-spike`), and `--flags`. Flags: `keepawake`,
  `probe-memory`, `insecure` (skips TLS checks), `name=<file>`, `segments=<1-4>`
  (parallel ranges, default 4), `log-udp=<ip>:<port>`, `install=<file>`.
- The payload answers `ready`, `busy` (another copy holds 9061) or `stopped`, then
  mirrors its log. Ctrl-C closes our end; the payload keeps running.
- To stop it: `python3 tools/spike/daemon/send.py <console-ip> stop`. A reboot also
  ends it. It does not exit on its own, even after the download finishes, so it can
  keep logging and serving.
- Sending the same URL again resumes from the checkpoint. To start over, delete
  `/data/pandora-spike/<name>.part*`. A different `segments` value also starts over.

### Reading the log

Each line starts with the UTC wall clock and the monotonic ms. `listen.py` adds the
Mac's receive time. Lines worth grepping for:

| Prefix | Meaning |
|---|---|
| `=== pandorad-spike` | start: pid, firmware, flags |
| `resolve <symbol>: found / MISSING / library unavailable` | which Sony APIs this firmware has |
| `tick gap=… bytes=… rate=… rss=… flag=… mgr=… game[…] procs[…]` | the 5-second heartbeat. `gap` over 10000 ms is marked `(SUSPENDED?)`. `flag` is the `SceSystemStateMgrInfo` value (1000 WORKING, 300 SUSPEND_ON_GOING, 500 MAIN_ON_STANDBY; negative = error code). `procs` lists processes with a title ID and their RSS |
| `event: …`, `state: …`, `signal: SIGCONT`, `PAUSE:`, `RESUME:` | console state changes |
| `segment N: …`, `download: …`, `checkpoint (…)` | transfer, retries, resume |
| `memprobe: N MiB ok / FAIL` | memory available (the `start` line comes first, so a kill mid-probe shows) |
| `keepawake:` / `autopowerdown:` | auto-power-down timer, once a minute |
| `notify …: rc=…` | each notification and its return code |
| `http #N …` | every request on 9061: method, path, query, Range, User-Agent, status, bytes, ms |
| `install: …` | T11: InstallByPackage result and the status every 5 s (raw dump when the stage changes) |
| `sony <name>: no answer in N ms` / `returned after all` | a Sony call that did not return in time; skipped until it does |

### What to copy back

- The `listen.py` file (`spike-*.log`) from the Mac.
- `/data/pandora-spike/log.txt` from the console, via any FTP payload. It has the
  lines UDP could not deliver, for example while the network was down in rest mode.
- What you saw on screen: toasts, the Downloads list, error dialogs, a crash or panic.
- Firmware, jailbreak (kstuff, kstuff-lite, etaHEN…), loader, storage, and the console's
  power settings: "Set time until PS5 enters rest mode" and "Features available in
  rest mode → Stay connected to the internet".

## Test matrix

Run B1 first. Keep `listen.py` running through every test, one output file per test.
Unless a test says otherwise, flags are `log-udp=<mac-ip>:9062 segments=2 probe-memory`.

### B1: survives the app closing

Background: payloads are children of the loader, not of the app that sent them, so
sending from the Mac and from Pandora should behave the same. B1 checks both.

1. `python3 tools/spike/daemon/build.py`
2. Terminal 1: `python3 tools/spike/daemon/listen.py --port 9062 --out spike-B1.log`
3. Open Pandora (or any app) on the console.
4. Terminal 2: `python3 tools/spike/daemon/send.py <console-ip> https://ash-speed.hetzner.com/10GB.bin --flags "log-udp=<mac-ip>:9062 segments=2 probe-memory"`
5. Wait for `ready` and three `tick` lines with `bytes` growing. Press Ctrl-C in terminal 2.
6. Close Pandora (PS button → close the app). Stay on the home screen for 10 min.

Watch: `tick` lines keep coming every ~5 s with `gap≈5000`, and `rate` stays steady.
Note the home-screen MB/s (go criterion: ≥ 10 MB/s, if the line allows it) and the
`memprobe` results at home.

Pass: ticks continue after the socket closes and after the app closes.

### B2: a heavy game for 30 min

1. With B1's copy still downloading (or send it again), launch a demanding game
   and play it for 30 min.
2. Go back home, close the game, and wait 5 min.

Watch: `gap` (does the game starve our threads? ps5upload saw `sleep(5)` overrun 60 s),
`rate` in game against at home, `memprobe` in game (go criterion: 128 MiB `ok` while the
game runs), `rss`, and `flex_free`.

Pass: it survives the whole session, with numbers for in-game speed and allocatable memory.

### B3: auto-rest, with the timer reset off and then on

1. Settings → System → Power Saving → "Set time until PS5 enters rest mode" → the
   shortest value (20 min). Note the setting.
2. Send without `keepawake`. Leave the controller alone for 40 min.
3. Wake the console. Note whether the payload still runs (ticks after wake, the
   `bytes` value) and what B4 shows.
4. Stop it (`send.py <ip> stop`). Send again with `keepawake` added. Leave it for 40 min.

Watch: the `autopowerdown: remaining …` lines count down; the `keepawake: … reset rc=…, after …`
lines show whether the reset moves the counter back. In the second run, check whether
the console stays awake.

Pass: auto-rest happens without `keepawake` and does not happen with it. Note what
happened to the payload in the first run.

### B4: rest mode for 5 min, then wake

1. Send with the usual flags. Wait for three ticks.
2. Put the console in rest mode (PS button → Power → Enter rest mode). Wait 5 min.
3. Wake it with the controller and wait 3 min.
4. Repeat once with "Stay connected to the internet" switched the other way.

Watch: `state: SceSystemStateMgrInfo` changes (300/500 then 1000), `event:` lines and their
types, `signal: SIGCONT`, `PAUSE`/`RESUME`, the first `tick` after wake (its `gap` is the
time frozen), `segment N: resuming`, retries until the network returns, and whether
`bytes` keeps growing. The UDP log goes quiet in rest mode; `log.txt` has the lines
written while UDP could not deliver them.

Pass: it survives, sees the events and continues, **or** it dies without a kernel panic.
If the console panics or will not wake, record that first.

### B5: singleton

1. With a copy running, send again (any URL).
2. Then `python3 tools/spike/daemon/send.py <console-ip> stop`, and send once more.

Watch: the second send answers `busy` and logs `busy: 127.0.0.1:9061 is taken`; the
first copy keeps ticking. `stop` answers `stopped`, the first copy logs `=== exit (quit)`,
and the next send gets `ready`.

### B6: notifications

At every start: a "Pandora spike running" toast, then the kernel's plain toast
("pandorad-spike: kernel notification"), then "Spike update 1 of 3" to "3 of 3", 2 s apart.

Watch: which toasts appear, whether updates 2 and 3 replace the previous one (one toast
on screen) or stack (three toasts), and what the notification list keeps. Check the
`notify … rc=` lines. At the end of a download you should see "Spike download finished"
(or "failed").

### B7: running-game detection

Read the `game[…]` and `procs[…]` part of the ticks in four states: home screen with
no game; a game in front; a game suspended in the background (open it, go home, don't
close it); and after closing the game.

Watch: which of `sysRunBig`, `lncRunBig`, `sysList`, `lncList`, `title`, `appStatus`
changes with the game, and whether `procs` shows the game's title ID and RSS. The API
that tells "game in front" apart from "game in background" is the one E3 uses.

### T11: the console's installer pulls a PKG from the spike

1. Put a PS4 fake PKG that is known to install (BATTLESHIP `CUSA04286` was installed
   on 13.60 with `pkg-installer.elf`) in `/data/pandora-spike/`, under a short plain
   name such as `t11.pkg`. Either copy it by FTP, or let the spike download it:
   `send.py <ip> <pkg-url> --flags "name=t11.pkg install=t11.pkg log-udp=<mac-ip>:9062"`.
2. If the file is already there: `send.py <ip> - --flags "install=t11.pkg log-udp=<mac-ip>:9062"`.
3. Watch the home screen's Downloads list until the game is playable or fails.

Watch: `install: InstallByPackage http://127.0.0.1:9061/i/t11.pkg … rc=…`, then every
`http #N` line from the installer. Note its method, Range sizes, the `?product=…&serverIpAddr=…`
query, its User-Agent, how many connections it holds at once, whether it asks for a `.crc`,
and the time between ranges. Also note the `install: status=…` lines and the raw dump of
the status block (for E5's layout), and the final toast.

Pass: the installer fetches from the loopback URL and the game installs.

## Results template (for docs/HARDWARE.md)

```markdown
## Background download spike (0B) — YYYY-MM-DD

Console <model>, firmware <x.xx> (kernel <uname>), <jailbreak>, <loader> on 9021,
storage <internal /data | USB | M.2>. `pandorad-spike.elf` SHA-256 <hash>, built from
<commit>. Rest-mode setting: <20 min/...>, "Stay connected to the internet": <on/off>.
Download source: <url>, line speed <Mbps>.

| Test | Result | Numbers | Notes |
|---|---|---|---|
| B1 close the app | survives / dies | home MB/s: <n>; memprobe at home: <64/128/256/384 ok/fail> | |
| B2 heavy game 30 min (<game>) | survives / dies | in-game MB/s: <n>; max tick gap: <ms>; memprobe in game: <...>; game RSS: <MiB> | |
| B3 auto-rest, reset off | rest at <min> / no rest | payload after wake: <alive/dead> | |
| B3 auto-rest, reset on | stays awake / rests | remaining before/after reset: <s>/<s> | |
| B4 rest 5 min | survives / dies / panic | flag values seen: <...>; events: <types>; SIGCONT: <y/n>; frozen gap: <s>; transfer resumed: <y/n after s> | |
| B5 singleton | busy / two copies | | |
| B6 toasts | <shown/not> | updates replaced: <y/n>; kernel toast: <y/n>; rc values | |
| B7 game detection | | API that works: <name>; front vs background: <...> | |
| T11 loopback install | installs / fails <code> | installer requests: <ranges, conns, query, .crc> | |

Resolved at run time: <list of found / MISSING from the `resolve` lines>.
```

## Known gaps

- Nothing here has run on a console yet. The ELF compiles and links (all imports come
  from libkernel_sys, libSceLibcInternal and libSceNet stubs), and the host build was
  smoke-tested on a Mac: download, 4-range resume after a kill, pause and resume,
  the 9061 range server, singleton and stop.
- The struct layouts and signatures of the probed APIs come from scene code, not Sony
  documentation: the `sceSystemServiceReceiveEvent` event block (a 16 KiB buffer is
  passed to be safe), the event type values (0x10000000 ON_RESUME and 0x10000001
  BEFORE_SLEEP, as ezRemote uses them), the `...ListOfBigApp` and `sceSystemServiceGetAppStatus`
  arguments, and the notification JSON. `sceUserServiceGetPlayingBigAppTitleId` is only
  resolved, never called: its signature is unknown.
- RSS comes from `sysctl(KERN_PROC_PID).ki_rssize` × the SDK's `PAGE_SIZE` (the raw page
  count is logged too, in case the console's page size differs).
- DNS goes through the payload SDK libc's `getaddrinfo` (libSceNet resolver). It has not
  been tested from a payload without a `sceNetInit` call.
