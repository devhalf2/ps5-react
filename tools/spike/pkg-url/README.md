# Spike 0A: PKG install from a URL

Can the console's own installer (`sceAppInstUtilInstallByPackage`) pull a PKG
from an `http(s)://` URL, how does it request the bytes, and does it keep going
once our process exits, a game runs, the console rests or reboots, or the server
stalls? This kit answers that from a Mac, with no app change. It is Phase 0A of
the background-downloads plan; the answers decide whether "install while
downloading" (Route U) goes ahead.

Nothing here touches the console except `send.py`, which you run yourself.

| File | What it does |
|---|---|
| `build.sh` | Builds `pkg-installer.elf` alone into `.build/spike/` |
| `content_id.py` | Prints a PKG's content ID and the URL to hand the console |
| `serve.py` | Serves one folder with HEAD and Range, logs every request as JSON |
| `send.py` | Sends the ELF and a request to the loader on 9021, logs every answer |
| `summary.py` | Condenses a `serve.py` log: request pattern, connections, speed, gaps |

## The request

`pkg-installer.elf` still takes the app's `PKI1` request unchanged. The spike
uses `PKI2`, whose lines are:

```
PKI2
<package path under /data, /mnt/usb*, /mnt/ext*, or an http(s) URL of at most 127 bytes>
<content ID; required for a URL, checked against the file for a path when given>
<name the home screen shows>
<icon URL, may be empty>
<flags: any of "dump", "detach", "more", space-separated>
```

