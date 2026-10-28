# sys-agent client

Dependency-free Python client for the sys-agent Switch automation protocol. It mirrors every
non-FTP sys-agent command behind grouped subcommands; FTP is intentionally not built in (use
`curl ftp://switch:6001/` for SD-card file access).

## Commands

```
version   Show the client version and the version the host reports
backend   Inspect or configure the process-memory backend
system    Query or control system state
audio     Query or control system audio
game      Launch, close, or inspect the running game
debug     Trace instruction entry and inspect arbitrary processes
memory    Read or write process memory
freeze    Manage value freezing
input     Send controller, touch, or keyboard input
screen    Capture or control the screen
utility   Show device and application information
config    Change sys-agent runtime settings
search    Exact and unknown-value memory search
raw       Send any single-line command and print the raw response
```

Run `python3 client/sysagent.py --help` for the full list; every leaf command has its own
`--help` with argument descriptions.

## Version

The client is versioned on its own: `CLIENT_VERSION` in `client/sysagent.py` moves when that file
changes, and the sysmodule on the console keeps its own version, which only `getVersion` (and the
commands that wrap it) can report. The two are deliberately independent -- the console can gain a
build without the client changing, and the other way round -- so `version` prints both, labelled,
instead of picking one meaning for the word:

```bash
python3 client/sysagent.py --version                       # the client alone, one line, no host
python3 client/sysagent.py --host switch version           # both, labelled
python3 client/sysagent.py --host switch utility version   # the console's version alone
```

```text
client    1.1.1
sys-agent 2.16.1  (switch:6000)
```

The `client` line needs no network and is printed first; if the console cannot be reached the
`sys-agent` line is replaced by an error on stderr and the exit status is 2, so `version` doubles
as a reachability check. `utility version` is unchanged, which keeps scripts that already read the
console's version working.

## Backend

```bash
python3 client/sysagent.py --host switch backend status
python3 client/sysagent.py --host switch backend set auto
python3 client/sysagent.py --host switch backend probe
```

`auto` prefers Atmosphere `dmnt:cht`; `dmnt` requires it; `direct` preserves the original
standalone debugger behavior and will fail if dmnt already owns the game's debug handle.

## System

```bash
python3 client/sysagent.py --host switch system capabilities
python3 client/sysagent.py --host switch system query info
python3 client/sysagent.py --host switch system log --count 8
python3 client/sysagent.py --host switch system query network
python3 client/sysagent.py --host switch system process-list --offset 0 --count 64
python3 client/sysagent.py --host switch system wireless enabled
python3 client/sysagent.py --host switch system lock-screen status
python3 client/sysagent.py --host switch system action reboot
python3 client/sysagent.py --host switch system action reboot-emummc
python3 client/sysagent.py --host switch system action reboot --no-unlock
```

`system query network-profile` returns the Wi-Fi passphrase over an unauthenticated,
unencrypted TCP connection. Use it only on a trusted isolated network. The client never
automatically retries reboot, shutdown, sleep, wireless changes, or application termination.

## Game

```bash
python3 client/sysagent.py --host switch game status
python3 client/sysagent.py --host switch game wait --wait-timeout 60
python3 client/sysagent.py --host switch game launch-headless 0x01006F8002326000
python3 client/sysagent.py --host switch game terminate
python3 client/sysagent.py --host switch game name
python3 client/sysagent.py --host switch game version
python3 client/sysagent.py --host switch game icon --output icon.bin
```

Game subcommands are `status`, `wait`, `launch-headless`, `terminate`, `name`, `author`,
`rating`, `version`, and `icon`. `wait` polls until a game is running (bounded by
`--wait-timeout`, default 60s) instead of leaving the polling to the caller.
`status` reports the running application identity, version, memory
bases, Build ID, and name; `icon` writes a binary icon file with `--output` (default
`game-icon-<unix time>.bin`). `launch-headless` starts the game process without showing it on
screen (foreground launch requires the home-menu/applet flow, which a sysmodule cannot drive);
`terminate` is the system-level final termination (the same forced path the HOME-menu close
flow falls back to after its graceful-close timeout), because the graceful `RequestExit` API is
only reachable by system applets. `launch-headless` and `terminate` take effect immediately
with no confirmation. Numeric arguments (including Title IDs) accept `0x`-prefixed hex, bare
hex, or decimal.

