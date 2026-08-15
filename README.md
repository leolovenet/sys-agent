# sys-botbase — leolovenet custom build

This fork is an independently maintained sys-botbase build based on upstream v2.5. It keeps
the original TCP protocol compatible while adding Switch-side memory search and a unified
process-memory backend that can coexist with Atmosphère cheats.

> **Custom-build warning:** this is not an official upstream release. It can read and write
> game-process memory and runs as an Atmosphère sysmodule. Back up the existing
> `atmosphere/contents/430000000000000B` directory before installing, and use it only on a
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
- Search sessions pin their process ID and backend, preventing a running scan from silently
  switching to a different process or debug owner.
- The original controller, screen-capture, and memory commands remain available. New commands
  are additive so existing clients can continue to ignore capabilities they do not use.

See [commands.md](commands.md), [the memory-backend design](docs/process-memory-backend.md),
and [the search design](docs/search-a-level-design.md) for protocol and implementation details.

A Nintendo Switch (CFW) sysmodule that allows users to remotely control their Switch over a
TCP socket and read or write game memory. It can be used for bots, automation, and controlled
memory research.

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

## Disclaimer:
This project was created for the purpose of development for bot automation. The creators and maintainers of this project are not liable for any damages caused or bans received. Use at your own risk.

## Installation

1. Download the ZIP from this fork's
   [latest release](https://github.com/leolovenet/sys-botbase/releases/latest).
2. Power off the Switch and back up the existing directory:
   `atmosphere/contents/430000000000000B`.
3. Extract the ZIP into the **root of the SD card**. The final files must be:

   ```text
   atmosphere/contents/430000000000000B/exefs.nsp
   atmosphere/contents/430000000000000B/flags/boot2.flag
   ```

4. Safely eject the SD card and fully restart the Switch. Restarting only the game is not
   sufficient when replacing a running sysmodule.
5. Verify the service over TCP, for example:

   ```bash
   printf 'getVersion\r\n' | nc -w 3 switch 6000
   printf 'memoryBackendProbe\r\n' | nc -w 3 switch 6000
   printf 'searchCapabilities\r\n' | nc -w 3 switch 6000
   ```

When installed correctly, sys-botbase will make the docked Joy-Con HOME button glow during
Switch startup. If this does not happen, check the directory layout and `boot2.flag`.

### Upgrade and rollback

To upgrade, replace the complete `430000000000000B` directory with the one from the new
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

The build produces `sys-botbase/sys-botbase.nsp` and refreshes the ignored installation tree
at `sys-botbase/430000000000000B`. The NSP is copied there as `exefs.nsp`.

![](joycon-glow.gif)

## Credits
- Big thank you to [jakibaki](https://github.com/jakibaki/sys-netcheat) for a great sysmodule base to learn and work with, as well as being helpful on the Reswitched discord!
- Thanks to RTNX on discord for bringing to my attention a nasty little bug that would very randomly cause RAM poking to go bad and the switch (sometimes) crashing as a result.
- Thanks to Anubis for stress testing!
- Thanks to the Atmosphere project for documenting and providing the `dmnt:cht` service used by
  the unified process-memory backend.
