#!/usr/bin/env python3
"""Client for the additive sys-botbase asynchronous exact-search protocol."""

from __future__ import annotations

import argparse
import dataclasses
import socket
import sys
import time
from collections.abc import Iterator


TERMINAL_STATES = {"done", "cancelled", "error"}


class SysBotProtocolError(RuntimeError):
    pass


def parse_response(line: str) -> dict[str, str]:
    parts = line.strip().split()
    if not parts or parts[0] not in {"OK", "ERR"}:
        raise SysBotProtocolError(f"invalid response: {line!r}")
    response = {"result": parts[0]}
    for part in parts[1:]:
        if "=" not in part:
            raise SysBotProtocolError(f"invalid response field: {part!r}")
        key, value = part.split("=", 1)
        if not key or key in response:
            raise SysBotProtocolError(f"invalid response key: {key!r}")
        response[key] = value
    return response


def require_ok(response: dict[str, str]) -> dict[str, str]:
    if response.get("result") != "OK":
        raise SysBotProtocolError(f"sys-botbase error: {response.get('code', 'UNKNOWN')}")
    return response


def parse_int(value: str) -> int:
    return int(value, 0)


@dataclasses.dataclass(frozen=True)
class SearchStatus:
    session: int
    state: str
    start: int
    end: int
    scanned: int
    total: int
    matches: int
    stored: int
    truncated: bool
    read_errors: int
    error: int
    type: str = "bytes"
    region: str = "absolute"
    base: int = 0
    region_offset: int = 0
    alignment: int = 1

    @classmethod
    def from_response(cls, response: dict[str, str]) -> "SearchStatus":
        require_ok(response)
        return cls(
            session=parse_int(response["session"]),
            state=response["state"],
            start=int(response["start"], 16),
            end=int(response["end"], 16),
            scanned=parse_int(response["scanned"]),
            total=parse_int(response["total"]),
            matches=parse_int(response["matches"]),
            stored=parse_int(response["stored"]),
            truncated=response["truncated"] == "1",
            read_errors=parse_int(response["readErrors"]),
            error=parse_int(response["error"]),
            type=response.get("type", "bytes"),
            region=response.get("region", "absolute"),
            base=int(response.get("base", "0"), 16),
            region_offset=int(response.get("regionOffset", response["start"]), 16),
            alignment=parse_int(response.get("alignment", "1")),
        )


class SysBotSearchClient:
    def __init__(self, host: str = "switch", port: int = 6000, timeout: float = 10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._socket: socket.socket | None = None
        self._buffer = bytearray()

    def __enter__(self) -> "SysBotSearchClient":
        self.connect()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def connect(self) -> None:
        if self._socket is not None:
            return
        self._socket = socket.create_connection((self.host, self.port), self.timeout)
        self._socket.settimeout(self.timeout)

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None
        self._buffer.clear()

    def command(self, command: str) -> dict[str, str]:
        if "\r" in command or "\n" in command:
            raise ValueError("command must be a single line")
        self.connect()
        assert self._socket is not None
        self._socket.sendall(command.encode("ascii") + b"\r\n")
        return parse_response(self._readline())

    def _readline(self) -> str:
        assert self._socket is not None
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self._buffer[:newline])
                del self._buffer[: newline + 1]
                return line.rstrip(b"\r").decode("ascii")
            data = self._socket.recv(4096)
            if not data:
                raise SysBotProtocolError("connection closed before a complete response")
            self._buffer.extend(data)
            if len(self._buffer) > 1024 * 1024:
                raise SysBotProtocolError("response exceeds 1 MiB safety limit")

    def capabilities(self) -> dict[str, str]:
        return require_ok(self.command("searchCapabilities"))

    def start(self, start: int, end: int, pattern: bytes) -> int:
        if start < 0 or end <= start:
            raise ValueError("end must be greater than start")
        if not pattern:
            raise ValueError("pattern must not be empty")
        response = require_ok(self.command(f"searchStart 0x{start:X} 0x{end:X} {pattern.hex().upper()}"))
        return parse_int(response["session"])

    def start_region(
        self,
        value_type: str,
        region: str,
        offset: int,
        size: int,
        value: str | int | bytes,
        alignment: int | None = None,
    ) -> int:
        if value_type not in {"bytes", "u8", "u16", "u32", "u64"}:
            raise ValueError("type must be bytes, u8, u16, u32, or u64")
        if region not in {"absolute", "heap", "main"}:
            raise ValueError("region must be absolute, heap, or main")
        if offset < 0 or size <= 0:
            raise ValueError("offset must be non-negative and size must be positive")
        if value_type == "bytes":
            if not isinstance(value, bytes) or not value:
                raise ValueError("bytes searches require a non-empty bytes value")
            encoded = value.hex().upper()
        else:
            if isinstance(value, bytes):
                raise ValueError("integer searches require an integer or numeric string")
            encoded = str(value)
        command = f"searchStartRegion {value_type} {region} 0x{offset:X} 0x{size:X} {encoded}"
        if alignment is not None:
            if alignment <= 0:
                raise ValueError("alignment must be positive")
            command += f" {alignment}"
        response = require_ok(self.command(command))
        return parse_int(response["session"])

    def status(self, session: int) -> SearchStatus:
        return SearchStatus.from_response(self.command(f"searchStatus {session}"))

    def results(self, session: int, offset: int, count: int) -> tuple[list[int], int]:
        response = require_ok(self.command(f"searchResults {session} {offset} {count}"))
        addresses_text = response.get("addresses", "")
        addresses = [] if not addresses_text else [int(value, 16) for value in addresses_text.split(",")]
        expected = parse_int(response["count"])
        if len(addresses) != expected:
            raise SysBotProtocolError(f"response count says {expected}, received {len(addresses)} addresses")
        return addresses, parse_int(response["stored"])

    def iter_results(self, session: int, page_size: int = 256) -> Iterator[int]:
        if page_size <= 0 or page_size > 256:
            raise ValueError("page_size must be in 1..256")
        offset = 0
        while True:
            addresses, stored = self.results(session, offset, page_size)
            yield from addresses
            offset += len(addresses)
            if not addresses or offset >= stored:
                break

    def cancel(self, session: int) -> None:
        require_ok(self.command(f"searchCancel {session}"))

    def close_session(self, session: int) -> None:
        require_ok(self.command(f"searchClose {session}"))

    def wait(self, session: int, poll_interval: float = 0.25) -> SearchStatus:
        while True:
            status = self.status(session)
            if status.state in TERMINAL_STATES:
                return status
            time.sleep(poll_interval)