`game launch-headless` auto-detects the storage holding the title's update (Patch) by querying
each storage's ncm content meta database and launches through that storage first, so the
running process is the updated build rather than the base build; it reports the detected
storage in `updateStorage=` and the storage that actually launched in `storage=`. An optional
second positional argument forces a specific storage:
`game launch-headless 0x01006F8002326000 BuiltInUser` (valid values: `SdCard`,
`BuiltInUser`, `GameCard`, `None`). Launch failures list every storage attempt in `attempts=`.

For updates whose code NCA uses rights-id encryption, `launch-headless` also registers the
external key before starting: it resolves the update's rights id and looks the rights id up
in the workspace `SDcard/switch/title.keys` mirror (override with `--titlekeys <path>`).
Preferred: a customized line `rightsId = <title_key_block hex> <keygen>` — the client asks
the sysmodule (`gameExternalKeyPrepareCommon`) to compute the current boot's AccessKey via
`spl:es PrepareCommonEsTitleKey` and register it, reported as
`externalKey=titlekey.block+spl`. This works on every boot without a manual game start.
Legacy fallbacks: a plain `rightsId = key` line (a boot-specific AccessKey value, stale
after a reboot; `externalKey=title.keys`), then `--keys <prod.keys>` (ticket + titlekek
decryption), then a plain launch (`externalKey=none`). Only the title-key block/keygen or
the titlekey travels to the console; `prod.keys` never leaves the host. See
`docs/headless-launch-rights-key-notes.md` for the full mechanism.

## Debug trace

Start with [`docs/debug-workbench.md`](../docs/debug-workbench.md) for the complete,
task-oriented workflow. The examples below are the command reference; the workbench guide
explains which path to choose and how to interpret the exported evidence.

```bash
python3 client/sysagent.py --host switch debug trace main+0x123456 --pid 0x71
python3 client/sysagent.py --host switch debug trace \
  main+0x123456 main+0x123460 --ring 64 --snapshot x0+0x10:0x40
python3 client/sysagent.py --host switch debug trace-status
python3 client/sysagent.py --host switch debug trace-read 0
python3 client/sysagent.py --host switch debug trace-dump --output trace.jsonl
python3 client/sysagent.py --host switch debug multitrace \
  0x71 main+0x1000 0x85 main+0x2000 \
  --extra-target1 main+0x1010 --extra-target2 main+0x2010 \
  --ring 64 --duration 30 --snapshot x1:0x100
python3 client/sysagent.py --host switch debug multitrace-status
python3 client/sysagent.py --host switch debug multitrace-dump --output multi.jsonl
python3 client/sysagent.py debug trace-analyze trace.jsonl \
  --pair 0x...:0x... --json
python3 client/sysagent.py debug trace-diff before.jsonl after.jsonl
python3 client/sysagent.py debug trace-ipc trace.jsonl --profile profile.json --json
python3 client/sysagent.py debug ipc-decode HEX_BYTES --mode request
python3 client/sysagent.py --host switch debug trace-plan profile.json
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --duration 30 --ring 64
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --wait-process --output trace.jsonl
python3 client/sysagent.py debug trace-report trace.jsonl profile.json --json
python3 client/sysagent.py --host switch debug read 0x71 main+0x1000 4
python3 client/sysagent.py debug trace-timeline first.jsonl second.jsonl --json
```

