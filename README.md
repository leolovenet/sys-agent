# sys-agent

This fork is an independently maintained sys-agent build based on upstream v2.5. It keeps
the original TCP protocol compatible while adding Switch-side memory search and a unified
process-memory backend that can coexist with Atmosphère cheats.

> **Custom-build warning:** this is not an official upstream release. It can read and write
> game-process memory and runs as an Atmosphère sysmodule. Back up the existing
> `atmosphere/contents/43000000000000A6` directory before installing, and use it only on a
> console where you understand the risks.

## Changes in this fork

- A unified `ProcessMemoryBackend` routes legacy `peek`/`poke`, multi, pointer, freeze,
  metadata, and search operations through one selected backend.
- The default `auto` policy prefers Atmosphère's `dmnt:cht` service, sharing dmnt's existing
  debug handle instead of attempting a conflicting second `svcDebugActiveProcess` attachment.
- `dmnt` and `direct` policies are available for diagnostics. Direct-debug handles are scoped
  to an operation and released afterward; this fork never force-closes a dmnt-owned handle.
- Asynchronous exact and typed `u8`/`u16`/`u32`/`u64` memory search supports absolute, main,
  and heap-relative ranges, alignment, progress, cancellation, sessions, and paged results.
- SD-backed unknown-value sessions support exact, changed, unchanged, increased, and decreased
  multi-pass refinement without keeping millions of candidates in the sysmodule heap.
- Search sessions pin their process ID and backend, preventing a running scan from silently
  switching to a different process or debug owner.
- An isolated, low-priority FTP worker exposes the SD card at `ftp://switch:6001` while a game
  remains open. It supports normal file and directory CRUD and can be controlled over the
  existing sys-agent TCP connection.
- Grouped system-management commands report system, power, storage, network, account,
  application, and process state. Additive actions cover normal reboot/shutdown, wireless and
  lock-screen settings, foreground-application termination, and one-shot Hekate emuMMC reboot.
- The original controller, screen-capture, and memory commands remain available. New commands
  are additive so existing clients can continue to ignore capabilities they do not use.

See [commands.md](commands.md), [the memory-backend design](docs/process-memory-backend.md),
[the exact-search design](docs/search-a-level-design.md), and
[the unknown-search design](docs/search-c-level-design.md) for protocol and implementation
details. FTP architecture, validation results, known filename limitations, and remaining
hardware acceptance checks are recorded in [the FTP server notes](docs/ftp-server.md).

A Nintendo Switch (CFW) sysmodule that allows users to remotely control their Switch over a
TCP socket and read or write game memory. It can be used for bots, automation, and controlled
memory research.

**Security warning:** the command socket on port 6000 is unauthenticated. The explicitly
enabled `networkProfile` command returns the current Wi-Fi passphrase, while other commands
return serial and account identifiers or change system state. Expose this port only on a
trusted isolated network and restrict it with the surrounding network firewall.

## Features:
### Remote Control:
- Set controller state
- Simulate buttons press, hold, and release
- Simulate touch screen drawing

### Memory Reading and Writing:
- Read/write x amount bytes of consecutive memory from RAM based on:
    1. Absolute memory address
    2. Address relative to main nso base
    3. Address relative to heap base
- Share Atmosphere dmnt's active debug handle so memory commands and searches can coexist with
  the cheat VM, with an explicit direct-debug fallback.
- Run asynchronous exact and typed memory searches with progress, cancellation, and paged
  results.

### Screen Capture:
- Capture current screen and return as JPG

### SD card FTP

The custom build starts an anonymous FTP server on port `6001` by default. Its root is the SD
card root; it does not expose BIS, save data, gamecard, or other system mounts.

The FTP core runs as one event-driven worker thread and accepts up to four simultaneous client
sessions. Connections and transfers are polled and advanced in turns; they do not receive one
thread each and multiple large transfers do not run in true CPU-parallel fashion. Concurrent
transfers also share Wi-Fi and SD-card bandwidth.

