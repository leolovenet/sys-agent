# Available Commands

## System management

The system-management protocol is additive and uses single-line `OK key=value` or
`ERR code=...` responses. It does not change any legacy command response.

> **Security warning:** port 6000 has no authentication or encryption. `networkProfile`
> deliberately exposes the current Wi-Fi passphrase, and `systemInfo` and `accountStatus`
> expose device and account identifiers. Any host able to connect to port 6000 can read this
> data and invoke the state-changing commands below. Use only on a trusted isolated network
> and restrict port 6000 with the surrounding network firewall.

|Command|Description|Parameters|Usage|
|--|--|--|--|
|systemCapabilities|Lists protocol version, query groups, actions, limits, and sensitive-data policy|none|`systemCapabilities`|
|systemInfo|Reports firmware, hardware model, region, language, device nickname, and serial number|none|`systemInfo`|
|systemTime|Reports Unix time, timezone, and uptime|none|`systemTime`|
|powerStatus|Reports battery, charger, voltage, health, temperature, and current-limit fields|none|`powerStatus`|
|storageStatus|Reports SD mount state and total/free/used bytes|none|`storageStatus`|
|networkStatus|Reports link state, signal strength, and current IPv4 configuration|none|`networkStatus`|
|networkProfile|Reports the current profile, SSID, **Wi-Fi passphrase**, IPv4/DNS settings, and MTU|none|`networkProfile`|
|accountStatus|Reports the last-opened account UID and nickname|none|`accountStatus`|
|applicationStatus|Reports the current application identity, version, memory bases, Build ID, and name|none|`applicationStatus`|
|processList|Returns a PID/Title-ID page; count is 1 through 64|offset, count|`processList 0 64`|
|systemReboot|Requests a normal Atmosphere/Horizon reboot|none|`systemReboot`|
|systemRebootEmuMMC|One-shot reboot directly to the Hekate entry with `id=Atm-Emu`; does not change global autoboot|none|`systemRebootEmuMMC`|
|systemShutdown|Requests an orderly shutdown|none|`systemShutdown`|
|systemSleep|Invokes the experimental system-sleep SVC|none|`systemSleep`|
|networkSet|Enables or disables wireless communication|`enabled` or `disabled`|`networkSet disabled`|
|lockScreenStatus|Reports Horizon's persistent sleep-mode lock-screen flag|none|`lockScreenStatus`|
|lockScreenSet|Enables or disables Horizon's persistent sleep-mode lock-screen flag|`enabled` or `disabled`|`lockScreenSet disabled`|
|applicationTerminate|Terminates only the current foreground application|none|`applicationTerminate`|

Arbitrary strings are returned as uppercase hexadecimal bytes with an adjacent `*Len` field.
Grouped queries return `NA` for unavailable optional fields and append native Result values in
`errors`. Power actions, sleep, wireless disable, and application termination can interrupt
searches, FTP transfers, open files, and the command connection. A successful response may not
reach the client before the requested state change takes effect. Clients must never retry these
non-idempotent commands automatically. `systemSleep` remains experimental until real-device
acceptance succeeds and is advertised as such by `systemCapabilities`.

`systemRebootEmuMMC` reads `sdmc:/bootloader/hekate_ipl.ini` on every invocation and requires
exactly one main-file boot entry with `id=Atm-Emu`. It refuses to reboot if the ID is missing,
duplicated, or outside Hekate's RTC index range. On Erista it validates and patches
`bootloader/update.bin` in memory with Hekate's one-shot Boot-from-ID fields; on Mariko it
writes the corresponding one-shot RTC autoboot reason before requesting reboot. It never
rewrites `hekate_ipl.ini` and never falls back to a guessed entry.
The Mariko path passed real-device acceptance on the current `model=3` console and booted the
`Atm-Emu` entry directly. The Erista payload-copy branch remains experimental.

## SD card FTP server

This custom build runs a low-priority FTP server independently of the original command socket.
It listens on port `6001` by default and maps FTP `/` directly to the SD card root. BIS, save
data, gamecard, and other system mounts are not exposed.

