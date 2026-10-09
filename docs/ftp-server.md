# Built-in SD card FTP server

## Status

The custom sys-agent branch contains a working FTP service for full SD-card CRUD without
leaving the running game. The core file operations, lifecycle controls, interruption cleanup,
and coexistence with the original TCP service have passed real-device testing.

One acceptance check remains before describing the FTP work as fully closed: run FTP
concurrently with an active C-level memory search and verify the protection of
`/switch/sys-agent/search`, SD contention, status latency, and cancellation on hardware.
This does not block ordinary FTP use, but it must be completed before making a final
release-quality claim.

Cold-boot startup is observable and retried: the worker keeps `desiredRunning` set and calls
`ftpsrv_init` again one second after a failed bind, so a boot window where the network stack
is not ready yet heals on its own. `ftpStatus` now reports `listener=up/down`,
`bindAttempts`, and `lastBindError`, and a post-reboot check showed `bindAttempts=3` with
`listener=up` -- the retry loop engaging before the listener came up. An earlier report of
"port 6000 answers but 6001 times out" was not reproduced; the post-reboot outage observed
during validation was the whole console dropping off Wi-Fi (both ports unreachable, both
recovering together two minutes later), which is a network-level condition rather than a
listener-level one. The listener-level report therefore stays open for a dedicated
reproduction.

## Architecture

The implementation is deliberately isolated from the original port-6000 command server:

- `sys-agent/source/ftp_server.c` owns lifecycle state, the low-priority worker thread,
  counters, retry behavior, and the five additive control commands.
- `sys-agent/source/ftp_config.c` parses `/config/sys-agent/ftp.ini` and validates the
  port, authentication, and time-zone display settings.
- `sys-agent/source/ftp_path.c` normalizes FTP paths and prevents traversal or access to
  non-SD device namespaces.
- `sys-agent/source/ftp_vfs_sd.c` adapts the FTP core to libnx `FsFileSystem` operations.
  It exposes only the already-mounted SD card, not BIS, save data, gamecard, or USB mounts.
- `third_party/ftpsrv` is a Git submodule pinned to commit
  `7c82402e8f9a53400ea33b82eebd961dfa83a422`. Its source files carry MIT SPDX headers.

The FTP layer reuses sys-agent's existing socket and SD filesystem initialization. It must
not independently call the matching global initialize, mount, unmount, or exit functions.
FTP failures are retained in FTP status and do not terminate the sysmodule or port 6000.

The server uses one event-driven FTP worker and supports up to four simultaneous FTP sessions.
The worker polls each session and advances its commands and transfers cooperatively; it does
not create one thread per client. Multiple clients may remain connected, but concurrent large
transfers are interleaved rather than executing in true CPU-parallel fashion and share Wi-Fi
and SD-card bandwidth. Transfers use 64 KiB buffers, while the SD VFS preallocates growing
uploaded files in 1 MiB increments. Listener or poll initialization failure leaves the desired
state enabled and is retried after one second, covering the boot window where networking is
not ready yet.

## Configuration and control

With no configuration file, FTP is enabled, anonymous, listens on port `6001`, and renders
file modification times in the console's configured time zone (`use_localtime=1`). An example
is available at `config/ftp.ini.template`; the runtime location is:

```text
/config/sys-agent/ftp.ini
```

The port-6000 commands are:

```text
ftpStatus
ftpStart
ftpStop
ftpRestart
ftpReload
```

They are asynchronous and additive, so existing sys-agent clients remain compatible.
`ftpStatus` reports lifecycle state, whether the listener is bound (`listener=up/down`), the
`ftpsrv_init` attempt count and last bind error (`bindAttempts`, `lastBindError`), the active
transfer count, byte counters, the effective `use_localtime` setting, the lifecycle error,
and `lastFsResult` for the latest native filesystem error. Complete syntax and configuration
rules are in `commands.md`.

Anonymous access grants complete read, create, upload, append, rename, and delete rights over
the SD card. Use it only on a trusted network. Stop or reconfigure any old sys-ftpd instance
that uses the same port.

## Path and search-session safety

FTP `/` maps to the SD root. Normalization rejects parent traversal and device-name prefixes.
The stable filename baseline on the tested console is ASCII, including spaces. Chinese and
Japanese filenames returned native FS Result `0x202`; some Latin extended names could be
opened but were not enumerated reliably. The server reports the error and never silently
renames a path.

While a C-level search is queued or running, FTP mutations under
`/switch/sys-agent/search` are rejected. Reads there and operations elsewhere remain
available. This prevents FTP from corrupting the search generation transaction. The remaining
hardware concurrency acceptance test is listed in the status section above.

## Rename and SIZE behaviour

`RNTO` replaces an existing file. The console filesystem refuses to rename onto an existing
path, so the SD VFS first tries the rename; only when that fails and a *file* destination
really exists does it delete that destination and retry once. A rename that fails for any
other reason (for example a missing source) therefore never deletes the destination.
Directories keep the original non-overwriting behaviour.

`SIZE` returns the open file's logical size. Treat it as advisory: a client that queries a
file while it is still being uploaded can observe the size the SD VFS preallocated in 1 MiB
increments rather than the bytes written so far. Verify transfers by content (compare the
bytes or a hash after close) rather than trusting `SIZE` alone. The off-by-a-few-bytes
`SIZE` report was not reproduced during validation; size-verified 16 MiB uploads matched
exactly.

## Real-device validation

The following tests passed with the custom sysmodule installed:

| Test | Result |
|---|---|
| Connect while a game remains open | Passed |
| ASCII file and directory CRUD | Passed |
| Names containing spaces | Passed |
| 16 MiB upload/download with hash verification | Passed |
| 16 MiB upload | 4.18 s, approximately 4.02 MB/s |
| 16 MiB download | 1.68 s, approximately 9.98 MB/s |
| `REST`/`RETR` resume from 4 MiB | Passed with matching hash |
| Forced client interruption and handle cleanup | Passed |
| `ftpStop` followed by `ftpStart` | Passed |
| Port 6000 availability during FTP operations | Passed |
| Concurrent `ftpStatus` and `getVersion` during upload | Passed |
| Non-ASCII filename behavior | Limited as documented above |

Test files and directories were removed after validation.

## Build, installation, and rollback

Clone with submodules and build with the fixed Docker image documented in `README.md`. The
build produces `sys-agent/sys-agent.nsp` and refreshes
`sys-agent/43000000000000A6/exefs.nsp`.

Back up the installed `atmosphere/contents/43000000000000A6` before replacement. When using
FTP to deploy an NSP, upload it under an ASCII temporary name, verify the transfer, then rename
it atomically to `exefs.nsp`. A complete console restart is required; restarting only the game
does not reload the sysmodule. Restore the backed-up directory and restart to roll back.
