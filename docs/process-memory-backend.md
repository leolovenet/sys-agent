# Unified process-memory backend

## Purpose

Atmosphere dmnt and the original sys-botbase implementation cannot independently attach debug
handles to the same application. The unified backend makes sys-botbase a `dmnt:cht` IPC client
when dmnt is available, so cheats, EdiZon-style access, legacy TCP memory commands, freezes,
and asynchronous searches share Atmosphere's existing debug handle and locking.

## Selection and ownership

The runtime policy defaults to `auto`. It connects to `dmnt:cht`, checks command 65000, calls
`ForceOpenCheatProcess` (65003) when no process is attached, then validates command 65002
metadata against the current application PID. Only when dmnt has not acquired or reported
ownership may auto mode fall back to direct debug. Observed dmnt ownership is remembered
conservatively across IPC failures; this prevents an intermittent service error from causing a
second `svcDebugActiveProcess` attempt.

Forced `dmnt` never falls back. Forced `direct` never contacts dmnt. The sysmodule closes only
its IPC service session and never sends `ForceCloseCheatProcess`, because ownership belongs to
Atmosphere and may be shared with other clients.

## Concurrency and lifecycle

One mutex protects backend selection and every open/read/write/query/close sequence. Legacy
multi-read and pointer operations hold one session for their complete batch. The freeze worker
holds one session for metadata and all writes in a cycle. Search captures backend kind and PID
at start, then opens that exact backend for one mapping query or 256 KiB read at a time, keeping
the existing cancellation and command-interleaving behavior.

The dmnt service is initialized lazily; its absence does not prevent sys-botbase from starting.
Policy is volatile and resets to auto after reboot. Search sessions reject policy changes while
queued or running.

### Debug-handle lifetime

Disabling individual Atmosphere cheat entries does not mean that `dmnt:cht` is unavailable or
that dmnt has detached from the game. Under the default auto policy, if the dmnt service is
available but has not attached, sys-botbase calls `ForceOpenCheatProcess`; dmnt then owns the
single shared debug handle even when no cheat entry is enabled.

sys-botbase never releases that dmnt-owned handle with `ForceCloseCheatProcess`. It closes only
its own IPC service session during sysmodule shutdown. Atmosphere remains responsible for the
debug handle until it closes the cheat process, the game exits, or the console restarts. This
preserves other dmnt clients and allows cheats or EdiZon to be enabled later without another
debugger attach.

The direct backend has different ownership. It calls `svcDebugActiveProcess` for one
`ProcessMemorySession` and always calls `svcCloseHandle` when that session closes. A legacy
peek/poke/pointer batch holds one direct handle for the command; the freeze worker holds one per
freeze cycle; search opens and closes one for each mapping query or 256 KiB read. Failed opens
that obtained a handle also close it during cleanup. Thus direct handles do not remain owned by
sys-botbase after an operation finishes.

## IPC surface

The project contains a minimal client implemented from Atmosphere's documented service ABI:

- 65000: has cheat process
- 65002: process metadata
- 65003: force open
- 65004/65005: pause/resume
- 65102/65103/65104: read/write/query process memory

No EdiZon source is copied. Direct Debug SVC calls exist only inside the direct backend.

## Real-device validation (2026-08-15)

The user deployed the build with ACNH 3.0.3 and confirmed Atmosphere cheats were active. The
first `memoryBackendProbe` selected `dmnt`, reported dmnt available and attached, and returned
the current ACNH PID/title ID without `0xF401`. Legacy version, title, heap/main base, build ID,
absolute/main reads, and a known main pointer all remained functional.

Byte and typed searches over heap and main captured `backend=dmnt`, returned their expected
addresses, and completed with zero read errors. A 128 MiB dmnt search ran concurrently with a
legacy command, rejected a policy change with `BUSY`, and cancelled cleanly after 6,881,280
bytes. After closing the session, forced direct probing returned the expected `0xF401` while
dmnt owned the process; switching back to auto immediately selected dmnt again, with cheats
remaining active.

No write or freeze was performed. A proposed same-value write to an unverified heap metadata
address was intentionally rejected as unsafe. Write/freeze coexistence still requires a known,
backed-up, explicitly approved test address.