One event-driven FTP worker supports up to four simultaneous client sessions. It polls their
commands and transfers cooperatively instead of creating one thread per client, so concurrent
large transfers are interleaved and share network and SD-card throughput.

|Command|Description|Parameters|Usage|
|--|--|--|--|
|ftpStatus|Reports lifecycle state, effective configuration, active file transfers, byte counters, lifecycle error, and the latest native FS Result|none|`ftpStatus`|
|ftpStart|Asynchronously starts the server using the loaded configuration|none|`ftpStart`|
|ftpStop|Asynchronously closes the listener, clients, transfers, and open FTP files|none|`ftpStop`|
|ftpRestart|Restarts the server without rereading the configuration file|none|`ftpRestart`|
|ftpReload|Stops the server, rereads `/config/sys-agent/ftp.ini`, and applies `enabled`|none|`ftpReload`|

The configuration defaults to `enabled=1`, `port=6001`, `anonymous=1`, and `timeout=30` when
the file is absent. If anonymous access is disabled, both `username` and `password` are
required. Commands are additive and never emit unsolicited FTP events on a sys-agent client.

FTP grants complete SD read/write/create/rename/delete access. During a queued or running
C-level search, mutations within `/switch/sys-agent/search` return an FTP error; reads and
all unrelated paths remain available.

The tested stable filename set is ASCII, including spaces. Horizon's SD filesystem returned
native Result `0x202` for Chinese and Japanese filenames and enumerated other non-ASCII names
inconsistently. sys-agent reports the latest native error as `lastFsResult` and does not
silently rename a requested path.

## Process memory backend

All memory commands, pointer traversal, freezes, and asynchronous searches use one shared
process-memory backend. The default `auto` policy prefers Atmosphere `dmnt:cht`, asks dmnt to
open the current application when necessary, and falls back to direct debug only before dmnt
ownership has been observed. Once dmnt owns the debug handle, an IPC error is returned instead
of attempting a conflicting second `svcDebugActiveProcess`.

|Command|Description|Parameters|Usage|
|--|--|--|--|
|memoryBackend|Reports the configured policy, last selected backend, dmnt state, process identity, and last open error|optional `auto`, `dmnt`, or `direct` policy|`memoryBackend`<br>`memoryBackend dmnt`|
|memoryBackendProbe|Opens and closes the configured backend once, then reports its resolved state|none|`memoryBackendProbe`|

Policies are runtime-only and reset to `auto` when the sysmodule restarts. `dmnt` never falls
back to direct; `direct` never contacts dmnt. Policy changes are rejected with `BUSY` while a
search is queued or running. sys-agent never force-closes dmnt's cheat process.

Turning off individual cheat entries does not disable `dmnt:cht`. In auto mode sys-agent may
still ask dmnt to attach, and Atmosphere retains ownership of that shared debug handle. In
direct mode, sys-agent owns the handle only for the current command, freeze cycle, mapping
query, or search read and closes it immediately afterward.

## Asynchronous exact memory search

The search extension is additive: existing commands and responses are unchanged. Legacy search
ranges use absolute addresses and an exclusive end address. Region searches accept an offset
and size relative to `absolute`, `heap`, or `main`. Integer values are encoded in Switch-native
little-endian byte order. Only one search may be queued or running at a time.

The worker scans readable process mappings in 256 KiB chunks, preserves cross-chunk matches,
and yields between chunks. It stores the first 65,536 matching addresses, continues counting
after that limit, and reports `truncated=1`. A result page is limited to 256 addresses.

