# Native modules

`@ps5-react/core` exposes platform services as React Native-style modules.
Apps import them like any other export; they never touch the SDK, libkernel,
or SDL directly.

```jsx
import {Platform, DeviceInfo, FileSystem, Controller, useGamepad} from '@ps5-react/core';

const {model, firmware} = DeviceInfo.get();
FileSystem.writeFile(`${FileSystem.dataDir}/save.json`, JSON.stringify(state));
Controller.setLightBar('#38bdf8');
```

[`apps/system-explorer`](../apps/system-explorer) exercises every module.

## Contract

The modules in `runtime/js/native.js` wrap `globalThis.__ps5ReactNative`, which
each host installs before the bundle runs. Its exact shape (ABI v11) is
documented in [`native/shared/host_api.hpp`](../native/shared/host_api.hpp).
`native/shared/host_api.cpp` implements the filesystem with POSIX and the object
itself; `native/ps5/` and `native/desktop/` implement the platform functions.

- **Synchronous bridge.** Each native call returns directly. Networking calls
  only submit/cancel/poll bounded tasks; `Http.request` and `task.done` expose
  promises in the JavaScript wrapper while native workers perform I/O.
- **Render thread.** Calls run on the same thread as React and the rasterizer.
  Keep them cheap: avoid listing large directories or reading files every
  frame. Call them from event handlers, effects, or state initializers rather
  than repeatedly during render.
- **Errors.** A failed call throws an `Error` whose message names the call, the
  path, and the system error, for example
  `fs.readDir /download0/missing: No such file or directory`. Wrap calls in
  `try`/`catch` and show the message.
- **Missing host.** If `__ps5ReactNative` does not exist (for example when a
  module is evaluated outside a PS5 React host), every call throws an error
  explaining that a PS5 React host is required.
- **QuickJS runtime.** The runtime has no `Date` objects (`Date.now()` works),
  typed arrays, or `Proxy`; values cross the boundary as strings, numbers,
  booleans, plain objects, arrays, and `null`.

## Platform

| Member | Returns | Notes |
| --- | --- | --- |
| `Platform.OS` | `'ps5' \| 'desktop'` | The running host |
| `Platform.select(spec)` | `spec[OS]`, else `spec.default` | Keys: `ps5`, `desktop`, `default` |

```js
const label = Platform.select({ps5: 'Console', desktop: 'Preview', default: 'Unknown'});
```

## DeviceInfo

`DeviceInfo.get()` returns:

| Field | Unit | Notes |
| --- | --- | --- |
| `model` | string | Console model, or `Desktop preview (<Mac model>)` |
| `firmware` | string | System software version, or `macOS <version>` |
| `cpuTemperature` | °C | `null` on desktop |
| `socTemperature` | °C | `null` on desktop |
| `cpuFrequency` | Hz | `null` when unknown (always on Apple silicon) |
| `freeMemory` | bytes | Memory available to the app |
| `processTime` | µs | CPU time used by this process |
| `language` | string | System language as a BCP 47 tag (`en-US`, `pt-BR`, `zh-Hans`) |
| `consoleId` | string | The console's OpenPSID (`sceKernelGetOpenPsId`) as 32 lowercase hex digits, fixed per console; see below |

Any value the host cannot read is `null`.

`consoleId` identifies the console, not the user, and is the same for every
title. Treat it as personal data: to bind something to a console, send a hash
of it salted for your app rather than the ID itself. On PS5 it is read once per
launch: from `sceKernelGetOpenPsId`, looked up in `libkernel_sys.sprx`,
`libkernel.sprx` and `libkernel_web.sprx` in turn, else from the kernel's
`machdep.openpsid` sysctl (the same 16 bytes). The kernel log names the source
(`consoleId <first 4 digits>... (OpenPSID via <source>)`). When neither answers,
it is `null` and the title keeps running. The desktop preview returns `PS5_REACT_CONSOLE_ID` when that holds 32
hex digits (lowercased), a fixed preview ID
(`70733572656163742d70726576696577`) when it is unset, and `null` when it is
set to anything else, which lets you exercise the unknown case.

## FileSystem

By default, the title runs sandboxed. App paths are logical roots; explicit
console paths become accessible only after opt-in filesystem access succeeds.

| Constant | Path | Access |
| --- | --- | --- |
| `FileSystem.appDir` | `/app0` | The installed application folder; read-only |
| `FileSystem.dataDir` | `/download0` | Writable, persists between launches |
| `FileSystem.tempDir` | `/temp0` | Writable scratch space |

On the console, listing `/app0` may return an empty array even though files
inside it can be read by path. Paths containing `..` components are rejected.

