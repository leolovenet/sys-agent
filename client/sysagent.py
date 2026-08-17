#!/usr/bin/env python3
"""Client for the additive sys-agent asynchronous exact-search protocol."""

from __future__ import annotations

import argparse
import dataclasses
import socket
import sys
import time
from collections.abc import Iterator


TERMINAL_STATES = {"done", "cancelled", "error"}

# Ordinary command responses are small, but the screenCapture (legacy
# "pixelPeek") command returns the captured JPEG as a single hex line. The
# sys-agent capture buffer is 0x7D000 bytes, so the worst-case hex line is
# 1,048,576 characters plus the newline, which sits exactly at the old 1 MiB
# cap. 4 MiB gives comfortable headroom for that response.
RESPONSE_LIMIT = 4 * 1024 * 1024


class SysAgentProtocolError(RuntimeError):
    pass


def parse_response(line: str) -> dict[str, str]:
    parts = line.strip().split()
    if not parts or parts[0] not in {"OK", "ERR"}:
        raise SysAgentProtocolError(f"invalid response: {line!r}")
    response = {"result": parts[0]}
    for part in parts[1:]:
        if "=" not in part:
            raise SysAgentProtocolError(f"invalid response field: {part!r}")
        key, value = part.split("=", 1)
        if not key or key in response:
            raise SysAgentProtocolError(f"invalid response key: {key!r}")
        response[key] = value
    return response


def require_ok(response: dict[str, str]) -> dict[str, str]:
    if response.get("result") != "OK":
        raise SysAgentProtocolError(f"sys-agent error: {response.get('code', 'UNKNOWN')}")
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
    backend: str = "none"
    kind: str = "exact"
    generation: int = 0
    candidates: int = 0
    operation: str = "scan"
    disk_bytes: int = 0
    pause: bool = False
    committed: bool = False
    resumable: bool = False
    failure: str = "NONE"

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
            backend=response.get("backend", "none"),
            kind=response.get("kind", "exact"),
            generation=parse_int(response.get("generation", "0")),
            candidates=parse_int(response.get("candidates", response["stored"])),
            operation=response.get("operation", "scan"),
            disk_bytes=parse_int(response.get("diskBytes", "0")),
            pause=response.get("pause", "0") == "1",
            committed=response.get("committed", "0") == "1",
            resumable=response.get("resumable", "0") == "1",
            failure=response.get("failure", "NONE"),
        )


@dataclasses.dataclass(frozen=True)
class BackendStatus:
    policy: str
    active: str
    dmnt_available: bool
    dmnt_attached: bool
    process_id: int
    title_id: int
    last_error: int

    @classmethod
    def from_response(cls, response: dict[str, str]) -> "BackendStatus":
        require_ok(response)
        return cls(
            policy=response["policy"],
            active=response["active"],
            dmnt_available=response["dmntAvailable"] == "1",
            dmnt_attached=response["dmntAttached"] == "1",
            process_id=int(response["pid"], 16),
            title_id=int(response["titleId"], 16),
            last_error=parse_int(response["lastError"]),
        )


