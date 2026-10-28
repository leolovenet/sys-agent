# Trace Profiles

Trace profiles turn a set of process selectors, module offsets and target roles into a
validated hardware-trace plan. They are consumed by the Python client; the sysmodule remains
generic and does not contain game-specific or system-service-specific offsets.

## Commands

```bash
python3 client/sysagent.py --host switch debug trace-plan profile.json
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --duration 30 --ring 64
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --wait --output trace.jsonl
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --wait-process --wait-process-timeout 60000 --output trace.jsonl
python3 client/sysagent.py debug trace-report trace.jsonl profile.json --json
python3 client/sysagent.py --host switch debug read 0x71 main+0x1000 4
```

`trace-plan` resolves the profile without arming a trace. `trace-run` resolves it again and
starts the matching `debug trace` or `debug multitrace` command. Without `--wait` or
`--output`, it returns as soon as the hardware breakpoints are armed, so the consumer can
trigger the target operation. `--output` implies waiting and exports a versioned JSONL file.
That export is annotated with the profile target IDs, process IDs and original module offsets.

`--wait-process` waits through the generic PM creation hooks before resolving process IDs and
arming. Foreground selectors use `debug wait-application`; Title ID selectors use
`debug wait-title`. The timeout is per process. This removes repeated client-side polling, but
it is still an attach-after-creation primitive: PM exposes the created PID, not an
instruction-zero suspension point.

`trace-report` uses the module bases recorded in the trace file, not the console's current
address layout. It verifies each target offset, correlates configured entry/return IDs, and
marks each return-time `x0` as zero or non-zero.

## Schema version 2

```json
{
  "schemaVersion": 2,
  "name": "client-server-entry-return",
  "symbols": {
    "client_entry": "0x1000",
    "client_return": "0x1010",
    "server_entry": "0x2000",
    "server_return": "0x2010"
  },
  "processes": [
    {
      "id": "client",
      "foreground": true,
      "expect": {
        "titleId": "0100000000000000",
        "buildId": "0123456789ABCDEF"
      }
    },
    {
      "id": "server",
      "titleId": "0100000000000001",
      "mainModuleIndex": 0,
      "expect": {
        "buildId": "FEDCBA9876543210"
      }
    }
  ],
  "targets": [
    {
      "id": "client-entry",
      "process": "client",
      "symbol": "client_entry",
      "expectBytes": "FD7BBFA9",
      "snapshots": [
        {"spec": "x1:0x100", "name": "request", "kind": "ipc"}
      ]
    },
    {
      "id": "server-entry",
      "process": "server",
      "symbol": "server_entry",
      "snapshots": [
        {"spec": "x1:0x100", "name": "request", "kind": "ipc"}
      ]
    },
    {
      "id": "client-return",
      "process": "client",
      "symbol": "client_return",
      "snapshots": [
        {"spec": "x1:0x100", "name": "response", "kind": "ipc"}
      ]
    },
    {"id": "server-return", "process": "server", "symbol": "server_return"}
  ],
  "pairs": [
    {
      "entry": "client-entry",
      "return": "client-return",
      "ipc": {
        "protocol": "cmif",
        "service": "example-service",
        "interfaceHash": "0x0123456789ABCDEF",
        "commands": {
          "8": "ExampleCommand"
        }
      }
    },
    {"entry": "server-entry", "return": "server-return"}
  ],
  "trace": {
    "hits": 64,
    "ring": 64,
    "duration": 30,
    "args": 8
  }
}
```

Profile schema 1 remains readable. It uses global `trace.snapshots` strings and target `offset`.
Schema 2 adds per-target named snapshots, a consumer-owned `symbols` offset table, and
`expectBytes`. These symbols are not a symbol database bundled with sys-agent.

### Process selectors

Each process uses exactly one selector:

- `"foreground": true`: the currently running application. Its main module is selected by
  matching `applicationStatus.main`, regardless of the module index.
- `"titleId": "0100..."`: find the running PID with that title ID. Unless
  `mainModuleIndex` is present, module index 0 is used.
- `"pid": "0x..."`: use a fixed PID. This is useful for short-lived diagnostics, but the PID
  usually changes after reboot.

`expect.titleId` and `expect.buildId` are optional assertions. Build IDs may be complete or
prefixes. A mismatch stops the command before any hardware breakpoint is armed.

### Targets and pairs

- A one-process profile may contain one to four targets.
- A two-process profile must contain exactly two targets per process, four in total.
- Target and process IDs must be unique.
- `offset` is relative to the selected process's main module and must be
  `0x`-prefixed, non-negative and four-byte aligned.
- Schema 2 targets use exactly one of `offset` or `symbol`; each symbol references the
  top-level offsets map.
- `expectBytes` is optional. Before arming, `debug read PID ADDRESS SIZE` validates the bytes
  through a short-lived debug attach.
