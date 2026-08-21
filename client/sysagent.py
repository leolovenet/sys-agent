#!/usr/bin/env python3
"""Dependency-free client for the sys-agent Switch automation protocol."""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import socket
import sys
import time
from collections.abc import Iterator, Sequence


TERMINAL_STATES = {"done", "cancelled", "error"}

# Ordinary command responses are small, but the screenCapture (legacy
# "pixelPeek") command returns the captured JPEG as a single hex line. The
# sys-agent capture buffer is 0x7D000 bytes, so the worst-case hex line is
# 1,048,576 characters plus the newline, which sits exactly at the old 1 MiB
# cap. 4 MiB gives comfortable headroom for that response.
RESPONSE_LIMIT = 4 * 1024 * 1024


class SysAgentProtocolError(RuntimeError):
    pass


# --- AES-128 (FIPS-197), single-block ECB decrypt, dependency-free ----------
# Used to decrypt the ticket titlekey on the host with a titlekek from
# prod.keys. The kek itself never leaves the host.

_AES_SBOX = (
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
)

_AES_INV_SBOX = bytes(_AES_SBOX.index(i) for i in range(256))

_AES_RCON = (0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _aes_gf_mul(a: int, b: int) -> int:
    """Multiply two GF(2^8) elements (AES polynomial 0x11B)."""
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        high = a & 0x80
        a = (a << 1) & 0xFF
        if high:
            a ^= 0x1B
        b >>= 1
    return result


def _aes_expand_key_128(key: bytes) -> list[list[list[int]]]:
    """Expand a 128-bit key; returns 11 round keys as 4x4 grids [round][row][col]."""
    if len(key) != 16:
        raise ValueError("AES-128 requires a 16-byte key")
    words = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    for i in range(4, 44):
        temp = words[i - 1]
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [_AES_SBOX[b] for b in temp]
            temp[0] ^= _AES_RCON[i // 4]
        words.append([words[i - 4][j] ^ temp[j] for j in range(4)])
    # words[i] is column i (rows 0..3); round key grid[r][c] = words[4*round+c][r].
    return [
        [[words[4 * rnd + col][row] for col in range(4)] for row in range(4)]
        for rnd in range(11)
    ]


def _aes128_ecb_decrypt_block(key: bytes, block: bytes) -> bytes:
    """Decrypt one 16-byte block with AES-128 (no padding, ECB)."""
    if len(block) != 16:
        raise ValueError("AES block must be 16 bytes")
    round_keys = _aes_expand_key_128(key)
    state = [[block[row + 4 * col] for col in range(4)] for row in range(4)]

    state = [[state[row][col] ^ round_keys[10][row][col] for col in range(4)] for row in range(4)]
    for rnd in range(9, 0, -1):
        state = [[state[row][(col - row) % 4] for col in range(4)] for row in range(4)]  # InvShiftRows
        state = [[_AES_INV_SBOX[state[row][col]] for col in range(4)] for row in range(4)]  # InvSubBytes
        state = [[state[row][col] ^ round_keys[rnd][row][col] for col in range(4)] for row in range(4)]
        for col in range(4):  # InvMixColumns
            a0, a1, a2, a3 = (state[row][col] for row in range(4))
            state[0][col] = _aes_gf_mul(a0, 14) ^ _aes_gf_mul(a1, 11) ^ _aes_gf_mul(a2, 13) ^ _aes_gf_mul(a3, 9)
            state[1][col] = _aes_gf_mul(a0, 9) ^ _aes_gf_mul(a1, 14) ^ _aes_gf_mul(a2, 11) ^ _aes_gf_mul(a3, 13)
            state[2][col] = _aes_gf_mul(a0, 13) ^ _aes_gf_mul(a1, 9) ^ _aes_gf_mul(a2, 14) ^ _aes_gf_mul(a3, 11)
            state[3][col] = _aes_gf_mul(a0, 11) ^ _aes_gf_mul(a1, 13) ^ _aes_gf_mul(a2, 9) ^ _aes_gf_mul(a3, 14)

    state = [[state[row][(col - row) % 4] for col in range(4)] for row in range(4)]  # InvShiftRows
    state = [[_AES_INV_SBOX[state[row][col]] for col in range(4)] for row in range(4)]  # InvSubBytes
    state = [[state[row][col] ^ round_keys[0][row][col] for col in range(4)] for row in range(4)]
    return bytes(state[row][col] for col in range(4) for row in range(4))


# --- Common-ticket parsing and titlekey decryption --------------------------

_TICKET_SIG_BLOCK_SIZES = {
    0x10000: 0x240,  # RSA-4096 SHA-1
    0x10001: 0x140,  # RSA-2048 SHA-1
    0x10002: 0x80,   # ECC-480 SHA-1
    0x10003: 0x240,  # RSA-4096 SHA-256
    0x10004: 0x140,  # RSA-2048 SHA-256
    0x10005: 0x80,   # ECC-480 SHA-256
    0x10006: 0x40,   # HMAC-160 SHA-1
}


def parse_common_ticket(ticket: bytes) -> tuple[bytes, bytes, int]:
    """Return (rights_id, encrypted_titlekey, master_key_revision) for a common ticket.

    Offsets verified against a real ACNH update ticket (RSA-2048, 0x2C0 bytes):
    title_key_type at data+0x141, master_key_revision at data+0x145, rights id
    at data+0x160, encrypted titlekey at data+0x40.
    """
    if len(ticket) < 0x140:
        raise SysAgentProtocolError("ticket is too short")
    sig_type = int.from_bytes(ticket[0:4], "little")
    data_offset = _TICKET_SIG_BLOCK_SIZES.get(sig_type)
    if data_offset is None:
        raise SysAgentProtocolError(f"unsupported ticket signature type 0x{sig_type:X}")
    if len(ticket) < data_offset + 0x180:
        raise SysAgentProtocolError("ticket data is truncated")
    data = ticket[data_offset:data_offset + 0x180]
    if data[0x141] != 0:
        raise SysAgentProtocolError("personalized ticket is not supported (common ticket required)")
    rights_id = data[0x160:0x170]
    encrypted_key = data[0x40:0x50]
    key_gen = data[0x145]
    return rights_id, encrypted_key, key_gen


def load_titlekek(keys_path: str, key_gen: int) -> bytes:
    """Read titlekek_<keygen> from a prod.keys file (host-side, read-only)."""
    wanted = f"titlekek_{key_gen:02x}"
    with open(keys_path, "r", encoding="utf-8", errors="replace") as keys_file:
        for raw_line in keys_file:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip().lower() == wanted:
                try:
                    key = bytes.fromhex(value.strip())
                except ValueError as exc:
                    raise SysAgentProtocolError(
                        f"invalid {wanted} value in {keys_path}") from exc
                if len(key) != 16:
                    raise SysAgentProtocolError(f"{wanted} in {keys_path} is not 16 bytes")
                return key
    raise SysAgentProtocolError(f"{wanted} not found in {keys_path}")


def load_titlekey_from_titlekeys(keys_path: str, rights_id_hex: str) -> bytes | None:
    """Read a decrypted 16-byte titlekey for a rights id from a title.keys file.

    Line format is the standard ``<rights id hex> = <16-byte key hex>``. Returns
    None when the rights id is absent so callers can fall back to ticket
    decryption; raises on a malformed matching line.
    """
    wanted = rights_id_hex.upper()
    with open(keys_path, "r", encoding="utf-8", errors="replace") as keys_file:
        for raw_line in keys_file:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip().upper() != wanted:
                continue
            try:
                key = bytes.fromhex(value.strip())
            except ValueError as exc:
                raise SysAgentProtocolError(
                    f"invalid key for {rights_id_hex} in {keys_path}") from exc
            if len(key) != 16:
                raise SysAgentProtocolError(
                    f"key for {rights_id_hex} in {keys_path} is not 16 bytes")
            return key
    return None


def load_titlekey_block_from_blocks(blocks_path: str, rights_id_hex: str) -> tuple[str, int] | None:
    """Read a titlekey-block source entry from a title.keys file.

    Customized line format (same file as ``title.keys``):
    ``<rights id hex> = <16-byte title_key_block hex> <keygen>``.
    The block is the ticket's encrypted title key (titlekek-wrapped); the
    console computes the current boot's AccessKey from it via spl:es
    ``PrepareCommonEsTitleKey``, so this material is boot-independent.
    Returns None when the matching line is a plain 16-byte titlekey (the
    standard format) or when the rights id is absent.
    """
    wanted = rights_id_hex.upper()
    with open(blocks_path, "r", encoding="utf-8", errors="replace") as blocks_file:
        for raw_line in blocks_file:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip().upper() != wanted:
                continue
            parts = value.strip().split()
            if len(parts) == 1:
                # Standard title.keys line (plain decrypted key): not a
                # block source; let the legacy registration path handle it.
                return None
            if len(parts) != 2:
                raise SysAgentProtocolError(
                    f"invalid titlekey-block entry for {rights_id_hex} in {blocks_path}")
            block_hex = parts[0]
            if len(block_hex) != 32:
                raise SysAgentProtocolError(
                    f"titlekey block for {rights_id_hex} in {blocks_path} is not 16 bytes")
            try:
                gen = int(parts[1], 0)
            except ValueError as exc:
                raise SysAgentProtocolError(
                    f"invalid keygen for {rights_id_hex} in {blocks_path}") from exc
            return block_hex.upper(), gen
    return None


def decrypt_ticket_titlekey(titlekek: bytes, encrypted_key: bytes) -> bytes:
    return _aes128_ecb_decrypt_block(titlekek, encrypted_key)


def parse_response(line: str) -> dict[str, str]:
    parts = line.strip().split()
    if not parts or parts[0] not in {"OK", "ERR"}:
        raise SysAgentProtocolError(f"invalid response: {line!r}")
    # The envelope lives under "ok" so it cannot collide with server fields
    # such as "result=" (native Result codes) or "status=" (networkStatus).
    response = {"ok": parts[0]}
    for part in parts[1:]:
        if "=" not in part:
            raise SysAgentProtocolError(f"invalid response field: {part!r}")
        key, value = part.split("=", 1)
        if not key or key in response:
            raise SysAgentProtocolError(f"invalid response key: {key!r}")
        response[key] = value
    return response


def require_ok(response: dict[str, str]) -> dict[str, str]:
    if response.get("ok") != "OK":
        details = ", ".join(
            f"{key}={response[key]}"
            for key in ("stage", "result", "attempts") if key in response
        )
        suffix = f" ({details})" if details else ""
        raise SysAgentProtocolError(
            f"sys-agent error: {response.get('code', 'UNKNOWN')}{suffix}"
        )
    return response


def parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError:
        return int(value, 16)


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

    def _bare_command(self, command_line: str, expect_output: bool = True) -> str | None:
        """Send a legacy command that answers with a bare line instead of OK/ERR.

        Legacy commands print raw hex, plain text, or nothing. ``expect_output``
        controls whether a response line is read; an empty response line is
        treated as a failed command and raises.
        """
        if "\r" in command_line or "\n" in command_line:
            raise ValueError("command must be a single line")
        self.connect()
        assert self._socket is not None
        self._socket.sendall(command_line.encode("ascii") + b"\r\n")
        if not expect_output:
            return None
        line = self._readline().decode("utf-8", errors="replace")
        if not line:
            raise SysAgentProtocolError(f"command returned an empty response: {command_line}")
        return line

    def raw_command(self, command_line: str) -> str | None:
        """Send any single-line command and return the raw response (or None on timeout)."""
        if "\r" in command_line or "\n" in command_line:
            raise ValueError("command must be a single line")
        self.connect()
        assert self._socket is not None
        self._socket.sendall(command_line.encode("ascii") + b"\r\n")
        try:
            return self._readline().decode("utf-8", errors="replace")
        except socket.timeout:
            return None

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

    def audio_volume(self, volume: int | None = None) -> dict[str, str]:
        if volume is not None and not (0 <= volume <= 100):
            raise ValueError("volume must be in 0..100")
        command = "audioVolume" if volume is None else f"audioVolume {volume}"
        return require_ok(self.command(command))

    def audio_mute(self, state: str | None = None) -> dict[str, str]:
        if state is not None and state not in {"enabled", "disabled"}:
            raise ValueError("state must be enabled or disabled")
        command = "audioMute" if state is None else f"audioMute {state}"
        return require_ok(self.command(command))

    # ---- Legacy memory read -------------------------------------------------

    def peek(self, offset: int, size: int) -> str:
        return self._bare_command(f"peek 0x{offset:X} 0x{size:X}")

    def peek_absolute(self, address: int, size: int) -> str:
        return self._bare_command(f"peekAbsolute 0x{address:X} 0x{size:X}")

    def peek_main(self, offset: int, size: int) -> str:
        return self._bare_command(f"peekMain 0x{offset:X} 0x{size:X}")

    def peek_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekMulti {args}")

    def peek_absolute_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekAbsoluteMulti {args}")

    def peek_main_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekMainMulti {args}")

    def pointer(self, jumps: Sequence[int]) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps)
        return self._bare_command(f"pointer {args}")

    def pointer_all(self, jumps: Sequence[int], final: int) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerAll {args}")

    def pointer_relative(self, jumps: Sequence[int], final: int) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerRelative {args}")

    def pointer_peek(self, size: int, jumps: Sequence[int], final: int) -> str:
        args = f"0x{size:X} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerPeek {args}")

    def pointer_peek_multi(self, chains: Sequence[tuple[int, Sequence[int], int]]) -> str:
        blocks = []
        for size, jumps, final in chains:
            args = f"0x{size:X} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
            blocks.append(args)
        return self._bare_command("pointerPeekMulti " + " * ".join(blocks))

    # ---- Legacy memory write ------------------------------------------------

    def poke(self, offset: int, data: bytes) -> None:
        self._bare_command(f"poke 0x{offset:X} {data.hex().upper()}", expect_output=False)

    def poke_absolute(self, address: int, data: bytes) -> None:
        self._bare_command(f"pokeAbsolute 0x{address:X} {data.hex().upper()}", expect_output=False)

    def poke_main(self, offset: int, data: bytes) -> None:
        self._bare_command(f"pokeMain 0x{offset:X} {data.hex().upper()}", expect_output=False)

    def pointer_poke(self, data: bytes, jumps: Sequence[int], final: int) -> None:
        args = f"{data.hex().upper()} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        self._bare_command(f"pointerPoke {args}", expect_output=False)

    # ---- Freeze --------------------------------------------------------------

    def freeze(self, address: int, data: bytes) -> None:
        self._bare_command(f"freeze 0x{address:X} {data.hex().upper()}", expect_output=False)

    def unfreeze(self, address: int) -> None:
        self._bare_command(f"unFreeze 0x{address:X}", expect_output=False)

    def freeze_count(self) -> str:
        return self._bare_command("freezeCount")

    def freeze_clear(self) -> None:
        self._bare_command("freezeClear", expect_output=False)

    def freeze_pause(self) -> None:
        self._bare_command("freezePause", expect_output=False)

    def freeze_unpause(self) -> None:
        self._bare_command("freezeUnpause", expect_output=False)

    # ---- Controller input ----------------------------------------------------

    def press(self, button: str) -> None:
        self._bare_command(f"press {button}", expect_output=False)

    def release(self, button: str) -> None:
        self._bare_command(f"release {button}", expect_output=False)

    def click(self, button: str) -> None:
        self._bare_command(f"click {button}", expect_output=False)

    def set_stick(self, side: str, x: int, y: int) -> None:
        if side not in {"LEFT", "RIGHT"}:
            raise ValueError("side must be LEFT or RIGHT")
        if not (-0x8000 <= x <= 0x7FFF) or not (-0x8000 <= y <= 0x7FFF):
            raise ValueError("stick coordinates must be in -0x8000..0x7FFF")
        self._bare_command(f"setStick {side} {x} {y}", expect_output=False)

    def click_seq(self, sequence: str, wait_done: bool = True) -> str | None:
        line = self._bare_command(f"clickSeq {sequence}", expect_output=wait_done)
        if wait_done and line != "done":
            raise SysAgentProtocolError(f"expected 'done' from clickSeq, got {line!r}")
        return line

    def click_cancel(self) -> None:
        self._bare_command("clickCancel", expect_output=False)

    def detach_controller(self) -> None:
        self._bare_command("detachController", expect_output=False)

    def touch(self, points: Sequence[tuple[int, int]]) -> None:
        args = " ".join(f"{x} {y}" for x, y in points)
        self._bare_command(f"touch {args}", expect_output=False)

    def touch_hold(self, x: int, y: int, milliseconds: int) -> None:
        self._bare_command(f"touchHold {x} {y} {milliseconds}", expect_output=False)

    def touch_draw(self, points: Sequence[tuple[int, int]]) -> None:
        args = " ".join(f"{x} {y}" for x, y in points)
        self._bare_command(f"touchDraw {args}", expect_output=False)

    def touch_cancel(self) -> None:
        self._bare_command("touchCancel", expect_output=False)

    def key(self, keys: Sequence[int]) -> None:
        args = " ".join(str(key) for key in keys)
        self._bare_command(f"key {args}", expect_output=False)

    def key_mod(self, pairs: Sequence[tuple[int, int]]) -> None:
        args = " ".join(f"{key} {mod}" for key, mod in pairs)
        self._bare_command(f"keyMod {args}", expect_output=False)

    def key_multi(self, keys: Sequence[int]) -> None:
        args = " ".join(str(key) for key in keys)
        self._bare_command(f"keyMulti {args}", expect_output=False)

    # ---- Screen ---------------------------------------------------------------

    def screen_off(self) -> None:
        self._bare_command("screenOff", expect_output=False)

    def screen_on(self) -> None:
        self._bare_command("screenOn", expect_output=False)

    # ---- Utility --------------------------------------------------------------

    def get_title_id(self) -> str:
        return self._bare_command("getTitleID")

    def get_title_version(self) -> str:
        return self._bare_command("getTitleVersion")

    def get_system_language(self) -> str:
        return self._bare_command("getSystemLanguage")

    def get_build_id(self) -> str:
        return self._bare_command("getBuildID")

    def get_heap_base(self) -> str:
        return self._bare_command("getHeapBase")

    def get_main_nso_base(self) -> str:
        return self._bare_command("getMainNsoBase")

    def is_program_running(self, program_id: int) -> str:
        return self._bare_command(f"isProgramRunning 0x{program_id:X}")

    def game(self, field: str) -> str:
        if field not in {"icon", "version", "rating", "author", "name"}:
            raise ValueError("field must be icon, version, rating, author, or name")
        return self._bare_command(f"game {field}")

    def game_launch_headless(self, title_id: int, storage: str | None = None) -> dict[str, str]:
        if not 0 < title_id <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("title_id must be a positive 64-bit Title ID")
        if storage is not None and storage not in {"SdCard", "BuiltInUser", "GameCard", "None"}:
            raise ValueError("storage must be SdCard, BuiltInUser, GameCard, or None")
        command = f"gameLaunchHeadless 0x{title_id:016X}"
        if storage is not None:
            command += f" {storage}"
        return require_ok(self.command(command))

    def game_ticket_read(self, title_id: int) -> dict[str, str]:
        if not 0 < title_id <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("title_id must be a positive 64-bit Title ID")
        return require_ok(self.command(f"gameTicketRead 0x{title_id:016X}"))

    def game_external_key_register(self, rights_id_hex: str, title_key_hex: str) -> dict[str, str]:
        return require_ok(self.command(
            f"gameExternalKeyRegister {rights_id_hex} {title_key_hex}"))

    def game_external_key_unregister(self, rights_id_hex: str) -> dict[str, str]:
        """Remove a rights id's external key with fsp-srv 617."""
        return require_ok(self.command(f"gameExternalKeyUnregister {rights_id_hex}"))

    def game_external_key_prepare_common(
        self, rights_id_hex: str, block_hex: str, gen: int
    ) -> dict[str, str]:
        """Compute the current boot's AccessKey via spl:es and register it.

        ``block_hex`` is the ticket's 16-byte title_key_block; ``gen`` is the
        spl:es generation argument (the secure monitor subtracts one, so
        ``gen=11`` selects titlekek index 0x0A). The returned AccessKey is
        wrapped with this boot's random seal, so it is valid only for this
        boot — exactly like the key es provides during a manual launch.
        """
        return require_ok(self.command(
            f"gameExternalKeyPrepareCommon {rights_id_hex} {block_hex} {gen}"))

    def game_launch_headless_with_keys(
        self,
        title_id: int,
        storage: str | None = None,
        keys_path: str | None = None,
    ) -> dict[str, str]:
        """Launch headless after registering the update external key.

        Reads the update's encrypted ticket from the console, decrypts the
        titlekey on this host with ``titlekek_<keygen>`` from ``keys_path``
        (the kek never leaves the host), sends only the 16-byte titlekey back
        to sys-agent for fsp-srv ``RegisterExternalKey``, then launches.
        """
        if keys_path is None:
            raise ValueError("keys_path is required for external-key registration")
        ticket_response = self.game_ticket_read(title_id)
        rights_id_hex = ticket_response.get("rightsId", "")
        ticket_hex = ticket_response.get("ticket", "")
        if len(rights_id_hex) != 32 or len(ticket_hex) == 0:
            raise SysAgentProtocolError(
                "no common ticket returned for the update title; is the game "
                "owned digitally on this console?")
        try:
            ticket = bytes.fromhex(ticket_hex)
        except ValueError as exc:
            raise SysAgentProtocolError("sys-agent returned a malformed ticket") from exc

        ticket_rights_id, encrypted_key, key_gen = parse_common_ticket(ticket)
        if ticket_rights_id.hex().upper() != rights_id_hex.upper():
            raise SysAgentProtocolError(
                f"ticket rights id {ticket_rights_id.hex().upper()} does not match "
                f"update rights id {rights_id_hex}")

        titlekek = load_titlekek(keys_path, key_gen)
        title_key = decrypt_ticket_titlekey(titlekek, encrypted_key)
        self.game_external_key_register(rights_id_hex, title_key.hex().upper())

        result = self.game_launch_headless(title_id, storage=storage)
        result["externalKey"] = "ok"
        result["rightsId"] = rights_id_hex
        return result

    def resolve_update_rights_id(self, title_id: int) -> str:
        """Resolve the installed update's rights id (read-only probe)."""
        line = self._bare_command(f"gameExternalKeyProbe 0x{title_id:016X}")
        if line is None:
            raise SysAgentProtocolError(
                "gameExternalKeyProbe returned no response")
        match = re.search(r"\brightsId=([0-9A-Fa-f]{32})\b", line)
        if match is None:
            raise SysAgentProtocolError(
                "could not resolve the update rights id from gameExternalKeyProbe")
        return match.group(1).upper()

    def game_launch_headless_auto(
        self,
        title_id: int,
        storage: str | None = None,
        titlekeys_path: str | None = None,
        fallback_keys_path: str | None = None,
    ) -> dict[str, str]:
        """Launch headless, registering the update external key when available.

        Key priority:
        1. a customized title.keys line (``rightsId = title_key_block keygen``):
           the ticket's encrypted title key + keygen; the console computes the
           current boot's AccessKey via spl:es ``PrepareCommonEsTitleKey`` and
           registers it (works every boot, no manual game start needed);
        2. a decrypted titlekey from a title.keys file
           (``titlekeys_path``) — a boot-specific AccessKey value that goes
           stale after a reboot;
        3. ticket decryption with a prod.keys file (``fallback_keys_path``);
        4. a plain launch.
        """
        rights_id_hex = self.resolve_update_rights_id(title_id)
        source = "none"

        # Preferred: per-boot AccessKey computed by spl:es from the ticket's
        # encrypted title key block. No manual launch ever required.
        block_info = None
        if titlekeys_path is not None and os.path.isfile(titlekeys_path):
            block_info = load_titlekey_block_from_blocks(
                titlekeys_path, rights_id_hex)
        if block_info is not None:
            block_hex, gen = block_info
            # Drop any stale direct registration first (see note below).
            try:
                self.game_external_key_unregister(rights_id_hex)
            except SysAgentProtocolError:
                pass
            self.game_external_key_prepare_common(rights_id_hex, block_hex, gen)
            source = "titlekey.block+spl"
            try:
                result = self.game_launch_headless(title_id, storage=storage)
            except Exception:
                # The launch failed, so the key we just registered is useless;
                # drop it so a later manual launch does not hit es's
                # NcaExternalKeyInconsistent assertion.
                try:
                    self.game_external_key_unregister(rights_id_hex)
                except SysAgentProtocolError:
                    pass
                raise
            result["rightsId"] = rights_id_hex
            result["externalKey"] = source
            return result

        title_key = None
        if titlekeys_path is not None:
            title_key = load_titlekey_from_titlekeys(titlekeys_path, rights_id_hex)
            if title_key is not None:
                source = "title.keys"
        if title_key is None and fallback_keys_path is not None:
            result = self.game_launch_headless_with_keys(
                title_id, storage=storage, keys_path=fallback_keys_path)
            result["externalKey"] = "ticket+titlekek"
            return result
        if title_key is not None:
            # Drop any stale direct registration first. A key registered
            # directly via fsp-srv 607 is NOT cleaned up when a game exits, so
            # it would make es's own registration at the next manual launch
            # fail with NcaExternalKeyInconsistent (which crashes es).
            try:
                self.game_external_key_unregister(rights_id_hex)
            except SysAgentProtocolError:
                pass  # nothing registered yet is fine
            self.game_external_key_register(rights_id_hex, title_key.hex().upper())
        try:
            result = self.game_launch_headless(title_id, storage=storage)
        except Exception:
            # The launch failed, so the key we just registered is useless;
            # drop it so a later manual launch does not hit es's
            # NcaExternalKeyInconsistent assertion.
            try:
                self.game_external_key_unregister(rights_id_hex)
            except SysAgentProtocolError:
                pass
            raise
        result["rightsId"] = rights_id_hex
        result["externalKey"] = source
        return result

    def get_version(self) -> str:
        return self._bare_command("getVersion")

    def charge(self) -> str:
        return self._bare_command("charge")

    def fd_count(self) -> str:
        return self._bare_command("fdCount")

    def configure(self, parameter: str, value: int) -> None:
        if not parameter or any(char.isspace() for char in parameter):
            raise ValueError("parameter must be a single token")
        self._bare_command(f"configure {parameter} {value}", expect_output=False)

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