class SysAgentClient:
    def __init__(self, host: str = "switch", port: int = 6000, timeout: float = 10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._socket: socket.socket | None = None
        self._buffer = bytearray()

    def __enter__(self) -> "SysAgentClient":
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
        return parse_response(self._readline().decode("ascii"))

    def _readline(self, max_bytes: int = RESPONSE_LIMIT) -> bytes:
        assert self._socket is not None
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self._buffer[:newline])
                del self._buffer[: newline + 1]
                return line.rstrip(b"\r")
            data = self._socket.recv(4096)
            if not data:
                raise SysAgentProtocolError("connection closed before a complete response")
            self._buffer.extend(data)
            if len(self._buffer) > max_bytes:
                raise SysAgentProtocolError(f"response exceeds {max_bytes} byte safety limit")

    def screenshot(self) -> bytes:
        """Capture the current screen and return the JPEG bytes.

        The sys-agent `screenCapture` command (legacy name `pixelPeek`)
        returns the JPEG as one bare uppercase-hex line without an OK/ERR
        envelope. An empty line means the capture failed.
        """
        self.connect()
        assert self._socket is not None
        self._socket.sendall(b"screenCapture\r\n")
        line = self._readline()
        if line.startswith(b"OK ") or line.startswith(b"ERR "):
            raise SysAgentProtocolError(line.decode("ascii", "replace"))
        if not line:
            raise SysAgentProtocolError("screen capture returned an empty response")
        try:
            return bytes.fromhex(line.decode("ascii"))
        except (UnicodeDecodeError, ValueError) as error:
            raise SysAgentProtocolError(f"invalid screen capture response: {error}") from error

    def capabilities(self) -> dict[str, str]:
        return require_ok(self.command("searchCapabilities"))

    def backend_status(self) -> BackendStatus:
        return BackendStatus.from_response(self.command("memoryBackend"))

    def set_backend_policy(self, policy: str) -> BackendStatus:
        if policy not in {"auto", "dmnt", "direct"}:
            raise ValueError("policy must be auto, dmnt, or direct")
        return BackendStatus.from_response(self.command(f"memoryBackend {policy}"))

    def probe_backend(self) -> BackendStatus:
        return BackendStatus.from_response(self.command("memoryBackendProbe"))

    def system_capabilities(self) -> dict[str, str]:
        return require_ok(self.command("systemCapabilities"))

    def system_query(self, name: str) -> dict[str, str]:
        queries = {
            "info": "systemInfo",
            "time": "systemTime",
            "power": "powerStatus",
            "storage": "storageStatus",
            "network": "networkStatus",
            "network-profile": "networkProfile",
            "account": "accountStatus",
            "application": "applicationStatus",
        }
        if name not in queries:
            raise ValueError("unsupported system query")
        return require_ok(self.command(queries[name]))

    def process_list(self, offset: int = 0, count: int = 64) -> dict[str, str]:
        if offset < 0 or count < 1 or count > 64:
            raise ValueError("offset must be non-negative and count must be in 1..64")
        return require_ok(self.command(f"processList {offset} {count}"))

    def system_action(self, action: str) -> dict[str, str]:
        commands = {
            "reboot": "systemReboot",
            "reboot-emummc": "systemRebootEmuMMC",
            "shutdown": "systemShutdown",
            "sleep": "systemSleep",
            "terminate-application": "applicationTerminate",
        }
        if action not in commands:
            raise ValueError("unsupported system action")
        return require_ok(self.command(commands[action]))

    def set_wireless(self, enabled: bool) -> dict[str, str]:
        state = "enabled" if enabled else "disabled"
        return require_ok(self.command(f"networkSet {state}"))

    def lock_screen_status(self) -> dict[str, str]:
        return require_ok(self.command("lockScreenStatus"))

    def set_lock_screen(self, enabled: bool) -> dict[str, str]:
        state = "enabled" if enabled else "disabled"
        return require_ok(self.command(f"lockScreenSet {state}"))

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

    def begin_unknown(
        self,
        value_type: str,
        region: str,
        offset: int,
        size: int,
        alignment: int | None = None,
        pause: bool = False,
    ) -> int:
        if value_type not in {"u8", "u16", "u32", "u64"}:
            raise ValueError("type must be u8, u16, u32, or u64")
        if region not in {"absolute", "heap", "main", "alias", "addressSpace"}:
            raise ValueError("unsupported unknown-search region")
        if offset < 0 or size <= 0:
            raise ValueError("offset must be non-negative and size must be positive")
        if alignment is None:
            alignment = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}[value_type]
        if alignment <= 0:
            raise ValueError("alignment must be positive")
        response = require_ok(self.command(
            f"searchBegin {value_type} {region} 0x{offset:X} 0x{size:X} "
            f"{alignment} {1 if pause else 0}"
        ))
        return parse_int(response["session"])

    def refine(self, session: int, mode: str, value: str | int | None = None,
               pause: bool = False) -> None:
        commands = {
            "exact": "searchRefineExact",
            "changed": "searchRefineChanged",
            "unchanged": "searchRefineUnchanged",
            "increased": "searchRefineIncreased",
            "decreased": "searchRefineDecreased",
        }
        if mode not in commands:
            raise ValueError("unsupported refine mode")
        if mode == "exact":
            if value is None:
                raise ValueError("exact refine requires a value")
            command = f"{commands[mode]} {session} {value} {1 if pause else 0}"
        else:
            if value is not None:
                raise ValueError(f"{mode} refine does not accept a value")
            command = f"{commands[mode]} {session} {1 if pause else 0}"
        require_ok(self.command(command))

    def results(self, session: int, offset: int, count: int) -> tuple[list[int], int]:
        response = require_ok(self.command(f"searchResults {session} {offset} {count}"))
        addresses_text = response.get("addresses", "")
        addresses = [] if not addresses_text else [int(value, 16) for value in addresses_text.split(",")]
        expected = parse_int(response["count"])
        if len(addresses) != expected:
            raise SysAgentProtocolError(f"response count says {expected}, received {len(addresses)} addresses")
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
    backend = subparsers.add_parser("backend")
    backend.add_argument("policy", nargs="?", choices=("auto", "dmnt", "direct"))
    subparsers.add_parser("backend-probe")
    subparsers.add_parser("system-capabilities")
    query = subparsers.add_parser("system-query")
    query.add_argument("query", choices=("info", "time", "power", "storage", "network",
        "network-profile", "account", "application"))
    processes = subparsers.add_parser("process-list")
    processes.add_argument("--offset", type=int, default=0)
    processes.add_argument("--count", type=int, default=64)
    system_action = subparsers.add_parser("system-action")
    system_action.add_argument("command", choices=("reboot", "shutdown", "sleep",
        "reboot-emummc", "terminate-application"))
    wireless = subparsers.add_parser("wireless")
    wireless.add_argument("state", choices=("enabled", "disabled"))
    lock_screen = subparsers.add_parser("lock-screen")
    lock_screen.add_argument("state", choices=("status", "enabled", "disabled"))

    screenshot = subparsers.add_parser("screenshot")
    screenshot.add_argument("--output",
        help="write the JPEG to this path (default: screenshot-<unix time>.jpg)")

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
        with SysAgentClient(args.host, args.port, args.timeout) as client:
            if args.action == "capabilities":
                print_fields(client.capabilities())
            elif args.action == "backend":
                status = client.backend_status() if args.policy is None \
                    else client.set_backend_policy(args.policy)
                print(dataclasses.asdict(status))
            elif args.action == "backend-probe":
                print(dataclasses.asdict(client.probe_backend()))
            elif args.action == "system-capabilities":
                print_fields(client.system_capabilities())
            elif args.action == "system-query":
                print_fields(client.system_query(args.query))
            elif args.action == "process-list":
                print_fields(client.process_list(args.offset, args.count))
            elif args.action == "system-action":
                print_fields(client.system_action(args.command))
            elif args.action == "wireless":
                print_fields(client.set_wireless(args.state == "enabled"))
            elif args.action == "lock-screen":
                response = client.lock_screen_status() if args.state == "status" \
                    else client.set_lock_screen(args.state == "enabled")
                print_fields(response)
            elif args.action == "screenshot":
                data = client.screenshot()
                output = args.output or f"screenshot-{int(time.time())}.jpg"
                with open(output, "wb") as image:
                    image.write(data)
                print(output)
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
    except (OSError, ValueError, SysAgentProtocolError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