|Command|Description|Parameters|Usage|
|--|--|--|--|
|searchCapabilities|Reports protocol limits and supported modes|none|`searchCapabilities`|
|searchStart|Starts an asynchronous exact byte search|1. absolute start address<br>2. exclusive absolute end address<br>3. even-length hex byte pattern|`searchStart 0x80000000 0x88000000 DEADBEEF`|
|searchExact|Alias for `searchStart`|same as `searchStart`|`searchExact 0x80000000 0x88000000 0xDEADBEEF`|
|searchStartRegion|Starts a byte or typed region search|1. `bytes`, `u8`, `u16`, `u32`, or `u64`<br>2. `absolute`, `heap`, or `main`<br>3. region-relative offset<br>4. size in bytes<br>5. byte pattern or unsigned integer value<br>6. optional alignment|`searchStartRegion u32 heap 0x0 0x100000 0x12345678 4`|
|searchStatus|Reports state, progress, matches, truncation and read errors|session ID|`searchStatus 1`|
|searchResults|Returns a page of stored absolute addresses|1. session ID<br>2. zero-based result offset<br>3. count, capped at 256|`searchResults 1 0 100`|
|searchCancel|Requests cancellation; the worker observes it no later than the current chunk boundary|session ID|`searchCancel 1`|
|searchClose|Releases a completed, cancelled or failed session|session ID|`searchClose 1`|

Search commands return one newline-terminated `OK ...` or `ERR code=...` response. Progress is
polled with `searchStatus`; the worker never writes unsolicited data to a client socket.
The dependency-free macOS client and examples are in `client/sysagent.py` and
`client/README.md`. Deployment constraints and the concurrency/memory audit are recorded in
`docs/search-a-level-design.md`.

For typed searches, alignment defaults to the integer width. Byte searches default to one-byte
alignment. An explicit alignment must be a power of two from 1 through 256 and is applied to
the absolute candidate address. `searchStatus` always reports resolved absolute `start`/`end`
and additionally reports `type`, `region`, `base`, `regionOffset`, and `alignment`. Region bases
are captured when the session starts; a game restart still terminates the session through the
existing process-ID check.
Each search captures one process-memory backend at start and reports it as `backend=dmnt` or
`backend=direct`; it never changes backend in the middle of a session.

## Unknown-value and multi-pass search

Protocol version 3 adds one SD-backed unknown-value session. It supports unsigned
`u8`/`u16`/`u32`/`u64` values over explicit `absolute`, `heap`, `main`, `alias`, or
`addressSpace` ranges. The default alignment is the value width. The optional `pause` argument
is `0` or `1` and defaults to `0`.

|Command|Description|Parameters|Usage|
|--|--|--|--|
|searchBegin|Captures all aligned readable values as generation 0|type, region, offset, size, optional alignment, optional pause|`searchBegin u32 heap 0 0x100000 4 0`|
|searchRefineExact|Keeps candidates whose current value equals the supplied value|session, value, optional pause|`searchRefineExact 0x8000000000000001 100 0`|
|searchRefineChanged|Keeps candidates different from the previous generation|session, optional pause|`searchRefineChanged 0x8000000000000001 0`|
|searchRefineUnchanged|Keeps candidates equal to the previous generation|session, optional pause|`searchRefineUnchanged 0x8000000000000001 0`|
|searchRefineIncreased|Keeps candidates greater than the previous generation|session, optional pause|`searchRefineIncreased 0x8000000000000001 0`|
|searchRefineDecreased|Keeps candidates less than the previous generation|session, optional pause|`searchRefineDecreased 0x8000000000000001 0`|

Every successful refine stores current values as the next baseline. `searchResults` pages all
committed candidates from disk and has no 65,536-result cap for this session type. It returns
`BUSY` while a generation is being written. Cancellation or a runtime error discards the
temporary generation and preserves the last committed generation for retry.

The session pins PID, Title ID, Build ID, and memory backend until `searchClose`. Backend policy
changes return `BUSY` during that lifetime. Status appends `kind`, `generation`, `candidates`,
`operation`, `diskBytes`, `pause`, `committed`, `resumable`, and `failure`. Temporary files live
only in `/switch/sys-agent/search`; startup and `searchClose` clean that dedicated directory.

Before writing a generation, sys-agent checks that the SD has room for the estimated output
plus a 64 MiB reserve. C-level search can return `SD_UNAVAILABLE`, `INSUFFICIENT_STORAGE`,
`CORRUPT_SESSION`, `PROCESS_CHANGED`, `PAUSE_FAILED`, or `IO_ERROR`. A requested pause never
silently falls back to a live scan, and every successfully paused operation attempts to resume
the game on completion, cancellation, or error.
The sys-agent freeze worker temporarily skips writes during every active C-level operation;
it resumes automatically when that operation completes, is cancelled, or fails.

