# Built-in SD card FTP server

## Status

The custom sys-agent branch contains a working FTP service for full SD-card CRUD without
leaving the running game. The core file operations, lifecycle controls, interruption cleanup,
and coexistence with the original TCP service have passed real-device testing.

Uploads above 4 GiB work as well: they are converted into the FAT32 split form and passed a
6,694,728,444-byte upload with a whole-file hash match (*Files above 4 GiB* has the numbers).

One acceptance check remains before describing the FTP work as fully closed: run FTP
concurrently with an active C-level memory search and verify the protection of
`/switch/sys-agent/search`, SD contention, status latency, and cancellation on hardware.
This does not block ordinary FTP use, but it must be completed before making a final
release-quality claim.

Cold-boot startup is observable and retried: the worker keeps `desiredRunning` set and calls
`ftpsrv_init` again one second after a failed bind, so a boot window where the network stack
is not ready yet heals on its own. `ftpStatus` reports `listener=up/down`, `bindAttempts`, and
`lastBindError`, and a post-reboot check showed `bindAttempts=3` with `listener=up` -- the retry
loop engaging before the listener came up.

**A single loop error no longer costs the listener.** `ftpsrv_loop` answers an error when its
poll fails or when the listening socket itself reports an error, and the worker used to treat
either as fatal: it called `ftpsrv_exit()` and re-bound one second later. That drops every session
and every transfer in flight and leaves the port unanswered until the bind is back -- which is what
"the console stutters while a transfer runs, and the transfer dies" looked like from the
client side. The worker now retries the loop and escalates only after
`FTP_LOOP_ERROR_LIMIT` (3) consecutive errors, which is still quick for a listener that is really
gone (its poll answers the error immediately). `ftpStatus` counts both sides apart:
`loopErrors` (polls that answered an error) and `listenerDrops` (times a run of them made the
listener be re-bound), with `lastLoopError` holding the errno. `loopErrors` climbing while
`listenerDrops` stays flat is the healthy case: the error was survived.

First readings from those counters, taken on 2.13.2 minutes after a reboot whose only traffic was a
64 KiB upload and a `getVersion` poll: `loopErrors=6`, `listenerDrops=2`, `lastLoopError=5` (EIO) --
two runs of three consecutive errors, each run re-binding the listener. The errors therefore
cluster around the bind window rather than around data movement, which is the likely origin of the
`bindAttempts` counts the older sessions showed; the retry rule keeps a lone error from costing
anything, and a listener whose poll keeps answering an error is still re-bound after three.

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

## Files above 4 GiB on the SD card

FAT32 cannot describe a file of 4 GiB or more -- the directory entry's size field is 32 bits --
and the console's `fs` enforces that on writes. Measured on hardware with a probe build: appending
to a plain file stops at exactly 4 GiB (4,290,772,992 + one 4 MiB chunk = 4,294,967,296) and
`fsFileSetSize(4.5 GB)` fails outright, while a file the filesystem created as a *concatenation
file* takes the same size call without complaint.

A concatenation file is a directory of numbered parts that the system presents to every program as
one file. libnx documents the query that creates the second form from the first:

> `fsFsSetConcatenationFileAttribute(fs, path)` -- "Uses fsFsQueryEntry to set the archive bit on
> the specified absolute directory path. This will cause HOS to treat the directory as if it were
> a file containing the directory's concatenated contents."