`debug trace` arms one to four hardware execution breakpoints without patching target code. It
records target index/address, thread/core/PC/LR/SP, `x0`–`x7`, and instruction bytes. Use
`--ring 64 --duration 300` to keep recent hits, or `--hits N` for a one-shot hit limit. Ring
mode defaults to a 64-hit safety cap; `--hits 0 --ring 64` explicitly removes it. Each
`--duration-from first-hit` starts the duration clock at the first matching hit, which is useful
when the trace must stay armed while a person reaches the operation. The default, `arm`, starts
the clock as soon as the breakpoints are armed. Each
`--snapshot REG[+/-OFFSET]:SIZE` reads memory through `x0`–`x30`, `sp`, `pc`, or `lr` while the
target is stopped and records its own Result; two snapshots are supported. On each retained hit
the tracer clears that execution slot, single-steps the stopped thread once, restores the slot,
and then resumes, so later targets can execute. The selected PID defaults to the foreground
application; `main+OFFSET` is resolved against that process. Configure a caller return address
as another target to capture its return-time `x0`; the trace itself is not a call-stack tracer.
`debug trace-stop`
releases the debug handle, after which memory commands can attach normally again. `trace-dump`
writes the retained ring as versioned JSONL (`schemaVersion=1`, one metadata record and event
records, including best-effort PID/Title ID/main-module/Build ID identity) and rejects a ring
that changed during the dump. `trace-analyze` combines one or more
exports, summarizes processes/threads/targets and pairs explicit entry/return addresses per
session and thread; the return event's `x0` is reported as the candidate Result. Multiple files
are directly comparable when captured in the same boot, because ordering uses the device system
tick. `trace-diff` compares event, process, thread and target counts between an A/B pair.

`debug multitrace` is the experimental two-process client for sysmodule `2.16.1+`. It assigns
one instruction target to each explicitly supplied PID, using runtime-probed context and
instruction slots. `--extra-target1` and `--extra-target2` add one more target to each side;
use them together to observe client/server entry and caller-return sites in one session. The
return event's `x0` is the actual function Result when the runtime function returns through
that address.
`--snapshot` may be repeated up to twice and captures memory relative to a register at every
hit. A typical HIPC boundary uses `--snapshot x1:0x100`. Profile v2 emits per-target
`tsnap=TARGET_INDEX:SPEC` rules so different sites can capture different pointers.
`multitrace-dump` writes the same versioned JSONL schema with a `targets[]` process table, so
`trace-analyze` attributes each event to the correct PID by target index.

`trace-plan`, `trace-run` and `trace-report` consume the versioned profile documented in
`docs/trace-profiles.md`. Profiles select processes by foreground status, Title ID or explicit
PID, verify title/Build IDs, resolve main-module offsets, and retain stable target IDs for
entry/return reporting. `trace-run --output FILE` waits for completion and exports JSONL;
without that option it returns immediately after the breakpoints are armed so the caller can
trigger the operation. A profile can set `trace.durationFrom` to `first-hit`; the explicit
`--duration-from arm|first-hit` option overrides it.

Profile v2 adds a consumer-owned symbol-offset table, per-target named/typed snapshots and
optional `expectBytes` validation through the generic `debug read PID ADDRESS SIZE` command.
Targets default to `base: "main"`; `base: "code"` resolves against the process's lowest
executable mapping, allowing an instruction-zero target such as `code + 0x0`.
`--wait-process` uses PM creation hooks to park a title before its main thread starts, attaches
and arms while it is suspended, then releases it after `armed=1`.
The client-side `debug wait-title` / `debug wait-application` commands poll the background
session until it is parked and then print the PID. `debug wait-status` observes that underlying
session directly; `debug start-process PID` releases it. The sysmodule's raw wait command returns
immediately with `active=1`.

`trace-ipc` scans trace snapshots for HIPC envelopes containing CMIF `SFCI` or `SFCO`
headers. It reports the target/thread/sequence, command ID for requests, Result for responses,
request token, response InterfaceId, domain metadata, handle values and payload. When the trace
metadata contains profile pairs, matching request/response records are also emitted as
transactions. If several requests are outstanding on the same thread/pair, the current
thread-FIFO correlation is explicitly marked ambiguous instead of being presented as proof.
A pair's `ipc.correlation` can instead extract a request/response key from event fields,
CMIF header/domain fields, IPC payload bytes or captured snapshot bytes; `thread: false` allows
cross-thread completion and `fallback` controls whether unmatched keys may degrade to thread
FIFO.
Client 1.1.1 renames the old response `token`/`responseToken` fields to
`interfaceId`/`responseInterfaceId`; firmware 14.0.0+ defines that field as an InterfaceId.
`ipc-decode` applies the same parser to one raw hex message. `trace-ipc --profile` can supply
target IDs and consumer-defined service/interface/command labels for manually dumped traces.
CMIF domain object IDs are summarized with their observed time range, request/response totals,
explicit convert-response `createCount` and domain-close `closeCount`.

