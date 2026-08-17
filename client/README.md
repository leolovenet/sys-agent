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
python3 client/sysagent.py --host switch game launch-headless 0x01006F8002326000
python3 client/sysagent.py --host switch game terminate
python3 client/sysagent.py --host switch game name
python3 client/sysagent.py --host switch game version
python3 client/sysagent.py --host switch game icon --output icon.bin
```

Game subcommands are `status`, `launch-headless`, `terminate`, `name`, `author`, `rating`,
`version`, and `icon`. `status` reports the running application identity, version, memory
bases, Build ID, and name; `icon` writes a binary icon file with `--output` (default
`game-icon-<unix time>.bin`). `launch-headless` starts the game process without showing it on
screen (foreground launch requires the home-menu/applet flow, which a sysmodule cannot drive);
`terminate` is the system-level final termination (the same forced path the HOME-menu close
flow falls back to after its graceful-close timeout), because the graceful `RequestExit` API is
only reachable by system applets. `launch-headless` and `terminate` take effect immediately
with no confirmation. Numeric arguments (including Title IDs) accept `0x`-prefixed hex, bare
hex, or decimal.

`game launch-headless` automatically tries SD card, built-in user storage, game card, then the
generic `None` storage, and reports which one succeeded in the `storage=` response field.

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
python3 client/sysagent.py --host switch screen off
python3 client/sysagent.py --host switch screen on
```

Without `--output`, `screen capture` writes `screenshot-<unix timestamp>.jpg` in the current
directory and prints the path. The Switch side exposes this as `screenCapture` (legacy name
`pixelPeek`); the JPEG arrives as a single hex line, which is why the client response buffer
allows up to 4 MiB.

## Memory

```bash
python3 client/sysagent.py --host switch memory peek 0x100 0x10
python3 client/sysagent.py --host switch memory peek-absolute 0x45075880 0x10
python3 client/sysagent.py --host switch memory peek-multi 0x100 0x4 0x200 0x4
python3 client/sysagent.py --host switch memory poke 0x100 DEADBEEF
python3 client/sysagent.py --host switch memory pointer 0x45097552 0x10
python3 client/sysagent.py --host switch memory pointer-all 0x45097552 0x10 0x4
python3 client/sysagent.py --host switch memory pointer-peek 0x10 0x45097552 0x10 0x4
python3 client/sysagent.py --host switch memory pointer-peek-multi 0x4 0xAAA 0x10 0x20 * 0x4 0xBBB 0x10
python3 client/sysagent.py --host switch memory pointer-poke DEADBEEF 0x45097552 0x10 0x4
```

Memory subcommands are `peek`, `peek-absolute`, `peek-main`, `peek-multi`,
`peek-absolute-multi`, `peek-main-multi`, `poke`, `poke-absolute`, `poke-main`, `pointer`,
`pointer-all`, `pointer-relative`, `pointer-peek`, `pointer-peek-multi`, and `pointer-poke`.
Addresses and sizes accept decimal or `0x`; data is hex.

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

## Utility and config

```bash
python3 client/sysagent.py --host switch utility version
python3 client/sysagent.py --host switch utility heap-base
python3 client/sysagent.py --host switch utility main-nso-base
python3 client/sysagent.py --host switch utility title-id
python3 client/sysagent.py --host switch utility is-program-running 0x01006F8002326000
python3 client/sysagent.py --host switch utility charge
python3 client/sysagent.py --host switch config set freezeRate 10
```

Utility subcommands are `version`, `title-id`, `title-version`, `system-language`, `build-id`,
`heap-base`, `main-nso-base`, `is-program-running`, `charge`, and `fd-count`. `config set`
validates the known parameters
(`mainLoopSleepTime`, `buttonClickSleepTime`, `echoCommands`, `printDebugResultCodes`,
`keySleepTime`, `fingerDiameter`, `pollRate`, `freezeRate`, `controllerType`).

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

## Safety

This client is a power tool: memory writes (`memory poke*`, `memory pointer-poke`),
`freeze add`, `screen off`, `config set`, input commands, and system actions execute
immediately without confirmation. Port 6000 is unauthenticated, and `system query
network-profile` exposes the Wi-Fi passphrase. Use only on a trusted isolated network and back
up memory values before writing.