- `dump` prints `raw <hex>`, the first 0x300 bytes of a zeroed 4 KiB status
  block, every second for 30 s and then whenever they change; a failed status
  call prints `raw error <code>`. The status layout is disputed (see "Status
  layout" below), so this is how we settle it.
- `detach` makes the payload exit right after `started` (T2).
- `more` keeps the process and reads another `PKI2` request once this install
  ends (T9). `send.py` sets it when you repeat `--uri`.

The answers match `PKI1`: `ready`, `started <content ID>`, `p <done> <total> <stage>`
each second, then `ok` or `fail <code> <reason>`. `MetaInfo` stays fully zeroed
(0x70 bytes) apart from its pointers. `sceAppInstUtilInitialize` gets 10 s,
then the payload answers `fail 00000000 The install service did not answer
within 10 s.` A URL install gets 60 s of unreadable status before the watch
gives up, instead of 10. The install itself carries on either way.

## Setup (once)

```sh
cd ~/Documents/ps5-react            # or this worktree
export PATH="$HOME/.homebrew/bin:$PATH" PS5_LLVM=$HOME/.homebrew/opt/llvm@18/bin
tools/spike/pkg-url/build.sh        # prints the ELF's sha256: note it for the results
```

`build.sh` needs the payload SDK in `.deps/`, which any earlier `npm run build`
fetched. `npm run build` also builds the same file as
`.build/starter/ps5/pkg-installer.elf`.

Pick a folder outside the repo for packages and logs, and set the two
addresses. Run the `export` lines in both Terminal windows you use below. The console must have network access even for a LAN URL (the
installer refuses with 0x80B21121 offline).

```sh
mkdir -p ~/pkg-spike/logs
export PS5=192.168.1.50             # the console
export MAC=192.168.1.10             # this Mac; serve.py prints it on start too
export K=tools/spike/pkg-url ELF=.build/spike/pkg-installer.elf
```

Give each package a short plain name (`t1.pkg`): the whole URL must fit in 127
bytes, and `content_id.py` checks that:

```sh
cp /path/to/BATTLESHIP.pkg ~/pkg-spike/t1.pkg
python3 $K/content_id.py ~/pkg-spike/*.pkg --base-url http://$MAC:8080/
# ~/pkg-spike/t1.pkg  PS4  UP0001-CUSA04286_00-...  1246167040 B  http://192.168.1.10:8080/t1.pkg  32 B
export CID=UP0001-CUSA04286_00-XXXXXXXXXXXXXXXX     # from that output
```

Before every test: load the ELF loader (and kstuff) as usual, and delete the
game from the console if an earlier run installed it, unless the test says
otherwise. On the first run macOS may ask whether Python may accept incoming
connections: allow it.

Each test uses two Terminal windows from the repo root: **A** runs the server,
**B** runs `send.py`. Stop the server with Ctrl-C once the test ends.

## Tests

### T1: base case, LAN http URL (1 GB PS4 fPKG)

```sh
# A
python3 $K/serve.py --dir ~/pkg-spike --port 8080 --log ~/pkg-spike/logs/t1-serve.jsonl
# B
python3 $K/send.py --ip $PS5 --elf $ELF --log ~/pkg-spike/logs/t1-send.log \
  pki2 --uri http://$MAC:8080/t1.pkg --content-id $CID --name "T1 URL" --dump
```

Watch on the console: a notification, the title in Downloads with progress, the
game tile, and whether the game starts. Watch in A: a HEAD, the `.crc` request
answered 404, then ranged GETs. B should end with `ok`. Afterwards:

```sh
python3 $K/summary.py ~/pkg-spike/logs/t1-serve.jsonl > ~/pkg-spike/logs/t1-summary.txt
```

### T2: the caller exits right after `started`

Same as T1 with `--detach` (and new log names). The payload exits after
`started`, so `send.py` ends there without `ok`. Check that Downloads keeps going and that A keeps logging ranges to
the end, and that the game starts.

### T3: a game runs during the install

Start T1 (or T2). Once A shows ranged GETs, launch a game from the home screen
and play for 2-3 minutes. Note when you launched it. The summary's throughput
rows show the speed before and during the game. Note whether Downloads still
shows progress while the game runs.

### T4: rest mode during the install

Note the setting Settings > System > Power Saving > Features Available in Rest
Mode > Stay Connected to the Internet (on or off). Start T2 (`--detach`, since
the payload is unlikely to survive rest). At around 30% progress put the console
in rest mode. Wait 5 minutes, wake it, and wait until the install either ends
or stops moving for 2 minutes. Note: do requests arrive in A during rest? Does
the download resume by itself after waking, or need a tap in Downloads? Any
error code? If the setting was off, repeat once with it on.

### T5: reboot during the install, server still up

Start T2. At around 30%, restart the console (Power > Restart PS5). Do not load
the ELF loader yet: leave the console at the home screen for 3 minutes and watch
A. Then load the loader as usual. Note: does the task come back in Downloads on
its own, and when do requests restart (before or after the loader)?

### T6: public HTTPS URL with a redirect

T6a, a LAN redirect over http, which separates the redirect from TLS:

```sh
# A
python3 $K/serve.py --dir ~/pkg-spike --port 8080 --log ~/pkg-spike/logs/t6a-serve.jsonl --redirect /r=/t1.pkg
# B
python3 $K/send.py --ip $PS5 --elf $ELF --log ~/pkg-spike/logs/t6a-send.log \
  pki2 --uri http://$MAC:8080/r --content-id $CID --name "T6a redirect" --dump
```

A should log `/r` with 302, then the follow-up requests on `/t1.pkg`; check which
path the ranged GETs use.

T6b, public HTTPS: use a PKG you are allowed to redistribute that is already a
public GitHub release asset (`https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.pkg`
redirects to a long signed URL). Download it once to the Mac to read its content
ID with `content_id.py`, delete the game first if installed, then:

```sh
python3 $K/send.py --ip $PS5 --elf $ELF --log ~/pkg-spike/logs/t6b-send.log \
  pki2 --uri "https://github.com/OWNER/REPO/releases/download/TAG/FILE.pkg" --content-id <its ID> --name "T6b https" --dump
```

`send.py` refuses URLs over 127 bytes before connecting. There is no server log
for T6b; the console's result and the `send.py` log are the evidence. Check the
console clock is right first.

### T7: a URL that already has a query

Same as T1 with `--uri "http://$MAC:8080/t1.pkg?x=1"`. In the summary, the
`queries` line shows what the console made of it: `x=1&product=...`,
`x=1?product=...`, or something else. Note whether the install completes.

### T8: the server goes quiet

Three runs each of two kinds, deleting the game between runs. Use `--detach` so
the payload's own timeouts do not matter. 268435456 bytes is 256 MiB, about a
fifth of a 1.2 GB package; check progress is moving before the stall starts.

T8a, the server holds its connections open and sends nothing for 30 s, 120 s,
then 600 s:

```sh
python3 $K/serve.py --dir ~/pkg-spike --port 8080 --log ~/pkg-spike/logs/t8a-30-serve.jsonl \
  --stall-after-bytes 268435456 --stall-seconds 30
```

T8b, the server is gone: start A as in T1, press Ctrl-C at around 20%, wait 30 s
(then 120 s, then 600 s), and start it again with the same `--log` (it appends).

For each run note: does the install pick up again by itself when the server
returns? What error does Downloads show, and when (time it from the stall)?
After a failure, does a retry from Downloads work? `summary.py` shows the
silence and the requests retried after it. Optionally also try
`--die-after-bytes 268435456`, which drops every connection once and keeps
listening.

### T9: two installs in a row, same process vs fresh process

Needs two different packages (`t1.pkg`, `t2.pkg`) and both content IDs. Same
process, with A running:

```sh
python3 $K/send.py --ip $PS5 --elf $ELF --log ~/pkg-spike/logs/t9-same-send.log \
  pki2 --uri http://$MAC:8080/t1.pkg --uri http://$MAC:8080/t2.pkg \
  --content-id $CID --content-id $CID2 --name "T9" --dump
```

The second request starts only after the first ends with `ok` or `fail`. Then
delete both games and run them as two separate `send.py` commands (fresh
process each). PKG Manager reports that the second install in one process fails
on 9.60 and later; note the code if it does.

### T10: a PS5 package (`\x7FFIH`)

`content_id.py` says `PS5` for these. Over the URL as in T1, with its own
content ID. If that fails, also try it from the console's disk (copy it to
`/data/pkg-spike/t10.pkg` with FTP) to separate the URL from the package type:

```sh
python3 $K/send.py --ip $PS5 --elf $ELF --log ~/pkg-spike/logs/t10-local-send.log \
  pki2 --uri /data/pkg-spike/t10.pkg --name "T10 local" --dump
```

PS5 fPKGs install but are reported not to boot above 11.60; note whether it
installs and, separately, whether it starts.

## Status layout

Two layouts for the status block circulate. `pkg_installer.c` reads etaHEN's:

| Offset | Field |
|---|---|
| 0x00 | stage, text (16 bytes) |
| 0x10 | source type, text (8) |
| 0x18 | remaining time, u32 |
| 0x20 | downloaded, u64 |
| 0x28 | initial chunk, u64 |
| 0x30 | total, u64 |
| 0x38 | promote progress, u32 |
| 0x3C | error code, i32, then version, then a 512-byte description from 0x44 |

ShadowMountPlus uses a 0x2C8-byte block with the error code at 0xA0. The `raw`
dumps decide it: `send.py` prints which byte spans changed between dumps (the
full hex stays in the log), so check that the `p` line's numbers match the
bytes at 0x20 and 0x30, and look at 0x3C and 0xA0 when an install fails (T8 is
the likeliest place to see an error).

## What to send back

For each test: the `*-send.log`, the `*-serve.jsonl` and the `summary.py`
output from `~/pkg-spike/logs/`, the ELF's sha256 from `build.sh`, photos of
any console error dialogs with the code, and one line on what the console
showed. Logs contain your LAN addresses; that is fine for us, but trim them
before posting publicly.

## Results template (for docs/HARDWARE.md)

```markdown
## PKG install from a URL (spike 0A) — YYYY-MM-DD

CFI-series console on firmware 13.60, kstuff-lite, John Törnblom's ELF loader on
port 9021. `pkg-installer.elf` (PKI2) sha256 `<hash>`, built from `<commit>`,
sent from a Mac with `tools/spike/pkg-url/send.py`; packages served from the Mac
by `serve.py` over <wired/Wi-Fi> LAN. Rest-mode internet setting: <on/off>.

| # | Test | Package | Result | Code | What the console did | Requests seen |
|---|---|---|---|---|---|---|
| T1 | LAN http URL | CUSA04286, 1,246,167,040 B | | | | HEAD, .crc 404, N×16 MiB ranges on K connections, X MB/s |
| T2 | Caller exits after `started` | | | | | |
| T3 | Game running mid-install | | | | | speed before / during |
| T4 | Rest mode mid-install | | | | | |
| T5 | Reboot mid-install | | | | | |
| T6a | LAN 302 redirect | | | | | |
| T6b | Public HTTPS + redirect | | | | | n/a |
| T7 | URL with `?x=1` | | | | | query as received |
| T8a | Silent stall 30 s / 120 s / 600 s | | | | | |
| T8b | Server down 30 s / 120 s / 600 s | | | | | |
| T9 | Second install, same process / fresh process | | | | | |
| T10 | PS5 `\x7FFIH` package (URL / local) | | | | | |

Status layout: <which offsets matched the progress; where the error code was>.
```