`trace-timeline` merges multiple sessions from the same boot into one monotonic timeline,
including decoded IPC messages, transactions and domain create/close events. New exports carry
`bootEpochUnixMs`; mixing files from different boots is rejected, while two seconds of
same-boot epoch-estimation drift is tolerated. Use
`--profile FILE` for plain `multitrace-dump` inputs that do not embed target IDs or IPC pairs.

## Controller

```bash
python3 client/sysagent.py --host switch controller status
python3 client/sysagent.py --host switch controller dump
```

`controller status` prints one line of virtual-controller state: `initialised`, HDLS
handle/session, the `attached` flag, device type, npad interface, `idleRelease`, `takeover`, the
slot currently held in the hid:dbg assignment table, the last `hiddbgSetHdlsState` error, and the
hidsys owner of player 1 (read-only: it reports `initialised=0` instead of creating a device).
`controller dump` prints the full diagnostic (every controller hid:dbg knows about, the raw
`HdlsNpadAssignment` table, and the hidsys player-slot owners plus the controller layout
signature, pad count, interface and controller number) as a multi-line block terminated by
`END controllerDump`.

Recovery aids for input problems:

```bash
python3 client/sysagent.py --host switch controller paired
python3 client/sysagent.py --host switch controller reconnect 5000 98:41:5C:65:A6:FD
python3 client/sysagent.py --host switch controller kick 0
```

`controller paired` lists the console's paired Bluetooth devices, which separates "the pairing is
gone" from "the console is not connecting it". `controller reconnect [ms] [addr]` triggers
`btdrvTriggerConnection` for the controllers a takeover disconnected, and an explicit address is
added to that list rather than replacing it; a Bluetooth controller that the console
disconnected never pages back on its own - not
even with SYNC - so this is the only way to pull it back without re-seating it on the rail.
`controller kick <0-7>` performs the takeover's first step manually.

The virtual controller rebuilds after controller topology changes, takes player 1 over
via a takeover (`configure controllerTakeover`, default 1: disconnect the holding controller,
rebuild and retry - no synthetic button press) when the player's own controller holds it, releases
the slot `configure controllerIdleRelease` seconds (default 1; `controllerIdleReleaseMs` for
sub-second values, which is the wait in front of the reconnect) after use, and pulls the
disconnected controller back automatically (the adapter pages one link at a time, so the second
controller of a pair is retried at the main loop rate until the first link has established, which
measured 0.5-1.3 s); with nothing to yield to it simply stays attached,
and a state write that stays refused is reported once as `ERR controllerState …`.
Mechanism, evidence and the remaining system limits are in
`docs/hdls-virtual-controller-notes.md`.

## Audio

```bash
python3 client/sysagent.py --host switch audio volume
python3 client/sysagent.py --host switch audio volume 30
python3 client/sysagent.py --host switch audio mute
python3 client/sysagent.py --host switch audio mute enabled
```

Volume is reported and set as an integer `0..100`; the server maps it to the audctl
`0.0..1.0` float range and the master-volume API requires firmware 4.0.0+. Mute applies to the
current (or default) audio output target. Volume and mute changes are immediate system-wide
side effects with no confirmation or undo; query first if you need to restore the previous
state.

## Screen

```bash
python3 client/sysagent.py --host switch screen capture --output screen.jpg
python3 client/sysagent.py --host switch screen capture --output after.jpg --diff before.jpg --json
python3 client/sysagent.py --host switch screen off
python3 client/sysagent.py --host switch screen on
```

Without `--output`, `screen capture` writes `screenshot-<unix timestamp>.jpg` in the current
directory and prints the path. The Switch side exposes this as `screenCapture` (legacy name
`pixelPeek`); the JPEG arrives as a single hex line, which is why the client response buffer
allows up to 4 MiB. With `--diff BASELINE`, the capture is compared against the baseline and
the changed-pixel count, ratio, and bounding box are printed (`--json` for structured output).
This is the only place the client needs an optional dependency: the comparison uses Pillow
(`pip install pillow`), and the command fails with a clear message if it is missing.

## Memory