- Schema 2 targets may declare zero to two snapshots. A snapshot is either an old-style string
  or an object with `spec`, optional stable `name`, and optional `kind`.
- When `debug maps` returns the complete mapping list, the resolved address must fall inside a
  readable executable mapping. If the console reports `truncated=1` before the module mapping is
  listed, the plan keeps the module-base validation and marks that target as
  `unverified-truncated`.
- Resolved absolute addresses must be unique because event routing uses the exception PC.
- A pair names two target IDs owned by the same process. The return target should normally be
  the caller's return address or an actual return instruction.
- A pair may attach optional `ipc` metadata. `service`, `interfaceHash` and a
  `commands` ID-to-name table are supplied by the consumer; they are semantic labels, not
  values guessed from the HIPC message.

The client sends targets to `debug multitrace` in deterministic order:

```text
index 0: process 0, first target
index 1: process 1, first target
index 2: process 0, second target
index 3: process 1, second target
```

For a single process, profile declaration order is the trace target order.

### Trace options

The optional `trace` object supplies defaults for `hits`, `ring`, `duration`, `args` and
legacy global `snapshots`. Command-line options override them. The sysmodule's normal safety
rules still apply:

- `hits 0` requires a ring;
- ring size is 1 through 64;
- captured arguments are limited to `x0` through `x7`.
- snapshot specifications use `REG[+/-OFFSET]:SIZE`, are limited to two entries and may read at
  most 0x100 bytes per snapshot.

Schema 1 global snapshots apply to every target. Schema 2 per-target snapshots override the
global rule for that target. Wire protocol uses `snap=SPEC` for a global snapshot and
`tsnap=TARGET_INDEX:SPEC` for a target-specific snapshot.

## Triggering while armed

`trace-run` deliberately does not press controller buttons. A consumer can schedule input
before arming, for example:

```bash
python3 client/sysagent.py --host switch input click-seq --no-wait "W3000,A"
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --wait --output trace.jsonl
```

Use this only when the current UI state has already been verified. For interactive work, run
`trace-run` without `--wait`, perform the action, then use `multitrace-stop`/`trace-stop` and
the existing dump commands.

`debug wait-title TITLE_ID [TIMEOUT_MS]` and `debug wait-application [TIMEOUT_MS]` are also
available directly. PM allows one program-ID hook at a time, so profiles that wait for two
not-yet-running Title IDs wait for them sequentially.

## Report semantics

`trace-report` pairs events by trace session and thread. Each call reports:

- entry and return sequence/tick;
- owning target IDs;
- return-time `x0`;
- whether `x0` is non-zero;
- the first non-zero `x0` for the pair.

This is address-based correlation, not a complete call stack. Recursive calls are matched as a
LIFO stack per thread. A target omitted from the profile, a mismatched Build ID, or an address
that differs from the trace metadata is rejected.

The generic client does not assume that every `x0` is an `nn::Result`. A consumer that targets
a real Result return can interpret the non-zero `x0` as a candidate Result; a normal function
may return a pointer or another integer instead.

## HIPC and CMIF decoding

```bash
python3 client/sysagent.py debug ipc-decode HEX_BYTES --mode request
python3 client/sysagent.py debug trace-ipc trace.jsonl --profile profile.json --json
```

The decoder accepts a captured HIPC message beginning with its normal header. It handles the
special header and optional PID, copy/move handles, request static and buffer descriptors,
response statics, data-word alignment, CMIF domain wrappers and `SFCI`/`SFCO` headers.

Decoded request headers contain `commandId` and `token`; response headers contain `result` and
`token`. `trace-ipc` scans all snapshot slots in one or more trace files and skips snapshots
that do not contain a valid CMIF header. The decoder intentionally does not claim that a
Command ID has a particular service-specific meaning.

If target metadata marks a snapshot as `"kind": "ipc"`, `trace-ipc` scans only those slots.
This avoids trying unrelated object/state snapshots first.

For trace files produced by `trace-run --output`, the embedded profile pairs let `trace-ipc`
combine matching request and response records into transactions. Each transaction reports the
request/return sequences and ticks, thread, target IDs, Command ID, both tokens and the
response Result. Consumer-provided service/interface/command labels are attached when present.
Unmatched requests and responses are counted separately. `--profile` can supply target IDs and
semantic labels for traces exported through plain `multitrace-dump`, where the profile was not
embedded by `trace-run`.

## Current limits

- Profiles capture HIPC/CMIF bytes but do not yet assign interface hashes or semantic names to
  Command IDs.
- Dual-process profiles currently use two targets per process because that is the sysmodule's
  four-register multitrace layout.
- Process identification is based on the currently running title table; boot-time tracing
  before a process starts remains a separate feature.
