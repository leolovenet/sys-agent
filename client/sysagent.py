#!/usr/bin/env python3
"""Dependency-free client for the sys-agent Switch automation protocol."""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import socket
import sys
import textwrap
import time
from collections.abc import Iterator, Sequence
from datetime import datetime, timezone


TERMINAL_STATES = {"done", "cancelled", "error"}

# The client is versioned on its own: this number moves when this file changes, while the
# sysmodule on the console keeps its own version (whatever `getVersion` answers).  `version`
# prints both, labelled, so a mismatch is obvious; `--version` prints this one alone.
CLIENT_VERSION = "1.1.0"
TRACE_SCHEMA_VERSION = 1
TRACE_PROFILE_SCHEMA_VERSION = 2

# The console's default keySleepTime (25 ms) is fast enough for the home menu but too
# fast for the game's on-screen keyboard: measured in ACNH 3.0.3's chat field, 25/30 ms
# drop characters, 35 ms occasionally drops one, and 40 ms was clean in 10/10 runs.
# `type_text` temporarily raises the setting to this floor and restores it afterwards.
TEXT_MIN_KEY_SLEEP_MS = 50

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
        code = response.get("code", "UNKNOWN")
        details = ", ".join(
            f"{key}={response[key]}"
            for key in ("stage", "result", "attempts") if key in response
        )
        suffix = f" ({details})" if details else ""
        raise SysAgentProtocolError(f"sys-agent error: {code}{suffix}{_describe_error(response)}")
    return response


# Frequently seen native Result codes and `ERR code=` names. Each entry only adds
# a human-readable suffix; the raw code always stays in the message so it can
# still be looked up. Result keys are lower-case hex with the 0x prefix.
RESULT_CODE_NOTES = {
    "0x10801": "debug handle unavailable or already owned (dmnt:cht)",
    "0x10201": "debug handle is owned by an active watch/trace or another debugger",
    "0xf401": "debug handle already owned by another debugger (dmnt:cht)",
    "0x202": "filesystem: access denied, or the path name encoding is unsupported",
    "0x2f4471": "bluetooth: adapter is busy paging another link",
    "0x668ce": "screen capture: the debug unit flag is required",
    "0xf601": "the service closed the session",
}

ERROR_CODE_NOTES = {
    "READ_FAILED": "the target address could not be read",
    "READ_UNSTABLE": "two reads of the address disagreed; the region is changing",
    "WRITE_FAILED": "the target address could not be written",
    "READBACK_FAILED": "the write landed but the verification read-back failed",
    "WRITE_VERIFY_FAILED": "the write did not read back as sent (concurrent writer?)",
    "QUERY_FAILED": "the memory mapping for the address could not be queried",
    "NO_APP": "no foreground application is running",
    "BACKEND_UNAVAILABLE": "the process-memory backend could not be opened",
    "MODULES_UNAVAILABLE": "the module list could not be read",
    "INVALID_HEX_PAYLOAD": "the payload is not an even-length hex byte string",
    "INVALID_ADDRESS": "the address argument could not be parsed",
    "NO_HIT": "no watchpoint hit has been recorded yet",
    "NO_TRACE_EVENT": "no execution-trace event is available at that index",
    "DEBUG_TRACE_START_FAILED": "the execution tracer could not attach and arm",
    "DEBUG_TRACE_START_TIMEOUT": "the execution tracer did not finish arming in time",
    "TRACE_BUSY": "another watch/trace session is active or the trace parameters are invalid",
    "DEBUG_BUSY": "an active watch/trace owns the process debug handle",
    "INVALID_SNAPSHOT": "snapshot must be REG[+/-OFFSET]:SIZE, with REG in x0..x30, sp, pc, or lr",
    "DUPLICATE_TARGET": "trace target addresses must be distinct",
    "MULTITRACE_BUSY": "another watch/trace session is active or multitrace could not start",
    "MULTITRACE_START_FAILED": "the dual-process trace could not attach and arm its targets",
    "MULTITRACE_STOP_INCOMPLETE": "the dual-process trace did not release all debug state",
    "DEBUG_READ_FAILED": "the target process memory read failed",
    "WAIT_HOOK_FAILED": "the PM process-creation hook is unavailable or already in use",
    "PROCESS_START_FAILED": "PM could not release the suspended process",
    "WAIT_TIMEOUT": "the requested process was not created before the timeout",
}


def _describe_error(response: dict[str, str]) -> str:
    """Return a ' - explanation' suffix for an ERR envelope, or an empty string."""
    note = ERROR_CODE_NOTES.get(response.get("code", ""), "")
    if not note and "result" in response:
        note = RESULT_CODE_NOTES.get(response["result"].lower(), "")
    return f" - {note}" if note else ""


def parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError:
        return int(value, 16)


def parse_log_line(line: str) -> dict[str, str]:
    """Parse one `log <i> tick=... cmd=...` line."""
    if not line.startswith("log "):
        raise SysAgentProtocolError(f"invalid log line: {line!r}")
    index_text, _, rest = line[4:].partition(" tick=")
    tick, _, command = rest.partition(" cmd=")
    return {"index": index_text, "tick": tick, "cmd": command}


# Runtime settings the sysmodule's `configure` command accepts. Reading one back
# uses the same names, except for the release window: it is stored in
# milliseconds, so only `controllerIdleReleaseMs` can be reported exactly and the
# seconds name is a write-only alias for it.
CONFIG_PARAMETERS = frozenset({
    "mainLoopSleepTime", "buttonClickSleepTime", "echoCommands",
    "printDebugResultCodes", "keySleepTime", "fingerDiameter",
    "pollRate", "freezeRate", "controllerType",
    "controllerIdleRelease", "controllerIdleReleaseMs", "controllerTakeover",
})
CONFIG_READ_ALIASES = {"controllerIdleRelease": "controllerIdleReleaseMs"}


def parse_config_block(block: str) -> dict[str, str]:
    """Splits a `configure` answer (``name=value`` lines) into a mapping."""
    settings: dict[str, str] = {}
    for line in block.splitlines():
        if line.startswith(("OK ", "END ")) or "=" not in line:
            continue
        name, _, value = line.partition("=")
        settings[name.strip()] = value.strip()
    return settings


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


@dataclasses.dataclass(frozen=True)
class MemoryInfo:
    """Typed `memory query` result (svcQueryMemory semantics)."""

    address: int
    base: int
    size: int
    type: int
    type_name: str
    attr: int
    perm: int
    perm_name: str

    @classmethod
    def from_response(cls, response: dict[str, str]) -> "MemoryInfo":
        require_ok(response)
        return cls(
            address=int(response["addr"], 16),
            base=int(response["base"], 16),
            size=int(response["size"], 16),
            type=parse_int(response["type"]),
            type_name=response.get("typeName", "unknown"),
            attr=parse_int(response["attr"]),
            perm=parse_int(response["perm"]),
            perm_name=response.get("permName", ""),
        )


@dataclasses.dataclass(frozen=True)
class GameStatus:
    """Typed `applicationStatus` result; `raw` keeps every original field."""

    running: bool
    pid: int
    title_id: int
    version: int
    main_base: int
    heap_base: int
    build_id: str
    name: str
    raw: dict[str, str]

    @classmethod
    def from_response(cls, response: dict[str, str]) -> "GameStatus":
        require_ok(response)
        return cls(
            running=response.get("running") == "1",
            pid=int(response.get("pid", "0"), 16),
            title_id=int(response.get("titleId", "0"), 16),
            version=parse_int(response.get("version", "0")),
            main_base=int(response.get("main", "0"), 16),
            heap_base=int(response.get("heap", "0"), 16),
            build_id=response.get("buildId", ""),
            name=response.get("name", ""),
            raw=response,
        )


# The value `clickSeqCapabilities` reports for a build that expands `(A,B)*N`.
SEQUENCE_REPEAT_SYNTAX = "paren-star"


@dataclasses.dataclass(frozen=True)
class ClickSequence:
    """Reply of `clickSeq`: what the console expanded and whether it finished."""

    tokens: int
    estimated_ms: int
    done: bool


class SysAgentClient:
    def __init__(self, host: str = "switch", port: int = 6000, timeout: float = 10.0,
                 retries: int = 2):
        self.host = host
        self.port = port
        self.timeout = timeout
        # Extra attempts for read-only commands only (see `command`/`_bare_command`).
        # Write commands never retry: an empty reply can mean the write already
        # landed, and repeating it would double the side effect.
        self.retries = max(0, retries)
        self._socket: socket.socket | None = None
        self._buffer = bytearray()
        # Set after a command whose response is not a single line the client reads:
        # the sysmodule may still print a diagnostic line on that connection, and
        # reading it as the next command's answer would desynchronise the stream.
        self._stream_dirty = False
        # Cached `clickSeqCapabilities`; a client instance talks to one sysmodule.
        self._sequence_capabilities: dict[str, str] | None = None
        # Set when `configure` reads time out (an older sysmodule has only the write form).
        # One timeout is enough: later probes use the compiled-in defaults instead of
        # waiting again for an answer that will never come.
        self._config_read_unavailable = False

    def __enter__(self) -> "SysAgentClient":
        # Deliberately no connection here: the first command opens one (`_command_once` calls
        # `connect`), so a command that can answer before it needs the console still does.  That
        # is what lets `version` print the client line before it asks the host for the other one,
        # the way `docker version` prints its client section with the daemon down.
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def connect(self) -> None:
        if self._socket is not None:
            if not self._stream_dirty:
                return
            # A silent command can still be printing its diagnostics; start the next
            # command on a fresh connection instead of reading that line as if it
            # were the answer.
            self.close()
        self._socket = socket.create_connection((self.host, self.port), self.timeout)
        self._socket.settimeout(self.timeout)
        self._stream_dirty = False

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None
        self._buffer.clear()
        self._stream_dirty = False

    def _retryable(self, action):
        """Run a read-only action, rebuilding the stream between attempts."""
        attempts = 1 + self.retries
        for attempt in range(attempts):
            try:
                return action()
            except (SysAgentProtocolError, OSError):
                if attempt + 1 >= attempts:
                    raise
                # A half-read response would desynchronise the next command, so
                # drop the socket and let the next attempt reconnect.
                self.close()
                time.sleep(min(0.05 * (2 ** attempt), 0.5))

    def command(self, command: str, retry: bool = False) -> dict[str, str]:
        if retry and self.retries > 0:
            return self._retryable(lambda: self._command_once(command))
        return self._command_once(command)

    def _command_once(self, command: str) -> dict[str, str]:
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

    def _bare_command(self, command_line: str, expect_output: bool = True,
                      retry: bool = False) -> str | None:
        if retry and expect_output and self.retries > 0:
            return self._retryable(lambda: self._bare_command_once(command_line, expect_output))
        return self._bare_command_once(command_line, expect_output)

    def _bare_command_once(self, command_line: str, expect_output: bool = True) -> str | None:
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
            # Legacy commands normally answer with nothing, but the sysmodule
            # prints diagnostics (e.g. "ERR controllerState ...") when something
            # failed. Drain what has already arrived and surface it to the user;
            # a line that comes later is covered by the reconnect in connect().
            self._stream_dirty = True
            self._socket.settimeout(0.02)
            try:
                line = self._readline().decode("utf-8", errors="replace")
            except (socket.timeout, OSError):
                line = None
            finally:
                self._socket.settimeout(self.timeout)
            if line:
                print(f"warning: {line}", file=sys.stderr)
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
        return require_ok(self.command("searchCapabilities", retry=True))

    def backend_status(self) -> BackendStatus:
        return BackendStatus.from_response(self.command("memoryBackend", retry=True))

    def set_backend_policy(self, policy: str) -> BackendStatus:
        if policy not in {"auto", "dmnt", "direct"}:
            raise ValueError("policy must be auto, dmnt, or direct")
        return BackendStatus.from_response(self.command(f"memoryBackend {policy}"))

    def probe_backend(self) -> BackendStatus:
        return BackendStatus.from_response(self.command("memoryBackendProbe", retry=True))

    def system_capabilities(self) -> dict[str, str]:
        return require_ok(self.command("systemCapabilities", retry=True))

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
        return require_ok(self.command(queries[name], retry=True))

    def game_status(self) -> GameStatus:
        """Typed `applicationStatus` (running, bases, build id, name)."""
        return GameStatus.from_response(self.command("applicationStatus", retry=True))

    def operation_log(self, count: int | None = None) -> list[dict[str, str]]:
        """Recent commands, oldest first (see `commands.md` for the diagnostic log)."""
        if count is not None and not 1 <= count <= 32:
            raise ValueError("count must be in 1..32")
        command = "log" if count is None else f"log count {count}"
        block = self._read_block(command, "END log")
        return [parse_log_line(line) for line in block.splitlines()
                if line.startswith("log ")]

    def process_list(self, offset: int = 0, count: int = 64) -> dict[str, str]:
        if offset < 0 or count < 1 or count > 64:
            raise ValueError("offset must be non-negative and count must be in 1..64")
        return require_ok(self.command(f"processList {offset} {count}", retry=True))

    def debug_read(self, pid: int, address: int, size: int) -> bytes:
        if pid <= 0 or address <= 0 or not 1 <= size <= 0x1000:
            raise ValueError("debug read needs pid/address > 0 and size in 1..0x1000")
        response = require_ok(self.command(
            f"debug read 0x{pid:X} 0x{address:X} 0x{size:X}", retry=True))
        try:
            data = bytes.fromhex(response["data"])
        except (KeyError, ValueError) as exc:
            raise SysAgentProtocolError("invalid debug read response") from exc
        if len(data) != size:
            raise SysAgentProtocolError(
                f"debug read returned {len(data)} bytes, expected {size}")
        return data

    def debug_wait_process(self, title_id: int, timeout_ms: int = 60000) -> dict[str, str]:
        if title_id <= 0 or timeout_ms <= 0:
            raise ValueError("title ID and timeout must be positive")
        response = require_ok(self.command(
            f"debug wait-title 0x{title_id:X} {timeout_ms}", retry=False))
        return self._wait_for_debug_wait_result(response, timeout_ms)

    def debug_wait_application(self, timeout_ms: int = 60000) -> dict[str, str]:
        if timeout_ms <= 0:
            raise ValueError("timeout must be positive")
        response = require_ok(self.command(
            f"debug wait-application {timeout_ms}", retry=False))
        return self._wait_for_debug_wait_result(response, timeout_ms)

    def debug_wait_status(self) -> dict[str, str]:
        return require_ok(self.command("debug wait-status", retry=True))

    def _wait_for_debug_wait_result(self, response: dict[str, str],
                                    timeout_ms: int) -> dict[str, str]:
        deadline = time.monotonic() + timeout_ms / 1000 + 10
        while response.get("active") == "1":
            if time.monotonic() >= deadline:
                raise SysAgentProtocolError(
                    f"process wait did not finish within {timeout_ms} ms")
            time.sleep(0.05)
            response = self.debug_wait_status()
        state = response.get("state", "")
        if state in {"parked", "exists"}:
            return response
        if state == "timeout":
            raise SysAgentProtocolError(
                f"WAIT_TIMEOUT result={response.get('lastError', '0x0')}")
        raise SysAgentProtocolError(
            f"process wait failed in stage {response.get('stage', '')}: "
            f"result={response.get('lastError', '0x0')}")

    def debug_start_process(self, pid: int) -> dict[str, str]:
        if pid <= 0:
            raise ValueError("pid must be positive")
        return require_ok(self.command(
            f"debug start-process 0x{pid:X}"))

    def system_action(self, action: str, unlock: bool = True) -> dict[str, str]:
        commands = {
            "reboot": "systemReboot",
            "reboot-emummc": "systemRebootEmuMMC",
            "shutdown": "systemShutdown",
            "sleep": "systemSleep",
            "terminate-application": "applicationTerminate",
        }
        if action not in commands:
            raise ValueError("unsupported system action")
        command = commands[action]
        # Both reboot actions unlock the console by default (the sysmodule writes
        # its one-shot request file before rebooting); `noUnlock` opts out.
        if not unlock:
            if action not in {"reboot", "reboot-emummc"}:
                raise ValueError("--no-unlock only applies to reboot and reboot-emummc")
            command += " noUnlock"
        return require_ok(self.command(command))

    def set_wireless(self, enabled: bool) -> dict[str, str]:
        state = "enabled" if enabled else "disabled"
        return require_ok(self.command(f"networkSet {state}"))

    def lock_screen_status(self) -> dict[str, str]:
        return require_ok(self.command("lockScreenStatus", retry=True))

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
        return self._bare_command(f"peek 0x{offset:X} 0x{size:X}", retry=True)

    def peek_absolute(self, address: int, size: int) -> str:
        return self._bare_command(f"peekAbsolute 0x{address:X} 0x{size:X}", retry=True)

    def peek_main(self, offset: int, size: int) -> str:
        return self._bare_command(f"peekMain 0x{offset:X} 0x{size:X}", retry=True)

    def peek_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekMulti {args}", retry=True)

    def peek_absolute_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekAbsoluteMulti {args}", retry=True)

    def peek_main_multi(self, pairs: Sequence[tuple[int, int]]) -> str:
        args = " ".join(f"0x{address:X} 0x{size:X}" for address, size in pairs)
        return self._bare_command(f"peekMainMulti {args}", retry=True)

    # ---- Mapping query and verified read ------------------------------------

    def memory_query(self, address: int) -> dict[str, str]:
        """svcQueryMemory semantics: the mapping type/permission covering an address."""
        return require_ok(self.command(f"memoryQuery 0x{address:X}", retry=True))

    def memory_info(self, address: int) -> MemoryInfo:
        """Typed form of `memory_query`."""
        return MemoryInfo.from_response(
            self.command(f"memoryQuery 0x{address:X}", retry=True))

    def memory_hash(self, address: int, size: int) -> dict[str, str]:
        """FNV-1a 32 fingerprint of a region; compare hashes to find what changed."""
        if size <= 0:
            raise ValueError("size must be positive")
        return require_ok(self.command(f"memoryHash 0x{address:X} 0x{size:X}", retry=True))

    def peek_verified(self, offset: int, size: int, attempts: int = 3) -> dict[str, str]:
        return require_ok(self.command(
            f"peekVerified 0x{offset:X} 0x{size:X} attempts={attempts}", retry=True))

    def peek_absolute_verified(self, address: int, size: int, attempts: int = 3) -> dict[str, str]:
        return require_ok(self.command(
            f"peekAbsoluteVerified 0x{address:X} 0x{size:X} attempts={attempts}", retry=True))

    def peek_main_verified(self, offset: int, size: int, attempts: int = 3) -> dict[str, str]:
        return require_ok(self.command(
            f"peekMainVerified 0x{offset:X} 0x{size:X} attempts={attempts}", retry=True))

    # ---- Readiness / condition waits ----------------------------------------

    def wait_for_game(self, timeout: float = 60.0, poll: float = 0.5) -> bool:
        """Poll until a game is running; returns False on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            try:
                if self.system_query("application").get("running") == "1":
                    return True
            except (SysAgentProtocolError, OSError):
                pass
            if time.monotonic() >= deadline:
                return False
            time.sleep(poll)

    def wait_for_value(self, address: int, size: int, value: bytes,
                       timeout: float = 30.0, poll: float = 0.2) -> bool:
        """Poll a verified absolute read until the bytes equal `value`."""
        expected = value.hex().upper()
        deadline = time.monotonic() + timeout
        while True:
            try:
                if self.peek_absolute_verified(address, size)["data"].upper() == expected:
                    return True
            except (SysAgentProtocolError, OSError):
                pass
            if time.monotonic() >= deadline:
                return False
            time.sleep(poll)

    def dump(self, start: int, size: int, output: str, chunk: int = 0x100000) -> int:
        """Read [start, start+size) with verified reads, writing raw bytes to `output`.

        The sysmodule answers one text line per read, so the client chunks the
        region (1 MiB by default, well under the 4 MiB response limit) rather than
        asking for the whole range in one command.
        """
        if size <= 0:
            raise ValueError("size must be positive")
        if not 0 < chunk <= 0x100000:
            raise ValueError("chunk must be in 1..0x100000")
        written = 0
        with open(output, "wb") as handle:
            while written < size:
                this_chunk = min(chunk, size - written)
                data = self.peek_absolute_verified(start + written, this_chunk)["data"]
                handle.write(bytes.fromhex(data))
                written += this_chunk
        return written

    def pointer(self, jumps: Sequence[int]) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps)
        return self._bare_command(f"pointer {args}", retry=True)

    def pointer_all(self, jumps: Sequence[int], final: int) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerAll {args}", retry=True)

    def pointer_relative(self, jumps: Sequence[int], final: int) -> str:
        args = " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerRelative {args}", retry=True)

    def pointer_peek(self, size: int, jumps: Sequence[int], final: int) -> str:
        args = f"0x{size:X} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        return self._bare_command(f"pointerPeek {args}", retry=True)

    def pointer_peek_multi(self, chains: Sequence[tuple[int, Sequence[int], int]]) -> str:
        blocks = []
        for size, jumps, final in chains:
            args = f"0x{size:X} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
            blocks.append(args)
        return self._bare_command("pointerPeekMulti " + " * ".join(blocks), retry=True)

    # ---- Legacy memory write ------------------------------------------------

    def poke(self, offset: int, data: bytes) -> None:
        self._bare_command(f"poke 0x{offset:X} {data.hex().upper()}", expect_output=False)

    def poke_absolute(self, address: int, data: bytes) -> None:
        self._bare_command(f"pokeAbsolute 0x{address:X} {data.hex().upper()}", expect_output=False)

    def poke_main(self, offset: int, data: bytes) -> None:
        self._bare_command(f"pokeMain 0x{offset:X} {data.hex().upper()}", expect_output=False)

    def poke_verified(self, offset: int, data: bytes) -> dict[str, str]:
        return require_ok(self.command(f"pokeVerified 0x{offset:X} {data.hex().upper()}"))

    def poke_absolute_verified(self, address: int, data: bytes) -> dict[str, str]:
        return require_ok(
            self.command(f"pokeAbsoluteVerified 0x{address:X} {data.hex().upper()}")
        )

    def poke_main_verified(self, offset: int, data: bytes) -> dict[str, str]:
        return require_ok(self.command(f"pokeMainVerified 0x{offset:X} {data.hex().upper()}"))

    def pointer_poke(self, data: bytes, jumps: Sequence[int], final: int) -> None:
        args = f"{data.hex().upper()} " + " ".join(f"0x{value:X}" for value in jumps) + f" 0x{final:X}"
        self._bare_command(f"pointerPoke {args}", expect_output=False)

    # ---- Freeze --------------------------------------------------------------

    def freeze(self, address: int, data: bytes) -> None:
        self._bare_command(f"freeze 0x{address:X} {data.hex().upper()}", expect_output=False)

    def unfreeze(self, address: int) -> None:
        self._bare_command(f"unFreeze 0x{address:X}", expect_output=False)

    def freeze_count(self) -> str:
        return self._bare_command("freezeCount", retry=True)

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

    def sequence_capabilities(self) -> dict[str, str]:
        """Click-seq syntax the connected sysmodule accepts."""
        return require_ok(self.command("clickSeqCapabilities", retry=True))

    def _require_sequence_syntax(self, sequence: str) -> None:
        """Reject what the console would refuse or silently mis-read.

        Whitespace can never be part of a sequence: the sysmodule splits the
        command line on spaces, so `(A, B)*2` arrives as three arguments and is
        dropped.  Repeat syntax needs a build that expands it - an older
        sysmodule reads `(A` and `B)*2` as unknown tokens, sends no input at all
        and still answers `done`.
        """
        if any(character.isspace() for character in sequence):
            raise ValueError("sequence must not contain whitespace: the console splits "
                             "the command line on spaces")
        if not any(character in sequence for character in "()*"):
            return
        if self._sequence_capabilities is None:
            self._sequence_capabilities = self.sequence_capabilities()
        if self._sequence_capabilities.get("repeat") != SEQUENCE_REPEAT_SYNTAX:
            raise SysAgentProtocolError(
                "this sys-agent build does not expand click-seq repeats "
                f"('(A,B)*N'): capabilities={self._sequence_capabilities}")

    def click_seq(self, sequence: str, wait_done: bool = True) -> ClickSequence:
        """Run a sequence; see `input click-seq -h` for the syntax.

        The console answers `OK tokens=… estMs=…` before it runs the sequence and
        `done` when it has finished.  The wait for `done` is extended to the
        sequence's own estimate, so a long repeat does not surface as a socket
        timeout while the console is still clicking.
        """
        self._require_sequence_syntax(sequence)
        self.connect()
        assert self._socket is not None
        self._socket.sendall(f"clickSeq {sequence}".encode("ascii") + b"\r\n")

        if not wait_done:
            # Same shape as the other silent commands: whatever the console prints
            # is drained by the next command's `warning:` line.
            self._stream_dirty = True
            self._socket.settimeout(0.02)
            try:
                line = self._readline().decode("utf-8", errors="replace")
            except (socket.timeout, OSError):
                line = None
            finally:
                self._socket.settimeout(self.timeout)
            if line:
                print(f"warning: {line}", file=sys.stderr)
            return ClickSequence(tokens=0, estimated_ms=0, done=False)

        line = self._readline().decode("utf-8", errors="replace")
        if line.startswith("ERR "):
            raise SysAgentProtocolError(line)

        tokens = 0
        estimated_ms = 0
        if line.startswith("OK "):
            fields = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
            tokens = parse_int(fields.get("tokens", "0"))
            estimated_ms = parse_int(fields.get("estMs", "0"))
            self._socket.settimeout(max(self.timeout, estimated_ms / 1000.0 + 5.0))
            try:
                line = self._readline().decode("utf-8", errors="replace")
            finally:
                self._socket.settimeout(self.timeout)
        if line != "done":
            raise SysAgentProtocolError(f"expected 'done' from clickSeq, got {line!r}")
        return ClickSequence(tokens=tokens, estimated_ms=estimated_ms, done=True)

    def click_cancel(self) -> None:
        self._bare_command("clickCancel", expect_output=False)

    def detach_controller(self) -> None:
        self._bare_command("detachController", expect_output=False)

    def controller_status(self) -> dict[str, str]:
        return require_ok(self.command("controllerStatus", retry=True))

    def _read_block(self, command: str, end_token: str) -> str:
        """Send a command whose answer is a multi-line block ending in end_token."""
        self.connect()
        assert self._socket is not None
        self._socket.sendall(command.encode("ascii") + b"\r\n")
        lines: list[str] = []
        while True:
            line = self._readline().decode("utf-8", errors="replace")
            lines.append(line)
            if len(lines) == 1 and line.startswith("ERR "):
                # Not every error path prints the END token (controllerKick returns
                # right after its ERR), so the block cannot be drained: drop the
                # connection instead of leaving a stale line for the next command.
                self.close()
                raise SysAgentProtocolError(line)
            if line == end_token:
                break
        return "\n".join(lines)

    def controller_dump(self) -> str:
        """Full virtual-controller dump (player slots, HDLS devices, hidsys owners)."""
        return self._read_block("controllerDump", "END controllerDump")

    def controller_kick(self, npad_id: int) -> str:
        if not 0 <= npad_id <= 7:
            raise ValueError("npad id must be 0..7")
        return self._read_block(f"controllerKick {npad_id}", "END controllerKick")

    def controller_reconnect(self, timeout_ms: int = 5000, address: str | None = None) -> str:
        """Pull back a controller: saved kick addresses, or one explicit address."""
        if timeout_ms <= 0:
            raise ValueError("timeout must be positive")
        command = f"controllerReconnect {timeout_ms}"
        if address:
            command += f" {address}"
        return self._read_block(command, "END controllerReconnect")

    def controller_paired_devices(self) -> str:
        """List the console's paired Bluetooth devices (addresses and names)."""
        return self._read_block("controllerPairedDevices", "END controllerPairedDevices")

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

    def type_text(self, value: str, *, per_char_ms: float | None = None) -> None:
        """Type `value` on the console's virtual keyboard, character by character.

        The console can press keys, not "type a string": it takes a list of HID reports, and the
        system software keyboard translates a US HID layout into text.  This method does that
        translation (see `text_runs`), and it also handles the things a caller cannot see from
        the wire format:

        * characters that need shift go out as one `keyMulti` (shift keycode 0xE1 plus the key),
          which is what a real keyboard sends -- the keyboard autopilot's own modifier byte is not
          honoured by the software keyboard;
        * the console types asynchronously: a `key`/`keyMulti` command queues a report list, and
          the keyboard thread takes it on its next poll (it holds `keyMutex` while typing).  A
          command arriving before that poll replaces the queued list; one arriving during the run
          waits.  The client therefore waits `per_char_ms` (default: the console's own
          `keySleepTime` + `pollRate`) for every character of the command it just sent before
          sending the next one;
        * the game's on-screen keyboard drops keys at the console's default 25 ms gap, so the
          console's `keySleepTime` is raised to at least `TEXT_MIN_KEY_SLEEP_MS` for the duration
          of this call and restored afterwards.  The raise happens before `per_char_ms` is read,
          so the client's own pacing also uses the effective gap.

        Only printable ASCII is typable this way; anything else (a Chinese character, a newline)
        is refused with `SysAgentProtocolError` *before* the first key is sent, so a partially
        typed field never looks like a complete one.  Fields that need the console's IME or a
        paste are out of reach of HID reports.
        """
        runs = text_runs(value)
        if not runs:
            return
        original_key_sleep = self._raise_key_sleep_for_text()
        try:
            if per_char_ms is None:
                per_char_ms = self.text_per_char_ms()
            for kind, codes in runs:
                if kind == "shift":
                    self.key_multi([KEY_SHIFT, *codes])
                else:
                    self.key(codes)
                time.sleep(len(codes) * per_char_ms / 1000.0 + 0.15)
        finally:
            self._restore_key_sleep(original_key_sleep)

    def _config_value(self, name: str) -> str | None:
        """Read one runtime setting, or None when the console cannot answer.

        A sysmodule that predates the read form of `configure` stays silent; the read then
        sits until the socket timeout.  One such timeout marks the read path unavailable and
        drops the connection, so the remaining probes (and the next `input text`) use the
        compiled-in defaults instead of waiting again.
        """
        if self._config_read_unavailable:
            return None
        try:
            return self.config_get(name)
        except (SysAgentProtocolError, ValueError):
            return None
        except OSError:
            self._config_read_unavailable = True
            self.close()
            return None

    def _raise_key_sleep_for_text(self) -> int | None:
        """Raise the console's key gap to the text floor; return the value to restore.

        A console that cannot report or change the setting (an older sysmodule, or one whose
        `configure` reply is missing) is left alone: the caller still gets typed text, paced by
        `text_per_char_ms`'s compiled-in fallback.
        """
        value = self._config_value("keySleepTime")
        if value is None:
            return None
        try:
            current = int(value)
        except ValueError:
            return None
        if current >= TEXT_MIN_KEY_SLEEP_MS:
            return None
        try:
            self.configure("keySleepTime", TEXT_MIN_KEY_SLEEP_MS)
        except (SysAgentProtocolError, ValueError, OSError) as error:
            print(f"warning: could not raise keySleepTime: {error}", file=sys.stderr)
            return None
        return current

    def _restore_key_sleep(self, original: int | None) -> None:
        if original is None:
            return
        try:
            self.configure("keySleepTime", original)
        except (SysAgentProtocolError, ValueError, OSError) as error:
            print(f"warning: could not restore keySleepTime to {original}: {error}",
                  file=sys.stderr)

    def text_per_char_ms(self) -> float:
        """How long the console needs per typed character, from its own settings.

        `keySleepTime` is the gap after each report and `pollRate` the gap when a release has to
        be inserted between two identical reports; together they bound the per-character cost.
        An older console that does not answer `configure` falls back to the compiled-in defaults.
        """
        total = 0.0
        for name, default in (("keySleepTime", 25.0), ("pollRate", 17.0)):
            value = self._config_value(name)
            if value is None:
                total += default
                continue
            try:
                total += float(value)
            except ValueError:
                total += default
        return total

    # ---- Screen ---------------------------------------------------------------

    def screen_off(self) -> None:
        self._bare_command("screenOff", expect_output=False)

    def screen_on(self) -> None:
        self._bare_command("screenOn", expect_output=False)

    # ---- Utility --------------------------------------------------------------

    def get_title_id(self) -> str:
        return self._bare_command("getTitleID", retry=True)

    def get_title_version(self) -> str:
        return self._bare_command("getTitleVersion", retry=True)

    def get_system_language(self) -> str:
        return self._bare_command("getSystemLanguage", retry=True)

    def get_build_id(self) -> str:
        return self._bare_command("getBuildID", retry=True)

    def get_heap_base(self) -> str:
        return self._bare_command("getHeapBase", retry=True)

    def get_main_nso_base(self) -> str:
        return self._bare_command("getMainNsoBase", retry=True)

    def is_program_running(self, program_id: int) -> str:
        return self._bare_command(f"isProgramRunning 0x{program_id:X}", retry=True)

    def game(self, field: str) -> str:
        if field not in {"icon", "version", "rating", "author", "name"}:
            raise ValueError("field must be icon, version, rating, author, or name")
        return self._bare_command(f"game {field}", retry=True)

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
        return self._bare_command("getVersion", retry=True)

    def charge(self) -> str:
        return self._bare_command("charge")

    def fd_count(self) -> str:
        return self._bare_command("fdCount")

    def configure(self, parameter: str, value: int) -> None:
        if not parameter or any(char.isspace() for char in parameter):
            raise ValueError("parameter must be a single token")
        self._bare_command(f"configure {parameter} {value}", expect_output=False)

    def config_list(self) -> dict[str, str]:
        """Every runtime setting the sysmodule reports, as name -> value."""
        return parse_config_block(self._read_block("configure", "END configure"))

    def config_get(self, parameter: str) -> str:
        """One runtime setting. The seconds alias maps onto the millisecond one."""
        name = CONFIG_READ_ALIASES.get(parameter, parameter)
        if not name or any(char.isspace() for char in name):
            raise ValueError("parameter must be a single token")
        block = self._read_block(f"configure {name}", "END configure")
        settings = parse_config_block(block)
        if name not in settings:
            raise SysAgentProtocolError(f"config answer did not contain {name}: {block!r}")
        return settings[name]

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
        return SearchStatus.from_response(self.command(f"searchStatus {session}", retry=True))

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
        response = require_ok(
            self.command(f"searchResults {session} {offset} {count}", retry=True))
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