```bash
curl ftp://switch:6001/
printf 'ftpStatus\r\n' | nc -w 3 switch 6000
printf 'ftpStop\r\n' | nc -w 3 switch 6000
printf 'ftpStart\r\n' | nc -w 3 switch 6000
```

Anonymous mode permits uploads, renames, and deletion across the complete SD card. This is
intended for a trusted development network. For account access, copy
[`config/ftp.ini.template`](config/ftp.ini.template) to
`/config/sys-agent/ftp.ini` on the SD card, set `anonymous=0`, provide both credentials, and
run `ftpReload`. Invalid credentials or ports leave FTP in an error state without stopping
the controller or memory service.

ASCII filenames, including spaces, are the supported stable baseline. On the tested Horizon
SD filesystem, Chinese and Japanese names return native FS `0x202`, while some other non-ASCII
names can be opened but are not returned reliably by directory enumeration. The server does
not silently rewrite names; use an ASCII temporary or final filename when transferring such
files.

Do not run old sys-ftpd on port `6001`. Sphaira normally uses port `5000`, so both can coexist,
although two writers changing the same file is unsafe. For Atmosphere deployment, upload to a
temporary filename and rename it only after the transfer completes. During an active C-level
search, FTP modifications below `/switch/sys-agent/search` are rejected to protect the
transactional snapshot.

## Disclaimer:
This project was created for the purpose of development for bot automation. The creators and maintainers of this project are not liable for any damages caused or bans received. Use at your own risk.

## Installation

1. Download the ZIP from this fork's
   [latest release](https://github.com/leolovenet/sys-agent/releases/latest).
2. Power off the Switch and back up the existing directory:
   `atmosphere/contents/43000000000000A6`.
3. Extract the ZIP into the **root of the SD card**. The final files must be:

   ```text
   atmosphere/contents/43000000000000A6/exefs.nsp
   atmosphere/contents/43000000000000A6/flags/boot2.flag
   ```

4. Safely eject the SD card and fully restart the Switch. Restarting only the game is not
   sufficient when replacing a running sysmodule.
5. Verify the service over TCP, for example:

   ```bash
   printf 'getVersion\r\n' | nc -w 3 switch 6000
   printf 'memoryBackendProbe\r\n' | nc -w 3 switch 6000
   printf 'searchCapabilities\r\n' | nc -w 3 switch 6000
   ```

When installed correctly, sys-agent will make the docked Joy-Con HOME button glow during
Switch startup. If this does not happen, check the directory layout and `boot2.flag`.

### Upgrade and rollback

To upgrade, replace the complete `43000000000000A6` directory with the one from the new
release and fully restart the console. To roll back, restore the directory backed up before
installation and restart again. Do not mix `exefs.nsp` from one release with packaging files
from another release.

### Build with Docker

This branch is built with the pinned official devkitPro image:

```bash
docker run --rm \
  --platform linux/amd64 \
  -v "$PWD:/work" \
  -w /work \
  devkitpro/devkita64:20260219 \
  bash -lc 'source /opt/devkitpro/switchvars.sh && make clean && make && make test'
```

Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` before the
first build. The build uses the checked-out pinned FTP source and does not download dependencies.

The build produces `sys-agent/sys-agent.nsp` and refreshes the ignored installation tree
at `sys-agent/43000000000000A6`. The NSP is copied there as `exefs.nsp`.

Unknown-search snapshots are runtime-only temporary data under `/switch/sys-agent/search` on
the SD card. They are removed when the session closes or the sysmodule starts again. Do not
store personal files in that directory.

![](joycon-glow.gif)

## Credits
- Big thank you to [jakibaki](https://github.com/jakibaki/sys-netcheat) for a great sysmodule base to learn and work with, as well as being helpful on the Reswitched discord!
- Thanks to RTNX on discord for bringing to my attention a nasty little bug that would very randomly cause RAM poking to go bad and the switch (sometimes) crashing as a result.
- Thanks to Anubis for stress testing!
- Thanks to the Atmosphere project for documenting and providing the `dmnt:cht` service used by
  the unified process-memory backend.