def pair_values(values: Sequence[int]) -> list[tuple[int, int]]:
    if len(values) % 2:
        raise ValueError("expected an even number of arguments")
    return list(zip(values[0::2], values[1::2]))


def parse_pointer_chains(tokens: Sequence[str]) -> list[tuple[int, list[int], int]]:
    """Split ``pointer-peek-multi`` arguments on literal ``*`` tokens."""
    groups: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token == "*":
            groups.append(current)
            current = []
        else:
            current.append(token)
    groups.append(current)
    chains: list[tuple[int, list[int], int]] = []
    for group in groups:
        if len(group) < 3:
            raise ValueError("each pointer chain needs size, at least one jump, and a final offset")
        values = [int(value, 0) for value in group]
        chains.append((values[0], values[1:-1], values[-1]))
    return chains


# ---------------------------------------------------------------------------
# Declarative CLI: one Command per sys-agent command
# ---------------------------------------------------------------------------


_UNSET = object()


@dataclasses.dataclass(frozen=True)
class Arg:
    """A single argument (positional or option) of a registered subcommand."""

    name: str
    help: str
    nargs: str | int | None = None
    choices: tuple[str, ...] | None = None
    type: Callable[[str], Any] | None = None
    default: Any = _UNSET
    action: str | None = None
    flags: tuple[str, ...] = ()
    metavar: str | None = None

    def add_to(self, parser: argparse.ArgumentParser) -> None:
        kwargs: dict[str, Any] = {"help": self.help}
        if self.nargs is not None:
            kwargs["nargs"] = self.nargs
        if self.choices is not None:
            kwargs["choices"] = self.choices
        if self.type is not None:
            kwargs["type"] = self.type
        if self.default is not _UNSET:
            kwargs["default"] = self.default
        if self.action is not None:
            kwargs["action"] = self.action
        if self.metavar is not None:
            kwargs["metavar"] = self.metavar
        if self.flags:
            parser.add_argument(*self.flags, dest=self.name, **kwargs)
        else:
            parser.add_argument(self.name, **kwargs)