C-level scans process memory in 256 KiB blocks and yield the sysmodule's only permitted CPU
core for 20 ms between blocks. This keeps controller and TCP commands responsive during long
live scans. Finalizing a large generation still performs a synchronous SD flush: on the tested
128 MiB snapshot, ordinary status requests remained responsive during scanning, while one
request at final commit took about 2.3 seconds. This bounded commit delay is not a stalled
search; wait for the terminal status before retrying or closing the session.

## RAM reading
### Single Read
|Command|Description|Parameters|Usage|
|--|--|--|--|
|peek  |Reads memory at given address relative to heap  |1. address to read from relative to heap in hex<br>2. amount of bytes to read<br>Return: hex string |peek 0x45075880 344   |
|peekAbsolute  |Reads memory at given absolute address  |1. address to read from in hex<br>2. amount of bytes to read<br>Return: hex string |peekAbsolute 0x45075880 344   |
|peekMain  |Reads memory at given address relative to NSOMain |1. address to read from relative to NSOMain in hex<br>2. amount of bytes to read<br>Return: hex string |peekAbsolute 0x45075880 344   |

### Multiple Reads
|Command|Description|Parameters|Usage|
|--|--|--|--|
|peekMulti  |Reads memory at given addresses relative to heap  |1. address to read from relative to heap in hex<br>2. amount of bytes to read<br>...<br>n. address to read from relative to heap in hex<br>n+1. amount of bytes to read <br>Return: hex string |peekMulti 0x45075880 344 0x45097552 344 0x45774450 344  |
|peekAbsoluteMulti  |Reads memory at given absolute addresses  |1. address to read in hex<br>2. amount of bytes to read<br>...<br>n. address to read in hex<br>n+1. amount of bytes to read <br>Return: hex string |peekMulti 0x45075880 344 0x45097552 344 0x45774450 344  |
|peekMainMulti  |Reads memory at given absolute addresses  |1. address to read relative to NSOMain in hex<br>2. amount of bytes to read<br>...<br>n. address to read relative to NSOMain in hex<br>n+1. amount of bytes to read <br>Return: hex string |peekMulti 0x45075880 344 0x45097552 344 0x45774450 344  |

### Pointer Reads
|Command|Description|Parameters|Usage|
|--|--|--|--|
|pointerPeek|Follows a chain of pointers and reads the final value|1. amount of bytes to read<br>2. first jump relative to NSOMain<br>3. offset after following first pointer<br>...<br>n. offset after following previous pointer<br>n+1 Final offset to reach the value to read| pointerPeek 344 0x45097552 0x10 0x20 0x30
|pointerPeekMulti|Follows a chain of pointers and reads the final value, accepts more than one chain separated by *|1. amount of bytes to read<br>2. first jump relative to NSOMain<br>3. offset after following first pointer<br>...<br>n. offset after following previous pointer<br>n+1 Final offset to reach the value to read| pointerPeek 344 0x45097552 0x10 0x20 0x30 * 344 0x62097552 0x10 0x4
|pointer|Follows a chain of pointers and prints the final absolute address|1. first jump relative to NSOMain<br>2. offset after following first pointer<br>...<br>n. offset after following previous pointer (will still jump to this one)| pointer 0x45097552 0x10 0x20
|pointerAll|Follows a chain of pointers and prints the final absolute address, allows adding a final offset without jumping|1. first jump relative to NSOMain<br>2. offset after following first pointer<br>...<br>n. offset after following previous pointer (will **not** jump to this one)| pointerAll 0x45097552 0x10 0x20 0x4
|pointerRelative|Follows a chain of pointers and prints the final address relative to heap, allows adding a final offset without jumping|1. first jump relative to NSOMain<br>2. offset after following first pointer<br>...<br>n. offset after following previous pointer (will **not** jump to this one)| pointerRelative 0x45097552 0x10 0x20 0x4