```bash
python3 client/sysagent.py --host switch memory peek 0x100 0x10
python3 client/sysagent.py --host switch memory peek-absolute 0x45075880 0x10
python3 client/sysagent.py --host switch memory peek-absolute-verified 0x45075880 0x10
python3 client/sysagent.py --host switch memory peek-multi 0x100 0x4 0x200 0x4
python3 client/sysagent.py --host switch memory query 0x45075880
python3 client/sysagent.py --host switch memory hash 0x45075880 0x100
python3 client/sysagent.py --host switch memory dump --start 0x45075880 --size 0x400000 --output region.bin
python3 client/sysagent.py --host switch memory wait-value 0x45075880 4 DEADBEEF --wait-timeout 30
python3 client/sysagent.py --host switch memory poke 0x100 DEADBEEF
python3 client/sysagent.py --host switch memory pointer 0x45097552 0x10
python3 client/sysagent.py --host switch memory pointer-all 0x45097552 0x10 0x4
python3 client/sysagent.py --host switch memory pointer-peek 0x10 0x45097552 0x10 0x4
python3 client/sysagent.py --host switch memory pointer-peek-multi 0x4 0xAAA 0x10 0x20 * 0x4 0xBBB 0x10
python3 client/sysagent.py --host switch memory pointer-poke DEADBEEF 0x45097552 0x10 0x4
```

Memory subcommands are `peek`, `peek-absolute`, `peek-main`, `peek-multi`,
`peek-absolute-multi`, `peek-main-multi`, `peek-verified`, `peek-absolute-verified`,
`peek-main-verified`, `query`, `wait-value`, `poke`, `poke-absolute`, `poke-main`,
`pointer`, `pointer-all`, `pointer-relative`, `pointer-peek`, `pointer-peek-multi`, and
`pointer-poke`, `hash`, and `dump`. Addresses and sizes accept decimal or `0x`; data is hex.

`hash` returns an FNV-1a 32 fingerprint of an absolute region, so "which of these objects
changed" costs one short reply per object instead of pulling every byte back. `dump` reads an
absolute region to a local file in verified chunks (1 MiB by default, `--chunk` to change),
so regions larger than one protocol response can be captured. `query`, `hash`, and the
verified reads accept `--json` and `--out FILE`; `game status` and `system query` accept them
too.

`query` reports the mapping covering an address (`base`/`size`/`typeName`/`permName`), so an
unmapped or read-only address can be told apart from a transient failure before reading or
writing it. The `peek-*-verified` reads read the range twice and retry until two consecutive
reads agree (default 3 attempts, `--attempts` up to 16), which separates "the value is zero"
from "the read failed". `wait-value` polls a verified read until the bytes match.

Read-only commands (`peek*`, `pointer*`, `query`, `freeze count`, the verified reads) retry
automatically on an empty response or a dropped connection, rebuilding the socket between
attempts; write commands never retry, because a lost reply can mean the write already landed.
Two retries is the default (`--retries N` to change it, `--retries 0` to disable); the
reconnect backoff is 50 ms then 100 ms.

`memory poke`, `memory poke-absolute`, and `memory poke-main` verify by default: the
sysmodule writes, reads the range back, and compares before answering, so a silent write
failure is reported as `WRITE_FAILED`, a write that landed but whose verification read
failed as `READBACK_FAILED`, and a write whose bytes did not read back as sent as
`WRITE_VERIFY_FAILED`, instead of passing unnoticed. Pass `--no-verify` to fall back to the
legacy blind `poke*` command, which is only useful for a target that rewrites its own memory
fast enough to make the read-back mismatch. The client also translates common native Result
codes and `ERR code=` names into a readable suffix while keeping the raw code.

## Freeze

```bash
python3 client/sysagent.py --host switch freeze add 0x45097552 00C8
python3 client/sysagent.py --host switch freeze remove 0x45097552
python3 client/sysagent.py --host switch freeze count
python3 client/sysagent.py --host switch freeze clear
python3 client/sysagent.py --host switch freeze pause
python3 client/sysagent.py --host switch freeze resume
```

## Input

