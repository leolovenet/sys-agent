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
python3 client/sysagent.py debug trace-timeline first.jsonl second.jsonl --json
python3 client/sysagent.py debug trace-timeline trace.jsonl --profile profile.json --json
```

`trace-plan` resolves the profile without arming a trace. `trace-run` resolves it again and
starts the matching `debug trace` or `debug multitrace` command. Without `--wait` or
`--output`, it returns as soon as the hardware breakpoints are armed, so the consumer can
trigger the target operation. `--output` implies waiting and exports a versioned JSONL file.
That export is annotated with the profile target IDs, process IDs and original module offsets.

`trace-timeline` accepts multiple trace files and sorts raw events and decoded IPC transactions
by the console's monotonic system tick. New exports record a best-effort `bootEpochUnixMs`;
files from different boots are rejected because their tick domains are unrelated. The optional
`--profile` supplies target IDs and IPC pairs for files produced by plain `multitrace-dump`.

`--wait-process` uses the generic PM creation hooks before resolving process IDs and arming.
Foreground selectors use `debug wait-application`; Title ID selectors use `debug wait-title`.
When a hook matches, PM intentionally skips `StartProcess`, so the main thread has not executed
when the PID becomes visible. The client resolves modules and arms the trace while the process
is suspended, waits for `armed=1`, then releases it through `debug start-process`. This is a
real instruction-zero launch handshake. The timeout is per process, and direct callers of the
wait commands must poll `debug wait-status` and perform the release step themselves. Waiting
runs in a sysmodule worker thread, so input, screenshots and other commands remain available.
The Python client's `debug wait-title` / `debug wait-application` wrappers perform that polling
and return only after the process is parked or already exists.

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
      "base": "main",
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
        "correlation": {
          "request": "event:x2",
          "response": "event:x1",
          "thread": false,
          "fallback": "none"
        },
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
    "durationFrom": "first-hit",
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
- `base` is optional and defaults to `main`. `base: "code"` resolves from the lowest executable
  mapping in the process instead; `offset: "0x0"` then denotes its code-region entrypoint (the
  kernel `StartProcess` entry). `base=code` requires `offset`, not a main-module symbol.
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

The optional `trace` object supplies defaults for `hits`, `ring`, `duration`, `durationFrom`,
`args` and legacy global `snapshots`. Command-line options override them. `durationFrom` is
`arm` by default and may be `first-hit`; the latter starts the duration failsafe at the first
matching target hit instead of when arming completes. `trace-run --duration-from` overrides the
profile value. In `first-hit` mode the implicit `trace-run --wait` timeout starts only after the
first hit; an explicit `--wait-timeout` remains a hard limit from trace start. The sysmodule's
normal safety rules still apply:

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
response statics, data-word alignment, CMIF domain wrappers and `SFCI`/`SFCO` headers. Each
copy/move handle is retained with its kind, per-kind index, numeric value and hex form.

Decoded request headers contain `commandId` and `token`; response headers contain `result` and
`interfaceId`. The response `+0xC` field is an InterfaceId on firmware 14.0.0+ and reserved on
older firmware, not the request token. `trace-ipc` scans all snapshot slots in one or more trace
files and skips snapshots that do not contain a valid CMIF header. The decoder intentionally
does not claim that a Command ID has a particular service-specific meaning.

If target metadata marks a snapshot as `"kind": "ipc"`, `trace-ipc` scans only those slots.
This avoids trying unrelated object/state snapshots first.

For trace files produced by `trace-run --output`, the embedded profile pairs let `trace-ipc`
combine matching request and response records into transactions. Each transaction reports the
request/return sequences and ticks, thread, target IDs, operation, Command ID, request token,
response InterfaceId, handles and the response Result. Consumer-provided
service/interface/command labels are attached when present. If both a profile
`interfaceHash` and a non-zero response InterfaceId are available,
`interfaceMatchesProfile` records whether they agree. A response is paired against the oldest
request on the same session, thread and profile pair. The transaction records
`outstandingAtResponse`; when more than one request was outstanding the correlation is marked
`ambiguous-thread-fifo` and counted separately. Unmatched requests and responses are also
counted separately. `--profile` can supply target IDs and semantic labels for traces exported
through plain `multitrace-dump`, where the profile was not embedded by `trace-run`.

For concurrent or asynchronous calls, a pair may define
`ipc.correlation.request` and optionally `.response`; when response is omitted it uses the
request expression. The key expressions are:

- `event:FIELD`: an event field (`thread`, `pc`, `lr`, `sp`, `target`, `targetAddress`, or
  `x0`-`x7`);
- `ipc:header:FIELD` and `ipc:domain:FIELD`: decoded CMIF header/domain values;
- `ipc:payload:OFFSET:SIZE`: bytes relative to the decoded CMIF payload;
- `snapshot:INDEX:OFFSET:SIZE`: bytes from the retained raw HIPC snapshot.

`thread` defaults to true and may be false when the request and completion run on different
threads. `fallback` defaults to `none`; `thread-fifo` permits an explicit, clearly labelled
fallback when no exact key is found. Duplicate requests with the same key remain ambiguous.
The summary reports both `ambiguousTransactions` and counts per `correlationMethods`.

For CMIF domain requests, `trace-ipc` also emits `domainObjects`. Each object ID records its
first/last observed tick, request and response counts, observed Command IDs and Result
frequencies. Generic CMIF control commands are classified without service-specific knowledge;
a successful ConvertCurrentObjectToDomain response creates an explicit domain object event,
and `CmifDomainRequestType=2` is decoded as a domain close even though it has no SFCI body.
`createCount`, `closeCount`, `createdTick` and `closedTick` come only from those observed
messages. Disappearing from a trace is not treated as proof of destruction.

## Current limits

- Profile labels are consumer-supplied metadata; the decoder does not independently prove that
  a Command ID belongs to a service or interface.
- Dual-process profiles currently use two targets per process because that is the sysmodule's
  four-register multitrace layout.
- PM hooks can park and trace a future process before `StartProcess`, but cannot retroactively
  observe initialization that completed before sys-agent initialized or before the hook was
  registered.
