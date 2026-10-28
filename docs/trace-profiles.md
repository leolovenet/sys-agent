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
python3 client/sysagent.py debug trace-report trace.jsonl profile.json --json
```

`trace-plan` resolves the profile without arming a trace. `trace-run` resolves it again and
starts the matching `debug trace` or `debug multitrace` command. Without `--wait` or
`--output`, it returns as soon as the hardware breakpoints are armed, so the consumer can
trigger the target operation. `--output` implies waiting and exports a versioned JSONL file.

`trace-report` uses the module bases recorded in the trace file, not the console's current
address layout. It verifies each target offset, correlates configured entry/return IDs, and
marks each return-time `x0` as zero or non-zero.

## Schema version 1

```json
{
  "schemaVersion": 1,
  "name": "client-server-entry-return",
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
    {"id": "client-entry", "process": "client", "offset": "0x1000"},
    {"id": "server-entry", "process": "server", "offset": "0x2000"},
    {"id": "client-return", "process": "client", "offset": "0x1010"},
    {"id": "server-return", "process": "server", "offset": "0x2010"}
  ],
  "pairs": [
    {"entry": "client-entry", "return": "client-return"},
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
- When `debug maps` returns the complete mapping list, the resolved address must fall inside a
  readable executable mapping. If the console reports `truncated=1` before the module mapping is
  listed, the plan keeps the module-base validation and marks that target as
  `unverified-truncated`.
- Resolved absolute addresses must be unique because event routing uses the exception PC.
- A pair names two target IDs owned by the same process. The return target should normally be
  the caller's return address or an actual return instruction.

The client sends targets to `debug multitrace` in deterministic order:

```text
index 0: process 0, first target
index 1: process 1, first target
index 2: process 0, second target
index 3: process 1, second target
```

For a single process, profile declaration order is the trace target order.

### Trace options

The optional `trace` object supplies defaults for `hits`, `ring`, `duration` and `args`.
Command-line options override them. The sysmodule's normal safety rules still apply:

- `hits 0` requires a ring;
- ring size is 1 through 64;
- captured arguments are limited to `x0` through `x7`.

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

## Current limits

- Profiles do not yet include CMIF/HIPC interface hashes or command IDs.
- Dual-process profiles currently use two targets per process because that is the sysmodule's
  four-register multitrace layout.
- Memory snapshots are available only through the existing single-process `debug trace`
  command, not through profile v1.
- Process identification is based on the currently running title table; boot-time tracing
  before a process starts remains a separate feature.