```bash
python3 client/sysagent.py --host switch input press A
python3 client/sysagent.py --host switch input set-stick LEFT 0x7FFF 0
python3 client/sysagent.py --host switch input click-seq A,W1000,B
python3 client/sysagent.py --host switch input click-seq A,W1000,B --no-wait
python3 client/sysagent.py --host switch input click-seq "PLUS,W500,(DDOWN,W100)*10,A,W200"
python3 client/sysagent.py --host switch input capabilities
python3 client/sysagent.py --host switch input touch 200 500 200 800
python3 client/sysagent.py --host switch input touch-hold 200 500 1000
python3 client/sysagent.py --host switch input touch-draw 100 200 100 500 200 500
python3 client/sysagent.py --host switch input key 11 8 15 15 18
python3 client/sysagent.py --host switch input key-mod 4 1
python3 client/sysagent.py --host switch input text "Anno 1800"
python3 client/sysagent.py --host switch input text "https://example.com/%e5%8f%8c.nsz"
```

Input subcommands are `press`, `release`, `click`, `set-stick`, `click-seq`, `capabilities`
(the click-seq syntax the console supports), `click-cancel`, `detach-controller`, `touch`,
`touch-hold`, `touch-draw`, `touch-cancel`, `key`, `key-mod`, `key-multi`, and `text`. `click-seq` blocks
until the server reports `done` unless `--no-wait` is given, and prints what the console expanded
(`done tokens=25 estMs=1400`); `touch`, `touch-hold`, and `touch-draw` expect `x y` coordinate
pairs.

`input text` types a string: the client maps the characters onto the HID keycodes the console's
software keyboard turns back into text (unshifted US layout, with `keyMulti` for the shifted
characters -- the autopilot's own modifier byte is ignored by that keyboard, so `keyMod` is not
what it uses), and waits for each batch of characters to be typed before sending the next command.
Only printable ASCII can be typed this way; anything else is refused before the first key goes out,
because the console's own input method is not reachable through HID reports. While typing, the
client raises the console's `keySleepTime` to at least 50 ms and restores the previous value
afterwards: the 25 ms default is fine for the home menu but drops characters in a game's chat
field (ACNH 3.0.3 measured 25/30 ms dropping keys, 35 ms occasionally, 40 ms clean).

`input press|release|click|click-seq -h` prints the full table of accepted button names
(`A`, `B`, `X`, `Y`, `L`, `R`, `ZL`, `ZR`, `PLUS`, `MINUS`, `DUP`/`DU`, `DDOWN`/`DD`,
`DLEFT`/`DL`, `DRIGHT`/`DR`, `LSTICK`, `RSTICK`, `HOME`, `CAPTURE`, `PALMA`, `UNUSED`);
names are case-sensitive, and an unknown name sends no button rather than a default key.

Sequences repeat with `item*N` (`DDOWN*3`) or `(items)*N` (`(DDOWN,W100)*10`): the count has to
be 1-1000, groups nest up to 4 deep and one sequence may expand to 1024 tokens (64 KiB of text).
A `W` wait inside a group repeats with it, so `(DDOWN,W100)*10` is ten presses with 100 ms between
them. The console expands the sequence before it runs anything and reports a sequence it cannot
expand as `ERR code=BAD_SEQUENCE index=<offset> detail=<reason>`; the client raises that as a
`SysAgentProtocolError` instead of letting a mangled sequence answer `done` while sending no
input. Repeats need the expander (sys-agent 2.12.0 or newer) and a sequence must not contain
whitespace - the client refuses that up front, and it checks the console's `clickSeqCapabilities`
before sending repeat syntax to an older build. The client also extends its own read timeout to
the sequence's `estMs`, so a long repeat does not look like a socket timeout.

A `click-seq` that is still running answers on the connection the console used last, so a
sequence that another command interrupts (for example `input click-cancel` from a second
connection) may not deliver `done` to the caller waiting on it; that caller then reports a
timeout while the console has stopped as asked. Start sequences that are meant to be interrupted
with `--no-wait`, so nothing is left waiting.

## Utility and config

```bash
python3 client/sysagent.py --host switch utility version
python3 client/sysagent.py --host switch utility heap-base
python3 client/sysagent.py --host switch utility main-nso-base
python3 client/sysagent.py --host switch utility title-id
python3 client/sysagent.py --host switch utility is-program-running 0x01006F8002326000
python3 client/sysagent.py --host switch utility charge
python3 client/sysagent.py --host switch config set freezeRate 10
python3 client/sysagent.py --host switch config get controllerIdleReleaseMs
python3 client/sysagent.py --host switch config list
```