The console's own content storage uses it in the other direction: placeholders are created as
`fs::CreateFile(path, size, fs::CreateOption_BigFile)` (Atmosphere's ncm), with the size known up
front. The PC-side splitting tools ("split for FAT32") build the same layout -- parts of
`0xFFFF0000` bytes named `00`, `01`, ... inside a directory named after the file. Every such file
measured on a console splits at exactly that size: the system's own 4,650,239,771-byte package and
a 6,694,728,444-byte upload through this server (`00` = 4,294,901,760, `01` = 2,399,826,684).

An FTP server only learns the size when the transfer is over, so `ftp_vfs_sd.c` starts every
upload as a plain file (what all ordinary uploads need, unchanged) and converts it *as metadata*
the moment a write would cross `0xFFFF0000`: close the handle, rename the file aside, create the
directory it becomes, move the file in as part `00`, set the archive bit, re-read the parent
directory (the entry's type and size come from a cached directory read -- the platform's own
repair re-reads after setting the same bit), reopen and continue. Not one byte is copied. After
that the filesystem adds `01`, `02`, ... on its own, so a 15 GB upload needs nothing further from
this code.

Behaviour worth knowing:

- uploads that stay under the ceiling are byte-for-byte what they were before: one plain file;
- a converted file is one file to every client: `LIST` and `SIZE` report the joined size, and
  `<name>/00`, `<name>/01`, ... are its parts, exactly as the platform presents them;
- writing over a file the console already reads as one big file deletes it first (that removes
  every part), so the new upload starts plain -- a small file over a big one ends up small;
- **a resumed upload is refused, not served.** `STOR` opens with `FtpVfsOpenMode_WRITE`, which this
  FTP core defines as create-and-truncate, so honouring a `REST` marker would restart the file at
  the marker and silently drop everything before it -- the client would report success on a corrupt
  file. The server answers `550 ... Not supported. Failed to fseek path:` instead (measured) and
  the client restarts the file from the beginning. Downloads (`RETR` with `REST`) resume exactly as
  before, and `APPE`, which never truncates, is untouched;
- if the conversion cannot be done, the file is put back the way it was and the transfer fails
  with `EFBIG`; the failing step's native result is kept in `ftpStatus`'s `lastFsResult`, so the
  reason is read rather than guessed. Partial files stay on the card for a later restart.

**A conversion that loses power leaves `<name>.bigfiletmp` behind**, and the console's rename
refuses an existing destination -- so the next upload of that name would stop at step 1 with
`EFBIG`. Step 1 handles it: when the rename is refused, it deletes a temp file sitting at exactly
that name -- it holds whatever the interrupted upload had stored, which the client is re-sending
anyway -- and renames once more. A normal upload never reaches that branch, so nothing about the
ordinary path changes. If even the delete fails, the file is put back as a plain file, the client
sees `EFBIG` with the failing call in `ftpStatus`'s `lastFsResult`, and the leftover can be removed
by hand (`DELE <name>.bigfiletmp` from any FTP client).

`<name>.bigfiletmp` is this adapter's own temp name, and the cleanup deletes exactly that one: a
file a user parks at that exact name goes with it, while a *directory* at that name cannot be
removed by the call used here (measured: `fsFsDeleteFile` refuses a directory, its contents stay),
so it survives and the conversion reports the failure instead.

Measured on hardware with a 1 MiB ceiling so the conversion runs on a 4 MiB upload: with a
31,744-byte leftover planted at `<name>.bigfiletmp` the upload converted, finished, read back
byte-identical and left no leftover, and the control run without one behaved the same
(`loopErrors=0`, `listenerDrops=0`, `bindAttempts=1` in both).

This cleanup was written once before and withdrawn: the transfer died with it in place. That turned
out to be the listener teardown fixed in 2.13.1 -- a control run with the leftover under an
unrelated name, which never reaches this branch, failed the same way -- so the branch was not what
broke, and it is back now that the real cause is gone.

**The conversion must never happen twice.**  An upload that resumes into a file which is already a
concatenation file used to be a trap: a `STOR` open truncates the target, the truncation takes the
parts with it, and the ceiling check then cannot recognise the shape any more -- so it converted
again, moving the whole concatenation directory in as part `00`, and again.  The fingerprint is a
file whose parts are nested (`<name>/00/00`).  Two rules now prevent it: the shape is checked (and
an existing concatenation file deleted) *before* the truncating open, and the conversion is only
ever attempted on a file the check says is not already a concatenation file.

Measured on hardware (19.x, Atmosphère, applet mode, `ftp://switch:6001`):

| Check | Result |
|---|---|
| 6,694,728,444-byte upload (crosses the ceiling at 4.29 GB) | Passed; landed as `00` = 4,294,901,760 + `01` = 2,399,826,684 |
| `SIZE` / `LIST` of that file | One entry, joined size 6,694,728,444 |
| Whole-file download and SHA-256 against the source | Identical (`efa0d2c2…`), 226 on the control connection at the end |
| Sampled windows (head, 1 MiB, 2 GiB, both sides of the part boundary, tail) | Byte-identical |
| 4 MiB upload under the ceiling | Stayed a plain file, byte-identical |
| Conversion exercised with a 1 MiB ceiling (`-DFTP_VFS_PLAIN_FILE_MAX=0x000FFFFF`) | 64 KiB and 4 MiB uploads both converted and read back byte-identical |
| `REST` + `STOR` (resumed upload) | Refused with `550 ... Not supported`, file left untouched |
| Conversion with a 31,744-byte `<name>.bigfiletmp` already present (1 MiB ceiling) | Converted, finished, read back byte-identical, leftover cleared |
| A file inside a directory named `<name>.bigfiletmp`, then `DELE` on that directory | Refused (`550`), directory and its file untouched |
| Delete of a converted file | `fsFsDeleteFile` on the entry, parts and all (measured earlier with the same VFS) |

Still owed: the exact boundary sizes `0xFFFFFFFF` and `0x100000000`, and free-space accounting
across the delete of a converted file. A three-part (9 GB) upload is deliberately not on the list:
the conversion is one metadata move and the filesystem rolls the parts on its own, so the two-part
case above is the acceptance case and a third part would add transfer time, not coverage.

**What a full check costs, and what to do instead.**  Throughput here is far below the network: 16
MiB measured 4.0 MB/s up and 10.0 MB/s down, and reading the 6,694,728,444-byte file end to end
through FTP took about 35 minutes (roughly 3 MB/s once a slow client makes the worker re-read blocks
it could not send).  A regression on this path therefore does not need a whole-file hash: `SIZE`,
the part sizes (`<name>/00`, `<name>/01`, ...) and a few 64 KiB windows read either side of the part
boundary plus the head and the tail catch every failure seen here -- a file that never converted, a
short file, a lost part, a wrong part size -- in seconds instead of half an hour.  Keep the
whole-file hash for a change to the code that creates the parts.

**Why the conversion does not commit the filesystem.**  The console's own content tools call
`fsFsCommit` after creating content, and this path deliberately does not: the call sits on the code
a working 6,694,728,444-byte upload already exercised, nothing observed needs it, and adding it
would mean re-measuring that path.  It stays out until a build that makes the call can be measured
against this one.

**Exercising the conversion without moving 4 GiB.** `make DEFINES=-DFTP_VFS_PLAIN_FILE_MAX=<n>`
raises or lowers the plain-file ceiling for a test build (the top-level `Makefile` forwards
`DEFINES`), so `-DFTP_VFS_PLAIN_FILE_MAX=0x000FFFFF` converts a 64 KiB upload. Everything below the
ceiling is the code ordinary uploads take, so the override changes only *when* the conversion
happens -- which is how the conversion above was measured without waiting for 4 GiB.

A transfer is the one thing that makes this console work hard, so the way to tell "the console is
busy" from "the server dropped the listener" is to read the counters while data moves. Measured on
2.13.1: idle, 11/11 `getVersion` answers between 55 ms and 279 ms (avg 130 ms); during a 64 MiB
upload, 28/28 answers between 13 ms and 251 ms (avg 100 ms) and the upload finished in 14.6 s;
across eight rounds of a harder pattern (a 64 MiB upload and a download at the same time, a third
session killed mid-transfer, the command port sampled throughout) 110 of 110 answers came back in
72-210 ms and every upload finished; and across five minutes of two uploads throttled to
250 KB/s -- the cheapest way to keep the worker polling -- `bindAttempts` stayed at 1 with
`transfers=2`, `loopErrors=0` and `listenerDrops=0` until both finished. So the port staying up
while data moves is the normal case.

What the older sessions showed instead was `bindAttempts` climbing into the twenties within
minutes. Outside of an explicit `ftpRestart`/`ftpReload`, the only path that re-binds is the
loop-error one, and every re-bind aborts the transfers in flight and leaves the port unanswered
until the bind returns -- which is what "the transfer died and the port went away" was. That build
treated one error as fatal, which is why the same event cost everything in flight; the retry rule
above is the response, and with it no drop could be reproduced under any of the loads listed here.
The counters are what turn this class of problem into a number instead of a guess.

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