def parse_pattern(value: str) -> bytes:
    if value.lower().startswith("0x"):
        value = value[2:]
    if not value or len(value) % 2:
        raise argparse.ArgumentTypeError("pattern must contain an even number of hex digits")
    try:
        return bytes.fromhex(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def print_fields(response: dict[str, str]) -> None:
    print(" ".join(f"{key}={value}" for key, value in response.items()))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="switch")
    parser.add_argument("--port", type=int, default=6000)
    parser.add_argument("--timeout", type=float, default=10.0)
    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("capabilities")

    start = subparsers.add_parser("start")
    start.add_argument("start", type=lambda value: int(value, 0))
    start.add_argument("end", type=lambda value: int(value, 0))
    start.add_argument("pattern", type=parse_pattern)

    region_start = subparsers.add_parser("start-region")
    region_start.add_argument("type", choices=("bytes", "u8", "u16", "u32", "u64"))
    region_start.add_argument("region", choices=("absolute", "heap", "main"))
    region_start.add_argument("offset", type=lambda value: int(value, 0))
    region_start.add_argument("size", type=lambda value: int(value, 0))
    region_start.add_argument("value")
    region_start.add_argument("--alignment", type=int)

    for action in ("status", "cancel", "close"):
        command = subparsers.add_parser(action)
        command.add_argument("session", type=int)

    results = subparsers.add_parser("results")
    results.add_argument("session", type=int)
    results.add_argument("--offset", type=int, default=0)
    results.add_argument("--count", type=int, default=256)

    search = subparsers.add_parser("search")
    search.add_argument("start", type=lambda value: int(value, 0))
    search.add_argument("end", type=lambda value: int(value, 0))
    search.add_argument("pattern", type=parse_pattern)
    search.add_argument("--poll-interval", type=float, default=0.25)
    search.add_argument("--page-size", type=int, default=256)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        with SysBotSearchClient(args.host, args.port, args.timeout) as client:
            if args.action == "capabilities":
                print_fields(client.capabilities())
            elif args.action == "start":
                print(client.start(args.start, args.end, args.pattern))
            elif args.action == "start-region":
                value: str | bytes = parse_pattern(args.value) if args.type == "bytes" else args.value
                print(client.start_region(args.type, args.region, args.offset, args.size,
                    value, args.alignment))
            elif args.action == "status":
                print(dataclasses.asdict(client.status(args.session)))
            elif args.action == "results":
                addresses, stored = client.results(args.session, args.offset, args.count)
                print(f"stored={stored}")
                for address in addresses:
                    print(f"0x{address:016X}")
            elif args.action == "cancel":
                client.cancel(args.session)
            elif args.action == "close":
                client.close_session(args.session)
            elif args.action == "search":
                session = client.start(args.start, args.end, args.pattern)
                print(f"session={session}", file=sys.stderr)
                try:
                    status = client.wait(session, args.poll_interval)
                except KeyboardInterrupt:
                    client.cancel(session)
                    print("cancel requested", file=sys.stderr)
                    return 130
                print(dataclasses.asdict(status), file=sys.stderr)
                if status.state == "error":
                    return 2
                for address in client.iter_results(session, args.page_size):
                    print(f"0x{address:016X}")
                return 0 if status.state == "done" else 1
        return 0
    except (OSError, ValueError, SysBotProtocolError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
