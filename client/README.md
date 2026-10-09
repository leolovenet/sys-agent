# sys-agent client

Dependency-free Python client for the sys-agent Switch automation protocol. It mirrors every
non-FTP sys-agent command behind grouped subcommands; FTP is intentionally not built in (use
`curl ftp://switch:6001/` for SD-card file access).

## Commands

```
backend   Inspect or configure the process-memory backend
system    Query or control system state
audio     Query or control system audio
game      Launch, close, or inspect the running game
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
python3 client/sysagent.py --host switch system query network
python3 client/sysagent.py --host switch system process-list --offset 0 --count 64
python3 client/sysagent.py --host switch system wireless enabled
python3 client/sysagent.py --host switch system lock-screen status
python3 client/sysagent.py --host switch system action reboot
python3 client/sysagent.py --host switch system action reboot-emummc
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
python3 client/sysagent.py --host switch input touch 200 500 200 800
python3 client/sysagent.py --host switch input touch-hold 200 500 1000
python3 client/sysagent.py --host switch input touch-draw 100 200 100 500 200 500
python3 client/sysagent.py --host switch input key 11 8 15 15 18
python3 client/sysagent.py --host switch input key-mod 4 1
```

Input subcommands are `press`, `release`, `click`, `set-stick`, `click-seq`, `click-cancel`,
`detach-controller`, `touch`, `touch-hold`, `touch-draw`, `touch-cancel`, `key`, `key-mod`, and
`key-multi`. `click-seq` blocks until the server reports `done` unless `--no-wait` is given;
`touch`, `touch-hold`, and `touch-draw` expect `x y` coordinate pairs.
`input press|release|click|click-seq -h` prints the full table of accepted button names
(`A`, `B`, `X`, `Y`, `L`, `R`, `ZL`, `ZR`, `PLUS`, `MINUS`, `DUP`/`DU`, `DDOWN`/`DD`,
`DLEFT`/`DL`, `DRIGHT`/`DR`, `LSTICK`, `RSTICK`, `HOME`, `CAPTURE`, `PALMA`, `UNUSED`);
names are case-sensitive, and an unknown name sends no button rather than a default key.

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
`heap-base`, `main-nso-base`, `is-program-running`, `charge`, and `fd-count`.

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

The client keeps one TCP connection to port 6000 and sends one command at a time; the sysmodule
accepts several simultaneous connections, but each is served serially, so a long command on one
connection delays another. Read-only commands transparently reconnect after an empty response
(see the retry note above); command output is a single line of `key=value` fields unless
`--json` is given.

## Safety

This client is a power tool: memory writes (`memory poke*`, `memory pointer-poke`),
`freeze add`, `screen off`, `config set`, input commands, and system actions execute
immediately without confirmation. Port 6000 is unauthenticated, and `system query
network-profile` exposes the Wi-Fi passphrase. Use only on a trusted isolated network and back
up memory values before writing.