Utility subcommands are `version`, `title-id`, `title-version`, `system-language`, `build-id`,
`heap-base`, `main-nso-base`, `is-program-running`, `charge`, and `fd-count`. `utility version` is
the console's version on its own; the top-level `version` above prints it next to the client's.

`config set` validates the known parameters and changes one; `config get <parameter>` and
`config list` read the values back from the sysmodule, so there is no need to remember what a
setting currently is. `controllerIdleRelease` is a write-only alias in seconds: reading it
reports `controllerIdleReleaseMs`, which is the value the sysmodule stores.
Parameters: `mainLoopSleepTime`, `buttonClickSleepTime`, `echoCommands`,
`printDebugResultCodes`, `keySleepTime`, `fingerDiameter`, `pollRate`, `freezeRate`,
`controllerType`, `controllerIdleRelease`, `controllerIdleReleaseMs`, `controllerTakeover`.

## Search

Exact searches:

```bash
python3 client/sysagent.py --host switch search capabilities
python3 client/sysagent.py --host switch search run 0x80000000 0x80010000 DEADBEEF
python3 client/sysagent.py --host switch search start 0x80000000 0x80010000 DEADBEEF
python3 client/sysagent.py --host switch search status 1
python3 client/sysagent.py --host switch search results 1 --offset 0 --count 256
python3 client/sysagent.py --host switch search cancel 1
python3 client/sysagent.py --host switch search close 1
```

`search run` starts, polls, prints all matching addresses, and closes the session. The
lower-level `search start` / `status` / `results` / `cancel` / `close` steps can be used
individually; Control-C during `search run` sends `searchCancel`.

Typed and region searches:

```bash
python3 client/sysagent.py --host switch search start-region u32 heap 0 0x1000000 0x12345678 --alignment 4
python3 client/sysagent.py --host switch search start-region bytes main 0 0x100000 DEADBEEF
```

`absolute` treats the offset as the absolute start address; integer types accept decimal or
`0x`; defaults are natural alignment for integers and one-byte alignment for byte patterns.

Unknown-value searches:

```bash
python3 client/sysagent.py --host switch search begin u32 heap 0 0x100000 --alignment 4 --pause
python3 client/sysagent.py --host switch search refine 9223372036854775809 changed
```

Refine modes are `exact`, `changed`, `unchanged`, `increased`, and `decreased`; exact mode
requires a value. The same flow is available from Python:

```python
with SysAgentClient("switch") as client:
    session = client.begin_unknown("u32", "heap", 0, 0x100000, pause=False)
    client.wait(session)

    # Change the value in the game, then keep only changed candidates.
    client.refine(session, "changed")
    status = client.wait(session)
    print(status.generation, status.candidates, status.disk_bytes)

    for address in client.iter_results(session):
        print(f"{address:016X}")
    client.close_session(session)
```

## Raw passthrough

```bash
python3 client/sysagent.py --host switch raw getVersion
```

`raw` sends any single-line command and prints the raw response, so future or FTP commands can
still be driven manually.

## Conventions

Numeric arguments accept `0x`-prefixed hex, decimal, or bare hex, so `10`, `0x10`, and `1A`
all parse (`0x10` is sixteen); hex byte payloads are always even-length byte pairs with an
optional `0x` prefix, never decimal. The parser rejects an odd length or a non-hex character
instead of guessing.

The client keeps one TCP connection to port 6000 and sends one command at a time; the connection
opens with the first command, not when the client object is created, so a command that can answer
without the console still does (`version` prints its client line even with the host down). The
sysmodule accepts several simultaneous connections, but each is served serially, so a long command
on one connection delays another. Read-only commands transparently reconnect after an empty
response (see the retry note above); command output is a single line of `key=value` fields unless
`--json` is given.

The Python API keeps the raw `dict[str, str]` returns and adds typed results where they help:
`memory_info()` returns a `MemoryInfo` dataclass, `game_status()` returns a `GameStatus`
(original fields preserved under `.raw`), and `SearchStatus`/`BackendStatus` are unchanged.

## Safety

This client is a power tool: memory writes (`memory poke*`, `memory pointer-poke`),
`freeze add`, `screen off`, `config set`, input commands, and system actions execute
immediately without confirmation. Port 6000 is unauthenticated, and `system query
network-profile` exposes the Wi-Fi passphrase. Use only on a trusted isolated network and back
up memory values before writing.
