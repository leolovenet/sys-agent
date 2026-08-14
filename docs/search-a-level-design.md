# A-level exact-search design and deployment gate

This document records the implementation boundary that was verified before any real-device
deployment. The feature is additive and keeps all original sys-botbase commands unchanged.

## Scope

- One asynchronous exact-byte search at a time.
- Absolute, half-open address ranges: `[start, end)`.
- Patterns from 1 through 256 bytes, supplied in memory byte order.
- Readable target-process mappings only.
- 256 KiB reads with `pattern length - 1` bytes of overlap inside a mapping.
- The first 65,536 absolute match addresses are retained; all matches are counted.
- Status polling, pages of at most 256 addresses, cancellation, and explicit session close.
- No snapshot, unknown-value, changed/unchanged, typed-value, heap-relative, or main-relative
  search in this A-level version.

## Threading and locks

The socket main loop creates a low-priority search worker. A start command only validates and
queues a session, then returns immediately. The worker never writes to stdout or a socket;
clients poll from the main command thread. This avoids the process-wide `dup2()` stdout routing
used by the original multi-client server.

`debugMutex` serializes every `attach()`/`detach()` pair used by legacy reads, writes, pointer
commands, freezes, and the search worker. Search holds this mutex for one mapping query or one
256 KiB debug read, not for the whole scan. Controller, touch, keyboard, and click-sequence work
does not require this mutex.

The search worker never holds the search-session mutex while acquiring `debugMutex`. The
existing main/freeze path may hold `freezeMutex` before `debugMutex`, but no path takes those
locks in reverse order. Pattern comparison holds only the session mutex for the current chunk;
therefore status and cancellation may wait for that comparison, but other TCP/controller
commands remain independent. Cancellation is observed no later than the current chunk boundary.

## Memory budget

The sysmodule's fixed newlib heap is `0x480000` bytes (4.5 MiB). The A-level session adds at
most approximately:

|Allocation|Maximum|
|--|--:|
|Stored result addresses|512 KiB|
|Scan buffer plus overlap|about 256.25 KiB|
|Search worker stack|64 KiB|
|Pattern and session metadata|less than 1 KiB|

The search-specific peak is about 833 KiB. Existing worker stacks, TCP line buffer, freeze
state, and temporary legacy-command allocations still share the 4.5 MiB heap. Large legacy
`peekMulti` or screenshot allocations can increase the concurrent peak, so real-device stress
testing must include those commands while a search is active.

## Failure behavior

- The process ID captured at start is checked during scanning. A changed or missing process
  terminates the session with an error.
- Invalid/overflowing mapping descriptions terminate the session.
- Unreadable mappings are counted as scanned and skipped.
- A debug read failure increments `readErrors`, discards overlap, skips that 256 KiB block, and
  continues. This avoids stalling on a changing mapping but can omit matches in that failed
  block.
- Result overflow sets `truncated=1`; it does not allocate more memory or stop total counting.
- Results are volatile and are lost when the sysmodule restarts.

## Host-side validation

Run dependency-free C and Python tests with:

```bash
make test
```

The tests cover exact and overlapping matches, the cross-chunk overlap convention, readable and
unreadable mapping plans, gaps, clipping, overflow rejection, TCP fragmentation, polling,
pagination, cancellation, malformed responses, and truncation-field parsing.

Build and test with the fixed toolchain:

```bash
docker run --rm \
  --platform linux/amd64 \
  -v "/Users/leo/Documents/switch 金手指/src/sys-botbase:/work" \
  -w /work \
  devkitpro/devkita64:20260219 \
  bash -lc 'source /opt/devkitpro/switchvars.sh && make clean && make && make test'
```

## Real-device gate

Compilation and simulated tests do not validate Horizon SVC behavior, scheduling latency, ACNH
mapping changes, or interaction with Atmosphere's debug manager. Before deployment:

1. Back up the exact currently installed official sys-botbase directory and hashes.
2. Prepare a tested SD-card rollback path before replacing `exefs.nsp`.
3. Disable the conflicting ACNH Atmosphere contents directory and restart the game, as recorded
   in the workspace ACNH sys-botbase notes.
4. Start with a known readable range of only a few KiB and a known pattern.
5. Verify capabilities, completion, addresses, paging, cancellation, controller commands, a
   normal `peek`, and reconnect behavior.
6. Increase through 1 MiB, 16 MiB, and 128 MiB only after the preceding tier passes.

No deployment is authorized merely by this document or by a successful build.

## Real-device validation (2026-08-14)

The custom build was installed by the user and validated against ACNH 3.0.3. The active
process reported title ID `01006F8002326000`, heap base `0x5ECA00000`, and main NSO base
`0x5370606000` for this launch. These bases are launch-specific and must not be reused as
static offsets.

- Legacy compatibility: `getVersion`, `getTitleID`, `getHeapBase`, `getMainNsoBase`, and a
  16-byte `peekAbsolute` all returned normally.