@dataclasses.dataclass(frozen=True)
class Command:
    """A leaf subcommand mirroring one sys-agent command."""

    name: str
    help: str
    handler: Callable[[SysAgentClient, argparse.Namespace], int | None]
    args: tuple[Arg, ...] = ()
    description: str = ""


@dataclasses.dataclass(frozen=True)
class CommandGroup:
    """A named namespace of related leaf subcommands."""

    name: str
    help: str
    children: tuple[Command, ...]


# ---- handlers ---------------------------------------------------------------


def _print_result(method_name: str) -> Callable[[SysAgentClient, argparse.Namespace], None]:
    def handler(client: SysAgentClient, args: argparse.Namespace) -> None:
        print(getattr(client, method_name)())
    return handler


def _silent(method_name: str) -> Callable[[SysAgentClient, argparse.Namespace], None]:
    def handler(client: SysAgentClient, args: argparse.Namespace) -> None:
        getattr(client, method_name)()
    return handler


def _cmd_capabilities(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.capabilities())


def _cmd_backend_status(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(dataclasses.asdict(client.backend_status()))


def _cmd_backend_set(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(dataclasses.asdict(client.set_backend_policy(args.policy)))


def _cmd_backend_probe(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(dataclasses.asdict(client.probe_backend()))


def _cmd_system_capabilities(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.system_capabilities())


def _cmd_system_query(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.system_query(args.query))


def _cmd_process_list(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.process_list(args.offset, args.count))


def _cmd_system_action(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.system_action(args.command))


def _cmd_wireless(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.set_wireless(args.state == "enabled"))


def _cmd_lock_screen(client: SysAgentClient, args: argparse.Namespace) -> None:
    response = client.lock_screen_status() if args.state == "status" \
        else client.set_lock_screen(args.state == "enabled")
    print_fields(response)


def _cmd_audio_volume(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.audio_volume(args.volume))


def _cmd_audio_mute(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.audio_mute(args.state))


def _cmd_screenshot(client: SysAgentClient, args: argparse.Namespace) -> None:
    data = client.screenshot()
    output = args.output or f"screenshot-{int(time.time())}.jpg"
    with open(output, "wb") as image:
        image.write(data)
    print(output)


def _cmd_peek(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek(args.offset, args.size))


def _cmd_peek_absolute(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek_absolute(args.address, args.size))


def _cmd_peek_main(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek_main(args.offset, args.size))


def _cmd_peek_multi(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek_multi(pair_values(args.pairs)))


def _cmd_peek_absolute_multi(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek_absolute_multi(pair_values(args.pairs)))


def _cmd_peek_main_multi(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.peek_main_multi(pair_values(args.pairs)))


def _cmd_poke(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.poke(args.offset, args.data)


def _cmd_poke_absolute(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.poke_absolute(args.address, args.data)


def _cmd_poke_main(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.poke_main(args.offset, args.data)


def _cmd_pointer(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.pointer(args.jumps))


def _cmd_pointer_all(client: SysAgentClient, args: argparse.Namespace) -> None:
    if len(args.args) < 2:
        raise ValueError("pointer-all needs at least one jump and a final offset")
    print(client.pointer_all(args.args[:-1], args.args[-1]))


def _cmd_pointer_relative(client: SysAgentClient, args: argparse.Namespace) -> None:
    if len(args.args) < 2:
        raise ValueError("pointer-relative needs at least one jump and a final offset")
    print(client.pointer_relative(args.args[:-1], args.args[-1]))


def _cmd_pointer_peek(client: SysAgentClient, args: argparse.Namespace) -> None:
    if len(args.args) < 3:
        raise ValueError("pointer-peek needs size, at least one jump, and a final offset")
    print(client.pointer_peek(args.args[0], args.args[1:-1], args.args[-1]))


def _cmd_pointer_peek_multi(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.pointer_peek_multi(parse_pointer_chains(args.args)))


def _cmd_pointer_poke(client: SysAgentClient, args: argparse.Namespace) -> None:
    if len(args.args) < 2:
        raise ValueError("pointer-poke needs at least one jump and a final offset")
    client.pointer_poke(args.data, args.args[:-1], args.args[-1])


def _cmd_freeze(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.freeze(args.address, args.data)


def _cmd_unfreeze(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.unfreeze(args.address)


def _cmd_press(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.press(args.button)


def _cmd_release(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.release(args.button)


def _cmd_click(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.click(args.button)


def _cmd_set_stick(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.set_stick(args.side, args.x, args.y)


def _cmd_click_seq(client: SysAgentClient, args: argparse.Namespace) -> None:
    line = client.click_seq(args.sequence, wait_done=not args.no_wait)
    if line is not None:
        print(line)


def _cmd_touch(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.touch(pair_values(args.points))


def _cmd_touch_hold(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.touch_hold(args.x, args.y, args.milliseconds)


def _cmd_touch_draw(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.touch_draw(pair_values(args.points))


def _cmd_key(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.key(args.keys)


def _cmd_key_mod(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.key_mod(pair_values(args.pairs))


def _cmd_key_multi(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.key_multi(args.keys)


def _require_game_running(client: SysAgentClient, command: str) -> None:
    status = client.system_query("application")
    running = status.get("running", "NA")
    if running != "1":
        raise SysAgentProtocolError(
            f"{command} requires a running game (applicationStatus running={running})")


def _cmd_game_meta(field: str) -> Callable[[SysAgentClient, argparse.Namespace], None]:
    def handler(client: SysAgentClient, args: argparse.Namespace) -> None:
        _require_game_running(client, f"game {field}")
        print(client.game(field))
    return handler


def _cmd_game_icon(client: SysAgentClient, args: argparse.Namespace) -> None:
    _require_game_running(client, "game icon")
    data = bytes.fromhex(client.game("icon"))
    output = args.output or f"game-icon-{int(time.time())}.bin"
    with open(output, "wb") as image:
        image.write(data)
    print(output)


def _cmd_game_status(client: SysAgentClient, args: argparse.Namespace) -> None:
    _require_game_running(client, "game status")
    print_fields(client.system_query("application"))


def _cmd_game_launch_headless(client: SysAgentClient, args: argparse.Namespace) -> None:
    titlekeys_path = args.titlekeys
    if titlekeys_path is None:
        titlekeys_path = _default_titlekeys_path()
    if titlekeys_path is not None and os.path.isfile(titlekeys_path):
        result = client.game_launch_headless_auto(
            args.title_id, storage=args.storage,
            titlekeys_path=titlekeys_path, fallback_keys_path=args.keys)
    elif args.keys:
        result = client.game_launch_headless_with_keys(
            args.title_id, storage=args.storage, keys_path=args.keys)
    else:
        result = client.game_launch_headless(args.title_id, storage=args.storage)
    print_fields(result)


def _default_titlekeys_path() -> str | None:
    """Locate the workspace title.keys mirror for automatic key registration."""
    # realpath follows PATH symlinks (e.g. ~/bin/sysagent.py), so the
    # file-relative candidate works regardless of how the client was invoked.
    client_dir = os.path.dirname(os.path.realpath(__file__))
    candidates = (
        os.path.join(os.getcwd(), "SDcard", "switch", "title.keys"),
        os.path.join(client_dir, "..", "..", "..", "SDcard", "switch", "title.keys"),
    )
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def _cmd_game_terminate(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.system_action("terminate-application"))


def _cmd_is_program_running(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.is_program_running(args.program_id))


def _cmd_configure(client: SysAgentClient, args: argparse.Namespace) -> None:
    known_params = {
        "mainLoopSleepTime", "buttonClickSleepTime", "echoCommands",
        "printDebugResultCodes", "keySleepTime", "fingerDiameter",
        "pollRate", "freezeRate", "controllerType",
    }
    if args.parameter not in known_params:
        raise ValueError(
            f"unsupported configure parameter: {args.parameter} "
            f"(known: {', '.join(sorted(known_params))})")
    client.configure(args.parameter, args.value)


def _cmd_begin(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.begin_unknown(args.type, args.region, args.offset, args.size,
                               alignment=args.alignment, pause=args.pause))


def _cmd_refine(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.refine(args.session, args.mode, args.value, pause=args.pause)


def _cmd_raw(client: SysAgentClient, args: argparse.Namespace) -> None:
    line = client.raw_command(" ".join(args.command))
    if line is not None:
        print(line)


def _cmd_start(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.start(args.start, args.end, args.pattern))


def _cmd_start_region(client: SysAgentClient, args: argparse.Namespace) -> None:
    value: str | bytes = parse_pattern(args.value) if args.type == "bytes" else args.value
    print(client.start_region(args.type, args.region, args.offset, args.size,
                              value, args.alignment))


def _cmd_status(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(dataclasses.asdict(client.status(args.session)))


def _cmd_cancel(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.cancel(args.session)


def _cmd_close(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.close_session(args.session)


def _cmd_results(client: SysAgentClient, args: argparse.Namespace) -> None:
    addresses, stored = client.results(args.session, args.offset, args.count)
    print(f"stored={stored}")
    for address in addresses:
        print(f"0x{address:016X}")


def _cmd_search(client: SysAgentClient, args: argparse.Namespace) -> int:
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


# ---- command registry -------------------------------------------------------


COMMANDS: tuple[Command | CommandGroup, ...] = (
    CommandGroup("backend", "Inspect or configure the process-memory backend", (
        Command("status", "Show the backend policy and state", _cmd_backend_status),
        Command("set", "Set the backend policy", _cmd_backend_set,
                (Arg("policy", "auto, dmnt, or direct", choices=("auto", "dmnt", "direct")),)),
        Command("probe", "Probe the configured backend", _cmd_backend_probe),
    )),

    CommandGroup("system", "Query or control system state", (
        Command("capabilities", "Show system-management capabilities",
                _cmd_system_capabilities),
        Command("query", "Query grouped system state", _cmd_system_query,
                (Arg("query", "info, time, power, storage, network, network-profile, "
                              "account, or application",
                     choices=("info", "time", "power", "storage", "network",
                              "network-profile", "account", "application")),)),
        Command("process-list", "List running processes (PID:TitleID)", _cmd_process_list,
                (Arg("offset", "result page offset", type=parse_int, default=0,
                     flags=("--offset",)),
                 Arg("count", "page size, 1..64", type=parse_int, default=64,
                     flags=("--count",)))),
        Command("action", "Run a system action", _cmd_system_action,
                (Arg("command", "reboot, shutdown, sleep, reboot-emummc, or "
                                "terminate-application",
                     choices=("reboot", "shutdown", "sleep", "reboot-emummc",
                              "terminate-application")),)),
        Command("wireless", "Enable or disable wireless communication", _cmd_wireless,
                (Arg("state", "enabled or disabled", choices=("enabled", "disabled")),)),
        Command("lock-screen", "Show or change the lock-screen flag", _cmd_lock_screen,
                (Arg("state", "status, enabled, or disabled",
                     choices=("status", "enabled", "disabled")),)),
    )),

    CommandGroup("audio", "Query or control system audio", (
        Command("volume", "Show or set the system master volume (0-100)",
                _cmd_audio_volume,
                (Arg("volume", "0-100 to set; omit to query", nargs="?", type=parse_int,
                     default=None),)),
        Command("mute", "Show or set the current output target mute state",
                _cmd_audio_mute,
                (Arg("state", "enabled or disabled; omit to query", nargs="?",
                     choices=("enabled", "disabled"), default=None),)),
    )),

    CommandGroup("game", "Launch, close, or inspect the running game", (
        Command("status", "Show the running game identity and memory layout",
                _cmd_game_status),
        Command("launch-headless", "Experimental: start a game process without showing it "
                                   "on screen (headless; foreground launch is not possible "
                                   "from a sysmodule)", _cmd_game_launch_headless,
                (Arg("title_id", "Title ID (hex or decimal)", type=parse_int),
                 Arg("storage", "Storage to force (auto-detected when omitted)",
                     nargs="?", choices=("SdCard", "BuiltInUser", "GameCard", "None"),
                     default=None),
                 Arg("keys", "prod.keys path; read the update ticket on the console, decrypt "
                             "the titlekey on this host, and register it before launching",
                     flags=("--keys",), default=None),
                 Arg("titlekeys", "title.keys path with decrypted titlekeys "
                                  "(rightsId = key, or customized "
                                  "rightsId = title_key_block keygen for "
                                  "per-boot spl:es AccessKey computation); "
                                  "auto-registers before launching",
                     flags=("--titlekeys",), default=None),)),
        Command("terminate", "Experimental: force-close the foreground game (system-level "
                             "final termination; hard kill: the Switch shows the "
                             "software-error dialog, then returns to the game-selection "
                             "screen)",
                _cmd_game_terminate),
        Command("name", "Show the running game name", _cmd_game_meta("name")),
        Command("author", "Show the running game author", _cmd_game_meta("author")),
        Command("rating", "Show the running game age rating", _cmd_game_meta("rating")),
        Command("version", "Show the running game version", _cmd_game_meta("version")),
        Command("icon", "Dump the running game icon as a binary file", _cmd_game_icon,
                (Arg("output", "write the icon to this path "
                               "(default: game-icon-<unix time>.bin)",
                     flags=("--output",)),)),
    )),

    CommandGroup("memory", "Read or write process memory", (
        Command("peek", "Read bytes relative to the heap", _cmd_peek,
                (Arg("offset", "heap-relative address", type=parse_int),
                 Arg("size", "byte count", type=parse_int))),
        Command("peek-absolute", "Read bytes from an absolute address", _cmd_peek_absolute,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("size", "byte count", type=parse_int))),
        Command("peek-main", "Read bytes relative to the main NSO base", _cmd_peek_main,
                (Arg("offset", "main-relative address", type=parse_int),
                 Arg("size", "byte count", type=parse_int))),
        Command("peek-multi", "Read multiple heap ranges in one request", _cmd_peek_multi,
                (Arg("pairs", "address size address size ...", nargs="+", type=parse_int),)),
        Command("peek-absolute-multi", "Read multiple absolute ranges in one request",
                _cmd_peek_absolute_multi,
                (Arg("pairs", "address size address size ...", nargs="+", type=parse_int),)),
        Command("peek-main-multi", "Read multiple main-relative ranges in one request",
                _cmd_peek_main_multi,
                (Arg("pairs", "address size address size ...", nargs="+", type=parse_int),)),
        Command("poke", "Write bytes relative to the heap", _cmd_poke,
                (Arg("offset", "heap-relative address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern))),
        Command("poke-absolute", "Write bytes at an absolute address", _cmd_poke_absolute,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern))),
        Command("poke-main", "Write bytes relative to the main NSO base", _cmd_poke_main,
                (Arg("offset", "main-relative address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern))),
        Command("pointer", "Resolve a pointer chain to an absolute address", _cmd_pointer,
                (Arg("jumps", "main-relative first jump followed by offsets", nargs="+",
                     type=parse_int),)),
        Command("pointer-all", "Resolve a pointer chain plus a final offset",
                _cmd_pointer_all,
                (Arg("args", "jumps... final-offset", nargs="+", type=parse_int),)),
        Command("pointer-relative", "Resolve a pointer chain relative to the heap",
                _cmd_pointer_relative,
                (Arg("args", "jumps... final-offset", nargs="+", type=parse_int),)),
        Command("pointer-peek", "Read bytes through a pointer chain", _cmd_pointer_peek,
                (Arg("args", "size jumps... final-offset", nargs="+", type=parse_int),)),
        Command("pointer-peek-multi", "Read bytes through several pointer chains",
                _cmd_pointer_peek_multi,
                (Arg("args", "size jumps... final [* size jumps... final] ...", nargs="+"),),
                "Separate chains with a literal * argument."),
        Command("pointer-poke", "Write bytes through a pointer chain", _cmd_pointer_poke,
                (Arg("data", "hex bytes to write", type=parse_pattern),
                 Arg("args", "jumps... final-offset", nargs="+", type=parse_int))),
    )),

    CommandGroup("freeze", "Manage value freezing", (
        Command("add", "Freeze a value at an absolute address", _cmd_freeze,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern))),
        Command("remove", "Unfreeze an absolute address", _cmd_unfreeze,
                (Arg("address", "absolute address", type=parse_int),)),
        Command("count", "Show the number of frozen addresses",
                _print_result("freeze_count")),
        Command("clear", "Unfreeze every address", _silent("freeze_clear")),
        Command("pause", "Pause the freeze worker", _silent("freeze_pause")),
        Command("resume", "Resume the freeze worker", _silent("freeze_unpause")),
    )),

    CommandGroup("input", "Send controller, touch, or keyboard input", (
        Command("press", "Press and hold a button", _cmd_press,
                (Arg("button", "HidNpadButton name, e.g. A"),)),
        Command("release", "Release a held button", _cmd_release,
                (Arg("button", "HidNpadButton name, e.g. A"),)),
        Command("click", "Press and release a button", _cmd_click,
                (Arg("button", "HidNpadButton name, e.g. A"),)),
        Command("set-stick", "Set a stick position", _cmd_set_stick,
                (Arg("side", "LEFT or RIGHT", choices=("LEFT", "RIGHT")),
                 Arg("x", "X coordinate in -0x8000..0x7FFF", type=parse_int),
                 Arg("y", "Y coordinate in -0x8000..0x7FFF", type=parse_int))),
        Command("click-seq", "Run a comma-separated input sequence", _cmd_click_seq,
                (Arg("sequence", "e.g. A,W1000,B,+X,-X,%%5000,1500"),
                 Arg("no_wait", "send without waiting for 'done'", action="store_true",
                     default=False, flags=("--no-wait",))),
                "Tokens: button=click, +button=press, -button=release, Wms=wait, "
                "%LX,LY left stick, &RX,RY right stick."),
        Command("click-cancel", "Interrupt the current click sequence",
                _silent("click_cancel")),
        Command("detach-controller", "Force-detach the virtual controller",
                _silent("detach_controller")),
        Command("touch", "Tap touchscreen points", _cmd_touch,
                (Arg("points", "x y x y ...", nargs="+", type=parse_int),)),
        Command("touch-hold", "Hold a touchscreen point", _cmd_touch_hold,
                (Arg("x", "X in 0..1280", type=parse_int),
                 Arg("y", "Y in 0..720", type=parse_int),
                 Arg("milliseconds", "hold duration in ms", type=parse_int))),
        Command("touch-draw", "Draw a touch path", _cmd_touch_draw,
                (Arg("points", "x y x y ...", nargs="+", type=parse_int),)),
        Command("touch-cancel", "Cancel the current touch operation",
                _silent("touch_cancel")),
        Command("key", "Type keys in sequence", _cmd_key,
                (Arg("keys", "HidKeyboardKey values", nargs="+", type=parse_int),)),
        Command("key-mod", "Type keys with modifiers", _cmd_key_mod,
                (Arg("pairs", "key modifier key modifier ...", nargs="+", type=parse_int),)),
        Command("key-multi", "Press several keys at once", _cmd_key_multi,
                (Arg("keys", "HidKeyboardKey values", nargs="+", type=parse_int),)),
    )),

    CommandGroup("screen", "Capture or control the screen", (
        Command("capture", "Capture the current screen as a JPEG", _cmd_screenshot,
                (Arg("output", "write the JPEG to this path "
                               "(default: screenshot-<unix time>.jpg)",
                     flags=("--output",)),)),
        Command("off", "Turn the screen off", _silent("screen_off")),
        Command("on", "Turn the screen on", _silent("screen_on")),
    )),

    CommandGroup("utility", "Show device and application information", (
        Command("version", "Show the sys-agent version", _print_result("get_version")),
        Command("title-id", "Show the running title ID", _print_result("get_title_id")),
        Command("title-version", "Show the running title version",
                _print_result("get_title_version")),
        Command("system-language", "Show the system language",
                _print_result("get_system_language")),
        Command("build-id", "Show the application Build ID", _print_result("get_build_id")),
        Command("heap-base", "Show the heap base address", _print_result("get_heap_base")),
        Command("main-nso-base", "Show the main NSO base address",
                _print_result("get_main_nso_base")),
        Command("is-program-running", "Check whether a program is running",
                _cmd_is_program_running,
                (Arg("program_id", "program ID", type=parse_int),)),
        Command("charge", "Show the battery charge percentage", _print_result("charge")),
        Command("fd-count", "Show the open client socket count", _print_result("fd_count")),
    )),

    CommandGroup("config", "Change sys-agent runtime settings", (
        Command("set", "Change a timing or settings value", _cmd_configure,
                (Arg("parameter", "mainLoopSleepTime, buttonClickSleepTime, echoCommands, "
                                  "printDebugResultCodes, keySleepTime, fingerDiameter, "
                                  "pollRate, freezeRate, or controllerType"),
                 Arg("value", "new value", type=parse_int))),
    )),

    CommandGroup("search", "Exact and unknown-value memory search", (
        Command("capabilities", "Show search protocol capabilities", _cmd_capabilities),
        Command("start", "Start an exact byte search session", _cmd_start,
                (Arg("start", "absolute start address", type=parse_int),
                 Arg("end", "exclusive absolute end address", type=parse_int),
                 Arg("pattern", "hex byte pattern", type=parse_pattern))),
        Command("start-region", "Start a typed or region search session", _cmd_start_region,
                (Arg("type", "bytes, u8, u16, u32, or u64",
                     choices=("bytes", "u8", "u16", "u32", "u64")),
                 Arg("region", "absolute, heap, or main", choices=("absolute", "heap", "main")),
                 Arg("offset", "region-relative offset (absolute start for absolute)",
                     type=parse_int),
                 Arg("size", "search size in bytes", type=parse_int),
                 Arg("value", "hex pattern or unsigned integer value"),
                 Arg("alignment", "candidate alignment (default: natural)", type=parse_int,
                     flags=("--alignment",)))),
        Command("status", "Show a search session's status", _cmd_status,
                (Arg("session", "session ID", type=parse_int),)),
        Command("results", "Page stored search results", _cmd_results,
                (Arg("session", "session ID", type=parse_int),
                 Arg("offset", "zero-based result offset", type=parse_int, default=0,
                     flags=("--offset",)),
                 Arg("count", "page size, 1..256", type=parse_int, default=256,
                     flags=("--count",)))),
        Command("cancel", "Request cancellation of a search session", _cmd_cancel,
                (Arg("session", "session ID", type=parse_int),)),
        Command("close", "Close a finished search session", _cmd_close,
                (Arg("session", "session ID", type=parse_int),)),
        Command("begin", "Start an SD-backed unknown-value search session", _cmd_begin,
                (Arg("type", "u8, u16, u32, or u64", choices=("u8", "u16", "u32", "u64")),
                 Arg("region", "absolute, heap, main, alias, or addressSpace",
                     choices=("absolute", "heap", "main", "alias", "addressSpace")),
                 Arg("offset", "region-relative offset", type=parse_int),
                 Arg("size", "scan size in bytes", type=parse_int),
                 Arg("alignment", "candidate alignment (default: value width)", type=parse_int,
                     flags=("--alignment",)),
                 Arg("pause", "pause the process during the scan", action="store_true",
                     default=False, flags=("--pause",)))),
        Command("refine", "Refine an unknown-value search session", _cmd_refine,
                (Arg("session", "session ID", type=parse_int),
                 Arg("mode", "exact, changed, unchanged, increased, or decreased",
                     choices=("exact", "changed", "unchanged", "increased", "decreased")),
                 Arg("value", "value for exact mode", nargs="?"),
                 Arg("pause", "pause the process during refinement", action="store_true",
                     default=False, flags=("--pause",)))),
        Command("run", "Run an exact search and print matching addresses", _cmd_search,
                (Arg("start", "absolute start address", type=parse_int),
                 Arg("end", "exclusive absolute end address", type=parse_int),
                 Arg("pattern", "hex byte pattern", type=parse_pattern),
                 Arg("poll_interval", "status poll interval in seconds", type=float,
                     default=0.25, flags=("--poll-interval",)),
                 Arg("page_size", "result page size, 1..256", type=parse_int, default=256,
                     flags=("--page-size",)))),
    )),

    Command("raw", "Send any single-line command and print the raw response", _cmd_raw,
            (Arg("command", "command words to join and send", nargs="+"),)),
)

COMMAND_BY_NAME: dict[str, Command] = {}
for _item in COMMANDS:
    if isinstance(_item, CommandGroup):
        for _child in _item.children:
            COMMAND_BY_NAME[f"{_item.name} {_child.name}"] = _child
    else:
        COMMAND_BY_NAME[_item.name] = _item

GROUP_BY_NAME = {item.name: item for item in COMMANDS if isinstance(item, CommandGroup)}


class _HelpfulParser(argparse.ArgumentParser):
    """argparse parser whose missing-argument errors also print the full help.

    The default error path only prints the one-line usage summary when a
    required subcommand is omitted (``sysagent.py`` or ``sysagent.py game``),
    which hides the command list the caller actually needs. On that specific
    failure, print the complete ``-h`` text to stderr before exiting.
    """

    def error(self, message: str) -> None:
        if message.startswith("the following arguments are required:"):
            print(f"{self.prog}: error: {message}\n", file=sys.stderr)
            self.print_help(sys.stderr)
            self.exit(2)
        super().error(message)


def build_parser() -> argparse.ArgumentParser:
    parser = _HelpfulParser(description=__doc__)
    parser.add_argument("--host", default="switch", help="sys-agent host (default: switch)")
    parser.add_argument("--port", type=int, default=6000,
                        help="sys-agent TCP port (default: 6000)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="socket timeout in seconds (default: 10.0)")
    subparsers = parser.add_subparsers(dest="action", required=True, metavar="COMMAND",
                                       title="commands", parser_class=_HelpfulParser)
    for item in COMMANDS:
        if isinstance(item, CommandGroup):
            group_parser = subparsers.add_parser(item.name, help=item.help,
                                                 description=item.help)
            group_subparsers = group_parser.add_subparsers(
                dest=f"{item.name}_action", required=True, metavar="COMMAND",
                title="commands")
            for child in item.children:
                child_parser = group_subparsers.add_parser(
                    child.name, help=child.help,
                    description=child.description or child.help)
                for argument in child.args:
                    argument.add_to(child_parser)
        else:
            subparser = subparsers.add_parser(item.name, help=item.help,
                                              description=item.description or item.help)
            for argument in item.args:
                argument.add_to(subparser)
    return parser


def _resolve_command(args: argparse.Namespace) -> Command:
    group = GROUP_BY_NAME.get(args.action)
    if group is None:
        return COMMAND_BY_NAME[args.action]
    child = getattr(args, f"{group.name}_action")
    return COMMAND_BY_NAME[f"{group.name} {child}"]


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    command = _resolve_command(args)
    try:
        with SysAgentClient(args.host, args.port, args.timeout) as client:
            return command.handler(client, args) or 0
    except (OSError, ValueError, SysAgentProtocolError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