## RAM Writing
### Single Write
|Command|Description|Parameters|Usage|
|--|--|--|--|
|poke  |Writes bytes to given address relative to heap  |1. address to write to relative to heap in hex<br>2. data in hex to write |poke 0x45075880 0xDEADBEEF   |
|pokeAbsolute  |Writes bytes to given absolute address  |1. absolute address to write to in hex<br>2. data in hex to write |poke 0x45075880 0xDEADBEEF   |
|pokeMain  |Writes bytes to given address relative to NSOMain  |1. address to write to relative to NSOMain in hex<br>2. data in hex to write |poke 0x45075880 0xDEADBEEF   |

### Pointer Write
|Command|Description|Parameters|Usage|
|--|--|--|--|
|pointerPoke  |Writes bytes to address resulting of following a pointer chain  |1. bytes in hex to write <br>2. first jump relative to NSOMain<br>3. offset after following first pointer<br>...<br>n. offset after following previous pointer<br>n+1. offset after following previous pointer (will **not** jump to this one) |pointerPoke 0xDEADBEEF 0x45075880 0x10 0x20 0x30   |

### Freezing of values
|Command|Description|Parameters|Usage|
|--|--|--|--|
|freeze|Freezes a value in RAM (writing every X milliseconds to it to ensure it does not get overwritten by game logic|1. absolute address to freeze<br>2. value to freeze it to in hex|freeze 0x45097552 200|
|unFreeze|Unfreezes a previously frozen value in RAM|1. absolute address to unfreeze|unFreeze 0x45097552|
|freezeCount|Returns number of frozen addresses in RAM|none|freezeCount|
|freezeClear|Unfreezes all values in RAM|none|freezeClear|
|freezePause|Pauses the freezing process, allows for unpause to refreeze|none|freezePause|
|freezeUnpause|Unpauses a previously paused freezing of all values|none|freezeUnpause|


## Controls
### Controller Input
See https://github.com/olliz0r/sys-botbase/blob/master/sys-botbase/source/util.c#L145 for a list of available buttontypes to press.
|Command|Description|Parameters|Usage|
|--|--|--|--|
|press|Presses and holds a button|1. buttonType| press A|
|release|Releases a button from being pressed|1. buttonType| release A|
|click| Holds a button pressed and releases it after a configured period of time<br>default 50ms| 1. buttonType| click A|
|setStick|Sets stick position|1. LEFT/RIGHT<br>2.XVal (-0x8000 is min, 0x7FFF is max)<br>3.YVal| setStick LEFT 0x7FFF 0x0|
|clickSeq|Sends several button inputs and wait commands in sequence|1. single string (no spaces) with comma-separated commands out of the following<br><br>**buttonType** for click<br>**+buttonType** for press<br>**-buttonType** for release<br>**Wnumber** to sleep number ms<br>**%X,Y** move left stick to position X Y<br>**&X,Y** move right stick to position X Y| clickSeq A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0
|clickCancel|Interrupts click sequence|none|clickCancel|
|detachController|Forces the virtual controller to detach, useful in cases where it bugs out|none|detachController|

### Touchscreen Input
|Command|Description|Parameters|Usage|
|--|--|--|--|
|touch|Sequential taps to the touchscreen|1.X in range 0-1280<br>2.Y in range 0-720<br>...<br>n-1. X in range 0-1280<br>n. Y in range 0-720|touch 200 500<br>touch 200 500 200 800|
|touchHold|Single tap to hold<br>Runs in its own thread but will not allow the call again while running|1. X in range 0-1280<br>2. Y in range 0.720<br>3. milliseconds to hold (at least 15)| touchHold 200 500 1000|
|touchDraw|Moves the touch from given position to the next, effectively drawing on the touchscreen<br>Runs in its own thread but will not allow the call again while running|1. X in range 0-1280 starting point<br>2. Y in range 0-720 starting point<br>3. X second point<br>4. Y second point<br>...<br>n-1. X last point<br>n. Y last point|touchDraw 100 200 100 500 200 500 200 200|
|touchCancel|Cancels current touch operation|none|touchCancel|

### Keyboard Input
See https://switchbrew.github.io/libnx/hid_8h.html HidKeyboardKey and HidKeyboardModifier for available keys and modifiers.
|Command|Description|Parameters|Usage|
|--|--|--|--|
|key|Types several keys on the keyboard in sequence|1. HidKeyboardKey1<br>...<br>n. HidKeyboardKeyN|key 11 8 15 15 18|
|keyMod|Types several keys on the keyboard in sequence with modifier keys<br>Do not bitshift the modifiers yourself, sys-agent will do the shifting| 1. HidKeyboardKey1<br>2.HidKeyboardModifier1<br>...<br>n-1. HidKeyboardKeyN<br>n. HidKeyboardModifierN|keyMod 4 1|
|keyMulti|Presses several keys at the same time|1. HidKeyboardKey1<br>...<br>n. HidKeyboardKeyN|keyMulti 224 226 23|


## Screen Control
|Command|Description|Parameters|Usage|
|--|--|--|--|
|screenCapture|Returns a JPEG of the current screen; `pixelPeek` is kept as a legacy alias|none|screenCapture|
|screenOff|Turns the screen off|none|screenOff|
|screenOn|Turns the screen on|none|screenOn|

## Utility

|Command|Description|Parameters|Usage|
|--|--|--|--|
|getTitleID|Returns TitleId of application currently running|none|getTitleID|
|getTitleVersion|Returns Version of Title currently running|none|getTitleVersion|
|getSystemLanguage|Returns Language of the Switch OS|none|getSystemLanguage|
|getBuildID|Returns BuildID of the Application running|none|getBuildID|
|getHeapBase|Returns Memory address of the Heap Base|none|getHeapBase|
|getMainNsoBase|Returns Memory address of the NSOMain|none|getMainNsoBase|
|isProgramRunning|Checks if program with given id is running|1. programID to check| isProgramRunning 0x420000000007e51a|
|game|Returns Metadata about the running game|1. one of the following<br>**icon** IconData<br>**version** Game Version<br>**rating** age rating<br>**author** Author of the game<br>**name** Name of the game|game rating|
|getVersion|Returns the sys-agent version|none|getVersion|
|charge|Returns charge status of the battery|none|charge|

## Configure
The configure command allows setting of some timing values in sys-agent:
|Configure parameter|Description|Parameters|Usage|
|--|--|--|--|
|mainLoopSleepTime|Time the main thread sleeps after every single command<br>default 50ms|1. New time in ms to sleep after every command|configure mainLoopSleepTime 10|
|buttonClickSleepTime|How long a button is held down during the "click" call. This blocks the main loop<br> default:50ms<br>Make sure this isn't lower than the fps on the game or a click might not get recognized by the game|1. New time in ms to hold a button down during click|configure buttonClickSleepTime 40|
|echoCommands|Returns every command back for debugging purposes<br>default 0|1 or 0| configure echoCommands 0|
|printDebugResultCodes|Prints some Resultcodes for debugging purposes<br>default 0|1 or 0|configure printDebugResultCodes 0|
|keySleepTime|How long a key is held down during the "key" call. This does not block the main loop<br>default 25|1. New key press sleep time|configure keySleepTime 40|
|fingerDiameter|Controls the diameter of the virtual touch finger<br>default 50|1. new diameter for touch events|configure fingerDiameter 100|
|pollRate|How long a touch event shall be held down<br>default 17<br>polling is linked to screen refresh rate (system UI) or game framerate. Most cases this is 1/60 or 1/30|1. New poll rate|configure pollRate 34|
|freezeRate|How often frozen values shall be rewritten to RAM<br>default 3ms|1. new freezerate in ms|configure freezeRate 10|
|controllerType|controllerType to use for controller input commands<br>default 3|See HidDeviceType on https://switchbrew.github.io/libnx/hid_8h.html|configure controllerType 12|
 