- Known 4 KiB search: the first 16 bytes at the main NSO base were read with
  `peekAbsolute`, searched over the same 4 KiB range, and returned exactly one match at the
  main NSO base. The session completed with zero read errors.
- Pagination: a one-byte zero-pattern search over the same 4 KiB range found and retained
  719 matches. Two seven-result pages were ordered, non-overlapping, and consistent with the
  stored count.
- Concurrency and cancellation: a 128 MiB heap search ran while a separate TCP connection
  completed `getVersion` and a 16-byte `peekAbsolute`. Cancellation was observed after
  10,027,008 bytes, with state `cancelled`, zero read errors, and no protocol failure.
- Chunk-boundary matching: a 16-byte pattern beginning eight bytes before a 256 KiB boundary
  was searched over 512 KiB. It returned exactly the expected address, proving the overlap
  path on real hardware.
- Reconnection: the boundary search was started on one TCP connection and polled/paged/closed
  on a new connection, confirming that sessions are not socket-owned.

No target-process memory was written during these tests. Larger sustained scans, result
truncation at 65,536 stored addresses, unreadable/changing mappings, controller latency under
load, and repeated-search soak behavior remain to be validated.

### Extended real-device validation

The next read-only test tier completed successfully on the same launch:

| Range | Elapsed | Throughput | Matches | Read errors |
|---:|---:|---:|---:|---:|
| 1 MiB | 0.050 s | 19.92 MiB/s | 1 | 0 |
| 16 MiB | 0.536 s | 29.87 MiB/s | 1 | 0 |
| 128 MiB | 3.043 s | 42.06 MiB/s | 1 | 0 |

All three searches used the 16 bytes read from the heap base and returned that expected base
address. During the 16 MiB and 128 MiB searches, legacy `getVersion` requests on a separate
connection completed in approximately 53--166 ms.

A one-byte zero-pattern search over 1 MiB counted 805,958 matches, retained exactly 65,536,
set `truncated=1`, and returned both the first 256-result page and the final 16-result page
consistently. Ten subsequent 1 MiB sessions were started, completed, paged, and closed; all
ten returned the expected single match with zero read errors. A 1 MiB low-address unmapped
range (`[0x1000, 0x101000)`) completed safely with zero matches and zero read errors.

The earlier cancellation test and these extended tests cover the main A-level deployment
gate. Physical controller responsiveness, changing mappings/process restart during a scan,
and much longer soak/stress runs still require separate validation.

## B-level extension implementation

The host-tested B-level extension adds `searchStartRegion` without changing the A-level
commands or their argument/response formats. It supports `bytes`, `u8`, `u16`, `u32`, and
`u64` over `absolute`, `heap`, and `main` regions. Heap and main bases are resolved once at
session creation; status and results continue to use absolute addresses. Unsigned integers are
validated for their width and encoded little-endian. Natural alignment is the default for
typed values, while byte patterns default to alignment 1; explicit power-of-two alignment up
to 256 is supported.

Protocol version 2 capabilities advertise these modes, regions, alignment limit, and byte
order. Existing `searchStart` and `searchExact` remain absolute `[start,end)` byte searches
with alignment 1. Existing version-1 host parsing remains valid because the original status
fields are unchanged and the new metadata fields are appended.

The B-level build must pass host tests and the fixed Docker toolchain build before deployment.
Its heap/main base resolution, typed matching, alignment behavior, and compatibility still
require staged real-device validation after the user explicitly installs that build.

### B-level real-device validation (2026-08-14)

The user deployed the B-level build and `searchCapabilities` reported protocol version 2,
all five modes, all three regions, little-endian values, and the alignment limit. On this
launch ACNH 3.0.3 reported heap base `0x354BA00000` and main base `0x6ABD606000`; these remain
launch-specific values.

Read-only 4 KiB searches used values first obtained with `peekAbsolute` and validated:

- `bytes`, `u8`, `u16`, `u32`, and `u64` searches over the heap;
- default alignments 1, 1, 2, 4, and 8 respectively;
- an unaligned heap-relative `u32` search beginning at offset 1 with explicit alignment 1;
- a main-relative byte search and an absolute `u64` search;
- resolved base/start/type/region/alignment status metadata and paged results;
- the legacy absolute `searchStart`, which still reported bytes/absolute/alignment 1.

Every expected address was present and every session completed with zero read errors. Invalid
type, region, oversized `u32` value, and non-power-of-two alignment returned `INVALID_TYPE`,
`INVALID_REGION`, `INVALID_VALUE`, and `INVALID_ALIGNMENT` respectively. A 128 MiB heap-relative
`u64` search ran concurrently with a successful legacy `getVersion` request and was cancelled
after 94,494,720 bytes with zero read errors. All test sessions were explicitly closed and no
target memory or controller state was modified.
