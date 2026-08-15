# sys-botbase exact-search client

This dependency-free Python client speaks the additive asynchronous search protocol. It does
not deploy sys-botbase or write target-process memory.

Check capabilities:

```bash
python3 client/sysbot_search.py --host switch capabilities
```

Inspect or select the shared process-memory backend and probe it:

```bash
python3 client/sysbot_search.py --host switch backend
python3 client/sysbot_search.py --host switch backend auto
python3 client/sysbot_search.py --host switch backend-probe
```

`auto` prefers Atmosphere `dmnt:cht`; `dmnt` requires it; `direct` preserves the original
standalone debugger behavior and will fail if dmnt already owns the game's debug handle.

Run an exact search over an exclusive absolute range, poll until completion, and print all
stored matching addresses:

```bash
python3 client/sysbot_search.py --host switch search \
  0x80000000 0x80010000 DEADBEEF
```

The lower-level `start`, `status`, `results`, `cancel`, and `close` subcommands can be used
individually. Pressing Control-C during `search` sends `searchCancel` before exiting.

Start a typed little-endian search over the first 16 MiB of the heap, aligned to four bytes:

```bash
python3 client/sysbot_search.py --host switch start-region \
  u32 heap 0 0x1000000 0x12345678 --alignment 4
```

Search raw bytes in a main-relative range:

```bash
python3 client/sysbot_search.py --host switch start-region \
  bytes main 0 0x100000 DEADBEEF
```

`absolute` treats the offset argument as the absolute start address. Integer types accept
decimal or `0x`-prefixed unsigned values. Defaults are natural alignment for integer types and
one-byte alignment for byte patterns.

Unknown-value searches are also available from Python:

```python
with SysBotSearchClient("switch") as client:
    session = client.begin_unknown("u32", "heap", 0, 0x100000, pause=False)
    client.wait(session)

    # Change the value in the game, then keep only changed candidates.
    client.refine(session, "changed")
    status = client.wait(session)
    print(status.generation, status.candidates, status.disk_bytes)

    for address in client.iter_results(session):
        print(f"{address:016X}")
    client.close_session(session)
```

Refine modes are `exact`, `changed`, `unchanged`, `increased`, and `decreased`. Exact mode
requires a value. The client always sends alignment and pause explicitly so the wire command is
unambiguous.