def _emit(response: dict[str, str], args: argparse.Namespace) -> None:
    """Print a response as fields (default), JSON (--json), or to a file (--out)."""
    if getattr(args, "json", False):
        payload = json.dumps(response, indent=2, sort_keys=True)
    else:
        payload = " ".join(f"{key}={value}" for key, value in response.items())
    out = getattr(args, "out", None)
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(payload + "\n")
        print(out)
    else:
        print(payload)


def pair_values(values: Sequence[int]) -> list[tuple[int, int]]:
    if len(values) % 2:
        raise ValueError("expected an even number of arguments")
    return list(zip(values[0::2], values[1::2]))


# --- typing text over the virtual HID keyboard ------------------------------
# The console's keyboard autopilot takes HID *keycodes*, and the software keyboard turns them into
# characters with its own layout: the unshifted US layout (HID usage 0x04..0x1D = a..z, 0x1E..0x27
# = 1..0, 0x2C space, then the punctuation a US keyboard has in those positions).  Measured on the
# console: pressing 0x22 types "5", 0x33 types ";", 0x2F types "[" -- while the on-screen keycaps
# show the Switch's own labels (":" and "_" sit where a US keyboard has ";" and "-"), so the
# keycaps are *not* the mapping to trust.
#
# Shift is a keycode in the same bitfield (0xE1, left shift), which is why a shifted character is
# sent as one report holding both keys -- `keyMulti`, not `keyMod`: the autopilot state's own
# modifier byte is ignored by the software keyboard (measured: `keyMod <key> 1` types the
# unshifted character).
KEY_SHIFT = 0xE1

_KEY_UNSHIFTED: dict[str, int] = {chr(ord("a") + i): 0x04 + i for i in range(26)}
_KEY_UNSHIFTED.update({chr(ord("1") + i): 0x1E + i for i in range(9)})
_KEY_UNSHIFTED["0"] = 0x27
_KEY_UNSHIFTED.update({
    " ": 0x2C, "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30, "\\": 0x31,
    ";": 0x33, "'": 0x34, "`": 0x35, ",": 0x36, ".": 0x37, "/": 0x38,
})

_KEY_SHIFTED: dict[str, int] = {chr(ord("A") + i): 0x04 + i for i in range(26)}
_KEY_SHIFTED.update({
    "!": 0x1E, "@": 0x1F, "#": 0x20, "$": 0x21, "%": 0x22, "^": 0x23, "&": 0x24,
    "*": 0x25, "(": 0x26, ")": 0x27, "_": 0x2D, "+": 0x2E, "{": 0x2F, "}": 0x30,
    "|": 0x31, ":": 0x33, '"': 0x34, "~": 0x35, "<": 0x36, ">": 0x37, "?": 0x38,
})


def text_runs(value: str) -> list[tuple[str, list[int]]]:
    """Turns a string into the keyboard reports that type it.

    Answers a list of ``(kind, keycodes)``: ``("keys", [...])`` is a run of characters typed by one
    `key` command, ``("shift", [code])`` is one character that needs shift (`keyMulti` with
    `KEY_SHIFT`).  Consecutive unshifted characters share a command, one shifted character is one
    command -- which is exactly how many commands the console needs, no more.

    Anything this layout cannot produce (non-ASCII, control characters) raises
    `SysAgentProtocolError` here, before any key is sent.
    """
    runs: list[tuple[str, list[int]]] = []
    for char in value:
        if char in _KEY_UNSHIFTED:
            codes = _KEY_UNSHIFTED[char]
            if runs and runs[-1][0] == "keys":
                runs[-1][1].append(codes)
            else:
                runs.append(("keys", [codes]))
        elif char in _KEY_SHIFTED:
            runs.append(("shift", [_KEY_SHIFTED[char]]))
        else:
            raise SysAgentProtocolError(
                f"the console's virtual keyboard cannot type {char!r} (U+{ord(char):04X}): it "
                "sends HID keyboard reports, so only printable ASCII works -- text in another "
                "script needs the console's own input method, which is not reachable this way")
    return runs


def image_diff(baseline: str, current: str) -> dict[str, object]:
    """Compare two JPEGs (Pillow) and report the changed ratio and bounding box.

    Pillow is this client's one optional dependency: the standard library cannot
    decode JPEG, so a real pixel comparison needs it. Without Pillow the caller
    gets a clear error instead of a bogus result.
    """
    try:
        from PIL import Image, ImageChops
    except ImportError as error:
        raise SysAgentProtocolError(
            "image diff needs Pillow; install it (pip install pillow) or compare "
            "the JPEGs yourself") from error
    with Image.open(baseline) as image:
        first = image.convert("RGB")
    with Image.open(current) as image:
        second = image.convert("RGB")
    if first.size != second.size:
        return {"same_size": False, "baseline_size": list(first.size),
                "current_size": list(second.size)}
    difference = ImageChops.difference(first, second).convert("L")
    total = first.size[0] * first.size[1]
    changed = total - difference.histogram()[0]
    bbox = difference.getbbox()
    return {
        "same_size": True,
        "width": first.size[0],
        "height": first.size[1],
        "changed_pixels": changed,
        "total_pixels": total,
        "changed_ratio": round(changed / total, 6),
        "bbox": list(bbox) if bbox else None,
    }


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


def parse_trace_pair_spec(value: str) -> tuple[int, int]:
    """Parse an ``ENTRY:RETURN`` trace correlation rule."""
    if value.count(":") != 1:
        raise ValueError("trace pair must be ENTRY:RETURN")
    entry_text, return_text = value.split(":", 1)
    entry = parse_int(entry_text)
    return_address = parse_int(return_text)
    if entry == return_address:
        raise ValueError("trace pair entry and return addresses must differ")
    return entry, return_address


