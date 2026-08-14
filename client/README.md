# sys-botbase exact-search client

This dependency-free Python client speaks the additive asynchronous search protocol. It does
not deploy sys-botbase or write target-process memory.

Check capabilities:

```bash
python3 client/sysbot_search.py --host switch capabilities
```

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