| Function | Returns | Notes |
| --- | --- | --- |
| `readDir(path)` | `FileStat[]` | Entries of a directory, without `.` and `..` |
| `stat(path)` | `FileStat \| null` | `null` when the path does not exist |
| `exists(path)` | `boolean` | `stat(path) !== null` |
| `readFile(path)` | `string` | UTF-8; throws above 8 MiB |
| `writeFile(path, text, {sync})` | — | Creates or truncates; `sync: true` flushes it to the disk before returning, for a file saved then renamed into place |
| `appendFile(path, text)` | — | Creates or appends |
| `mkdir(path, {recursive})` | — | `recursive` defaults to `false` |
| `remove(path)` | — | A file or an empty directory |
| `removeTree(path)` | — | A file, or a directory and everything in it; symbolic links are unlinked, never followed. Synchronous: for small app-owned trees |
| `removeAsync(path)` | `Promise<void>` | What `removeTree` removes, on a worker thread, one path at a time in the order asked. Resolves once the path is gone, or if it never existed; rejects with an `Error` naming the path and the system error. For large files and trees, whose removal holds the screen for seconds |
| `rename(from, to)` | — | Both are app paths |
| `chmod(path, mode)` | — | Permission bits, such as `0o777`; a file or directory the console's own services must read (a title's files) needs them open to every user |
| `mounts()` | `{device, path, type}[]` | Up to 64 mounted filesystems visible to this process, as host paths; see [Hardware status](#hardware-status) |
| `diskUsage(path)` | `{total, free}` | Bytes for the filesystem holding `path`; `free` is available to the app, clamped to `0..total` |

`FileStat` is `{name, isDirectory, isFile, size, modified, device, inode}`, where
`size` is in bytes and `modified` is milliseconds since the Unix epoch.
`device` and `inode` are decimal strings that preserve native 64-bit identities.
Use both to detect replacement of a directory or file, including another drive
mounted at the same path; these identities are filesystem-local, not universal
drive serial numbers.

Files are text only; there is no binary read or write.

For console-wide filesystem access, add this to the app manifest:

```json
{ "filesystemAccess": "console" }
```

Then query storage from an event handler or effect:

```js
const entries = FileSystem.readDir('/data');
const mounts = FileSystem.mounts();
const {total, free} = FileSystem.diskUsage('/data');
```

Keep `filesystemAccess` set to `"sandbox"` (the default) for apps that only
need their own files. The opt-in helper requires resident Lapy or a local ELF
loader; see [access requirements](#hardware-status). On failure, filesystem
calls retain their normal permission errors rather than returning fake values.


## Http and Downloads

`Http.request(url, options)` provides bounded text/JSON HTTP responses, effective
URLs and allowlisted lowercase response headers. Set `followRedirects: false`
to inspect redirect metadata; cookies and authentication headers stay private.
HTTP requests accept a custom `Range` header for bounded probes; downloads own
their ranges and reject it. Custom headers disable automatic redirects. Response
headers include `etag` and `last-modified` for stable file identity checks.
`Downloads.enqueue({url, destination, ...options})` returns a cancellable task
with progress subscriptions and a completion promise. Binary downloads stay
native, use parallel validated ranges when eligible, and resume durable ranges.
Progress snapshots carry `written`, the live disk count, and `completedBytes`,
the bytes of whole ranges a resume keeps; show resumable progress from the latter.
`Downloads.enqueueManifest({manifest, destination, ...options})` supports split
byte manifests with per-piece SHA-1 and whole-file SHA-256 verification.
`DownloadFormats` validates manifests and routes image, package, archive and
binary filenames to their appropriate destinations. Native download options
include opt-in `rejectHtml`, exact `expectedBytes`, `storageRoot` identity checks, `recoverCompleted`
for receipt-backed SHA-256 verification of published files, and normalized
`pieces: [{url, offset, size, sha1}]`. These options share ABI v2 on both hosts;
there are no additional global native functions.
PS5 builds require `networking: true` and `filesystemAccess: "console"`; desktop
networking is available by default. See [NETWORKING.md](NETWORKING.md) for
options, resource limits, errors, lifecycle and the hardware-validation gap.

## Images

`<Image source={{uri}}>` loads `http://` and `https://` URLs natively: fetched
and decoded by two workers off the render thread, at the size the element is
drawn, into a bounded LRU cache. `Image.prefetch(uri, {width, height,
resizeMode})` warms that cache. `Image.getColor(uri)` resolves to the art's most
prominent vivid colour as `'#rrggbb'`, or `null` for grey or dark art, computed
while decoding a small copy. The bridge (`image.load`, `image.release`,
`image.poll`, ABI v3; results carry that `color`; `image.warm` and `image.defer(ms)`, which
ScrollViews call while they move to hold network requests back) is internal to the `Image` component. See
[IMAGES.md](IMAGES.md) for sizing, formats, memory budgets and errors.

## Notifications

`Notifications.show(message, subMessage?)` shows a system notification and
returns `true` when the host accepted it.
On PS5 it uses libkernel's `sceKernelSendNotificationRequest`: a plain toast whose
subMessage follows the message on a second line.

`Notifications.send({id, title, body?, icon?, kind?})` (ABI v11) shows a rich
toast in the system's Downloads channel and returns `true` when one was sent.
A later `send` with the same `id` replaces the toast instead of stacking a new
one, so a job can post its start, completion and failure under one id.

| Field | Notes |
| --- | --- |
| `id` | Non-empty string; letters, digits, `_` and `-` are kept, others become `_`, and only the first 48 count |
| `title` | The toast's first line |
| `body` | Optional second line |
| `icon` | Optional app path (`/download0/icons/PPSA01234.png`, resolved to the console path) or http(s) URL |
| `kind` | `'info'` (default), `'done'`, `'failed'` or `'attention'`; without an `icon`, `'failed'` and `'attention'` show the system's caution icon, the others its download icon |

On PS5 it sends `sceNotificationSend(0xFE, true, json)` (the payload SDK's
`samples/notify`), with `viewTemplateType` `InteractiveToastTemplateB`,
`channelType` `Downloads`, `toastOverwriteType` `Always` and a `bundleName` of
`ps5react_<TITLE_ID>_<id>`, the key the system's overlay replaces toasts by
([ShadowMountPlus TOASTS.md](https://github.com/drakmor/ShadowMountPlus/blob/1.7/TOASTS.md)).
`sceNotificationSend` is looked up in `libSceNotification.sprx` at run time;
when that fails or the call returns an error, `send` falls back to
`show(title, body)`. An unknown `kind` or an `icon` path outside the app's paths
throws, naming the id. **Not yet validated on hardware**, from a title or from a
payload: neither the toast nor the replacement has been seen on a console.

## Users

| Function | Returns |
| --- | --- |
| `Users.getForeground()` | `{id, name} \| null` — the user who launched the app |
| `Users.getLoggedIn()` | `{id, name}[]` |

## Controller

| Function | Notes |
| --- | --- |
| `setLightBar(color)` | `'#rrggbb'` or `{r, g, b}` with 0–255 channels |
| `resetLightBar()` | Restores the system color |
| `vibrate(strength = 1, ms = 200)` | `strength` 0–1 |
| `getState()` | `GamepadState`, the latest polled state |

`GamepadState` is:

```ts
{
  connected: boolean;
  leftX: number; leftY: number;    // -1..1, deadzone applied
  rightX: number; rightY: number;  // -1..1, deadzone applied
  l2: number; r2: number;          // 0..1
  buttons: string[];               // held: up down left right cross circle triangle
                                   // square l1 r1 l2 r2 l3 r3 options touchpad
}
```

The host updates the state once per frame, before JavaScript timers and
controller actions run.

`useGamepad(intervalMs = 16)` polls `getState()` while the component is mounted
and re-renders only when the state changes:

```jsx
function Sticks() {
  const pad = useGamepad();
  return <Text>{pad.leftX} {pad.leftY} {pad.buttons.join(' ')}</Text>;
}
```

Menus use focus navigation ([NAVIGATION.md](NAVIGATION.md)). `useController`
receives the raw `up`, `down`, `left`, `right`, `confirm`, `back`, `l1`, `r1`,
`l2`, `r2`, `triangle`, and `square` actions (plus `previous`/`next` for
compatibility); `useGamepad` is for raw analog and
button state. Options is still reserved for the host's exit action.

## Sound

Short interface sounds: focus ticks, confirm, back, errors, notifications.
Import a WAV like an image; the build bakes it into the app and the import is
the sound's name:

```jsx
import {Sound, useNavigationEvents} from '@ps5-react/core';
import tick from './assets/sounds/focus.wav';

useNavigationEvents(event => { if (event.type === 'move') Sound.play(tick); });
```

| Function | Notes |
| --- | --- |
| `Sound.play(name, {volume = 1, pan = 0})` | Returns `true` when the sound started; `volume` 0–1, `pan` -1 (left) to 1 (right) |
| `Sound.setVolume(volume)` | Master volume 0–1 for every sound (0 mutes) |

- **Format.** 16-bit PCM WAV, mono or stereo, 8–96 kHz, at most 10 seconds;
  other files fail the build with the file name. Keep sounds short: they stay
  decoded in memory (8 bytes per frame). An app that imports no WAV opens no
  audio output.
- **One voice per sound.** While a sound still plays, playing it again is
  skipped and returns `false`, so a held D-pad keeps ticking without stacking.
  Different sounds overlap.
- **Cheap and non-blocking.** `play` posts a command to a lock-free queue and
  returns; mixing runs on an audio thread (the ps5-homebrew-ui mixer: 32 voices,
  48 kHz stereo, a soft limiter). It returns `false` when there is no audio
  output. An unknown name throws `sound.play <name>: no imported .wav has this
  name`.
- **Feedback for navigation.** `useNavigationEvents` ([NAVIGATION.md](NAVIGATION.md#navigation-events))
  reports each move, blocked direction, press, back and button action, so one
  hook at the root gives every screen its sounds.

`apps/starter/sounds.js` wires the framework's sound set (`focus`, `confirm`,
`back`, `error`, `page`, `notify`) this way, and `npm run create` copies it. The
set is derived from Google's Material Design sound resources (CC BY 4.0): keep
`licenses/material-sounds-NOTICE.txt` with an app that ships it. `node
tools/ui_sounds.mjs <material wav directory> <output directory>` rebuilds it from
the pack's WAV files.

## Linking

`Linking.openURL(url)` opens an `http://` or `https://` URL in the system
browser and returns `true` when the host launched it. Other schemes throw.

## Power

`Power.keepAwake(enabled)` keeps the console out of rest mode for inactivity
while `true`, for work that runs with nobody touching the controller, such as a
download of several hours. Turn it on when the work starts and off when it
ends; the last call wins, so an app with several jobs keeps one count of its
own.

The PS5 host calls `sceSystemServicePowerTick` every 30 seconds while it is on,
which restarts the inactivity timer as input does (the method FTP and download
homebrew use). The desktop preview disables or enables the SDL screen saver and
prints `[power] keep awake on|off`. **Not yet validated on hardware**: no test
has shown a console staying awake through its rest-mode timeout.

While it is on, the PS5 host also calls `sceShellCoreUtilResetAutoPowerDownTimer`
once a minute (what the system's installer screen does during an install), from
the console-state watcher thread of [System](#system-abi-v11), so the reset keeps
going while the render loop is throttled. The call is looked up in
`libSceSystemService.sprx` at run time; the log records whether it was found and
its first result. Unvalidated on hardware like the tick.

## System (ABI v11)

`System` reports the console's state to an app, or to a background process
built on this engine:

```js
import {System} from '@ps5-react/core';

const unsubscribe = System.onStateChange(({state, at, source, titleId}) => {
  if (state === 'sleeping') queue.pause();
  if (state === 'resumed') queue.revalidateAndResume();
});
const game = System.runningGame(); // {titleId: 'PPSA01234'} | null
```

| Call | Notes |
| --- | --- |
| `System.onStateChange(listener)` | Calls `listener` on the JavaScript thread for each change after the call; returns the function that unsubscribes. Polls the host every 500 ms while a listener is subscribed. |
| `System.runningGame()` | `{titleId}` of the game running beside the app as last checked, or `null`. |
| `System.simulate(state, {titleId}?)` | Test hook: reports a state as the console would, to listeners and to the native services. |

An event is `{state, at, source, titleId?, pausedMs?}`: `state` is
`'sleeping'`, `'resumed'`, `'game-started'` or `'game-ended'`; `at` is
milliseconds since the Unix epoch; `titleId` names the game of `game-started`
and `game-ended`.

A native thread (`native/shared/system_state.cpp`) watches once a second, with
an explicit 512 KiB stack, so Sony calls never run on the render thread. Each
`source` is one way the state is learned:

| `source` | Reports | PS5 | Desktop |
| --- | --- | --- | --- |
| `event` | `sleeping`, `resumed` | `sceSystemServiceReceiveEvent`: `BEFORE_SLEEP` (0x10000001), `ON_RESUME` (0x10000000), as ezRemote Server reads them | — |
| `state-flag` | `sleeping`, `resumed` | The `SceSystemStateMgrInfo` event flag (`sceKernelOpenEventFlag`/`sceKernelPollEventFlag`), as ShadowMountPlus reads it: `SUSPEND_ON_GOING` (300) or `MAIN_ON_STANDBY` (500) is sleeping, a return to `WORKING` (1000) a resume | — |
| `sigcont` | `resumed` | `SIGCONT`, which PKG Manager treats as the end of standby | `kill -CONT` |
| `gap` | `resumed`, with `pausedMs` | The watcher ran more than 10 s late by the monotonic or the wall clock (`network.cpp` uses the same rule) | The Mac's sleep, `kill -STOP` |
| `game-poll` | `game-started`, `game-ended` | Every 3 s: `sceSystemServiceGetAppIdOfRunningBigApp`, then `sceSystemServiceGetAppTitleId` | — |
| `simulated` | any | `System.simulate` | `System.simulate`, or `ps5_drive.py system <state> [titleId]` |

A sleep is reported once until the next resume, and a resume reported by
several sources within 15 seconds arrives once, from the first. A gap also
follows a game in front throttling background threads (a `sleep(5)` has
overrun 60 s there), so a `gap` resume does not prove rest mode. A process
frozen in rest mode receives `sleeping` and `resumed` only once it runs again.

`runningGame()` is the running big application other than this app, skipping
system applications (`NPXS…`): a game in the background of the home screen
still counts. It relies on the two `sceSystemService` calls etaHEN makes from
ShellUI, looked up in `libSceSystemService.sprx` at run time. **Unverified**:
whether a title or a payload may make them, and what they answer, needs a
console (spike B7 of the background-downloads plan). Within an app it is `null`
while the app itself is in front.

Native services read the same state without JavaScript, from any thread:
`system_state::suspended()` (between a sleep and the next resume),
`system_state::game_running()` and `system_state::resumed_at_us()` (monotonic
time of the last resume). `native/shared/system_state.hpp` declares them with
the platform probes; a host installs `__ps5ReactNative.system` and calls
`system_state::stop()` on exit. The network and archive services do not use
them yet.

**Not yet validated on hardware**: no PS5 has delivered any of these events to
this engine. The log names each Sony call's first result
(`system: sceSystemServiceReceiveEvent=0x…`) and every state-flag change.

## Fonts

`Fonts.setLanguage(tag)` sets the interface language (a BCP 47 tag such as
`'ja'`, `'zh-Hans'` or `'ar'`) for text drawn from the runtime fonts an app
packages with `textFonts` ([International text](TEXT.md)). It chooses the
Japanese or the Chinese form of Han characters and the language text is shaped
for. It starts as `DeviceInfo` `language`; call it before text renders, since
text already laid out keeps its layout until it changes. **Not yet validated on
hardware.**

## BackHandler

`BackHandler.exitApp()` asks the host to close the app after the current frame,
exactly like pressing Options (React Native's name for the same call).

`BackHandler.exitApp({relaunch: true})` also opens the app again once it has closed, and returns
whether that was arranged. An app cannot launch itself after it exits, so on PS5 it sends
`relauncher.elf` (`native/ps5/payloads/relauncher.c`, shipped in the app folder) to the payload
loader on `127.0.0.1:9021`; the payload waits for the title to close, then launches it for the
foreground user. Without the loader it returns `false`, logs why, and the app only closes. The
desktop preview runs itself again.

## Desktop preview behavior

`npm run dev` and `npm run preview` implement the same API on macOS:

- Files live in `.build/<app>/sandbox/`: `download0/` and `temp0/` are
  directories, and `app0` is a link to `apps/<app>/`. `/` lists the sandbox
  root. Unlike the console, `/app0` is writable on desktop; do not rely on it.
- `mounts()` lists the Mac's mounted filesystems.
- `Notifications.show` prints `[notify] message: subMessage` to the terminal.
- `Notifications.send` prints `[notify] title: body`, as `show` does, and returns `true`.
- `System` reports no console states of its own: `SIGCONT` and gaps (the Mac's
  sleep) still arrive, and `System.simulate` or `ps5_drive.py system <state>`
  feeds the rest. `runningGame()` is `null` unless simulated.
- `Users` reports the macOS user (`$USER`) with id `1`.
- Temperatures are `null`; `cpuFrequency` is `null` on Apple silicon.
- `language` comes from `PS5_REACT_LANGUAGE`, else `LC_ALL`, `LC_MESSAGES` or
  `LANG` (`pt_BR.UTF-8` reads `pt-BR`); `null` for the `C`/`POSIX` locale.
- `openURL` opens the URL in the default macOS browser.
- With an SDL game controller connected, the light bar and rumble use it;
  otherwise the calls print a `[pad]` line. Without a controller, `getState()`
  reports `connected: false` and neutral values.
- Sounds play through SDL's default output. `SDL_AUDIODRIVER=dummy` keeps the
  preview silent; the self-test sets it.

## Hardware status

The PS5 host implements this API against the public PS5 Payload SDK.
It has **not yet been validated on hardware** except where
[HARDWARE.md](HARDWARE.md) records a test for a specific artifact, firmware,
and loader. Desktop behavior and successful PS5 builds do not establish console
behavior; individual calls may fail or return `null` on a given firmware.

Some PS5 calls load a system module at runtime. It is unverified whether a
title can load these modules; when one cannot, only the dependent call degrades:

| Call | Module | Without it |
| --- | --- | --- |
| `DeviceInfo.get().model` | `libkernel_sys.sprx`, `libkernel.sprx` or `libkernel_web.sprx` | `null` |
| `DeviceInfo.get().consoleId` | The same, else the `machdep.openpsid` sysctl | `null` |
| `Notifications.send()` | `libSceNotification.sprx` | Falls back to `Notifications.show()` |
| `Power.keepAwake()` timer reset | `libSceSystemService.sprx` | Only the power tick runs |
| `System.runningGame()` | `libSceSystemService.sprx` | `null`, and no game events |

PS5 `diskUsage()` uses the native-title `_fstatfs` export on a descriptor opened
with `sceKernelOpen`. `mounts()` discovers kernel-provided records through
accessible descriptors, deduplicates them, and returns up to 64 records. Discovery
checks sandbox roots, common storage paths, and immediate children of `/` and
`/mnt`, with at most 96 metadata queries. It is not the privileged global mount
table returned by a payload's `getmntinfo`; inaccessible or deeper mounts can be
absent. No mount records or capacities are invented.

Hardware logs confirmed that our removed direct-syscall prototype terminated the
title with `SYSTEM_ILLEGAL_FUNCTION_CALL`. The replacement uses libkernel's
native wrapper, following a community title reference. The replacement
filesystem integration was confirmed by the user on firmware 13.60 with kstuff
and ShadowMount; exact application/helper hashes are recorded in
[Hardware evidence](HARDWARE.md#system-explorer-filesystem-success--2026-10-05). Directory records from `sceKernelGetdents` are bounds-checked before
names reach JavaScript. See [Storage investigation](STORAGE.md).

Standalone titles can still receive `EPERM` even with kstuff running. For a
console filesystem explorer, set `"filesystemAccess": "console"` in `app.json`
(default: `"sandbox"`). The build packages a pinned upstream Lapy one-shot
helper restricted to that title. Startup requests access before creating the
React thread and accepts success only after a write/read/remove proof in `/data`.
The console must provide resident Lapy or an ELF loader on localhost port 9021.
Startup can take several seconds; no access requests run during React renders.

After success, `/app0`, `/download0`, and `/temp0` remain logical app paths mapped
to their accessible sandbox mounts. Data and temporary roots must pass an
exclusive file creation, write, read and removal proof; opening a directory
for reading alone does not establish write access. If the original roots fail
that proof after the root change, data falls back to
`/data/ps5-react/<TITLE_ID>` and temporary files to its `tmp` directory. Directory
creation and fallback proofs are checked and failures log the path and errno.
That fallback persists until explicitly removed.
Failure leaves console path mapping disabled and logs the upstream status.
Optional native API modules are resolved before requesting the root change.
Desktop behavior is unchanged. The opt-in integration was user-confirmed on
firmware 13.60 with kstuff and ShadowMount. Other firmware/loader combinations
require their own validation.

`diskUsage` measures a filesystem, not recursive directory size or the console
Settings storage categories. On desktop it reports the real Mac filesystem
backing the per-app sandbox.

System Explorer caches these queries when its Files page opens. Navigate to a
directory or select **Refresh** to query again; focus changes do not query storage.

## Not yet available

- Global `fetch`, raw sockets and WebSocket.
- Music, streamed or long audio, and decoding compressed formats at run time.
- The system on-screen keyboard (IME) for text input.
- System save data; use `FileSystem.dataDir` for now.

## Package installation

```js
import {Packages} from '@ps5-react/core';
const task = Packages.install({path: '/data/downloads/packages/CUSA04286-game.pkg', name: 'BATTLESHIP'});
const unsubscribe = task.subscribe(({status, written, total}) => console.log(status, written, total));
try { await task.done; } finally { unsubscribe(); }
```

Installs a `.pkg` or `.fpkg` under `/data`, `/mnt/usb*` or `/mnt/ext*` through
the console's install service (`sceAppInstUtilInstallByPackage`). It takes a PS4
package (`\x7FCNT`, content ID at 0x40) or a PS5 one (`\x7FFIH`, whose PS4-style
header sits at the 64-bit little-endian offset at 0x58 and names the content ID
the same way); anything else, or a PS5 package cut short of that header, fails
before the console sees it. Only a PS4 fake package is confirmed on a console
(`docs/HARDWARE.md`); PS5 packages go through the same call untested, and a
retail one (byte 5 of the header 0x80) is encrypted for a licence the console
must hold. An app may not call that service, so each install sends
`pkg-installer.elf`, built from `native/ps5/payloads/pkg_installer.c` and
shipped in the app folder, to the payload loader on `127.0.0.1:9021`; a jailbroken
console without that loader fails with an error naming the port. The snapshot
carries the console's install `status` (`transferring`, `promoting`,
`playable`), `written`/`total` bytes and the package's `contentId`; the task
completes once the title is playable. One install runs at a time. `cancel()`
stops reporting only: the console finishes an install it accepted. The preview
fails each request. Fake packages need a kernel that accepts them (kstuff); on
firmware above 11.60, PS5 fake game packages install but do not launch.

## Archive extraction

```js
import {Archives} from '@ps5-react/core';
const task = Archives.extract({
  sources: ['/data/downloads/part-1.bin', '/data/downloads/part-2.bin'],
  destination: '/data/downloads/extracted/my-release',
  maxBytes: 100 * 1024 ** 3,
  password: 'published-archive-password', // optional; case and brackets are significant
});
const unsubscribe = task.subscribe(({state, written}) => console.log(state, written));
try { const {artifacts} = await task.done; console.log(artifacts); }
finally { unsubscribe(); }
```

`cancel()` requests cancellation. ZIP and TAR use libarchive in a worker thread.
RAR sets and every 7z without a password go to `rar-extract`, a program of its
own (started through the payload loader on the console, a child process on the
desktop): it decodes LZMA2 on several cores with the console's memory rather
than the title's heap. A 7z with a password, and either kind when the worker
cannot start, use libarchive in the app. Supply ordered archive volumes; no full
joined copy enters JS or RAM. One extraction may run at a time. Source files are preserved. Output paths,
links, entry counts and byte budgets are validated. Extraction publishes a staged
directory only after successful reading and SHA-256 output receipts. A matching
published directory is reverified; matching interrupted staging is restarted.
Unowned destinations/staging are preserved. Image artifacts are flattened to the
output root for ShadowMount scan depth; duplicate filenames fail without overwrite.
`artifacts` lists the images (by extension) and the packages: files named `.pkg`
or `.fpkg`, in any case, that start with a PKG signature (`\x7FCNT` or `\x7FFIH`); a file
so named that is something else is left out. The receipt is streamed to disk a
line per file, so its size does not limit the number of files (an archive may
still hold at most 100,000 entries). Extracted folders are mode 0755, `eboot.bin` and
`.elf`/`.self`/`.prx`/`.sprx` files (or any the archive marks executable) 0755,
and other files 0644, whatever the process umask.

`password` is optional, with at most 1024 UTF-8 bytes and no NUL. Wrong or missing
passwords fail without publishing partial output or removing source volumes.
ZIP decryption uses libarchive's passphrase support. RAR5 AES-256 headers/data are
decrypted in bounded buffers by the native reader before libarchive decompresses
them. Header CRCs, file checksums, decoder-memory limits and output-path validation
remain enforced. CBC padding is removed using compressed-block framing; no joined
or decrypted archive copy is written to disk. Ordered encrypted RAR5 volumes use
the same API. Optional quick-open (`QO`) service indexes are skipped; extraction
reads the original headers and verifies every file password and output CRC32.
An incomplete index payload or invalid enclosing header CRC still fails.
RAR5 files without CRC32, keyed CRC/BLAKE checksums, encrypted older RAR formats and
encrypted 7z are not supported. Password derivation is capped at 2^20 iterations
and runs on the extraction worker, never the render thread.

Unsupported encryption/compression methods and RAR5 dictionaries above
libarchive's supported limit fail explicitly. Before extracting, the headers are
read to refuse archives whose declared decoder windows (7z LZMA/LZMA2/PPMd/zstd
and BCJ2 buffers, RAR5 windows, RAR PPMd, ZIP LZMA/XZ/zstd/PPMd, and an outer
xz/zstd/lzip/lzma layer) exceed 64 MiB, since libarchive has no memory limit
and the title's heap is fixed. That budget assumes nothing else large runs during extraction. A 7z
refused for that alone, whose largest folder declares at most 288 MiB (the 256 MiB LZMA2 dictionary
repacks use), is extracted by `rar-extract`, with the console's memory rather than the title's heap. It first
takes that memory and writes to every page; when the console needs over a second to hand it over (it is
freeing memory from everything else: a probe froze a console from 512 MiB on), it fails with "The console
has too little free memory to extract this archive now. Close other apps and retry." before decoding.
Without a payload loader it fails with "This archive needs the payload loader to extract: none answered
on port 9021." Over 288 MiB the memory refusal stands. Self-extracting executables are refused. This API does not install PKGs or
register games. Desktop extraction and successful PS5 linking are not console
execution evidence.

`stream: true` starts an extraction before its download ends (`Archives.streams`
for RAR sets, `Archives.streamsSevenZip` for a 7z). For a RAR set only the first
volume must exist; each next one is read once a file appears under its final
name, so write volumes elsewhere and rename them. A 7z must be the one source a
`Downloads.enqueue` task is still writing (to `<source>.part`): `rar-extract`
decodes each folder as the download's contiguous prefix reaches it and ends once
the download is published under its final name. A 7z keeps its headers at its
end; with `tail: {url, headers}`, what the download requests, the engine fetches
them ahead with ranged requests of a few KiB into `<destination>.tail`, removed
when the extraction ends (or, after the app was closed during one, when the same
destination is extracted again). Without `tail` they are read once the download
reaches them. A streamed 7z with a password, or one only libarchive reads (branch
filters, PPMd, BCJ2, zstd), fails with "This 7z can't be extracted while it
downloads." and nothing written: extract it once the download completes. Both
need the worker: without one the task fails rather than extracting in the app.

`Archives.inspect(sources)` returns `{kind, refusal}` synchronously. The headers
are read on a worker thread and the call waits at most about 25 ms: a download
writing to the same drive can hold a read for seconds. When the read is still
running, the result also has `pending: true` and carries the previous answer for
the same sources (empty strings when there is none); the read finishes in the
background and the next call with those sources returns it. Call again later
rather than treating a pending empty `kind` as an answer. `kind` names the leading bytes: `rar`, `7z`, `zip`, `tar`,
`tar.gz`, `tar.bz2`, `tar.xz`, `tar.zst`, `pkg` (`\x7FCNT` or `\x7FFIH`), `exfat`, `ffpkg` (a
UFS2 superblock), `ffpfs`, `ffpfsc`, or `''`. For an archive, `refusal` is the
decoder-memory message above when the headers read so far already declare a
window over 64 MiB: an xz/zstd first block, or a RAR's file headers. A RAR's
first file header is always read; the walk then stops at the first header it
reaches after about 100 ms (each sits after the previous file's data, so a
volume of many files may be cut short). An extraction refuses the rest: its
preflight reads every header, and in a streamed set, which is checked by
`inspect` too, the RAR worker refuses any later header over its limits. It
works on a download's growing `.part` file: a hole or a missing end is not an
error, so a first volume can be refused before the remaining volumes download.
7z and ZIP keep their directories at the end and show no refusal until complete.
An empty kind is not proof of anything; image kinds are recognized by superblock
magic only, not validated as mountable.


## Shaders (ABI v10)

`ShaderView` and `BootIntro` ([SHADERS.md](SHADERS.md)) use
`__ps5ReactNative.shader`; apps use the components, not the namespace.

| Call | Notes |
| --- | --- |
| `shader.create(source)` | Layer ID for GLSL 4.10 core fragment source (a missing `#version` line is added); compiled when the presenter first draws it. At most 8 layers and 64 KB of source. |
| `shader.update(id, options)` | `{x, y, width, height}` in framebuffer pixels, `opacity` 0–1, `resolutionScale` 0.05–1, `uniforms` `{name: number \| [2–4 numbers]}` (at most 16). All optional; a bad option throws and changes nothing. |
| `shader.destroy(id)` | Deletes the layer; its GL objects go on the next frame. |
| `shader.error(id)` | The GLSL info log of a failed compile or link, else `null`. |

Errors name the call and the layer: `shader.update 3: resolutionScale must be 0.05 to 1`.
Both hosts implement it in `native/shared/shader_layers.cpp`, with the same GL. The desktop
self-test compiles a layer and reads back its pixels; not yet confirmed on hardware.

## Browser download capture (ABI v7)

```js
import {BrowserCapture} from '@ps5-react/core';
const task = BrowserCapture.capture({
  prefix: 'https://vikingfile.com/d/',
  suffix: '/Selected file.pkg',
  timeoutSeconds: 180,
});
const off = task.subscribe(({state}) => console.log(state));
try { const {url} = await task.done; /* Validate with HTTP before transferring. */ }
finally { off(); }
```

PS5-only, one capture at a time. Sends bundled `browser-capture.elf` to the
localhost ELF loader on port 9021. The desktop returns a failed task explaining
that a PS5 is required. The only permitted prefix is the exact Vikingfile HTTPS
download host; suffix is `/` plus a filename of at most 160 UTF-8 bytes without
controls, slashes, backslashes or `..`. The deadline is an integer from 1 to
180 seconds. Snapshots have `id`, `state`, `error` and a matching `url` on
completion. `cancel()` shuts down the connection, stops the payload's next
read, and rejects `done` with `AbortError`. Host exit stops and joins the worker.

The user must complete the provider's verification and select Download in the
system browser. This API does not bypass verification or open the page; the app
opens it with `Linking.openURL` after submitting capture. Only a uniquely
identified browser network process is read, with identity checked before every
read. Scanning is limited to anonymous readable/writable nonexecutable user
pages, 64 KiB every 32 ms, 128 MiB per session. No dumps, cookies, arbitrary
process selection or writes. Only complete ASCII/UTF-16 matching routes are
returned. Provenance: `native/ps5/payloads/browser/README.md` (Orbit Store 1.0.1).

A captured URL may already be stale; the app must validate host, decoded
filename, HTTP status, content type and length before starting immediately.
Local tests and compilation do not establish console/firmware support.

Native shape: `browser.capture(prefix, suffix, timeoutSeconds)` returns a task
ID; `browser.cancel(id)` requests cancellation; `browser.poll()` consumes
terminal results. Calls submit/poll synchronously on the render thread; all
payload transport runs on the worker.

## Daemon (ABI v11)

A daemon is a second, headless script of the app that keeps running after the
app closes: Pandora's download queue goes on while a game runs. The app declares
it in `app.json` and starts it; the daemon serves HTTP on `127.0.0.1:9061`, which
the app reaches with `Http.request`.

```json
{"daemon": "daemon/main.js", "networking": true, "filesystemAccess": "console"}
```

```js
// daemon/main.js: no JSX, no UI.
import {Daemon} from '@ps5-react/core';
const {token, stateDir, version} = Daemon.args;
Daemon.serve(async ({method, path, query, headers, body}) => {
  if (path === '/hello') return {status: 200, body: JSON.stringify({protocol: 1, version})};
  if (path === '/shutdown') { setTimeout(() => Daemon.exit(0), 0); return {status: 200, body: '{}'}; }
  return {status: 404, body: '{}'};
});

// The app.
const {ok, error} = await Daemon.start({token, stateDir: '/data/ps5-react/PPSA99060/queue'});
const hello = await Http.request('http://127.0.0.1:9061/hello');
```

| Member | Where | Notes |
| --- | --- | --- |
| `Daemon.supported` | both | The app declares a `daemon` entry |
| `Daemon.isDaemon` | both | True only inside the daemon |
| `Daemon.args` | daemon | `{token, stateDir, version}` from `Daemon.start`; `null` in the app |
| `Daemon.start({token, stateDir})` | app | `Promise<{ok, error?}>`; never rejects |
| `Daemon.serve(handler)` | daemon | Answers each request with what `handler` returns or resolves |
| `Daemon.exit(code)` | daemon | Answers what is pending, stops the native services, flushes the log, exits |

**Build.** The build bundles the entry with `@ps5-react/core` resolving to the
modules a daemon can use (`runtime/js/headless.js`): `Downloads`, `Http`,
`FileSystem`, `DownloadFormats`, `Archives`, `Packages`, `Notifications`,
`Power`, `Platform`, `DeviceInfo` and `Daemon`, plus timers and `console`.
Importing a UI module or `Linking`/`BrowserCapture` fails the build. It runs in
`pandorad`, a host with QuickJS and those native modules and no window, GL,
input or sound: `pandorad.elf` in the PS5 app folder, a `pandorad` program
beside the desktop preview. Its JavaScript runtime has the app's globals (no
`Date` objects) and a 32 MiB heap; timers run on the system clock.
The daemon requires `networking` and `filesystemAccess: "console"`.

**Start.** `Daemon.start` sends the daemon `PDD1\n<token>\n<stateDir>\n<version>\n`
(`version` is `app.json`'s) and resolves once it answers `ready`, within 10 s.
When the port is already taken the new daemon answers `busy` and exits with code
3; the start then succeeds if `GET /hello` answers 200. The token is one line of
at most 128 characters and `stateDir` an absolute path.

- PS5: the engine first copies `rar-extract.elf` and `pkg-installer.elf` from the
  app folder to `/data/ps5-react/<TITLE>/bin/` (SHA-256 checked, through a
  temporary file), then sends `pandorad.elf` to the payload loader on
  `127.0.0.1:9021`, like the other payloads. As the loader's child it outlives
  the app. It needs console filesystem access.
- Desktop: the preview spawns `pandorad` in a session of its own (`setsid`), so
  closing the preview leaves it running. `PS5_REACT_DAEMON_PORT` moves the
  preview's daemon off 9061, for tests beside other programs.

**Serving.** The first request waits until the script calls `Daemon.serve`; the
port is bound before the script runs, so a second daemon leaves before any of
its script runs. Each request reaches the handler on the script's thread as
`{method, path, query, headers, body}`: `path` as sent, `query` its decoded
parameters as an object, `headers` with lowercase names, `body` a string. The
handler returns `{status, headers?, body?}` (status 200-599, a string body);
framing headers are the server's. A handler that throws or returns anything else
answers 500, and the error goes to the log. The server answers by itself:

- 401 when `x-pandora-token` differs from `Daemon.args.token`, except `GET /hello`
  and paths under `/i/`;
- 413 for a body over 1 MiB (`Expect: 100-continue` is honored), 501 for chunked
  bodies, 431 for a request head over 16 KiB;
- 503 to a ninth concurrent connection. Connections are kept alive and dropped
  after a minute idle.

**Paths and log.** On PS5 the app's sandbox mounts go away when it closes, so in
the daemon `/download0` (`FileSystem.dataDir`) is `/data/ps5-react/<TITLE>`,
`/temp0` and `/cache0` are its `tmp` and `cache` folders, and `/app0` is refused;
other absolute paths are used as they are. Keep shared state on `/data` (an
absolute `stateDir`). The daemon starts workers from `<FileSystem.dataDir>/bin`.
On the desktop the paths resolve in the preview's sandbox. Console lines,
uncaught errors and the native log go to `<FileSystem.dataDir>/daemon.log`; the
previous run's is kept as `daemon.prev.log`.

**Not yet available:** the `/i/` range server for the console's installer
(E6), sleep and resume events, and idle exit, which the script decides with
`Daemon.exit`. Nothing of the daemon has run on a console yet
([HARDWARE.md](HARDWARE.md)).