def _profile_object(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def _profile_list(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    return value


def _profile_hex_id(value: object, label: str) -> int:
    if isinstance(value, int):
        parsed = value
    elif isinstance(value, str):
        try:
            parsed = int(value, 16)
        except ValueError as exc:
            raise ValueError(f"{label} must be a hexadecimal ID") from exc
    else:
        raise ValueError(f"{label} must be a hexadecimal ID")
    if parsed <= 0:
        raise ValueError(f"{label} must be positive")
    return parsed


def _profile_offset(value: object, label: str) -> int:
    if isinstance(value, int):
        parsed = value
    elif isinstance(value, str):
        if not value.lower().startswith("0x"):
            raise ValueError(f"{label} must use a 0x-prefixed hexadecimal offset")
        try:
            parsed = int(value, 16)
        except ValueError as exc:
            raise ValueError(f"{label} must be a hexadecimal offset") from exc
    else:
        raise ValueError(f"{label} must be a hexadecimal offset")
    if parsed < 0 or parsed & 3:
        raise ValueError(f"{label} must be a non-negative 4-byte-aligned offset")
    return parsed


def _validate_snapshot_spec(value: str, label: str) -> None:
    match = re.fullmatch(
        r"(x(?:[0-9]|[12][0-9]|30)|sp|pc|lr)"
        r"(?:([+-])(0x[0-9A-Fa-f]+|[0-9]+))?"
        r":(0x[0-9A-Fa-f]+|[0-9]+)",
        value)
    if match is None:
        raise ValueError(f"{label} has invalid snapshot syntax: {value!r}")

    def parse_number(text: str) -> int:
        return int(text, 16) if text.lower().startswith("0x") else int(text, 10)

    if match.group(2):
        magnitude = parse_number(match.group(3))
        if magnitude > 0x7FFFFFFFFFFFFFFF:
            raise ValueError(f"{label} snapshot offset is too large")
    size = parse_number(match.group(4))
    if not 1 <= size <= 0x100:
        raise ValueError(f"{label} snapshot size must be in 1..0x100")


def _profile_snapshot_entries(value: object,
                              label: str) -> list[dict[str, str]]:
    entries = _profile_list(value, label)
    if len(entries) > 2:
        raise ValueError(f"{label} supports at most two snapshots")
    result: list[dict[str, str]] = []
    names: set[str] = set()
    for index, raw_entry in enumerate(entries):
        if isinstance(raw_entry, str):
            entry = {"spec": raw_entry}
        else:
            entry = _profile_object(raw_entry, f"{label}[{index}]")
            spec = entry.get("spec")
            if not isinstance(spec, str):
                raise ValueError(f"{label}[{index}].spec must be a string")
            entry = {key: value for key, value in entry.items()}
        _validate_snapshot_spec(str(entry["spec"]), f"{label}[{index}]")
        name = entry.get("name")
        if name is not None:
            if not isinstance(name, str) or not name:
                raise ValueError(f"{label}[{index}].name must be a non-empty string")
            if name in names:
                raise ValueError(f"{label} has duplicate snapshot name {name!r}")
            names.add(name)
        kind = entry.get("kind")
        if kind is not None and not isinstance(kind, str):
            raise ValueError(f"{label}[{index}].kind must be a string")
        result.append({
            "spec": str(entry["spec"]),
            **({"name": name} if name is not None else {}),
            **({"kind": kind} if kind is not None else {}),
        })
    return result


def _profile_target_offset(profile: dict[str, object],
                           target: dict[str, object], label: str) -> int:
    if "offset" in target:
        return _profile_offset(target["offset"], f"{label} offset")
    symbol = target.get("symbol")
    symbols = profile.get("symbols", {})
    if isinstance(symbol, str) and isinstance(symbols, dict) and symbol in symbols:
        return _profile_offset(symbols[symbol], f"symbols[{symbol}]")
    raise ValueError(f"{label} needs an offset or a symbol present in symbols")


def load_trace_profile(path: str) -> dict[str, object]:
    """Load and validate a versioned, game-independent execution-trace profile."""
    with open(path, encoding="utf-8") as handle:
        profile = json.load(handle)
    profile = _profile_object(profile, "trace profile")
    schema = parse_int(str(profile.get("schemaVersion", "0")))
    if schema not in (1, TRACE_PROFILE_SCHEMA_VERSION):
        raise ValueError(f"unsupported trace profile schema {schema}")
    if "name" in profile and (
            not isinstance(profile["name"], str) or not profile["name"]):
        raise ValueError("profile name must be a non-empty string")
    symbols = profile.get("symbols", {})
    symbols = _profile_object(symbols, "symbols")
    for symbol, offset in symbols.items():
        if not isinstance(symbol, str) or not symbol:
            raise ValueError("symbol names must be non-empty strings")
        _profile_offset(offset, f"symbols[{symbol}]")

    processes = [
        _profile_object(value, f"processes[{index}]")
        for index, value in enumerate(
            _profile_list(profile.get("processes"), "processes"))
    ]
    if not 1 <= len(processes) <= 2:
        raise ValueError("trace profile needs one or two processes")
    process_ids: dict[str, dict[str, object]] = {}
    for index, process in enumerate(processes):
        process_id = process.get("id")
        if not isinstance(process_id, str) or not process_id:
            raise ValueError(f"processes[{index}].id must be a non-empty string")
        if process_id in process_ids:
            raise ValueError(f"duplicate process id: {process_id}")
        selectors = [
            name for name in ("foreground", "titleId", "pid")
            if name in process
        ]
        if len(selectors) != 1:
            raise ValueError(
                f"process {process_id} needs exactly one of foreground/titleId/pid")
        if selectors[0] == "foreground" and process["foreground"] is not True:
            raise ValueError(f"process {process_id} foreground must be true")
        if selectors[0] == "titleId":
            _profile_hex_id(process["titleId"], f"process {process_id} titleId")
        if selectors[0] == "pid":
            _profile_hex_id(process["pid"], f"process {process_id} pid")
        if "mainModuleIndex" in process:
            module_index = process["mainModuleIndex"]
            if (not isinstance(module_index, int) or module_index < 0):
                raise ValueError(
                    f"process {process_id} mainModuleIndex must be non-negative")
        expected = process.get("expect")
        if expected is not None:
            expected = _profile_object(expected, f"process {process_id} expect")
            if "titleId" in expected:
                _profile_hex_id(
                    expected["titleId"], f"process {process_id} expect.titleId")
            build_id = expected.get("buildId")
            if build_id is not None and not isinstance(build_id, str):
                raise ValueError(
                    f"process {process_id} expect.buildId must be a string")
        process_ids[process_id] = process

    targets = [
        _profile_object(value, f"targets[{index}]")
        for index, value in enumerate(
            _profile_list(profile.get("targets"), "targets"))
    ]
    if not 1 <= len(targets) <= 4:
        raise ValueError("trace profile needs one to four targets")
    target_ids: dict[str, dict[str, object]] = {}
    targets_per_process: dict[str, int] = {
        process_id: 0 for process_id in process_ids
    }
    for index, target in enumerate(targets):
        target_id = target.get("id")
        if not isinstance(target_id, str) or not target_id:
            raise ValueError(f"targets[{index}].id must be a non-empty string")
        if target_id in target_ids:
            raise ValueError(f"duplicate target id: {target_id}")
        process_id = target.get("process")
        if not isinstance(process_id, str) or process_id not in process_ids:
            raise ValueError(
                f"target {target_id} references unknown process {process_id!r}")
        target_base = target.get("base", "main")
        if target_base not in {"main", "code"}:
            raise ValueError(
                f"target {target_id} base must be main or code")
        if schema == 1:
            _profile_offset(target.get("offset"), f"target {target_id} offset")
        else:
            if ("offset" in target) == ("symbol" in target):
                raise ValueError(
                    f"target {target_id} needs exactly one of offset/symbol")
            if target_base == "code" and "symbol" in target:
                raise ValueError(
                    f"target {target_id} base=code cannot use a symbol")
            _profile_target_offset(profile, target, f"target {target_id}")
            if "snapshots" in target:
                target["_parsedSnapshots"] = _profile_snapshot_entries(
                    target["snapshots"], f"target {target_id}.snapshots")
            else:
                target.pop("_parsedSnapshots", None)
            expected_bytes = target.get("expectBytes")
            if expected_bytes is not None:
                if not isinstance(expected_bytes, str) or not expected_bytes:
                    raise ValueError(
                        f"target {target_id}.expectBytes must be a hex string")
                try:
                    parsed_bytes = bytes.fromhex(expected_bytes)
                except ValueError as exc:
                    raise ValueError(
                        f"target {target_id}.expectBytes must be a hex string") from exc
                if not 1 <= len(parsed_bytes) <= 32:
                    raise ValueError(
                        f"target {target_id}.expectBytes must be 1..32 bytes")
        targets_per_process[process_id] += 1
        target_ids[target_id] = target

    if len(processes) == 2 and any(
            count != 2 for count in targets_per_process.values()):
        raise ValueError(
            "two-process profiles need exactly two targets per process")
    if any(count > 4 for count in targets_per_process.values()):
        raise ValueError("a process cannot have more than four trace targets")

    pairs = [
        _profile_object(value, f"pairs[{index}]")
        for index, value in enumerate(
            _profile_list(profile.get("pairs", []), "pairs"))
    ]
    for index, pair in enumerate(pairs):
        entry_id = pair.get("entry")
        return_id = pair.get("return")
        if entry_id not in target_ids or return_id not in target_ids:
            raise ValueError(
                f"pairs[{index}] must reference existing entry and return target ids")
        if entry_id == return_id:
            raise ValueError(f"pairs[{index}] entry and return must differ")
        entry_process = target_ids[str(entry_id)].get("process")
        return_process = target_ids[str(return_id)].get("process")
        if entry_process != return_process:
            raise ValueError(
                f"pairs[{index}] entry and return must belong to the same process")
        if "ipc" in pair:
            ipc = _profile_object(pair["ipc"], f"pairs[{index}].ipc")
            protocol = ipc.get("protocol", "cmif")
            if protocol != "cmif":
                raise ValueError(
                    f"pairs[{index}].ipc.protocol currently supports only cmif")
            if "service" in ipc and (
                    not isinstance(ipc["service"], str) or not ipc["service"]):
                raise ValueError(
                    f"pairs[{index}].ipc.service must be a non-empty string")
            if "interfaceHash" in ipc:
                _profile_hex_id(
                    ipc["interfaceHash"],
                    f"pairs[{index}].ipc.interfaceHash")
            commands = ipc.get("commands", {})
            commands = _profile_object(
                commands, f"pairs[{index}].ipc.commands")
            for command_id, command_name in commands.items():
                parse_int(str(command_id))
                if not isinstance(command_name, str) or not command_name:
                    raise ValueError(
                        f"pairs[{index}].ipc.commands[{command_id}] "
                        "must be a non-empty string")
            if "correlation" in ipc:
                correlation = _profile_object(
                    ipc["correlation"],
                    f"pairs[{index}].ipc.correlation")
                request_key = _validate_correlation_expression(
                    correlation.get("request"),
                    f"pairs[{index}].ipc.correlation.request")
                response_key = correlation.get("response", request_key)
                _validate_correlation_expression(
                    response_key,
                    f"pairs[{index}].ipc.correlation.response")
                if "thread" in correlation and not isinstance(
                        correlation["thread"], bool):
                    raise ValueError(
                        f"pairs[{index}].ipc.correlation.thread must be boolean")
                fallback = correlation.get("fallback", "none")
                if fallback not in {"none", "thread-fifo"}:
                    raise ValueError(
                        f"pairs[{index}].ipc.correlation.fallback "
                        "must be none or thread-fifo")

    trace_options = profile.get("trace", {})
    if trace_options is not None:
        trace_options = _profile_object(trace_options, "trace")
        for name in ("hits", "ring", "duration", "args"):
            if name in trace_options and (
                    not isinstance(trace_options[name], int)
                    or trace_options[name] < 0):
                raise ValueError(f"trace.{name} must be a non-negative integer")
        duration_from = trace_options.get("durationFrom", "arm")
        if duration_from not in {"arm", "first-hit"}:
            raise ValueError("trace.durationFrom must be arm or first-hit")
        _profile_snapshot_entries(
            trace_options.get("snapshots", []), "trace.snapshots")
    return profile


def _parse_debug_modules(response: dict[str, str]) -> dict[int, dict[str, object]]:
    require_ok(response)
    modules: dict[int, dict[str, object]] = {}
    for raw_entry in response.get("modules", "").split(","):
        if not raw_entry or "=" not in raw_entry:
            continue
        index_text, payload = raw_entry.split("=", 1)
        address_size, _, build_id = payload.partition(":build=")
        base_text, _, size_text = address_size.partition("+")
        try:
            modules[int(index_text)] = {
                "base": int(base_text, 16),
                "size": int(size_text, 16),
                "buildId": build_id,
            }
        except ValueError as exc:
            raise SysAgentProtocolError(
                f"invalid debug modules entry: {raw_entry!r}") from exc
    if not modules:
        raise SysAgentProtocolError("debug modules returned no modules")
    return modules


def _parse_executable_maps(
        response: dict[str, str]) -> tuple[list[dict[str, int]], bool]:
    require_ok(response)
    maps: list[dict[str, int]] = []
    for raw_entry in response.get("maps", "").split(","):
        if not raw_entry or "=" not in raw_entry:
            continue
        _, payload = raw_entry.split("=", 1)
        address_size, _, attributes = payload.partition(":")
        base_text, _, size_text = address_size.partition("+")
        _, _, permission_text = attributes.partition("/p")
        try:
            base = int(base_text, 16)
            size = int(size_text, 16)
            permission = int(permission_text, 0)
        except ValueError as exc:
            raise SysAgentProtocolError(
                f"invalid debug maps entry: {raw_entry!r}") from exc
        if (permission & 5) == 5:
            maps.append({"base": base, "size": size, "permission": permission})
    return maps, response.get("truncated") == "1"


def _find_profile_pid(client: SysAgentClient, wanted_title: int) -> int:
    offset = 0
    matches: list[int] = []
    while True:
        listing = client.process_list(offset, 64)
        entries = listing.get("processes", "")
        for entry in entries.split(","):
            pid_text, _, title_text = entry.partition(":")
            try:
                if int(title_text, 16) == wanted_title:
                    pid = int(pid_text, 16)
                    if pid not in matches:
                        matches.append(pid)
            except ValueError:
                continue
        total = parse_int(listing.get("total", "0"))
        captured = parse_int(listing.get("captured", "0"))
        offset += captured
        if captured == 0 or offset >= total:
            break
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise SysAgentProtocolError(
            f"title ID {wanted_title:016X} has multiple running processes: "
            + ", ".join(f"0x{pid:X}" for pid in matches))
    raise SysAgentProtocolError(
        f"no running process has title ID {wanted_title:016X}")


def _profile_title_for_pid(client: SysAgentClient, pid: int) -> int | None:
    offset = 0
    while True:
        listing = client.process_list(offset, 64)
        entries = listing.get("processes", "")
        for entry in entries.split(","):
            pid_text, _, title_text = entry.partition(":")
            try:
                if int(pid_text, 16) == pid:
                    return int(title_text, 16)
            except ValueError:
                continue
        total = parse_int(listing.get("total", "0"))
        captured = parse_int(listing.get("captured", "0"))
        offset += captured
        if captured == 0 or offset >= total:
            break
    return None


def _profile_target_order(profile: dict[str, object]) -> list[dict[str, object]]:
    processes = [
        _profile_object(value, "process")
        for value in _profile_list(profile["processes"], "processes")
    ]
    targets = [
        _profile_object(value, "target")
        for value in _profile_list(profile["targets"], "targets")
    ]
    if len(processes) == 1:
        return targets
    by_process: dict[str, list[dict[str, object]]] = {
        str(process["id"]): [] for process in processes
    }
    for target in targets:
        by_process[str(target["process"])].append(target)
    return [
        by_process[str(processes[0]["id"])][0],
        by_process[str(processes[1]["id"])][0],
        by_process[str(processes[0]["id"])][1],
        by_process[str(processes[1]["id"])][1],
    ]


def resolve_trace_profile(client: SysAgentClient,
                          profile: dict[str, object]) -> dict[str, object]:
    """Resolve PIDs/modules/offsets in a validated profile into a trace plan."""
    game = client.game_status()
    trace_config = profile.get("trace", {})
    trace_config = trace_config if isinstance(trace_config, dict) else {}
    global_snapshots = _profile_snapshot_entries(
        trace_config.get("snapshots", []),
        "trace.snapshots")
    resolved_processes: list[dict[str, object]] = []
    resolved_by_id: dict[str, dict[str, object]] = {}
    executable_maps_by_pid: dict[
        int, tuple[list[dict[str, int]], bool]] = {}
    for raw_process in _profile_list(profile["processes"], "processes"):
        process = _profile_object(raw_process, "process")
        process_id = str(process["id"])
        if "foreground" in process:
            if not game.running:
                raise SysAgentProtocolError(
                    f"process {process_id} selects the foreground application, "
                    "but no application is running")
            pid = game.pid
            title_id = game.title_id
        elif "titleId" in process:
            title_id = _profile_hex_id(
                process["titleId"], f"process {process_id} titleId")
            pid = _find_profile_pid(client, title_id)
        else:
            pid = _profile_hex_id(process["pid"], f"process {process_id} pid")
            title_id = _profile_title_for_pid(client, pid)
            if title_id is None:
                raise SysAgentProtocolError(
                    f"process {process_id} PID {pid:X} is not running")

        modules = _parse_debug_modules(require_ok(client.command(
            f"debug modules 0x{pid:X} 2", retry=True)))
        if "foreground" in process:
            module_index = next(
                (index for index, module in modules.items()
                 if module["base"] == game.main_base),
                None)
            if module_index is None:
                raise SysAgentProtocolError(
                    f"foreground main base 0x{game.main_base:X} is absent "
                    "from debug modules")
        else:
            module_index = int(process.get("mainModuleIndex", 0))
        module = modules.get(module_index)
        if module is None:
            raise SysAgentProtocolError(
                f"process {process_id} has no module index {module_index}")
        executable_maps_by_pid[pid] = _parse_executable_maps(
            require_ok(client.command(
                f"debug maps 0x{pid:X} 256", retry=True)))
        executable_maps, _ = executable_maps_by_pid[pid]
        if not executable_maps:
            raise SysAgentProtocolError(
                f"process {process_id} has no executable mapping")
        code_base = min(int(entry["base"]) for entry in executable_maps)

        expected = process.get("expect", {})
        if expected:
            expected = _profile_object(expected, f"process {process_id} expect")
            expected_title = (
                _profile_hex_id(expected["titleId"], "expect.titleId")
                if "titleId" in expected else None)
            if expected_title is not None and title_id != expected_title:
                raise SysAgentProtocolError(
                    f"process {process_id} title ID mismatch: "
                    f"got {title_id:016X}, expected {expected_title:016X}")
            expected_build = expected.get("buildId")
            actual_build = str(module["buildId"])
            if (expected_build
                    and not actual_build.upper().startswith(str(expected_build).upper())):
                raise SysAgentProtocolError(
                    f"process {process_id} build ID mismatch: "
                    f"got {actual_build}, expected {expected_build}")

        resolved = {
            "id": process_id,
            "pid": pid,
            "titleId": f"{title_id:016X}" if title_id is not None else "NA",
            "moduleIndex": module_index,
            "moduleBase": f"0x{int(module['base']):X}",
            "moduleSize": f"0x{int(module['size']):X}",
            "codeBase": f"0x{code_base:X}",
            "buildId": module["buildId"],
        }
        resolved_processes.append(resolved)
        resolved_by_id[process_id] = resolved

    pids = [int(process["pid"]) for process in resolved_processes]
    if len(set(pids)) != len(pids):
        raise SysAgentProtocolError("trace profile resolves to duplicate PIDs")

    resolved_targets: list[dict[str, object]] = []
    addresses: set[int] = set()
    for index, raw_target in enumerate(_profile_target_order(profile)):
        target = _profile_object(raw_target, "target")
        target_id = str(target["id"])
        process = resolved_by_id[str(target["process"])]
        offset = _profile_target_offset(
            profile, target, f"target {target_id}")
        base_kind = str(target.get("base", "main"))
        base_address = int(str(
            process["codeBase"] if base_kind == "code"
            else process["moduleBase"]), 0)
        address = base_address + offset
        if address in addresses:
            raise SysAgentProtocolError(
                f"trace profile resolves duplicate target address 0x{address:X}")
        executable_maps, maps_truncated = executable_maps_by_pid[
            int(process["pid"])]
        mapping = next(
            (entry for entry in executable_maps
             if int(entry["base"]) <= address
             < int(entry["base"]) + int(entry["size"])),
            None)
        if mapping is None and not maps_truncated:
            raise SysAgentProtocolError(
                f"target {target_id} address 0x{address:X} is not in an "
                "executable mapping")
        expected_bytes = target.get("expectBytes")
        if expected_bytes is not None:
            expected = bytes.fromhex(str(expected_bytes))
            actual = client.debug_read(
                int(process["pid"]), address, len(expected))
            if actual != expected:
                raise SysAgentProtocolError(
                    f"target {target_id} instruction mismatch: expected "
                    f"{expected.hex().upper()}, got {actual.hex().upper()}")
        target_snapshots = target.get("_parsedSnapshots", global_snapshots)
        if not isinstance(target_snapshots, list):
            target_snapshots = global_snapshots
        addresses.add(address)
        resolved_targets.append({
            "id": target_id,
            "index": index,
            "processId": process["id"],
            "pid": process["pid"],
            "address": f"0x{address:X}",
            "moduleIndex": process["moduleIndex"],
            "moduleBase": process["moduleBase"],
            "codeBase": process["codeBase"],
            "base": base_kind,
            "offset": f"0x{offset:X}",
            "expectBytes": expected_bytes,
            "snapshots": target_snapshots,
            "mapping": (
                f"0x{int(mapping['base']):X}+0x{int(mapping['size']):X}"
                f":p{int(mapping['permission'])}"
                if mapping is not None else "unverified-truncated"),
        })

    return {
        "schemaVersion": TRACE_PROFILE_SCHEMA_VERSION,
        "kind": "trace-plan",
        "profile": profile.get("name", "trace-profile"),
        "mode": "single" if len(resolved_processes) == 1 else "multitrace",
        "processes": resolved_processes,
        "targets": resolved_targets,
        "pairs": profile.get("pairs", []),
        "trace": profile.get("trace", {}),
    }


def load_trace_jsonl(paths: Sequence[str]) -> list[dict[str, object]]:
    """Load versioned JSONL trace exports, accepting old event-only dumps."""
    documents: list[dict[str, object]] = []
    for path in paths:
        metadata: dict[str, object] | None = None
        events: list[dict[str, object]] = []
        legacy = False
        with open(path, encoding="utf-8") as handle:
            for line_number, raw_line in enumerate(handle, 1):
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise SysAgentProtocolError(
                        f"{path}:{line_number}: invalid JSON: {error}") from error
                if not isinstance(record, dict):
                    raise SysAgentProtocolError(
                        f"{path}:{line_number}: trace record must be an object")
                kind = record.get("record")
                if kind == "meta":
                    if metadata is not None:
                        raise SysAgentProtocolError(
                            f"{path}:{line_number}: duplicate trace metadata")
                    schema = parse_int(str(record.get("schemaVersion", "0")))
                    if schema != TRACE_SCHEMA_VERSION:
                        raise SysAgentProtocolError(
                            f"{path}:{line_number}: unsupported trace schema {schema}")
                    metadata = record
                elif kind == "event" or (kind is None and "seq" in record):
                    if kind is None:
                        legacy = True
                    if "seq" not in record:
                        raise SysAgentProtocolError(
                            f"{path}:{line_number}: trace event has no seq")
                    events.append(record)
                else:
                    raise SysAgentProtocolError(
                        f"{path}:{line_number}: unknown trace record type {kind!r}")
        if metadata is None:
            metadata = {
                "record": "meta",
                "schemaVersion": 0 if legacy else TRACE_SCHEMA_VERSION,
                "sessionId": os.path.basename(path),
                "legacy": True,
            }
        metadata.setdefault("sessionId", os.path.basename(path))
        documents.append({
            "path": path,
            "metadata": metadata,
            "events": events,
        })
    return documents


def _trace_address(value: object) -> int | None:
    if value is None:
        return None
    try:
        return int(str(value), 16)
    except ValueError:
        return None


CMIF_IN_HEADER_MAGIC = 0x49434653
CMIF_OUT_HEADER_MAGIC = 0x4F434653
CMIF_CONTROL_OPERATIONS = {
    0: "convert-current-object-to-domain",
    1: "copy-from-current-domain",
    2: "clone-current-object",
    3: "query-pointer-buffer-size",
    4: "clone-current-object-ex",
}
TRACE_CORRELATION_EVENT_FIELDS = {
    "thread", "pc", "lr", "sp", "target", "targetAddress",
    *(f"x{index}" for index in range(8)),
}


def _read_u32(data: bytes, offset: int, label: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise SysAgentProtocolError(f"{label} is truncated at offset 0x{offset:X}")
    return int.from_bytes(data[offset:offset + 4], "little")


def _read_u64(data: bytes, offset: int, label: str) -> int:
    if offset < 0 or offset + 8 > len(data):
        raise SysAgentProtocolError(f"{label} is truncated at offset 0x{offset:X}")
    return int.from_bytes(data[offset:offset + 8], "little")


def _validate_correlation_expression(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be a non-empty string")
    parts = value.split(":")
    if len(parts) == 2 and parts[0] == "event":
        if parts[1] not in TRACE_CORRELATION_EVENT_FIELDS:
            raise ValueError(f"{label} has unsupported event field {parts[1]!r}")
        return value
    if len(parts) == 3 and parts[0] == "ipc" and parts[1] in {
            "header", "domain"}:
        field = parts[2]
        allowed = (
            {"commandId", "token", "result", "interfaceId"}
            if parts[1] == "header" else {"type", "objectId", "token"})
        if field not in allowed:
            raise ValueError(
                f"{label} has unsupported ipc.{parts[1]} field {field!r}")
        return value
    if len(parts) == 4 and parts[0] in {"ipc", "snapshot"}:
        try:
            offset = parse_int(parts[2])
            size = parse_int(parts[3])
        except ValueError as exc:
            raise ValueError(
                f"{label} offset and size must be integers") from exc
        if offset < 0 or not 1 <= size <= 64:
            raise ValueError(f"{label} needs offset >= 0 and size 1..64")
        if parts[0] == "ipc" and parts[1] != "payload":
            raise ValueError(f"{label} ipc bytes must use ipc:payload")
        if parts[0] == "snapshot" and parts[1] not in {"0", "1"}:
            raise ValueError(f"{label} snapshot index must be 0 or 1")
        return value
    raise ValueError(
        f"{label} must be event:FIELD, ipc:header:FIELD, "
        "ipc:domain:FIELD, ipc:payload:OFFSET:SIZE, or "
        "snapshot:INDEX:OFFSET:SIZE")


def _correlation_scalar(value: object) -> str:
    if isinstance(value, int):
        return f"{value:X}"
    text = str(value)
    try:
        return f"{int(text, 16):X}"
    except ValueError:
        return text.upper()


def _correlation_value(event: dict[str, object],
                       decoded: dict[str, object],
                       expression: str) -> str | None:
    parts = expression.split(":")
    if parts[0] == "event":
        value = event.get(parts[1])
        if value is None:
            return None
        return _correlation_scalar(value)
    if parts[0] == "snapshot":
        encoded = event.get(f"snap{parts[1]}Data")
        if not isinstance(encoded, str):
            return None
        try:
            data = bytes.fromhex(encoded)
        except ValueError:
            return None
        offset = parse_int(parts[2])
        size = parse_int(parts[3])
        if offset + size > len(data):
            return None
        return data[offset:offset + size].hex().upper()
    cmif = decoded.get("cmif")
    if not isinstance(cmif, dict):
        return None
    if parts[1] == "payload":
        encoded = cmif.get("payloadHex")
        if not isinstance(encoded, str):
            return None
        try:
            data = bytes.fromhex(encoded)
        except ValueError:
            return None
        offset = parse_int(parts[2])
        size = parse_int(parts[3])
        if offset + size > len(data):
            return None
        return data[offset:offset + size].hex().upper()
    container = cmif.get(
        "header" if parts[1] == "header" else "domainHeader")
    if not isinstance(container, dict):
        return None
    value = container.get(parts[2])
    return None if value is None else _correlation_scalar(value)


def _pair_correlation(pair: dict[str, object]) -> dict[str, object] | None:
    ipc = pair.get("ipc")
    if not isinstance(ipc, dict):
        return None
    correlation = ipc.get("correlation")
    if correlation is None:
        return None
    if not isinstance(correlation, dict):
        raise ValueError("pair ipc.correlation must be an object")
    request_key = _validate_correlation_expression(
        correlation.get("request"), "pair ipc.correlation.request")
    _validate_correlation_expression(
        correlation.get("response", request_key),
        "pair ipc.correlation.response")
    if "thread" in correlation and not isinstance(correlation["thread"], bool):
        raise ValueError("pair ipc.correlation.thread must be boolean")
    fallback = correlation.get("fallback", "none")
    if fallback not in {"none", "thread-fifo"}:
        raise ValueError(
            "pair ipc.correlation.fallback must be none or thread-fifo")
    return correlation


def _remove_pending_request(requests: list[dict[str, object]],
                            request: dict[str, object]) -> None:
    for index, candidate in enumerate(requests):
        if candidate is request:
            requests.pop(index)
            return


def _hipc_static_descriptor(data: bytes, offset: int) -> dict[str, int]:
    first = _read_u32(data, offset, "HIPC static descriptor")
    second = _read_u32(data, offset + 4, "HIPC static descriptor")
    return {
        "index": first & 0x3F,
        "address": (
            second
            | (((first >> 6) & 0x3F) << 36)
            | (((first >> 12) & 0xF) << 32)
        ),
        "size": (first >> 16) & 0xFFFF,
    }


def _hipc_buffer_descriptor(data: bytes, offset: int) -> dict[str, int]:
    size_low = _read_u32(data, offset, "HIPC buffer descriptor")
    address_low = _read_u32(data, offset + 4, "HIPC buffer descriptor")
    attributes = _read_u32(data, offset + 8, "HIPC buffer descriptor")
    return {
        "mode": attributes & 0x3,
        "address": (
            address_low
            | (((attributes >> 28) & 0xF) << 32)
            | (((attributes >> 2) & 0x3FFFFF) << 36)
        ),
        "size": size_low | (((attributes >> 24) & 0xF) << 32),
    }


def _cmif_operation(command_type: object, cmif: dict[str, object]) -> str | None:
    """Classify generic CMIF request mechanics without assigning service semantics."""
    if cmif.get("mode") != "request":
        return None
    domain_header = cmif.get("domainHeader")
    if cmif.get("domain") and isinstance(domain_header, dict):
        if domain_header.get("type") == 2:
            return "close-domain-object"
        if domain_header.get("type") == 1:
            return "invoke-method"
        return "unknown-domain-operation"
    header = cmif.get("header")
    if not isinstance(header, dict):
        return None
    if command_type in {3, 5, 7}:
        command_id = header.get("commandId")
        if isinstance(command_id, int) and command_id in CMIF_CONTROL_OPERATIONS:
            return CMIF_CONTROL_OPERATIONS[command_id]
    return "invoke-method"


def decode_hipc_cmif(data: bytes, mode: str = "auto") -> dict[str, object]:
    """Decode an HIPC request/response containing a CMIF SFCI or SFCO payload."""
    if mode not in {"auto", "request", "response"}:
        raise ValueError("HIPC mode must be auto, request or response")
    if len(data) < 8:
        raise SysAgentProtocolError("HIPC data is shorter than its header")

    def decode_one(selected_mode: str) -> dict[str, object]:
        header0 = _read_u32(data, 0, "HIPC header")
        header1 = _read_u32(data, 4, "HIPC header")
        metadata = {
            "type": header0 & 0xFFFF,
            "sendStatics": (header0 >> 16) & 0xF,
            "sendBuffers": (header0 >> 20) & 0xF,
            "receiveBuffers": (header0 >> 24) & 0xF,
            "exchangeBuffers": (header0 >> 28) & 0xF,
            "dataWords": header1 & 0x3FF,
            "receiveStaticMode": (header1 >> 10) & 0xF,
            "specialHeader": (header1 >> 31) & 1,
            "copyHandles": 0,
            "moveHandles": 0,
            "pid": None,
        }
        offset = 8
        if metadata["specialHeader"]:
            special = _read_u32(data, offset, "HIPC special header")
            metadata["copyHandles"] = (special >> 1) & 0xF
            metadata["moveHandles"] = (special >> 5) & 0xF
            offset += 4
            if special & 1:
                metadata["pid"] = _read_u64(data, offset, "HIPC PID")
                offset += 8

        handle_bytes = 4 * (
            int(metadata["copyHandles"]) + int(metadata["moveHandles"]))
        if offset + handle_bytes > len(data):
            raise SysAgentProtocolError("HIPC handle area is truncated")
        handles: list[dict[str, object]] = []
        for kind, count in (
                ("copy", int(metadata["copyHandles"])),
                ("move", int(metadata["moveHandles"]))):
            for handle_index in range(count):
                value = _read_u32(data, offset, "HIPC handle")
                handles.append({
                    "kind": kind,
                    "index": handle_index,
                    "value": value,
                    "valueHex": f"0x{value:08X}",
                })
                offset += 4

        descriptors: list[dict[str, object]] = []
        if selected_mode == "request":
            for name, count, parser, descriptor_size in (
                    ("sendStatic", int(metadata["sendStatics"]),
                     _hipc_static_descriptor, 8),
                    ("sendBuffer", int(metadata["sendBuffers"]),
                     _hipc_buffer_descriptor, 12),
                    ("receiveBuffer", int(metadata["receiveBuffers"]),
                     _hipc_buffer_descriptor, 12),
                    ("exchangeBuffer", int(metadata["exchangeBuffers"]),
                     _hipc_buffer_descriptor, 12)):
                for _ in range(count):
                    descriptor = parser(data, offset)
                    descriptor["kind"] = name
                    descriptors.append(descriptor)
                    offset += descriptor_size
        else:
            for _ in range(int(metadata["sendStatics"])):
                descriptor = _hipc_static_descriptor(data, offset)
                descriptor["kind"] = "sendStatic"
                descriptors.append(descriptor)
                offset += 8

        data_words_offset = offset
        data_words_end = data_words_offset + int(metadata["dataWords"]) * 4
        if data_words_end > len(data):
            raise SysAgentProtocolError("HIPC data-word area is truncated")
        cmif_offset = (data_words_offset + 15) & ~15
        cmif = None
        if (selected_mode == "request"
                and metadata["type"] == 4
                and cmif_offset + 16 <= data_words_end
                and data[cmif_offset] == 2):
            domain = {
                "type": data[cmif_offset],
                "numInObjects": data[cmif_offset + 1],
                "dataSize": int.from_bytes(
                    data[cmif_offset + 2:cmif_offset + 4], "little"),
                "objectId": _read_u32(
                    data, cmif_offset + 4, "CMIF domain header"),
                "token": _read_u32(
                    data, cmif_offset + 12, "CMIF domain header"),
            }
            if (domain["numInObjects"] == 0 and domain["dataSize"] == 0
                    and domain["objectId"] != 0):
                cmif = {
                    "mode": selected_mode,
                    "domain": True,
                    "domainHeader": domain,
                    "header": None,
                    "payloadOffset": cmif_offset + 16,
                    "payloadHex": data[
                        cmif_offset + 16:data_words_end].hex().upper(),
                }
        for is_domain in (False, True):
            if cmif is not None:
                break
            candidate = cmif_offset + (16 if is_domain else 0)
            if candidate + 16 > data_words_end:
                continue
            magic = _read_u32(data, candidate, "CMIF header")
            expected = (
                CMIF_IN_HEADER_MAGIC if selected_mode == "request"
                else CMIF_OUT_HEADER_MAGIC)
            if magic != expected:
                continue
            if is_domain:
                domain_offset = cmif_offset
                domain = {
                    "type": data[domain_offset],
                    "numInObjects": data[domain_offset + 1]
                    if selected_mode == "request" else None,
                    "dataSize": int.from_bytes(
                        data[domain_offset + 2:domain_offset + 4], "little")
                    if selected_mode == "request" else None,
                    "objectId": _read_u32(
                        data, domain_offset + 4, "CMIF domain header")
                    if selected_mode == "request" else None,
                    "token": _read_u32(
                        data, domain_offset + 12, "CMIF domain header")
                    if selected_mode == "request" else None,
                }
                if selected_mode == "response":
                    domain["numOutObjects"] = _read_u32(
                        data, domain_offset, "CMIF domain header")
            else:
                domain = None
            payload_offset = candidate + 16
            header = {
                "magic": f"{magic:08X}",
                "version": _read_u32(data, candidate + 4, "CMIF header"),
            }
            if selected_mode == "request":
                header["commandId"] = _read_u32(
                    data, candidate + 8, "CMIF header")
                header["token"] = _read_u32(
                    data, candidate + 12, "CMIF header")
            else:
                header["result"] = f"0x{_read_u32(data, candidate + 8, 'CMIF header'):X}"
                header["interfaceId"] = _read_u32(
                    data, candidate + 12, "CMIF header")
            cmif = {
                "mode": selected_mode,
                "domain": is_domain,
                "domainHeader": domain,
                "header": header,
                "payloadOffset": payload_offset,
                "payloadHex": data[payload_offset:data_words_end].hex().upper(),
            }
            break
        if cmif is None:
            raise SysAgentProtocolError(
                f"HIPC data does not contain a CMIF {selected_mode} header")
        cmif["operation"] = _cmif_operation(metadata["type"], cmif)
        hipc = {
            **metadata,
            "dataWordsOffset": data_words_offset,
            "dataWordsHex": data[
                data_words_offset:data_words_end].hex().upper(),
            "handles": handles,
            "descriptors": descriptors,
        }
        return {
            "hipc": hipc,
            "cmif": cmif,
        }

    modes = ("request", "response") if mode == "auto" else (mode,)
    errors: list[str] = []
    for selected_mode in modes:
        try:
            return decode_one(selected_mode)
        except SysAgentProtocolError as error:
            errors.append(str(error))
    raise SysAgentProtocolError("; ".join(errors))


def _domain_object_record(domain_objects: dict[str, dict[str, object]],
                          object_id: object, tick: object) -> dict[str, object]:
    object_key = str(object_id)
    return domain_objects.setdefault(object_key, {
        "objectId": object_id,
        "firstTick": tick,
        "lastTick": tick,
        "createCount": 0,
        "closeCount": 0,
        "requestCount": 0,
        "responseCount": 0,
        "commands": {},
        "results": {},
        "createdTick": None,
        "closedTick": None,
    })


def decode_trace_ipc(documents: Sequence[dict[str, object]],
                     snapshot_index: int | None = None,
                     profile: dict[str, object] | None = None
                     ) -> dict[str, object]:
    """Decode CMIF/HIPC snapshots embedded in versioned trace events."""
    records: list[dict[str, object]] = []
    transactions: list[dict[str, object]] = []
    domain_objects: dict[str, dict[str, object]] = {}
    domain_events: list[dict[str, object]] = []
    skipped = 0
    pending_requests: dict[
        tuple[str, str, str, str], list[dict[str, object]]] = {}
    pending_keyed: dict[tuple[object, ...], list[dict[str, object]]] = {}
    unmatched_responses = 0
    for document in documents:
        metadata = _profile_object(document.get("metadata"), "trace metadata")
        session_id = str(metadata.get("sessionId", document.get("path", "")))
        targets = metadata.get("targets")
        target_metadata = {
            str(target.get("index")): target
            for target in targets
            if isinstance(target, dict)
        } if isinstance(targets, list) else {}
        if profile is not None:
            trace_config = profile.get("trace", {})
            trace_config = trace_config if isinstance(trace_config, dict) else {}
            default_snapshots = _profile_snapshot_entries(
                trace_config.get("snapshots", []), "trace.snapshots")
            for index, raw_target in enumerate(_profile_target_order(profile)):
                target = _profile_object(raw_target, "profile target")
                trace_target = target_metadata.get(str(index))
                if not isinstance(trace_target, dict):
                    raise SysAgentProtocolError(
                        f"trace has no target index {index} for {target['id']}")
                process = _profile_object(
                    trace_target.get("process"), "trace target process")
                module_base = _trace_address(process.get("moduleBase"))
                trace_address = _trace_address(trace_target.get("address"))
                offset = _profile_target_offset(
                    profile, target, f"target {target['id']}")
                base_kind = str(target.get("base", "main"))
                code_base = _trace_address(trace_target.get("codeBase"))
                base_address = code_base if base_kind == "code" else module_base
                if (base_address is None or trace_address is None
                        or base_address + offset != trace_address):
                    raise SysAgentProtocolError(
                        f"trace target index {index} does not match profile "
                        f"target {target['id']}")
                target_metadata[str(index)] = {
                    **trace_target,
                    "id": target["id"],
                    "processId": target["process"],
                    "snapshots": target.get(
                        "_parsedSnapshots", default_snapshots),
                }
            profile_pairs = _profile_list(profile.get("pairs", []), "pairs")
        else:
            embedded = metadata.get("profile")
            embedded = embedded if isinstance(embedded, dict) else {}
            profile_pairs = embedded.get("pairs", [])
            if not isinstance(profile_pairs, list):
                profile_pairs = []
        pair_for_target: dict[str, dict[str, object]] = {}
        for pair in profile_pairs:
            if not isinstance(pair, dict):
                continue
            entry_id = str(pair.get("entry", ""))
            return_id = str(pair.get("return", ""))
            if entry_id:
                pair_for_target[entry_id] = pair
            if return_id:
                pair_for_target[return_id] = pair
        events = document.get("events")
        if not isinstance(events, list):
            continue
        for event in events:
            if not isinstance(event, dict):
                continue
            snapshot_count = parse_int(str(event.get("snapshots", "0")))
            target_index = str(event.get("target", "0"))
            target = target_metadata.get(target_index, {})
            snapshot_metadata = (
                target.get("snapshots", [])
                if isinstance(target, dict) else [])
            snapshot_metadata = (
                snapshot_metadata
                if isinstance(snapshot_metadata, list) else [])
            ipc_indexes = [
                index for index, entry in enumerate(snapshot_metadata)
                if isinstance(entry, dict) and entry.get("kind") == "ipc"
            ]
            indexes = (
                (snapshot_index,) if snapshot_index is not None
                else tuple(ipc_indexes) if ipc_indexes
                else tuple(range(snapshot_count)))
            for index in indexes:
                if not 0 <= index < 2:
                    raise ValueError("snapshot index must be 0 or 1")
                encoded = event.get(f"snap{index}Data")
                if not isinstance(encoded, str) or not encoded:
                    skipped += 1
                    continue
                try:
                    decoded = decode_hipc_cmif(
                        bytes.fromhex(encoded), mode="auto")
                except (ValueError, SysAgentProtocolError):
                    skipped += 1
                    continue
                snapshot_info = (
                    snapshot_metadata[index]
                    if index < len(snapshot_metadata)
                    and isinstance(snapshot_metadata[index], dict)
                    else {})
                record = {
                    "sessionId": session_id,
                    "seq": event.get("seq"),
                    "tick": event.get("tick"),
                    "thread": event.get("thread"),
                    "target": target_index,
                    "targetAddress": event.get("targetAddress"),
                    "targetId": target.get("id") if isinstance(target, dict) else None,
                    "snapshot": index,
                    "snapshotName": snapshot_info.get("name"),
                    "snapshotKind": snapshot_info.get("kind"),
                    "snapshotAddress": event.get(f"snap{index}Address"),
                    **decoded,
                }
                records.append(record)
                cmif = decoded["cmif"]
                assert isinstance(cmif, dict)
                operation = cmif.get("operation")
                domain_header = cmif.get("domainHeader")
                if (cmif.get("domain") and isinstance(domain_header, dict)
                        and domain_header.get("objectId")):
                    object_id = domain_header["objectId"]
                    object_record = _domain_object_record(
                        domain_objects, object_id, record.get("tick"))
                    object_record["lastTick"] = record.get("tick")
                    if cmif.get("mode") == "request":
                        if operation == "close-domain-object":
                            object_record["closeCount"] = (
                                int(object_record["closeCount"]) + 1)
                            object_record["closedTick"] = record.get("tick")
                            domain_events.append({
                                "type": "close-domain-object",
                                "sessionId": session_id,
                                "seq": record.get("seq"),
                                "tick": record.get("tick"),
                                "thread": record.get("thread"),
                                "targetId": record.get("targetId"),
                                "objectId": object_id,
                            })
                        else:
                            object_record["requestCount"] = (
                                int(object_record["requestCount"]) + 1)
                            header = cmif.get("header")
                            if isinstance(header, dict) and "commandId" in header:
                                command_key = str(header["commandId"])
                                commands = object_record["commands"]
                                assert isinstance(commands, dict)
                                commands[command_key] = (
                                    commands.get(command_key, 0) + 1)
                target_id = record.get("targetId")
                pair = pair_for_target.get(str(target_id))
                if pair is not None:
                    entry_id = str(pair["entry"])
                    return_id = str(pair["return"])
                    thread = str(event.get("thread", "0"))
                    correlation = _pair_correlation(pair)
                    key = (session_id, thread,
                        entry_id, return_id)
                    if (target_id == entry_id and cmif["mode"] == "request"
                            and operation != "close-domain-object"):
                        requests = pending_requests.setdefault(key, [])
                        outstanding_at_request = len(requests) + 1
                        request_correlation_key = None
                        if correlation is not None:
                            request_correlation_key = _correlation_value(
                                event, decoded,
                                str(correlation["request"]))
                            record["correlationKey"] = request_correlation_key
                            if request_correlation_key is not None:
                                correlation_key: tuple[object, ...] = (
                                    session_id, entry_id, return_id)
                                if correlation.get("thread", True):
                                    correlation_key += (thread,)
                                correlation_key += (request_correlation_key,)
                                keyed_requests = pending_keyed.setdefault(
                                    correlation_key, [])
                                outstanding_at_request = len(
                                    keyed_requests) + 1
                                keyed_requests.append(record)
                        record["outstandingAtRequest"] = outstanding_at_request
                        requests.append(record)
                    elif target_id == return_id and cmif["mode"] == "response":
                        request = None
                        outstanding_at_response = 0
                        correlation_kind = "thread-fifo"
                        response_correlation_key = None
                        if correlation is not None:
                            response_correlation_key = _correlation_value(
                                event, decoded,
                                str(correlation.get("response",
                                    correlation["request"])))
                            if response_correlation_key is not None:
                                correlation_key = (
                                    session_id, entry_id, return_id)
                                if correlation.get("thread", True):
                                    correlation_key += (thread,)
                                correlation_key += (response_correlation_key,)
                                keyed_requests = pending_keyed.get(
                                    correlation_key)
                                if keyed_requests:
                                    outstanding_at_response = len(keyed_requests)
                                    request = keyed_requests.pop(0)
                                    if not keyed_requests:
                                        pending_keyed.pop(correlation_key, None)
                                    request_thread = str(
                                        request.get("thread", "0"))
                                    request_fifo_key = (
                                        session_id, request_thread,
                                        entry_id, return_id)
                                    _remove_pending_request(
                                        pending_requests.get(
                                            request_fifo_key, []), request)
                                    correlation_kind = (
                                        "profile-key"
                                        if outstanding_at_response == 1
                                        else "ambiguous-profile-key")
                        if request is None:
                            requests = pending_requests.get(key)
                            fallback = (
                                correlation is None
                                or correlation.get("fallback") == "thread-fifo")
                            if not fallback or not requests:
                                unmatched_responses += 1
                                continue
                            outstanding_at_response = len(requests)
                            request = requests.pop(0)
                            correlation_kind = (
                                "thread-fifo"
                                if outstanding_at_response == 1
                                else "ambiguous-thread-fifo")
                            if correlation is not None:
                                correlation_kind = (
                                    "thread-fifo-fallback"
                                    if outstanding_at_response == 1
                                    else "ambiguous-thread-fifo-fallback")
                        if request is not None:
                            request_cmif = request["cmif"]
                            assert isinstance(request_cmif, dict)
                            request_header = request_cmif.get("header")
                            response_header = cmif["header"]
                            assert isinstance(response_header, dict)
                            request_operation = request_cmif.get("operation")
                            ipc_metadata = pair.get("ipc", {})
                            ipc_metadata = (
                                ipc_metadata
                                if isinstance(ipc_metadata, dict) else {})
                            command_id = (
                                request_header.get("commandId")
                                if isinstance(request_header, dict) else None)
                            command_name = None
                            commands = ipc_metadata.get("commands", {})
                            if isinstance(commands, dict):
                                for key_id, label in commands.items():
                                    if parse_int(str(key_id)) == command_id:
                                        command_name = label
                                        break
                            interface_hash = ipc_metadata.get("interfaceHash")
                            if interface_hash is not None:
                                interface_hash = (
                                    f"0x{_profile_hex_id(interface_hash, 'interfaceHash'):X}")
                            response_result = response_header.get("result")
                            response_interface_id = response_header.get(
                                "interfaceId")
                            interface_matches = None
                            if (interface_hash is not None
                                    and isinstance(response_interface_id, int)
                                    and response_interface_id != 0):
                                interface_matches = (
                                    response_interface_id
                                    == int(interface_hash, 16))
                            created_object_id = None
                            response_payload = cmif.get("payloadHex")
                            if (request_operation == "convert-current-object-to-domain"
                                    and str(response_result) in {"0x0", "0"}
                                    and isinstance(response_payload, str)
                                    and len(response_payload) >= 8):
                                created_object_id = int.from_bytes(
                                    bytes.fromhex(response_payload[:8]), "little")
                                if created_object_id == 0:
                                    created_object_id = None
                            transaction = {
                                "sessionId": session_id,
                                "thread": event.get("thread"),
                                "requestThread": request.get("thread"),
                                "responseThread": event.get("thread"),
                                "entryTarget": entry_id,
                                "returnTarget": return_id,
                                "requestSeq": request.get("seq"),
                                "returnSeq": record.get("seq"),
                                "requestTick": request.get("tick"),
                                "returnTick": record.get("tick"),
                                "operation": request_operation,
                                "commandId": command_id,
                                "commandName": command_name,
                                "service": ipc_metadata.get("service"),
                                "interfaceHash": interface_hash,
                                "requestToken": (
                                    request_header.get("token")
                                    if isinstance(request_header, dict) else None),
                                "responseInterfaceId": response_interface_id,
                                "interfaceMatchesProfile": interface_matches,
                                "result": response_result,
                                "objectId": (
                                    request_cmif.get("domainHeader", {}).get(
                                        "objectId")
                                    if isinstance(
                                        request_cmif.get("domainHeader"), dict)
                                    else None),
                                "createdObjectId": created_object_id,
                                "requestHandles": request["hipc"].get(
                                    "handles", []),
                                "responseHandles": record["hipc"].get(
                                    "handles", []),
                                "requestSnapshot": request.get("snapshot"),
                                "responseSnapshot": record.get("snapshot"),
                                "requestCorrelationKey":
                                    request.get("correlationKey"),
                                "responseCorrelationKey":
                                    response_correlation_key,
                                "outstandingAtResponse": outstanding_at_response,
                                "correlation": correlation_kind,
                                "correlationAmbiguous":
                                    outstanding_at_response > 1,
                            }
                            transactions.append(transaction)
                            if created_object_id is not None:
                                created_record = _domain_object_record(
                                    domain_objects, created_object_id,
                                    record.get("tick"))
                                created_record["createCount"] = (
                                    int(created_record["createCount"]) + 1)
                                created_record["createdTick"] = record.get("tick")
                                domain_events.append({
                                    "type": "create-domain-object",
                                    "sessionId": session_id,
                                    "seq": record.get("seq"),
                                    "tick": record.get("tick"),
                                    "thread": record.get("thread"),
                                    "targetId": record.get("targetId"),
                                    "objectId": created_object_id,
                                })
                            if transaction_object_id := transactions[-1].get(
                                    "objectId"):
                                object_record = _domain_object_record(
                                    domain_objects, transaction_object_id,
                                    record.get("tick"))
                                object_record["lastTick"] = record.get("tick")
                                object_record["responseCount"] = (
                                    int(object_record["responseCount"]) + 1)
                                result = str(response_result)
                                results = object_record["results"]
                                assert isinstance(results, dict)
                                results[result] = results.get(result, 0) + 1
    unmatched_requests = sum(
        len(requests) for requests in pending_requests.values())
    domain_objects_out = sorted(
        (record for record in domain_objects.values()
         if int(record["closeCount"]) != 0
         or int(record["createCount"]) != 0
         or int(record["requestCount"]) != 0),
        key=lambda item: int(item["objectId"]))
    domain_events.sort(key=lambda item: (
        parse_int(str(item.get("tick", "0"))),
        parse_int(str(item.get("seq", "0")))))
    ambiguous_transactions = sum(
        1 for transaction in transactions
        if transaction.get("correlationAmbiguous"))
    correlation_counts: dict[str, int] = {}
    for transaction in transactions:
        method = str(transaction.get("correlation", "unknown"))
        correlation_counts[method] = correlation_counts.get(method, 0) + 1
    return {
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "records": records,
        "transactions": transactions,
        "domainEvents": domain_events,
        "domainObjects": domain_objects_out,
        "summary": {
            "traceCount": len(documents),
            "ipcCount": len(records),
            "transactionCount": len(transactions),
            "unmatchedRequests": unmatched_requests,
            "unmatchedResponses": unmatched_responses,
            "ambiguousTransactions": ambiguous_transactions,
            "correlationMethods": correlation_counts,
            "domainEventCount": len(domain_events),
            "domainObjectCount": len(domain_objects_out),
            "skippedSnapshots": skipped,
        },
    }


def build_trace_timeline(
        documents: Sequence[dict[str, object]],
        profile: dict[str, object] | None = None) -> dict[str, object]:
    """Merge multiple trace sessions into one monotonic boot timeline."""
    boot_epochs: set[int] = set()
    timeline: list[dict[str, object]] = []
    process_counts: dict[str, int] = {}
    ipc_count = 0
    transaction_count = 0
    ambiguous_transactions = 0
    correlation_counts: dict[str, int] = {}
    domain_event_count = 0

    for document in documents:
        metadata = _profile_object(document.get("metadata"), "trace metadata")
        clock = metadata.get("clock")
        if isinstance(clock, dict):
            boot_epoch = clock.get("bootEpochUnixMs")
            if isinstance(boot_epoch, int):
                boot_epochs.add(boot_epoch)
        session_id = str(metadata.get("sessionId", document.get("path", "")))
        targets = metadata.get("targets")
        target_metadata = {
            str(target.get("index")): target
            for target in targets
            if isinstance(target, dict)
        } if isinstance(targets, list) else {}
        ipc_report = decode_trace_ipc([document], profile=profile)
        if profile is not None:
            for index, raw_target in enumerate(_profile_target_order(profile)):
                target = _profile_object(raw_target, "profile target")
                trace_target = target_metadata.get(str(index))
                if isinstance(trace_target, dict):
                    target_metadata[str(index)] = {
                        **trace_target,
                        "id": target["id"],
                        "processId": target["process"],
                    }
        ipc_by_event: dict[tuple[str, str], list[dict[str, object]]] = {}
        for ipc_record in ipc_report["records"]:
            assert isinstance(ipc_record, dict)
            key = (str(ipc_record.get("sessionId", "")),
                   str(ipc_record.get("seq", "")))
            cmif = ipc_record.get("cmif")
            assert isinstance(cmif, dict)
            header = cmif.get("header")
            header = header if isinstance(header, dict) else {}
            ipc_by_event.setdefault(key, []).append({
                "mode": cmif.get("mode"),
                "domain": cmif.get("domain"),
                "operation": cmif.get("operation"),
                "commandId": header.get("commandId"),
                "result": header.get("result"),
                "token": header.get("token"),
                "interfaceId": header.get("interfaceId"),
                "snapshot": ipc_record.get("snapshot"),
                "snapshotName": ipc_record.get("snapshotName"),
                "snapshotKind": ipc_record.get("snapshotKind"),
                "handles": ipc_record["hipc"].get("handles", []),
            })
        ipc_count += int(ipc_report["summary"]["ipcCount"])
        for transaction in ipc_report["transactions"]:
            assert isinstance(transaction, dict)
            transaction_count += 1
            if transaction.get("correlationAmbiguous"):
                ambiguous_transactions += 1
            method = str(transaction.get("correlation", "unknown"))
            correlation_counts[method] = correlation_counts.get(method, 0) + 1
            timeline.append({
                "type": "transaction",
                "tick": transaction.get("requestTick"),
                "sessionId": session_id,
                **transaction,
            })
        for domain_event in ipc_report["domainEvents"]:
            assert isinstance(domain_event, dict)
            domain_event_count += 1
            timeline.append(domain_event)

        events = document.get("events")
        if not isinstance(events, list):
            continue
        for event in events:
            if not isinstance(event, dict):
                continue
            target_index = str(event.get("target", "0"))
            target = target_metadata.get(target_index, {})
            process = (
                target.get("process", {})
                if isinstance(target, dict) else {})
            process = process if isinstance(process, dict) else {}
            pid = str(process.get("pid", "unknown"))
            process_counts[pid] = process_counts.get(pid, 0) + 1
            timeline.append({
                "type": "event",
                "sessionId": session_id,
                "seq": event.get("seq"),
                "tick": event.get("tick"),
                "thread": event.get("thread"),
                "pc": event.get("pc"),
                "lr": event.get("lr"),
                "sp": event.get("sp"),
                "x0": event.get("x0"),
                "target": target_index,
                "targetId": target.get("id") if isinstance(target, dict) else None,
                "targetAddress": event.get("targetAddress"),
                "pid": pid,
                "titleId": process.get("titleId"),
                "buildId": process.get("buildId"),
                "ipc": ipc_by_event.get((session_id, str(event.get("seq", ""))), []),
            })

    if len(boot_epochs) > 1:
        raise SysAgentProtocolError(
            "trace files come from different boots; system ticks are not comparable")
    timeline.sort(key=lambda event: (
        parse_int(str(event.get("tick", "0"))),
        0 if event.get("type") == "event" else 1,
        parse_int(str(event.get("seq", event.get("requestSeq", "0")))),
        str(event.get("sessionId", "")),
    ))
    return {
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "kind": "trace-timeline",
        "bootEpochUnixMs": next(iter(boot_epochs), None),
        "timeline": timeline,
        "summary": {
            "sessionCount": len(documents),
            "eventCount": sum(
                1 for event in timeline if event.get("type") == "event"),
            "ipcCount": ipc_count,
            "transactionCount": transaction_count,
            "ambiguousTransactions": ambiguous_transactions,
            "correlationMethods": correlation_counts,
            "domainEventCount": domain_event_count,
            "processCounts": process_counts,
            "tickFirst": min(
                (parse_int(str(event.get("tick", "0")))
                 for event in timeline), default=None),
            "tickLast": max(
                (parse_int(str(event.get("tick", "0")))
                 for event in timeline), default=None),
        },
    }


def analyze_trace_documents(
        documents: Sequence[dict[str, object]],
        pairs: Sequence[tuple[int, int]] = ()) -> dict[str, object]:
    """Summarize traces and correlate explicitly configured entry/return sites."""
    sessions: list[dict[str, object]] = []
    events: list[tuple[str, dict[str, object]]] = []
    process_counts: dict[str, int] = {}
    process_identities: dict[str, dict[str, object]] = {}
    target_counts: dict[str, int] = {}
    threads: set[str] = set()
    snapshot_errors = 0

    for document in documents:
        metadata = document["metadata"]
        assert isinstance(metadata, dict)
        session_id = str(metadata.get("sessionId", document["path"]))
        status = metadata.get("status")
        process_info = metadata.get("process")
        default_pid = "unknown"
        if isinstance(status, dict):
            pid = status.get("pid")
            if pid is not None:
                default_pid = str(pid)
        if isinstance(process_info, dict):
            process_identities[default_pid] = process_info
        default_process_info = process_info if isinstance(process_info, dict) else None
        target_processes: dict[str, dict[str, object]] = {}
        targets = metadata.get("targets")
        if isinstance(targets, list):
            for target in targets:
                if not isinstance(target, dict):
                    continue
                process = target.get("process")
                if isinstance(process, dict):
                    target_processes[str(target.get("index"))] = process
        document_events = document["events"]
        assert isinstance(document_events, list)
        sorted_events = sorted(
            (event for event in document_events if isinstance(event, dict)),
            key=lambda event: (
                parse_int(str(event.get("tick", "0"))),
                parse_int(str(event.get("seq", "0"))),
            ),
        )
        for event in sorted_events:
            process_info = target_processes.get(
                str(event.get("target", "0")), default_process_info)
            process_key = default_pid
            if (isinstance(process_info, dict)
                    and process_info.get("pid") is not None):
                process_key = str(process_info["pid"])
            if isinstance(process_info, dict):
                process_identities[process_key] = process_info
            process_counts[process_key] = (
                process_counts.get(process_key, 0) + 1)
            events.append((session_id, event))
            thread = str(event.get("thread", "0"))
            threads.add(thread)
            target = str(event.get("targetAddress", "unknown"))
            target_key = f"{process_key}:{target}"
            target_counts[target_key] = target_counts.get(target_key, 0) + 1
            for key, value in event.items():
                if (key.startswith("snap") and key.endswith("Result")
                        and value not in (None, "0x0", "0")):
                    snapshot_errors += 1
        sessions.append({
            "sessionId": session_id,
            "path": document["path"],
            "metadata": metadata,
            "eventCount": len(sorted_events),
        })

    events.sort(key=lambda item: (
        parse_int(str(item[1].get("tick", "0"))),
        parse_int(str(item[1].get("seq", "0"))),
    ))

    pair_reports: list[dict[str, object]] = []
    for entry_address, return_address in pairs:
        pending: dict[tuple[str, str], list[dict[str, object]]] = {}
        matched: list[dict[str, object]] = []
        unmatched = 0
        for session_id, event in events:
            pc = _trace_address(event.get("pc"))
            if pc is None:
                continue
            thread = str(event.get("thread", "0"))
            key = (session_id, thread)
            if pc == entry_address:
                pending.setdefault(key, []).append(event)
            elif pc == return_address:
                stack = pending.get(key)
                if not stack:
                    continue
                entry = stack.pop()
                matched.append({
                    "sessionId": session_id,
                    "thread": thread,
                    "entrySeq": entry.get("seq"),
                    "returnSeq": event.get("seq"),
                    "entryTick": entry.get("tick"),
                    "returnTick": event.get("tick"),
                    "x0": event.get("x0"),
                    "pc": event.get("pc"),
                })
        unmatched = sum(len(stack) for stack in pending.values())
        pair_reports.append({
            "entry": f"0x{entry_address:X}",
            "return": f"0x{return_address:X}",
            "matched": len(matched),
            "unmatchedEntries": unmatched,
            "calls": matched,
        })

    return {
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "sessions": sessions,
        "summary": {
            "sessionCount": len(sessions),
            "eventCount": len(events),
            "threadCount": len(threads),
            "snapshotErrors": snapshot_errors,
            "processCounts": process_counts,
            "processIdentities": process_identities,
            "targetCounts": target_counts,
            "tickFirst": min(
                (parse_int(str(event.get("tick", "0"))) for _, event in events),
                default=None),
            "tickLast": max(
                (parse_int(str(event.get("tick", "0"))) for _, event in events),
                default=None),
        },
        "pairs": pair_reports,
    }


def report_trace_with_profile(document: dict[str, object],
                              profile: dict[str, object]) -> dict[str, object]:
    """Correlate a trace export using target identities from a trace profile."""
    metadata = _profile_object(document.get("metadata"), "trace metadata")
    trace_targets = metadata.get("targets")
    if not isinstance(trace_targets, list):
        raise SysAgentProtocolError("trace has no target metadata")
    target_by_index: dict[int, dict[str, object]] = {}
    for raw_target in trace_targets:
        if not isinstance(raw_target, dict):
            continue
        index = parse_int(str(raw_target.get("index", "-1")))
        if index >= 0:
            target_by_index[index] = raw_target

    ordered_targets = _profile_target_order(profile)
    if len(target_by_index) < len(ordered_targets):
        raise SysAgentProtocolError(
            "trace target count does not match the profile")
    process_specs = {
        str(process["id"]): process
        for process in (
            _profile_object(value, "process")
            for value in _profile_list(profile["processes"], "processes")
        )
    }

    resolved_targets: list[dict[str, object]] = []
    address_by_id: dict[str, int] = {}
    for index, raw_target in enumerate(ordered_targets):
        target = _profile_object(raw_target, "target")
        target_id = str(target["id"])
        trace_target = target_by_index.get(index)
        if trace_target is None:
            raise SysAgentProtocolError(
                f"trace has no target index {index} for {target_id}")
        process = _profile_object(
            trace_target.get("process"), f"target {target_id} process")
        module_base = _trace_address(process.get("moduleBase"))
        trace_address = _trace_address(trace_target.get("address"))
        base_kind = str(target.get("base", "main"))
        code_base = _trace_address(trace_target.get("codeBase"))
        base_address = code_base if base_kind == "code" else module_base
        if base_address is None or trace_address is None:
            raise SysAgentProtocolError(
                f"trace target {target_id} has no module/address metadata")
        offset = _profile_target_offset(
            profile, target, f"target {target_id}")
        expected_address = base_address + offset
        if trace_address != expected_address:
            raise SysAgentProtocolError(
                f"trace target {target_id} address mismatch: "
                f"trace has 0x{trace_address:X}, profile expects "
                f"0x{expected_address:X}")
        process_spec = process_specs[str(target["process"])]
        expected = process_spec.get("expect", {})
        expected = expected if isinstance(expected, dict) else {}
        expected_title = (
            process_spec.get("titleId", expected.get("titleId")))
        actual_title = process.get("titleId")
        if expected_title is not None:
            try:
                parsed_actual_title = int(str(actual_title), 16)
            except (TypeError, ValueError):
                parsed_actual_title = None
            if (parsed_actual_title
                    != _profile_hex_id(expected_title, "expected titleId")):
                raise SysAgentProtocolError(
                    f"trace target {target_id} title ID does not match the profile")
        expected_build = expected.get("buildId")
        actual_build = process.get("buildId")
        if (expected_build and (
                not isinstance(actual_build, str)
                or not actual_build.upper().startswith(str(expected_build).upper()))):
            raise SysAgentProtocolError(
                f"trace target {target_id} build ID does not match the profile")
        address_by_id[target_id] = trace_address
        resolved_targets.append({
            "id": target_id,
            "index": index,
            "processId": target.get("process"),
            "pid": process.get("pid"),
            "titleId": process.get("titleId"),
            "buildId": process.get("buildId"),
            "address": f"0x{trace_address:X}",
            "base": base_kind,
            "moduleBase": process.get("moduleBase"),
            "codeBase": trace_target.get("codeBase"),
            "offset": f"0x{offset:X}",
            "snapshots": target.get("_parsedSnapshots", []),
        })

    pairs: list[dict[str, object]] = []
    numeric_pairs: list[tuple[int, int]] = []
    for raw_pair in _profile_list(profile.get("pairs", []), "pairs"):
        pair = _profile_object(raw_pair, "pair")
        entry_id = str(pair["entry"])
        return_id = str(pair["return"])
        numeric_pairs.append((address_by_id[entry_id], address_by_id[return_id]))
        pair_metadata = {
            "entryId": entry_id,
            "returnId": return_id,
        }
        if "ipc" in pair:
            pair_metadata["ipc"] = pair["ipc"]
        pairs.append(pair_metadata)

    report = analyze_trace_documents([document], numeric_pairs)
    raw_pair_reports = report.get("pairs")
    assert isinstance(raw_pair_reports, list)
    for pair, pair_report in zip(pairs, raw_pair_reports):
        assert isinstance(pair_report, dict)
        pair_report.update(pair)
        calls = pair_report.get("calls")
        assert isinstance(calls, list)
        first_nonzero = None
        for call in calls:
            assert isinstance(call, dict)
            result = _trace_address(call.get("x0"))
            call["returnX0"] = call.get("x0")
            call["x0NonZero"] = result is not None and result != 0
            if first_nonzero is None and result not in (None, 0):
                first_nonzero = call.get("x0")
        pair_report["firstNonZeroX0"] = first_nonzero
    report["profile"] = {
        "schemaVersion": profile["schemaVersion"],
        "name": profile.get("name"),
    }
    report["targets"] = resolved_targets
    return report


def diff_trace_summaries(left: dict[str, object],
                         right: dict[str, object]) -> dict[str, object]:
    """Compare trace summaries while preserving the complete inputs."""
    left_summary = left["summary"]
    right_summary = right["summary"]
    assert isinstance(left_summary, dict) and isinstance(right_summary, dict)

    def counts(key: str) -> tuple[dict[str, int], dict[str, int]]:
        a = left_summary.get(key, {})
        b = right_summary.get(key, {})
        assert isinstance(a, dict) and isinstance(b, dict)
        return ({str(k): int(v) for k, v in a.items()},
                {str(k): int(v) for k, v in b.items()})

    def delta(a: dict[str, int], b: dict[str, int]) -> dict[str, int]:
        return {
            key: b.get(key, 0) - a.get(key, 0)
            for key in sorted(set(a) | set(b))
            if b.get(key, 0) != a.get(key, 0)
        }

    left_processes, right_processes = counts("processCounts")
    left_targets, right_targets = counts("targetCounts")
    return {
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "left": left,
        "right": right,
        "delta": {
            "eventCount": (
                int(right_summary.get("eventCount", 0))
                - int(left_summary.get("eventCount", 0))),
            "threadCount": (
                int(right_summary.get("threadCount", 0))
                - int(left_summary.get("threadCount", 0))),
            "snapshotErrors": (
                int(right_summary.get("snapshotErrors", 0))
                - int(left_summary.get("snapshotErrors", 0))),
            "processCounts": delta(left_processes, right_processes),
            "targetCounts": delta(left_targets, right_targets),
        },
    }


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


def _cmd_version(client: SysAgentClient, args: argparse.Namespace) -> None:
    """Print this client's version and the version the console reports, both labelled.

    The client line comes first and needs no network; if the host cannot be reached the error
    goes to stderr and the exit status is 2, so this doubles as a reachability check.
    """
    print(f"client    {CLIENT_VERSION}")
    print(f"sys-agent {client.get_version()}  ({client.host}:{client.port})")


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
    _emit(client.system_query(args.query), args)


def _cmd_process_list(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.process_list(args.offset, args.count))


def _cmd_debug_read(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.debug_read(args.pid, args.address, args.size).hex().upper())


def _cmd_debug_wait_title(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.debug_wait_process(args.title_id, args.timeout_ms), args)


def _cmd_debug_wait_application(client: SysAgentClient,
                                args: argparse.Namespace) -> None:
    _emit(client.debug_wait_application(args.timeout_ms), args)


def _cmd_debug_wait_status(client: SysAgentClient,
                           args: argparse.Namespace) -> None:
    _emit(client.debug_wait_status(), args)


def _cmd_debug_start_process(client: SysAgentClient,
                             args: argparse.Namespace) -> None:
    _emit(client.debug_start_process(args.pid), args)


def _cmd_system_action(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.system_action(args.command, unlock=not args.no_unlock))


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
    if not args.diff:
        print(output)
        return
    report = image_diff(args.diff, output)
    report["baseline"] = args.diff
    report["current"] = output
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"{output} " + " ".join(f"{key}={value}" for key, value in report.items()))


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


def _cmd_memory_query(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.memory_query(args.address), args)


def _cmd_peek_verified(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.peek_verified(args.offset, args.size, args.attempts), args)


def _cmd_peek_absolute_verified(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.peek_absolute_verified(args.address, args.size, args.attempts), args)


def _cmd_peek_main_verified(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.peek_main_verified(args.offset, args.size, args.attempts), args)


def _cmd_memory_wait_value(client: SysAgentClient, args: argparse.Namespace) -> None:
    if client.wait_for_value(args.address, args.size, args.value,
                             args.wait_timeout, args.poll):
        print("matched")
    else:
        raise SysAgentProtocolError(
            f"address 0x{args.address:X} did not hold the value within "
            f"{args.wait_timeout:g}s")


def _cmd_memory_hash(client: SysAgentClient, args: argparse.Namespace) -> None:
    _emit(client.memory_hash(args.address, args.size), args)


def _cmd_system_log(client: SysAgentClient, args: argparse.Namespace) -> None:
    entries = client.operation_log(args.count)
    if getattr(args, "json", False):
        payload = json.dumps(entries, indent=2)
    else:
        payload = "\n".join(
            f"{entry['index']} tick={entry['tick']} cmd={entry['cmd']}"
            for entry in entries)
    out = getattr(args, "out", None)
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(payload + "\n")
        print(out)
    else:
        print(payload)


def _cmd_memory_dump(client: SysAgentClient, args: argparse.Namespace) -> None:
    written = client.dump(args.start, args.size, args.output, args.chunk)
    print(f"{args.output} ({written} bytes)")


def _cmd_game_wait(client: SysAgentClient, args: argparse.Namespace) -> None:
    if client.wait_for_game(args.wait_timeout, args.poll):
        print("running")
    else:
        raise SysAgentProtocolError(f"no game started within {args.wait_timeout:g}s")


def _cmd_poke(client: SysAgentClient, args: argparse.Namespace) -> None:
    if args.no_verify:
        client.poke(args.offset, args.data)
    else:
        print_fields(client.poke_verified(args.offset, args.data))


def _cmd_poke_absolute(client: SysAgentClient, args: argparse.Namespace) -> None:
    if args.no_verify:
        client.poke_absolute(args.address, args.data)
    else:
        print_fields(client.poke_absolute_verified(args.address, args.data))


def _cmd_poke_main(client: SysAgentClient, args: argparse.Namespace) -> None:
    if args.no_verify:
        client.poke_main(args.offset, args.data)
    else:
        print_fields(client.poke_main_verified(args.offset, args.data))


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
    result = client.click_seq(args.sequence, wait_done=not args.no_wait)
    if result.done:
        # Keep the word scripts already grep for, and add what the console
        # expanded: `tokens` shows a repeat took effect, `estMs` is its estimate.
        print(f"done tokens={result.tokens} estMs={result.estimated_ms}")


def _cmd_input_capabilities(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.sequence_capabilities())


def _cmd_touch(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.touch(pair_values(args.points))


def _cmd_controller_status(client: SysAgentClient, args: argparse.Namespace) -> None:
    print_fields(client.controller_status())


def _cmd_controller_dump(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.controller_dump())


def _cmd_controller_kick(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.controller_kick(args.npad))


def _cmd_controller_reconnect(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.controller_reconnect(args.timeout_ms, args.address))


def _cmd_controller_paired(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.controller_paired_devices())


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


def _cmd_text(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.type_text(" ".join(args.text))


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
    _emit(client.system_query("application"), args)


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
    if args.parameter not in CONFIG_PARAMETERS:
        raise ValueError(
            f"unsupported configure parameter: {args.parameter} "
            f"(known: {', '.join(sorted(CONFIG_PARAMETERS))})")
    client.configure(args.parameter, args.value)


def _cmd_config_get(client: SysAgentClient, args: argparse.Namespace) -> None:
    name = CONFIG_READ_ALIASES.get(args.parameter, args.parameter)
    if name not in CONFIG_PARAMETERS:
        raise ValueError(
            f"unsupported configure parameter: {args.parameter} "
            f"(known: {', '.join(sorted(CONFIG_PARAMETERS))})")
    print(f"{name}={client.config_get(name)}")


def _cmd_config_list(client: SysAgentClient, args: argparse.Namespace) -> None:
    for name, value in client.config_list().items():
        print(f"{name}={value}")


def _cmd_begin(client: SysAgentClient, args: argparse.Namespace) -> None:
    print(client.begin_unknown(args.type, args.region, args.offset, args.size,
                               alignment=args.alignment, pause=args.pause))


def _cmd_refine(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.refine(args.session, args.mode, args.value, pause=args.pause)


def _cmd_raw(client: SysAgentClient, args: argparse.Namespace) -> None:
    line = client.raw_command(" ".join(args.command))
    if line is not None:
        print(line)


def _cmd_debug_trace(client: SysAgentClient, args: argparse.Namespace) -> None:
    if not 1 <= len(args.addresses) <= 4:
        raise ValueError("trace needs one to four target addresses")
    if args.pid is not None and args.pid <= 0:
        raise ValueError("pid must be positive")
    if args.hits is not None and args.hits < 0:
        raise ValueError("hits must be non-negative")
    if args.hits == 0 and args.ring is None:
        raise ValueError("hits 0 (unlimited) requires --ring")
    if args.ring is not None and not 1 <= args.ring <= 64:
        raise ValueError("ring must be in 1..64")
    if args.duration is not None and args.duration < 0:
        raise ValueError("duration must be non-negative")
    if args.args is not None and not 0 <= args.args <= 8:
        raise ValueError("args must be in 0..8")
    # Arming hardware breakpoints on all four cores can take several seconds
    # when the target is busy. The normal command timeout is tuned for small
    # control replies, not for this attach-and-arm operation.
    client.timeout = max(client.timeout, 30.0)
    snapshots = args.snapshots or []
    if len(snapshots) > 2:
        raise ValueError("at most two --snapshot specifications are supported")
    parts = ["debug", "trace", *args.addresses]
    if args.pid is not None:
        parts.append(f"pid=0x{args.pid:X}")
    if args.hits is not None:
        parts.extend(("hits", str(args.hits)))
    if args.ring is not None:
        parts.extend(("ring", str(args.ring)))
    if args.duration is not None:
        parts.extend(("duration", str(args.duration)))
    if args.duration_from == "first-hit":
        parts.append("durationFrom=first-hit")
    if args.args is not None:
        parts.extend(("args", str(args.args)))
    for snapshot in snapshots:
        parts.append(f"snap={snapshot}")
    _emit(require_ok(client.command(" ".join(parts))), args)


def _cmd_debug_trace_status(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command("debug trace-status", retry=True)), args)


def _cmd_debug_trace_read(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command(f"debug trace-read {args.index}", retry=True)), args)


def _cmd_debug_trace_stop(client: SysAgentClient, args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command("debug trace-stop")), args)


def _trace_process_metadata(client: SysAgentClient,
                            pid_text: str) -> dict[str, str]:
    """Best-effort process identity for an exported trace session."""
    try:
        pid = int(pid_text, 16)
    except ValueError:
        return {"pid": pid_text}
    metadata = {"pid": pid_text, "titleId": "NA"}

    try:
        listing = client.process_list(0, 64)
        entries = listing.get("processes", "")
        for entry in entries.split(","):
            process_text, _, title_text = entry.partition(":")
            if process_text and int(process_text, 16) == pid:
                metadata["titleId"] = title_text
                break
    except (OSError, ValueError, SysAgentProtocolError):
        pass

    try:
        modules = client.command(f"debug modules 0x{pid:X} 2", retry=True)
        if modules.get("ok") != "OK":
            return metadata
        module_entries = {
            entry.split("=", 1)[0]: entry.split("=", 1)[1]
            for entry in modules.get("modules", "").split(",")
            if "=" in entry
        }
        entry = module_entries.get("1", module_entries.get("0", ""))
        if not entry:
            return metadata
        address_size, _, build_id = entry.partition(":build=")
        base_text, _, size_text = address_size.partition("+")
        metadata.update({
            "moduleBase": base_text,
            "moduleSize": size_text,
            "buildId": build_id,
        })
    except (OSError, ValueError, SysAgentProtocolError):
        pass
    try:
        maps, _ = _parse_executable_maps(require_ok(
            client.command(f"debug maps 0x{pid:X} 256", retry=True)))
        if maps:
            metadata["codeBase"] = f"0x{min(entry['base'] for entry in maps):X}"
    except (OSError, ValueError, SysAgentProtocolError):
        pass
    return metadata


def _trace_clock_metadata(client: SysAgentClient) -> dict[str, int]:
    """Best-effort identity for the current console boot timeline."""
    try:
        response = client.system_query("time")
        uptime_ms = parse_int(response.get("uptimeMs", "0"))
        unix_seconds = int(response.get("unix", "0"))
        if uptime_ms <= 0 or unix_seconds <= 0:
            return {}
        return {
            "uptimeMs": uptime_ms,
            "bootEpochUnixMs": unix_seconds * 1000 - uptime_ms,
        }
    except (OSError, ValueError, SysAgentProtocolError):
        return {}


def _write_versioned_trace(output: str, metadata: dict[str, object],
                           events: Sequence[dict[str, object]]) -> None:
    with open(output, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(metadata, sort_keys=True) + "\n")
        for event in events:
            record = {
                "record": "event",
                "schemaVersion": TRACE_SCHEMA_VERSION,
                **event,
            }
            handle.write(json.dumps(record, sort_keys=True) + "\n")


def _annotate_trace_with_plan(output: str, plan: dict[str, object]) -> None:
    document = load_trace_jsonl([output])[0]
    metadata = _profile_object(document.get("metadata"), "trace metadata")
    targets = metadata.get("targets")
    if not isinstance(targets, list):
        raise SysAgentProtocolError("trace metadata has no targets")
    plan_targets = {
        int(target["index"]): target
        for target in (
            _profile_object(value, "plan target")
            for value in _profile_list(plan["targets"], "plan targets")
        )
    }
    for raw_target in targets:
        if not isinstance(raw_target, dict):
            continue
        plan_target = plan_targets.get(int(raw_target.get("index", -1)))
        if plan_target is not None:
            raw_target.update({
                "id": plan_target["id"],
                "processId": plan_target["processId"],
                "moduleBase": plan_target["moduleBase"],
                "codeBase": plan_target["codeBase"],
                "base": plan_target["base"],
                "offset": plan_target["offset"],
                "mapping": plan_target["mapping"],
                "snapshots": plan_target.get("snapshots", []),
            })
    metadata["profile"] = {
        "schemaVersion": plan["schemaVersion"],
        "name": plan["profile"],
        "pairs": plan.get("pairs", []),
    }
    events = document.get("events")
    if not isinstance(events, list):
        raise SysAgentProtocolError("trace document has no events")
    _write_versioned_trace(output, metadata, events)


def _export_single_trace(client: SysAgentClient,
                         output: str) -> tuple[int, str]:
    client.timeout = max(client.timeout, 30.0)
    before = require_ok(client.command("debug trace-status", retry=True))
    ring_count = parse_int(before.get("ringCount", "0"))
    last_sequence = parse_int(before.get("lastSeq", "0"))
    if ring_count == 0 and last_sequence == 0:
        raise SysAgentProtocolError("the tracer has no recorded event to dump")
    event_count = ring_count if ring_count > 0 else 1

    events = []
    for index in range(event_count):
        response = require_ok(client.command(f"debug trace-read {index}", retry=True))
        events.append({key: value for key, value in response.items() if key != "ok"})

    after = require_ok(client.command("debug trace-status", retry=True))
    if (after.get("ringCount") != before.get("ringCount")
            or after.get("overwritten") != before.get("overwritten")):
        raise SysAgentProtocolError(
            "the trace ring changed during dump; stop the trace first and retry")

    session_id = (
        f"{int(time.time() * 1000):x}-"
        f"{before.get('pid', '0')}-"
        f"{before.get('address', '0')}")
    process = _trace_process_metadata(client, before.get("pid", "0"))
    target_count = parse_int(before.get("targetCount", "1"))
    target_addresses: dict[int, str] = {}
    for entry in before.get("targets", "").split(","):
        if not entry or ":" not in entry:
            continue
        index_text, address_text = entry.split(":", 1)
        try:
            target_addresses[int(index_text)] = address_text
        except ValueError:
            continue
    metadata = {
        "record": "meta",
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "sessionId": session_id,
        "kind": "single",
        "exportedAt": datetime.now(timezone.utc).isoformat(),
        "host": client.host,
        "clock": _trace_clock_metadata(client),
        "status": {key: value for key, value in before.items() if key != "ok"},
        "process": process,
        "targets": [
            {
                "index": index,
                "address": target_addresses.get(
                    index, before.get("address", "0") if index == 0 else "0"),
                "process": process,
            }
            for index in range(max(1, target_count))
        ],
    }
    _write_versioned_trace(output, metadata, events)
    return len(events), session_id


def _cmd_debug_trace_dump(client: SysAgentClient, args: argparse.Namespace) -> None:
    output = args.output or f"trace-{int(time.time())}.jsonl"
    event_count, session_id = _export_single_trace(client, output)
    print(f"{output} ({event_count} events, session={session_id})")


def _cmd_debug_multitrace(client: SysAgentClient, args: argparse.Namespace) -> None:
    if args.pid1 == args.pid2:
        raise ValueError("multitrace process IDs must differ")
    if ((args.extra_target1 is None) != (args.extra_target2 is None)):
        raise ValueError("--extra-target1 and --extra-target2 must be used together")
    if args.hits is not None and args.hits < 0:
        raise ValueError("hits must be non-negative")
    if args.hits == 0 and args.ring is None:
        raise ValueError("hits 0 (unlimited) requires --ring")
    if args.ring is not None and not 1 <= args.ring <= 64:
        raise ValueError("ring must be in 1..64")
    if args.duration is not None and args.duration < 0:
        raise ValueError("duration must be non-negative")
    if args.args is not None and not 0 <= args.args <= 8:
        raise ValueError("args must be in 0..8")
    snapshots = args.snapshots or []
    if len(snapshots) > 2:
        raise ValueError("at most two --snapshot specifications are supported")
    client.timeout = max(client.timeout, 30.0)
    parts = [
        "debug", "multitrace",
        f"0x{args.pid1:X}", args.target1,
        f"0x{args.pid2:X}", args.target2,
    ]
    if args.extra_target1 is not None:
        parts.extend(("also", args.extra_target1, args.extra_target2))
    if args.hits is not None:
        parts.extend(("hits", str(args.hits)))
    if args.ring is not None:
        parts.extend(("ring", str(args.ring)))
    if args.duration is not None:
        parts.extend(("duration", str(args.duration)))
    if args.duration_from == "first-hit":
        parts.append("durationFrom=first-hit")
    if args.args is not None:
        parts.extend(("args", str(args.args)))
    for snapshot in snapshots:
        parts.append(f"snap={snapshot}")
    _emit(require_ok(client.command(" ".join(parts))), args)


def _cmd_debug_multitrace_status(client: SysAgentClient,
                                 args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command("debug multitrace-status", retry=True)), args)


def _cmd_debug_multitrace_read(client: SysAgentClient,
                               args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command(
        f"debug multitrace-read {args.index}", retry=True)), args)


def _cmd_debug_multitrace_stop(client: SysAgentClient,
                               args: argparse.Namespace) -> None:
    client.timeout = max(client.timeout, 30.0)
    _emit(require_ok(client.command("debug multitrace-stop")), args)


def _export_multitrace(client: SysAgentClient,
                       output: str) -> tuple[int, str]:
    client.timeout = max(client.timeout, 30.0)
    before = require_ok(client.command("debug multitrace-status", retry=True))
    ring_count = parse_int(before.get("ringCount", "0"))
    last_sequence = parse_int(before.get("lastSeq", "0"))
    if ring_count == 0 and last_sequence == 0:
        raise SysAgentProtocolError("the multitrace has no recorded event to dump")
    event_count = ring_count if ring_count > 0 else 1
    events = []
    for index in range(event_count):
        response = require_ok(client.command(
            f"debug multitrace-read {index}", retry=True))
        events.append({key: value for key, value in response.items() if key != "ok"})

    after = require_ok(client.command("debug multitrace-status", retry=True))
    if (after.get("ringCount") != before.get("ringCount")
            or after.get("hitCount") != before.get("hitCount")):
        raise SysAgentProtocolError(
            "the multitrace ring changed during dump; stop the trace first and retry")

    process0 = _trace_process_metadata(client, before.get("pid0", "0"))
    process1 = _trace_process_metadata(client, before.get("pid1", "0"))
    processes = (process0, process1)
    target_count = parse_int(before.get("targetCount", "2"))
    if not 2 <= target_count <= 4:
        raise SysAgentProtocolError(
            f"unsupported multitrace target count: {target_count}")
    process_text = before.get("targetProcesses", "0,1,0,1")
    try:
        target_processes = [
            int(value) for value in process_text.split(",")
        ]
    except ValueError as exc:
        raise SysAgentProtocolError(
            f"invalid multitrace target process map: {process_text!r}") from exc
    if len(target_processes) < target_count:
        raise SysAgentProtocolError(
            "multitrace target process map is shorter than the target count")
    if any(value not in (0, 1) for value in target_processes[:target_count]):
        raise SysAgentProtocolError(
            "multitrace target process map contains an unknown process")
    session_id = (
        f"{int(time.time() * 1000):x}-"
        f"{before.get('pid0', '0')}-{before.get('pid1', '0')}")
    metadata = {
        "record": "meta",
        "schemaVersion": TRACE_SCHEMA_VERSION,
        "sessionId": session_id,
        "kind": "multitrace",
        "exportedAt": datetime.now(timezone.utc).isoformat(),
        "host": client.host,
        "clock": _trace_clock_metadata(client),
        "status": {key: value for key, value in before.items() if key != "ok"},
        "targets": [
            {
                "index": index,
                "address": before.get(f"target{index}", "0"),
                "processIndex": target_processes[index],
                "process": processes[target_processes[index]],
            }
            for index in range(target_count)
        ],
    }
    _write_versioned_trace(output, metadata, events)
    return len(events), session_id


def _cmd_debug_multitrace_dump(client: SysAgentClient,
                               args: argparse.Namespace) -> None:
    output = args.output or f"multitrace-{int(time.time())}.jsonl"
    event_count, session_id = _export_multitrace(client, output)
    print(f"{output} ({event_count} events, session={session_id})")


def _trace_profile_options(profile: dict[str, object],
                           args: argparse.Namespace) -> dict[str, object]:
    configured = profile.get("trace", {})
    configured = configured if isinstance(configured, dict) else {}
    options = {
        name: configured.get(name)
        for name in ("hits", "ring", "duration", "args")
    }
    for name in options:
        value = getattr(args, name, None)
        if value is not None:
            options[name] = value
    if options["hits"] is not None and int(options["hits"]) < 0:
        raise ValueError("hits must be non-negative")
    if options["hits"] == 0 and options["ring"] is None:
        raise ValueError("hits 0 (unlimited) requires a ring")
    if (options["ring"] is not None
            and not 1 <= int(options["ring"]) <= 64):
        raise ValueError("ring must be in 1..64")
    if options["duration"] is not None and int(options["duration"]) < 0:
        raise ValueError("duration must be non-negative")
    if options["args"] is not None and not 0 <= int(options["args"]) <= 8:
        raise ValueError("args must be in 0..8")
    parsed_options: dict[str, object] = {
        name: (int(value) if value is not None else None)
        for name, value in options.items()
    }
    configured_duration_from = configured.get("durationFrom", "arm")
    duration_from = (args.duration_from
                     if args.duration_from is not None
                     else configured_duration_from)
    parsed_options["durationFromFirstHit"] = duration_from == "first-hit"
    cli_snapshots = getattr(args, "snapshots", None) or []
    if len(cli_snapshots) > 2:
        raise ValueError("at most two --snapshot specifications are supported")
    for index, snapshot in enumerate(cli_snapshots):
        _validate_snapshot_spec(str(snapshot), f"snapshots[{index}]")
    parsed_options["globalSnapshots"] = cli_snapshots
    return parsed_options


def _start_plan_trace(client: SysAgentClient, plan: dict[str, object],
                      options: dict[str, object]) -> dict[str, str]:
    processes = _profile_list(plan["processes"], "plan processes")
    targets = _profile_list(plan["targets"], "plan targets")
    parts: list[str]
    if plan["mode"] == "single":
        address_args = [
            str(_profile_object(target, "plan target")["address"])
            for target in targets
        ]
        parts = ["debug", "trace", *address_args]
        process = _profile_object(processes[0], "plan process")
        parts.append(f"pid=0x{int(process['pid']):X}")
    else:
        if len(processes) != 2 or len(targets) != 4:
            raise ValueError("multitrace plans need two processes and four targets")
        primary0 = _profile_object(targets[0], "plan target")
        primary1 = _profile_object(targets[1], "plan target")
        secondary0 = _profile_object(targets[2], "plan target")
        secondary1 = _profile_object(targets[3], "plan target")
        process0 = _profile_object(processes[0], "plan process")
        process1 = _profile_object(processes[1], "plan process")
        parts = [
            "debug", "multitrace",
            f"0x{int(process0['pid']):X}", str(primary0["address"]),
            f"0x{int(process1['pid']):X}", str(primary1["address"]),
            "also", str(secondary0["address"]), str(secondary1["address"]),
        ]
    for name in ("hits", "ring", "duration", "args"):
        value = options.get(name)
        if value is not None:
            parts.extend((name, str(value)))
        if name == "duration" and options.get("durationFromFirstHit"):
            parts.append("durationFrom=first-hit")
    for snapshot in options.get("globalSnapshots", []):
        parts.append(f"snap={snapshot}")
    for raw_target in targets:
        target = _profile_object(raw_target, "plan target")
        target_snapshots = target.get("snapshots", [])
        if not isinstance(target_snapshots, list):
            continue
        for snapshot in target_snapshots:
            if isinstance(snapshot, str):
                spec = snapshot
            elif isinstance(snapshot, dict):
                spec = snapshot.get("spec")
            else:
                continue
            if isinstance(spec, str):
                parts.append(f"tsnap={target['index']}:{spec}")
    client.timeout = max(client.timeout, 30.0)
    return require_ok(client.command(" ".join(parts)))


def _start_waited_processes(client: SysAgentClient,
                            waited: dict[str, int]) -> list[dict[str, str]]:
    started: list[dict[str, str]] = []
    errors: list[str] = []
    for process_id, pid in waited.items():
        try:
            started.append(client.debug_start_process(pid))
        except (OSError, SysAgentProtocolError) as error:
            errors.append(f"{process_id} pid=0x{pid:X}: {error}")
    if errors:
        raise SysAgentProtocolError(
            "could not release PM-suspended process(es): "
            + "; ".join(errors))
    return started


def _release_waited_processes_best_effort(client: SysAgentClient,
                                          waited: dict[str, int]) -> None:
    for pid in waited.values():
        try:
            client.debug_start_process(pid)
        except (OSError, SysAgentProtocolError):
            pass


def _wait_for_profile_processes(client: SysAgentClient,
                                profile: dict[str, object],
                                timeout_ms: int) -> dict[str, int]:
    waited: dict[str, int] = {}
    try:
        for raw_process in _profile_list(profile["processes"], "processes"):
            process = _profile_object(raw_process, "process")
            process_id = str(process["id"])
            response: dict[str, str] | None = None
            if "foreground" in process:
                if not client.game_status().running:
                    try:
                        response = client.debug_wait_application(timeout_ms)
                    except SysAgentProtocolError as error:
                        expected = process.get("expect")
                        expected = (
                            expected if isinstance(expected, dict) else {})
                        fallback_title = expected.get("titleId")
                        if ("WAIT_HOOK_FAILED" not in str(error)
                                or fallback_title is None):
                            raise
                        response = client.debug_wait_process(
                            _profile_hex_id(
                                fallback_title,
                                f"process {process_id} expect.titleId"),
                            timeout_ms)
            elif "titleId" in process:
                title_id = _profile_hex_id(
                    process["titleId"], "process titleId")
                try:
                    _find_profile_pid(client, title_id)
                except SysAgentProtocolError:
                    response = client.debug_wait_process(title_id, timeout_ms)
            if response is not None and response.get("waited", "1") == "1":
                try:
                    pid = int(response["pid"], 16)
                except (KeyError, ValueError) as exc:
                    raise SysAgentProtocolError(
                        f"process {process_id} wait response has no valid PID") from exc
                waited[process_id] = pid
    except Exception:
        _release_waited_processes_best_effort(client, waited)
        raise
    return waited


def _wait_for_plan_trace(client: SysAgentClient, mode: str,
                         timeout_seconds: float,
                         start_timeout_at_first_hit: bool = False
                         ) -> dict[str, str]:
    if timeout_seconds <= 0:
        raise ValueError("wait timeout must be positive")
    command = (
        "debug trace-status" if mode == "single"
        else "debug multitrace-status")
    deadline = (
        None if start_timeout_at_first_hit
        else time.monotonic() + timeout_seconds)
    client.timeout = max(client.timeout, 30.0)
    while True:
        status = require_ok(client.command(command, retry=True))
        last_error = parse_int(status.get("lastError", "0"))
        if last_error:
            raise SysAgentProtocolError(
                f"trace failed in stage {status.get('stage', '')}: "
                f"result=0x{last_error:X}")
        if status.get("active") != "1":
            return status
        if (start_timeout_at_first_hit and deadline is None
                and parse_int(status.get("hitCount", "0")) > 0):
            deadline = time.monotonic() + timeout_seconds
        if deadline is not None and time.monotonic() >= deadline:
            break
        time.sleep(0.25)
    stop_command = (
        "debug trace-stop" if mode == "single"
        else "debug multitrace-stop")
    try:
        client.command(stop_command)
    except (OSError, SysAgentProtocolError):
        pass
    raise SysAgentProtocolError(
        f"trace did not finish within {timeout_seconds:g} seconds")


def _wait_for_plan_trace_armed(client: SysAgentClient, mode: str,
                               timeout_seconds: float = 30.0
                               ) -> dict[str, str]:
    command = (
        "debug trace-status" if mode == "single"
        else "debug multitrace-status")
    deadline = time.monotonic() + timeout_seconds
    client.timeout = max(client.timeout, 30.0)
    while time.monotonic() < deadline:
        status = require_ok(client.command(command, retry=True))
        last_error = parse_int(status.get("lastError", "0"))
        if last_error:
            raise SysAgentProtocolError(
                f"trace failed in stage {status.get('stage', '')}: "
                f"result=0x{last_error:X}")
        if status.get("armed") == "1":
            return status
        if status.get("active") != "1":
            raise SysAgentProtocolError(
                f"trace ended before arming in stage {status.get('stage', '')}")
        time.sleep(0.1)
    raise SysAgentProtocolError(
        f"trace was not armed within {timeout_seconds:g} seconds")


def _stop_plan_trace(client: SysAgentClient, mode: str) -> None:
    command = (
        "debug trace-stop" if mode == "single"
        else "debug multitrace-stop")
    try:
        client.command(command)
    except (OSError, SysAgentProtocolError):
        pass


def _cmd_debug_trace_plan(client: SysAgentClient,
                          args: argparse.Namespace) -> None:
    profile = load_trace_profile(args.profile)
    plan = resolve_trace_profile(client, profile)
    payload = json.dumps(plan, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(payload + "\n")
        print(args.out)
    else:
        print(payload)


def _cmd_debug_trace_run(client: SysAgentClient,
                         args: argparse.Namespace) -> None:
    profile = load_trace_profile(args.profile)
    waited: dict[str, int] = {}
    trace_started = False
    plan: dict[str, object] | None = None
    process_start_results: list[dict[str, str]] = []
    try:
        if args.wait_process:
            waited = _wait_for_profile_processes(
                client, profile, args.wait_process_timeout)
        plan = resolve_trace_profile(client, profile)
        options = _trace_profile_options(profile, args)
        started = _start_plan_trace(client, plan, options)
        trace_started = True
        if waited:
            _wait_for_plan_trace_armed(client, str(plan["mode"]))
            process_start_results = _start_waited_processes(client, waited)
            waited.clear()
        started["processesStarted"] = str(len(process_start_results))
        if not args.wait and not args.output:
            _emit(started, args)
            return

        duration = options.get("duration")
        default_wait = (
            90.0 if duration is None else max(30.0, float(duration) + 15.0))
        wait_timeout = args.wait_timeout if args.wait_timeout is not None else default_wait
        wait_from_first_hit = (
            bool(options.get("durationFromFirstHit"))
            and args.wait_timeout is None)
        status = _wait_for_plan_trace(
            client, str(plan["mode"]), wait_timeout,
            start_timeout_at_first_hit=wait_from_first_hit)
        output_path = args.output
        if output_path:
            if plan["mode"] == "single":
                event_count, session_id = _export_single_trace(client, output_path)
            else:
                event_count, session_id = _export_multitrace(client, output_path)
            _annotate_trace_with_plan(output_path, plan)
        else:
            event_count = parse_int(status.get("hitCount", "0"))
            session_id = ""
        result = {
            "ok": "OK",
            "mode": str(plan["mode"]),
            "started": started.get("state", started.get("active", "started")),
            "processesStarted": str(len(process_start_results)),
            "events": str(event_count),
            "session": session_id,
            "output": output_path or "",
            "plan": plan,
            "status": status,
        }
        if args.json or args.out:
            _emit(result, args)
        else:
            print(
                f"ok=OK mode={result['mode']} started={result['started']} "
                f"processesStarted={result['processesStarted']} "
                f"events={result['events']} output={result['output']} "
                f"stage={status.get('stage', '')} "
                f"lastError={status.get('lastError', '0x0')}")
    except Exception:
        if trace_started and plan is not None:
            _stop_plan_trace(client, str(plan["mode"]))
        raise
    finally:
        if waited:
            _release_waited_processes_best_effort(client, waited)


def _cmd_debug_trace_report(client: SysAgentClient,
                            args: argparse.Namespace) -> None:
    del client
    profile = load_trace_profile(args.profile)
    documents = load_trace_jsonl([args.trace])
    if len(documents) != 1:
        raise SysAgentProtocolError("trace-report needs exactly one trace file")
    report = report_trace_with_profile(documents[0], profile)
    if args.json or args.out:
        _emit(report, args)
        return
    summary = report["summary"]
    assert isinstance(summary, dict)
    print(
        f"profile={report['profile']['name']} "
        f"events={summary['eventCount']} threads={summary['threadCount']}")
    for pair in report["pairs"]:
        assert isinstance(pair, dict)
        print(
            f"pair {pair['entryId']}->{pair['returnId']} "
            f"matched={pair['matched']} unmatchedEntries={pair['unmatchedEntries']} "
            f"firstNonZeroX0={pair['firstNonZeroX0']}")
        calls = pair["calls"]
        assert isinstance(calls, list)
        for call in calls:
            assert isinstance(call, dict)
            print(
                f"  thread={call['thread']} entrySeq={call['entrySeq']} "
                f"returnSeq={call['returnSeq']} x0={call['returnX0']}")


def _cmd_debug_ipc_decode(client: SysAgentClient,
                          args: argparse.Namespace) -> None:
    del client
    decoded = decode_hipc_cmif(args.data, args.mode)
    cmif = decoded["cmif"]
    assert isinstance(cmif, dict)
    if args.json or args.out:
        _emit(decoded, args)
        return
    header = cmif.get("header")
    header = header if isinstance(header, dict) else {}
    header_detail = (
        f"magic={header['magic']} token={header['token']}"
        if "token" in header else
        f"magic={header['magic']} interfaceId={header['interfaceId']}"
        if "interfaceId" in header else "header=none")
    print(
        f"mode={cmif['mode']} domain={int(bool(cmif['domain']))} "
        f"{header_detail} operation={cmif.get('operation')}")
    if "commandId" in header:
        print(f"command={header['commandId']}")
    elif "result" in header:
        print(f"result={header['result']}")
    else:
        domain_header = cmif.get("domainHeader")
        object_id = (
            domain_header.get("objectId")
            if isinstance(domain_header, dict) else None)
        print(f"object={object_id}")
    print(f"payload={cmif['payloadHex']}")


def _cmd_debug_trace_ipc(client: SysAgentClient,
                         args: argparse.Namespace) -> None:
    del client
    if args.snapshot not in (None, 0, 1):
        raise ValueError("snapshot index must be 0 or 1")
    documents = load_trace_jsonl(args.traces)
    profile = load_trace_profile(args.profile) if args.profile else None
    report = decode_trace_ipc(documents, args.snapshot, profile)
    if args.json or args.out:
        _emit(report, args)
        return
    summary = report["summary"]
    assert isinstance(summary, dict)
    print(
        f"ipc={summary['ipcCount']} transactions={summary['transactionCount']} "
        f"ambiguous={summary['ambiguousTransactions']} "
        f"domainEvents={summary['domainEventCount']} "
        f"domainObjects={summary['domainObjectCount']} "
        f"skippedSnapshots={summary['skippedSnapshots']}")
    for record in report["records"]:
        assert isinstance(record, dict)
        cmif = record["cmif"]
        assert isinstance(cmif, dict)
        header = cmif.get("header")
        header = header if isinstance(header, dict) else {}
        if "commandId" in header:
            detail = f"command={header['commandId']}"
        elif "result" in header:
            detail = f"result={header['result']}"
        else:
            detail = f"operation={cmif.get('operation')}"
        header_detail = (
            f"token={header['token']}"
            if "token" in header else
            f"interfaceId={header['interfaceId']}"
            if "interfaceId" in header else "header=none")
        print(
            f"  seq={record['seq']} target={record['target']} "
            f"targetId={record['targetId']} "
            f"thread={record['thread']} snapshot={record['snapshot']} "
            f"mode={cmif['mode']} domain={int(bool(cmif['domain']))} "
            f"{header_detail} operation={cmif.get('operation')} {detail}")
    for transaction in report["transactions"]:
        assert isinstance(transaction, dict)
        print(
            f"  transaction seq={transaction['requestSeq']}->{transaction['returnSeq']} "
            f"thread={transaction['requestThread']}->"
            f"{transaction['responseThread']} "
            f"target={transaction['entryTarget']}->{transaction['returnTarget']} "
            f"service={transaction['service']} "
            f"interface={transaction['interfaceHash']} "
            f"interfaceMatch={transaction['interfaceMatchesProfile']} "
            f"command={transaction['commandId']} "
            f"name={transaction['commandName']} result={transaction['result']} "
            f"correlation={transaction['correlation']}")
    for domain_object in report["domainObjects"]:
        assert isinstance(domain_object, dict)
        print(
            f"  object id={domain_object['objectId']} "
            f"creates={domain_object['createCount']} "
            f"closes={domain_object['closeCount']} "
            f"requests={domain_object['requestCount']} "
            f"responses={domain_object['responseCount']} "
            f"commands={domain_object['commands']} "
            f"results={domain_object['results']} "
            f"ticks={domain_object['firstTick']}..{domain_object['lastTick']}")


def _cmd_debug_trace_timeline(client: SysAgentClient,
                              args: argparse.Namespace) -> None:
    del client
    profile = load_trace_profile(args.profile) if args.profile else None
    report = build_trace_timeline(load_trace_jsonl(args.traces), profile)
    if args.json or args.out:
        _emit(report, args)
        return
    summary = report["summary"]
    assert isinstance(summary, dict)
    print(
        f"sessions={summary['sessionCount']} events={summary['eventCount']} "
        f"ipc={summary['ipcCount']} transactions={summary['transactionCount']} "
        f"ambiguous={summary['ambiguousTransactions']} "
        f"domainEvents={summary['domainEventCount']} "
        f"bootEpochUnixMs={report['bootEpochUnixMs']}")
    for record in report["timeline"]:
        assert isinstance(record, dict)
        if record["type"] == "transaction":
            print(
                f"  tick={record['tick']} transaction={record['requestSeq']}->"
                f"{record['returnSeq']} thread={record['requestThread']}->"
                f"{record['responseThread']} "
                f"service={record.get('service')} command={record.get('commandId')} "
                f"name={record.get('commandName')} result={record.get('result')} "
                f"interfaceMatch={record.get('interfaceMatchesProfile')} "
                f"correlation={record.get('correlation')}")
        elif record["type"] in {"create-domain-object", "close-domain-object"}:
            print(
                f"  tick={record['tick']} {record['type']} "
                f"object={record['objectId']} thread={record.get('thread')} "
                f"target={record.get('targetId')}")
        else:
            ipc = record.get("ipc")
            ipc_text = ""
            if isinstance(ipc, list) and ipc:
                parts = []
                for message in ipc:
                    if not isinstance(message, dict):
                        continue
                    details = (
                        f"{message.get('mode')}:{message.get('commandId')}"
                        if message.get("commandId") is not None
                        else f"{message.get('mode')}:{message.get('result')}")
                    parts.append(details)
                ipc_text = " ipc=" + ",".join(parts)
            print(
                f"  tick={record['tick']} event={record['seq']} "
                f"pid={record['pid']} target={record.get('targetId')} "
                f"thread={record['thread']} pc={record['pc']}{ipc_text}")


def _cmd_debug_trace_analyze(client: SysAgentClient, args: argparse.Namespace) -> None:
    del client
    pairs = [parse_trace_pair_spec(value) for value in (args.pairs or [])]
    documents = load_trace_jsonl(args.traces)
    report = analyze_trace_documents(documents, pairs)
    if args.json or args.out:
        _emit(report, args)
        return
    summary = report["summary"]
    assert isinstance(summary, dict)
    print(
        f"sessions={summary['sessionCount']} events={summary['eventCount']} "
        f"threads={summary['threadCount']} snapshotErrors={summary['snapshotErrors']} "
        f"tickFirst={summary['tickFirst']} tickLast={summary['tickLast']}")
    for pair in report["pairs"]:
        assert isinstance(pair, dict)
        print(
            f"pair entry={pair['entry']} return={pair['return']} "
            f"matched={pair['matched']} unmatchedEntries={pair['unmatchedEntries']}")
        calls = pair["calls"]
        assert isinstance(calls, list)
        for call in calls:
            assert isinstance(call, dict)
            print(
                f"  thread={call['thread']} entrySeq={call['entrySeq']} "
                f"returnSeq={call['returnSeq']} x0={call['x0']}")


def _cmd_debug_trace_diff(client: SysAgentClient, args: argparse.Namespace) -> None:
    del client
    left = analyze_trace_documents(load_trace_jsonl([args.left]))
    right = analyze_trace_documents(load_trace_jsonl([args.right]))
    report = diff_trace_summaries(left, right)
    if args.json or args.out:
        _emit(report, args)
        return
    delta = report["delta"]
    assert isinstance(delta, dict)
    print(
        f"events={delta['eventCount']} threads={delta['threadCount']} "
        f"snapshotErrors={delta['snapshotErrors']} "
        f"processCounts={delta['processCounts']} "
        f"targetCounts={delta['targetCounts']}")


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


# ---- input button table -----------------------------------------------------

# Button names accepted by the sysmodule's parseStringToButton(). The table is
# repeated here so that the client help stands on its own offline; keep it in
# sync with src/sys-agent/sys-agent/source/util.c when buttons change.
BUTTON_GROUPS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("face buttons", ("A", "B", "X", "Y")),
    ("shoulders and triggers", ("L", "R", "ZL", "ZR")),
    ("stick clicks", ("LSTICK", "RSTICK")),
    ("D-pad", ("DUP", "DDOWN", "DLEFT", "DRIGHT")),
    ("system", ("PLUS", "MINUS", "HOME", "CAPTURE")),
    ("miscellaneous", ("PALMA", "UNUSED")),
)

BUTTON_ALIASES: tuple[tuple[str, str], ...] = (
    ("DU", "DUP"),
    ("DD", "DDOWN"),
    ("DL", "DLEFT"),
    ("DR", "DRIGHT"),
)


def _button_help() -> str:
    """Render the accepted button names as a multi-line help block."""
    lines = ["BUTTON is one of the following case-sensitive names:"]
    lines.extend(f"  {label}: {', '.join(names)}" for label, names in BUTTON_GROUPS)
    aliases = ", ".join(f"{alias}={name}" for alias, name in BUTTON_ALIASES)
    lines.append(f"  D-pad aliases: {aliases}")
    lines.append("An unknown name sends no button at all (never a default key).")
    return "\n".join(lines)


BUTTON_ARG = Arg("button", "button name; the accepted names are listed below",
                 metavar="BUTTON")

# Writes verify by default (write, read back, compare on the server). --no-verify
# falls back to the legacy blind poke, which is only needed for a target that
# rewrites its own memory fast enough to fail the read-back.
NO_VERIFY_ARG = Arg("no_verify", "skip the server-side read-back check",
                    action="store_true", default=False, flags=("--no-verify",))

# Verified reads retry until two consecutive reads agree (default 3 attempts).
ATTEMPTS_ARG = Arg("attempts", "verified-read attempts (default 3, max 16)",
                   type=parse_int, default=3, flags=("--attempts",))

# Structured output for result commands.
JSON_ARG = Arg("json", "print the response as JSON", action="store_true",
               default=False, flags=("--json",))
OUT_ARG = Arg("out", "write the response to this file instead of stdout",
              flags=("--out",), default=None)
DIFF_ARG = Arg("diff", "compare the capture against this baseline image",
               flags=("--diff",), default=None)

# Wait primitives poll; these bound the wait and its interval.
WAIT_TIMEOUT_ARG = Arg("wait_timeout", "seconds to wait (default 60)",
                       type=float, default=60.0, flags=("--wait-timeout",))
POLL_ARG = Arg("poll", "poll interval in seconds (default 0.5)",
               type=float, default=0.5, flags=("--poll",))

BUTTON_HELP = _button_help()

CLICK_SEQ_HELP = "\n".join((
    "Sequence tokens (comma separated, no spaces):",
    "  BUTTON      click the button",
    "  +BUTTON     press and hold",
    "  -BUTTON     release",
    "  W<milliseconds>   wait, e.g. W1000",
    "  %LX,LY      move the left stick, e.g. %5000,1500",
    "  &RX,RY      move the right stick, e.g. &0,0",
    "",
    "Repeat tokens (needs a sysmodule advertising repeat=paren-star, see",
    "`input capabilities`):",
    "  ITEM*N      run ITEM N times, e.g. DDOWN*3",
    "  (ITEMS)*N   run the group N times, e.g. (DDOWN,W100)*10",
    "  N is 1..1000, groups nest up to 4 deep, at most 1024 tokens overall; a",
    "  wait inside a group repeats with it. A sequence the console cannot expand",
    "  is refused with ERR code=BAD_SEQUENCE before any input is sent.",
    "",
    BUTTON_HELP,
))

TEXT_HELP = "\n".join((
    "Types a string by sending HID keyboard reports; the character-to-keycode",
    "mapping is the unshifted/shifted US layout, which is what the console's",
    "software keyboard turns those reports into.",
    "",
    "  input text 'https://example.com/download/%e5%8f%8c.nsz'",
    "",
    "Only printable ASCII is typable this way: a character this layout cannot",
    "produce (non-ASCII, a newline) is refused before any key is sent -- there is",
    "no HID report for the console's own input method.",
    "",
    "The command waits for the console to finish each batch of characters before",
    "sending the next one, sized from the console's own keySleepTime + pollRate.",
    "The console's keySleepTime is temporarily raised to at least 50 ms while typing",
    "(game keyboards drop keys at the 25 ms default) and restored afterwards.",
))

CONTROLLER_STATUS_HELP = "\n".join((
    "One line of virtual-controller state:",
    "  initialised/handle/session    current device (0 = none attached)",
    "  attached                      HOS still has that device attached?",
    "  deviceType / interface        what the pad presents itself as",
    "  idleRelease / takeover        the two behaviour settings",
    "  slot                          row held in the assignment table",
    "  assignmentRc/lastStateError   last dump / state write result",
    "  npad0                         hidsys owner of player 1 (free = none)",
    "",
    "Read-only: initialised=0 is reported instead of creating a device,",
    "so the command is safe to run at any time.",
    "attached=0 with a non-zero lastStateError means the console is not",
    "routing the virtual pad; see docs/hdls-virtual-controller-notes.md.",
))

CONTROLLER_DUMP_HELP = "\n".join((
    "Multi-line diagnostic dump:",
    "  npadAssignment  HDLS handle -> npad slot table (internal rows hidden)",
    "  hdlsStates      every controller hid:dbg knows about",
    "  hidsysNpads     layout signature, pad count, per-slot owners",
    "                  (npad 0-7, Other, Handheld) with interface + number",
))

SYSTEM_ACTION_HELP = "\n".join((
    "Actions:",
    "  reboot                  normal Atmosphere/Horizon reboot",
    "  reboot-emummc           one-shot reboot to the Hekate entry with id=Atm-Emu",
    "  shutdown                orderly shutdown",
    "  sleep                   experimental system-sleep SVC",
    "  terminate-application   terminate the foreground application",
    "",
    "Boot unlock:",
    "  reboot and reboot-emummc ask the sysmodule to arm the boot unlock before",
    "  rebooting, so the console comes back past Horizon's lock screen instead of",
    "  timing out into sleep (which cuts every sys-agent session off). The request",
    "  is one-shot: the boot that runs it consumes it.",
    "  --no-unlock skips that arming for this reboot; it means nothing for the",
    "  other actions, and the client rejects it there.",
    "  See `bootUnlock` in commands.md and docs/lock-screen-notes.md.",
))

SYSTEM_QUERY_HELP = "\n".join((
    "Queries:",
    "  info             firmware, model, region, language, nickname, serial",
    "  time             Unix time, timezone and uptime (uptimeMs = the boot clock)",
    "  power            battery, charger, voltage, health, temperature",
    "  storage          SD mount state and total/free/used bytes",
    "  network          link state, signal strength, IPv4 configuration",
    "  network-profile  current profile, SSID, Wi-Fi passphrase, IPv4/DNS, MTU",
    "  account          last-opened account UID and nickname",
    "  application      running application identity, version, memory bases, name",
    "",
    "--json prints the same fields as a JSON object; --out writes them to a file.",
))

CONFIG_SET_HELP = "\n".join((
    "Settings (`config get <name>` reads one back, `config list` prints them all):",
    "  mainLoopSleepTime        main-loop poll timeout in ms (default 50)",
    "  buttonClickSleepTime     how long `input click` holds a button (50 ms)",
    "  keySleepTime             gap between keyboard keys (default 25 ms)",
    "  fingerDiameter           touch radius in pixels (default 50)",
    "  pollRate                 touch/autopilot poll rate in ms (default 17)",
    "  freezeRate               freeze write interval in ms (default 3)",
    "  echoCommands             1 prints every received command (default 0)",
    "  printDebugResultCodes    1 prints diagnostic Result codes (default 0)",
    "  controllerType            virtual pad device type (default 3 = full key)",
    "  controllerIdleRelease     seconds before the virtual pad gives player 1 back",
    "                            (0 = never; ms twin: controllerIdleReleaseMs)",
    "  controllerIdleReleaseMs   the same delay in ms (default 1000)",
    "  controllerTakeover        0 = leave a real controller alone, 1 = take over",
    "                            when HOS refuses our state write (default 1)",
))

# ---- command registry -------------------------------------------------------


COMMANDS: tuple[Command | CommandGroup, ...] = (
    Command("version", "Show the client version and the version the host reports",
            _cmd_version),

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
                (Arg("query", "query group to read; see the table below",
                     choices=("info", "time", "power", "storage", "network",
                              "network-profile", "account", "application"),
                     metavar="QUERY"),
                 JSON_ARG, OUT_ARG),
                SYSTEM_QUERY_HELP),
        Command("log", "Show recent commands the console received (oldest first)",
                _cmd_system_log,
                (Arg("count", "entries to show (1..32; default all)",
                     type=parse_int, default=32, flags=("--count",)),
                 JSON_ARG, OUT_ARG)),
        Command("process-list", "List running processes (PID:TitleID)", _cmd_process_list,
                (Arg("offset", "result page offset", type=parse_int, default=0,
                     flags=("--offset",)),
                 Arg("count", "page size, 1..64", type=parse_int, default=64,
                     flags=("--count",)))),
        Command("action", "Run a system action", _cmd_system_action,
                (Arg("command", "action to run; see the table below",
                     choices=("reboot", "shutdown", "sleep", "reboot-emummc",
                              "terminate-application"),
                     metavar="ACTION"),
                 Arg("no_unlock", "do not arm the boot unlock for this reboot "
                                  "(reboot and reboot-emummc unlock by default)",
                     action="store_true", default=False,
                     flags=("--no-unlock",))),
                SYSTEM_ACTION_HELP),
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
                _cmd_game_status, (JSON_ARG, OUT_ARG)),
        Command("wait", "Wait until a game is running", _cmd_game_wait,
                (WAIT_TIMEOUT_ARG, POLL_ARG)),
        Command("launch-headless", "Experimental: start a game process without showing it "
                                   "on screen (headless; foreground launch is not possible "
                                   "from a sysmodule)", _cmd_game_launch_headless,
                (Arg("title_id", "Title ID (hex or decimal)", type=parse_int),
                 Arg("storage", "storage to force (auto-detected when omitted); see below",
                     nargs="?", choices=("SdCard", "BuiltInUser", "GameCard", "None"),
                     default=None, metavar="STORAGE"),
                 Arg("keys", "prod.keys path; read the update ticket on the console, decrypt "
                             "the titlekey on this host, and register it before launching",
                     flags=("--keys",), default=None),
                 Arg("titlekeys", "title.keys path with decrypted titlekeys "
                                  "(rightsId = key, or customized "
                                  "rightsId = title_key_block keygen for "
                                  "per-boot spl:es AccessKey computation); "
                                  "auto-registers before launching",
                     flags=("--titlekeys",), default=None),),
                "\n".join((
                    "STORAGE",
                    "  SdCard         SD card only (the usual choice for installed games)",
                    "  BuiltInUser    console memory",
                    "  GameCard       game card",
                    "  None           let the loader pick (default when omitted)",
                    "",
                    "The update ticket is what needs a key: without --titlekeys the launch",
                    "only works when the title's key was registered before (e.g. by playing",
                    "it once), because the AccessKey differs every boot.",
                ))),
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

    CommandGroup("debug", "Trace execution and inspect arbitrary processes", (
        Command("trace", "Record hardware-execution-breakpoint hits at up to four addresses",
                _cmd_debug_trace,
                (Arg("addresses", "one to four absolute or main+OFFSET instruction addresses",
                     nargs="+", metavar="ADDRESS"),
                 Arg("pid", "target process ID (default: foreground application)",
                     type=parse_int, default=None, flags=("--pid",)),
                 Arg("hits", "total-hit safety limit; 0 means unlimited with --ring",
                     type=parse_int, default=None, flags=("--hits",)),
                 Arg("ring", "keep the most recent N hits (1..64); duration/stop ends it",
                     type=parse_int, default=None, flags=("--ring",)),
                 Arg("duration", "failsafe duration in seconds; 0 means unlimited",
                     type=parse_int, default=None, flags=("--duration",)),
                 Arg("duration_from",
                     "start duration when armed or at the first matching hit",
                     choices=("arm", "first-hit"), default=None,
                     flags=("--duration-from",)),
                 Arg("args", "capture x0 through x(N-1), N=0..8",
                     type=parse_int, default=None, flags=("--args",)),
                 Arg("snapshots", "capture REG[+/-OFFSET]:SIZE at each hit "
                                  "(for example x0+0x10:0x40; max 2)",
                     action="append", default=None, flags=("--snapshot",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "The tracer attaches to any PID the kernel lets sys-agent debug and",
                    "uses up to four hardware execution breakpoints; target instructions are not",
                    "modified. The command returns after the breakpoint is armed, so it is",
                    "safe to start before the function or boot-time service path runs.",
                    "",
                    "Without --ring, --hits (default 1) ends the session and `trace read 0`",
                    "returns the last hit. With --ring, the session keeps the most recent N",
                    "and also stops at --hits (default 64) to protect a hot instruction;",
                    "--hits 0 removes that cap. --duration (default 60) and `trace stop`",
                    "remain independent lifetime bounds. --duration-from first-hit starts",
                    "the duration clock at the first matching hit instead of at arming.",
                    "",
                    "While the trace owns the target debug handle, memory commands are",
                    "rejected. Trace records are opt-in: no unsolicited data is written.",
                    "",
                    "Each --snapshot reads memory through x0..x30, sp, pc, or lr (plus an",
                    "optional signed byte offset) while the target is stopped. Snapshot read",
                    "failures are recorded per event instead of failing the whole trace.",
                ))),
        Command("trace-status", "Show tracer state, counters and last hit",
                _cmd_debug_trace_status, (JSON_ARG, OUT_ARG)),
        Command("trace-read", "Read one recorded hit, oldest first in ring mode",
                _cmd_debug_trace_read,
                (Arg("index", "ring index (default 0)", type=parse_int,
                     nargs="?", default=0), JSON_ARG, OUT_ARG)),
        Command("trace-stop", "Stop the active trace and release the debug handle",
                _cmd_debug_trace_stop, (JSON_ARG, OUT_ARG)),
        Command("multitrace", "Trace up to two instructions in each of two processes",
                _cmd_debug_multitrace,
                (Arg("pid1", "first process ID", type=parse_int),
                 Arg("target1", "first instruction address or main+OFFSET"),
                 Arg("pid2", "second process ID", type=parse_int),
                 Arg("target2", "second instruction address or main+OFFSET"),
                 Arg("extra_target1",
                     "second instruction for pid1, in addition to target1",
                     flags=("--extra-target1",), default=None),
                 Arg("extra_target2",
                     "second instruction for pid2, in addition to target2",
                     flags=("--extra-target2",), default=None),
                 Arg("hits", "shared total-hit safety limit; 0 means unlimited with --ring",
                     type=parse_int, default=None, flags=("--hits",)),
                 Arg("ring", "keep the most recent N events (1..64)",
                     type=parse_int, default=None, flags=("--ring",)),
                 Arg("duration", "failsafe duration in seconds; 0 means unlimited",
                     type=parse_int, default=None, flags=("--duration",)),
                 Arg("duration_from",
                     "start duration when armed or at the first matching hit",
                     choices=("arm", "first-hit"), default=None,
                     flags=("--duration-from",)),
                 Arg("args", "capture x0 through x(N-1), N=0..8",
                     type=parse_int, default=None, flags=("--args",)),
                 Arg("snapshots", "capture REG[+/-OFFSET]:SIZE at each hit "
                                  "(for example x1:0x100; max 2)",
                     action="append", default=None, flags=("--snapshot",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Experimental: assigns one runtime-probed context slot to each process",
                    "and one instruction slot to each target. Use the extra-target options",
                    "together to observe an entry/return pair on both sides of an IPC.",
                ))),
        Command("multitrace-status", "Show the experimental dual-process trace state",
                _cmd_debug_multitrace_status, (JSON_ARG, OUT_ARG)),
        Command("multitrace-read", "Read one dual-process trace event",
                _cmd_debug_multitrace_read,
                (Arg("index", "ring index (default 0)", type=parse_int,
                     nargs="?", default=0), JSON_ARG, OUT_ARG)),
        Command("multitrace-stop", "Stop the dual-process trace",
                _cmd_debug_multitrace_stop, (JSON_ARG, OUT_ARG)),
        Command("multitrace-dump", "Write dual-process trace events to versioned JSONL",
                _cmd_debug_multitrace_dump,
                (Arg("output", "output JSONL path "
                               "(default: multitrace-<unix time>.jsonl)",
                     flags=("--output",), default=None),)),
        Command("trace-plan", "Resolve a trace profile against the running console",
                _cmd_debug_trace_plan,
                (Arg("profile", "versioned trace profile JSON", metavar="PROFILE"),
                 OUT_ARG),
                "\n".join((
                    "Validates process identities, modules and Build IDs, then",
                    "prints the resolved PIDs and absolute target addresses as JSON.",
                    "The profile remains game-independent; consumer repositories",
                    "provide the actual modules, offsets and target roles.",
                ))),
        Command("trace-run", "Arm a trace from a versioned profile",
                _cmd_debug_trace_run,
                (Arg("profile", "versioned trace profile JSON", metavar="PROFILE"),
                 Arg("hits", "total-hit safety limit; 0 means unlimited with --ring",
                     type=parse_int, default=None, flags=("--hits",)),
                 Arg("ring", "keep the most recent N hits (1..64)",
                     type=parse_int, default=None, flags=("--ring",)),
                 Arg("duration", "failsafe duration in seconds; 0 means unlimited",
                     type=parse_int, default=None, flags=("--duration",)),
                 Arg("duration_from",
                     "override profile duration clock start",
                     choices=("arm", "first-hit"), default=None,
                     flags=("--duration-from",)),
                 Arg("args", "capture x0 through x(N-1), N=0..8",
                     type=parse_int, default=None, flags=("--args",)),
                 Arg("snapshots", "capture REG[+/-OFFSET]:SIZE at each hit "
                                  "(for example x1:0x100; max 2)",
                     action="append", default=None, flags=("--snapshot",)),
                 Arg("wait_process",
                     "park missing profile processes, arm, then release them",
                     action="store_true", default=False,
                     flags=("--wait-process",)),
                 Arg("wait_process_timeout",
                     "per-process creation timeout in milliseconds",
                     type=parse_int, default=60000,
                     flags=("--wait-process-timeout",)),
                 Arg("wait", "wait for the trace to finish before returning",
                     action="store_true", default=False, flags=("--wait",)),
                 Arg("wait_timeout", "maximum seconds to wait for completion",
                     type=float, default=None, flags=("--wait-timeout",)),
                 Arg("output", "wait for completion and write versioned JSONL",
                     flags=("--output",), default=None),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Resolves the profile immediately, then arms the matching",
                    "single-process or dual-process hardware trace.",
                    "--output implies --wait and additionally exports the retained ring.",
                ))),
        Command("trace-report", "Correlate a trace with its profile target roles",
                _cmd_debug_trace_report,
                (Arg("trace", "versioned trace JSONL", metavar="TRACE"),
                 Arg("profile", "profile used for the trace", metavar="PROFILE"),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Matches profile offsets against the module bases recorded in",
                    "the trace, reports entry/return pairs and identifies each call's",
                    "return-time x0 plus the first non-zero x0.",
                ))),
        Command("ipc-decode", "Decode an HIPC message containing CMIF headers",
                _cmd_debug_ipc_decode,
                (Arg("data", "raw HIPC message bytes as hex", type=parse_pattern),
                 Arg("mode", "message direction (default: auto-detect)",
                     choices=("auto", "request", "response"), default="auto",
                     flags=("--mode",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Parses HIPC metadata, descriptors, CMIF SFCI/SFCO headers,",
                    "domain headers, request token, response InterfaceId,",
                    "command ID or response Result, handles, and payload.",
                ))),
        Command("trace-ipc", "Decode CMIF/HIPC snapshots in trace JSONL",
                _cmd_debug_trace_ipc,
                (Arg("traces", "versioned trace JSONL files", nargs="+",
                     metavar="TRACE"),
                 Arg("snapshot", "only inspect snapshot 0 or 1",
                     type=parse_int, default=None, flags=("--snapshot",)),
                 Arg("profile", "attach target IDs and IPC semantics from a profile",
                     default=None, flags=("--profile",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Scans captured snapshots and reports every CMIF request or",
                    "response with its trace seq/target/thread and decoded header.",
                ))),
        Command("trace-timeline", "Merge trace sessions into one boot timeline",
                _cmd_debug_trace_timeline,
                (Arg("traces", "versioned trace JSONL files from one boot",
                     nargs="+", metavar="TRACE"),
                 Arg("profile", "optional profile for target IDs and IPC pairs",
                     default=None, flags=("--profile",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Sorts events from multiple sessions by console system tick,",
                    "keeps PID/Title/Build/target identity and attaches decoded",
                    "IPC request/response transactions. Files with different boot",
                    "epochs are rejected.",
                ))),
        Command("trace-dump", "Write retained trace events to a JSONL file",
                _cmd_debug_trace_dump,
                (Arg("output", "output JSONL path (default: trace-<unix time>.jsonl)",
                     flags=("--output",), default=None),)),
        Command("trace-analyze", "Summarize and correlate versioned trace files",
                _cmd_debug_trace_analyze,
                (Arg("traces", "JSONL trace files", nargs="+", metavar="TRACE"),
                 Arg("pairs", "ENTRY:RETURN address pair to correlate by session and thread",
                     action="append", default=None, flags=("--pair",)),
                 JSON_ARG, OUT_ARG),
                "\n".join((
                    "Events are ordered by the sysmodule's system tick. Entry/return",
                    "pairs are matched per session and thread; each return reports x0",
                    "at the return-site event. Multiple trace files can be combined when",
                    "they came from the same boot.",
                ))),
        Command("trace-diff", "Compare two trace files by event and target counts",
                _cmd_debug_trace_diff,
                (Arg("left", "baseline JSONL trace", metavar="LEFT"),
                 Arg("right", "candidate JSONL trace", metavar="RIGHT"),
                 JSON_ARG, OUT_ARG)),
        Command("read", "Read bytes from an arbitrary process and address",
                _cmd_debug_read,
                (Arg("pid", "target process ID", type=parse_int),
                 Arg("address", "absolute or main+OFFSET address"),
                 Arg("size", "byte count (1..0x1000)", type=parse_int))),
        Command("wait-title", "Park a title process before its main thread starts",
                _cmd_debug_wait_title,
                (Arg("title_id", "title ID", type=parse_int),
                 Arg("timeout_ms", "failure timeout in milliseconds",
                     type=parse_int, nargs="?", default=60000),
                 JSON_ARG, OUT_ARG)),
        Command("wait-application",
                "Park an application process before its main thread starts",
                _cmd_debug_wait_application,
                (Arg("timeout_ms", "failure timeout in milliseconds",
                     type=parse_int, nargs="?", default=60000),
                 JSON_ARG, OUT_ARG)),
        Command("wait-status", "Show the asynchronous PM process wait state",
                _cmd_debug_wait_status, (JSON_ARG, OUT_ARG)),
        Command("start-process",
                "Release a process parked by a PM create-process hook",
                _cmd_debug_start_process,
                (Arg("pid", "suspended process ID", type=parse_int),
                 JSON_ARG, OUT_ARG)),
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
        Command("query", "Query the memory mapping covering an address", _cmd_memory_query,
                (Arg("address", "absolute address", type=parse_int), JSON_ARG, OUT_ARG)),
        Command("hash", "Fingerprint an absolute region (FNV-1a 32)", _cmd_memory_hash,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("size", "byte count", type=parse_int),
                 JSON_ARG, OUT_ARG)),
        Command("dump", "Read an absolute region to a local file", _cmd_memory_dump,
                (Arg("start", "absolute start address", type=parse_int,
                     flags=("--start",)),
                 Arg("size", "byte count", type=parse_int, flags=("--size",)),
                 Arg("output", "write the raw bytes to this path",
                     flags=("--output",)),
                 Arg("chunk", "bytes per verified read (default 0x100000)",
                     type=parse_int, default=0x100000, flags=("--chunk",)))),
        Command("peek-verified", "Verified read relative to the heap", _cmd_peek_verified,
                (Arg("offset", "heap-relative address", type=parse_int),
                 Arg("size", "byte count", type=parse_int),
                 ATTEMPTS_ARG, JSON_ARG, OUT_ARG)),
        Command("peek-absolute-verified", "Verified read from an absolute address",
                _cmd_peek_absolute_verified,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("size", "byte count", type=parse_int),
                 ATTEMPTS_ARG, JSON_ARG, OUT_ARG)),
        Command("peek-main-verified", "Verified read relative to the main NSO base",
                _cmd_peek_main_verified,
                (Arg("offset", "main-relative address", type=parse_int),
                 Arg("size", "byte count", type=parse_int),
                 ATTEMPTS_ARG, JSON_ARG, OUT_ARG)),
        Command("wait-value", "Wait until an absolute address holds a value",
                _cmd_memory_wait_value,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("size", "byte count", type=parse_int),
                 Arg("value", "expected bytes as hex", type=parse_pattern),
                 WAIT_TIMEOUT_ARG, POLL_ARG)),
        Command("poke", "Write bytes relative to the heap", _cmd_poke,
                (Arg("offset", "heap-relative address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern),
                 NO_VERIFY_ARG)),
        Command("poke-absolute", "Write bytes at an absolute address", _cmd_poke_absolute,
                (Arg("address", "absolute address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern),
                 NO_VERIFY_ARG)),
        Command("poke-main", "Write bytes relative to the main NSO base", _cmd_poke_main,
                (Arg("offset", "main-relative address", type=parse_int),
                 Arg("data", "hex bytes", type=parse_pattern),
                 NO_VERIFY_ARG)),
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
                (BUTTON_ARG,), BUTTON_HELP),
        Command("release", "Release a held button", _cmd_release,
                (BUTTON_ARG,), BUTTON_HELP),
        Command("click", "Press and release a button", _cmd_click,
                (BUTTON_ARG,), BUTTON_HELP),
        Command("set-stick", "Set a stick position", _cmd_set_stick,
                (Arg("side", "LEFT or RIGHT", choices=("LEFT", "RIGHT")),
                 Arg("x", "X coordinate in -0x8000..0x7FFF", type=parse_int),
                 Arg("y", "Y coordinate in -0x8000..0x7FFF", type=parse_int))),
        Command("click-seq", "Run a comma-separated input sequence", _cmd_click_seq,
                (Arg("sequence", "e.g. A,W1000,B,+X,-X,%%5000,1500,(DUP,W200)*5"),
                 Arg("no_wait", "send without waiting for 'done'", action="store_true",
                     default=False, flags=("--no-wait",))),
                CLICK_SEQ_HELP),
        Command("capabilities", "Show the click-seq syntax this console supports",
                _cmd_input_capabilities),
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
        Command("text", "Type a string on the virtual keyboard", _cmd_text,
                (Arg("text", "text; multiple words are joined with spaces", nargs="+"),),
                TEXT_HELP),
    )),

    CommandGroup("controller", "Inspect and recover the virtual controller", (
        Command("status", "One-line virtual-controller status", _cmd_controller_status,
                (), CONTROLLER_STATUS_HELP),
        Command("dump", "Full dump: player slots, HDLS devices, hidsys owners",
                _cmd_controller_dump, (), CONTROLLER_DUMP_HELP),
        Command("kick", "Disconnect the real controller that holds a player slot",
                _cmd_controller_kick,
                (Arg("npad", "player slot 0-7", type=parse_int),)),
        Command("reconnect", "Pull back the controllers a kick disconnected (btdrv trigger)",
                _cmd_controller_reconnect,
                (Arg("timeout_ms", "host trigger timeout in ms", type=parse_int,
                     nargs="?", default=5000),
                 Arg("address", "explicit Bluetooth address AA:BB:CC:DD:EE:FF",
                     nargs="?", default=None))),
        Command("paired", "List the console's paired Bluetooth devices",
                _cmd_controller_paired, ()),
    )),

    CommandGroup("screen", "Capture or control the screen", (
        Command("capture", "Capture the current screen as a JPEG", _cmd_screenshot,
                (Arg("output", "write the JPEG to this path "
                               "(default: screenshot-<unix time>.jpg)",
                     flags=("--output",)),
                 DIFF_ARG, JSON_ARG)),
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
                (Arg("parameter", "setting name; see the table below",
                     metavar="NAME"),
                 Arg("value", "new value", type=parse_int)),
                CONFIG_SET_HELP),
        Command("get", "Print one runtime setting", _cmd_config_get,
                (Arg("parameter", "setting name; `config set --help` lists every name",
                     metavar="NAME"),)),
        Command("list", "Print every runtime setting", _cmd_config_list),
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


class _BlockHelpFormatter(argparse.RawDescriptionHelpFormatter):
    """Keep the line structure of descriptions, wrapping only long lines.

    The default formatter collapses every run of whitespace, which turns the
    button/sequence tables in the input command help into one paragraph.
    """

    def _fill_text(self, text: str, width: int, indent: str) -> str:
        lines = []
        for line in textwrap.dedent(text).splitlines():
            if not line.strip():
                lines.append("")
                continue
            leading = line[: len(line) - len(line.lstrip())]
            lines.append(textwrap.fill(line.strip(), width,
                                       initial_indent=indent + leading,
                                       subsequent_indent=indent + leading + " "))
        return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = _HelpfulParser(description=__doc__)
    parser.add_argument("--version", action="version", version=f"sysagent.py {CLIENT_VERSION}",
                        help="show the client version and exit")
    parser.add_argument("--host", default="switch", help="sys-agent host (default: switch)")
    parser.add_argument("--port", type=int, default=6000,
                        help="sys-agent TCP port (default: 6000)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="socket timeout in seconds (default: 10.0)")
    parser.add_argument("--retries", type=int, default=2,
                        help="extra attempts for read-only commands (default: 2; "
                             "write commands never retry)")
    subparsers = parser.add_subparsers(dest="action", required=True, metavar="COMMAND",
                                       title="commands", parser_class=_HelpfulParser)
    for item in COMMANDS:
        if isinstance(item, CommandGroup):
            group_parser = subparsers.add_parser(item.name, help=item.help,
                                                 description=item.help,
                                                 formatter_class=_BlockHelpFormatter)
            group_subparsers = group_parser.add_subparsers(
                dest=f"{item.name}_action", required=True, metavar="COMMAND",
                title="commands")
            for child in item.children:
                child_parser = group_subparsers.add_parser(
                    child.name, help=child.help,
                    description=child.description or child.help,
                    formatter_class=_BlockHelpFormatter)
                for argument in child.args:
                    argument.add_to(child_parser)
        else:
            subparser = subparsers.add_parser(item.name, help=item.help,
                                              description=item.description or item.help,
                                              formatter_class=_BlockHelpFormatter)
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
        with SysAgentClient(args.host, args.port, args.timeout, args.retries) as client:
            return command.handler(client, args) or 0
    except (OSError, ValueError, SysAgentProtocolError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
