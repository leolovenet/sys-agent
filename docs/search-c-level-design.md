# C-level unknown-value search

Protocol version 3 adds a single runtime-only, SD-backed unknown-value session. The worker and
the unified process-memory backend remain shared with A/B searches; exact and unknown jobs
cannot scan concurrently.

## Storage model

The dedicated directory is `sdmc:/switch/sys-agent/search` (`/switch/sys-agent/search` as
seen on the SD card). Startup and explicit session close remove only files inside that
directory. A session records PID, Title ID, Build ID, backend, type, region, resolved range,
alignment, and generation.

Each 256 KiB-scale block has a CRC32 and uses the smallest applicable representation:

- all slots, with implicit addresses;
- a bitmap plus dense values;
- sparse 32-bit slot indexes plus values.

The file ends with block index entries containing cumulative candidate counts, allowing a
result page to seek to relevant blocks without loading the candidate set into RAM. File and
candidate counts are 64-bit.

Refinement writes `session.tmp`, validates and flushes it, preserves the committed file as
`session.old`, promotes the temporary file, and then removes the old file. Cancellation or
failure deletes the temporary generation and leaves the previous committed baseline intact.

## Consistency and failure behavior

Unsigned values are decoded little-endian. Successful exact/changed/unchanged/increased/
decreased refinement stores the current value of every survivor as the next baseline.
Unreadable mappings are skipped during generation 0, but a query/read failure aborts the
operation instead of silently dropping candidates.

The default live scan opens the pinned backend around individual queries and reads so legacy
memory commands can interleave. With `pause=1`, one backend session pauses the process and is
held for the complete operation; the resume path runs for success, cancellation, and error.
A pause failure aborts rather than degrading to a live scan.
The sys-agent freeze worker skips its write cycle while any C-level search is queued or
running. This prevents it from holding `freezeMutex` while waiting for the repeatedly acquired
backend, preserves controller-command responsiveness, and prevents frozen writes from changing
values during an unknown snapshot. Freeze cycles resume after the operation becomes terminal.
The worker also rechecks the search state after acquiring `freezeMutex`, closing the race where
a search could be queued between the first check and backend acquisition.

All sysmodule threads are restricted to CPU 3 by the NPDM. The C-level worker therefore yields
for 20 ms after every 256 KiB block. A 1 ms yield was insufficient on hardware: TCP commands
could wait until a multi-megabyte scan completed even though the search thread had a lower
priority. The longer yield trades a small amount of throughput for bounded command latency.

The SD preflight reserves 64 MiB beyond the estimated next generation. Sessions are not
recoverable after sysmodule restart. C-level files are independent of EdiZon and no EdiZon
source or dump format is reused.

## Hardware validation

Testing used ACNH 3.0.3 with Atmosphere cheats active and the `dmnt` backend selected.

- A complete 128 MiB live `u32` heap snapshot finished in 27.9 seconds, committed 28,472,320
  candidates in a 113,922,560-byte file, and reported zero read errors.
- During the scan, status latency was normally below 140 ms. Final index write and synchronous
  SD flush produced one 2.28-second status latency spike; the session then reported `done`,
  `committed=1`, and `resumable=1`.
- First, deep, middle, and final result pages were read successfully, including offsets beyond
  the legacy 65,536-result limit.
- Cancelling an initial scan discarded its incomplete file. Cancelling a refinement preserved
  the prior generation, candidate count, disk size, and identical deep-page results.
- Live `changed`, `unchanged`, `exact`, `increased`, and `decreased` generations committed
  successfully. Small `alias` and `addressSpace` ranges skipped unreadable mappings, and a
  paused heap scan resumed the game afterward.
