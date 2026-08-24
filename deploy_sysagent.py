#!/usr/bin/env python3
"""Build sys-agent and update an installed copy on a running Switch.

Updates the current source tree onto an existing installation: runs the pinned devkitPro
Docker build, uploads the generated exefs.nsp to
``atmosphere/contents/43000000000000A6`` under a temporary ASCII name, verifies the
transfer, atomically renames it over the running file, and requests a full console reboot
directly into the virtual system (emuMMC) so the new sysmodule loads. It requires sys-agent
to already be running on the Switch because it uses the built-in FTP server (port 6001) and
the reboot command (port 6000); first-time installation must follow the manual SD-card
instructions in the README.

By design this script does NOT back up the Switch-side sys-agent. The console's existing
``atmosphere/contents/43000000000000A6`` directory is overwritten in place. Roll back by
re-deploying an older build or by editing the SD card manually.
"""

from __future__ import annotations

import argparse
import ftplib
import os
import socket
import subprocess
import sys
import time


REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
ARTIFACT = os.path.join(REPO_ROOT, "sys-agent", "43000000000000A6", "exefs.nsp")
REMOTE_DIR = "atmosphere/contents/43000000000000A6"
REMOTE_NAME = "exefs.nsp"
TEMP_NAME = "exefs.nsp.new"
DOCKER_IMAGE = "devkitpro/devkita64:20260219"
EXPECTED_AUDIO_CAPABILITY = "volume,mute"


def run_build() -> None:
    command = [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        "-v", f"{REPO_ROOT}:/work", "-w", "/work",
        DOCKER_IMAGE,
        "bash", "-lc", "source /opt/devkitpro/switchvars.sh && make",
    ]
    print(f"building sys-agent with {DOCKER_IMAGE}")
    subprocess.run(command, check=True)


def ensure_remote_dir(ftp: ftplib.FTP, path: str) -> None:
    """Change into path, creating missing levels on the SD card when needed.

    The built-in FTP server resolves paths relative to the current directory and
    joins them without normalizing away "..", so this helper always uses absolute
    paths (leading "/") to avoid double-nesting after a successful CWD.
    """
    current = ""
    for part in path.split("/"):
        if not part:
            continue
        current = f"{current}/{part}" if current else f"/{part}"
        try:
            ftp.cwd(current)
        except ftplib.error_perm:
            ftp.mkd(current)
            ftp.cwd(current)


def remove_if_present(ftp: ftplib.FTP, name: str) -> None:
    try:
        ftp.delete(name)
        print(f"removed stale remote file {name}")
    except ftplib.error_perm:
        pass


def upload_and_rename(host: str, ftp_port: int, local_size: int) -> None:
    ftp = ftplib.FTP()
    try:
        ftp.connect(host, ftp_port, timeout=15)
        ftp.login()
        print(f"connected to ftp://{host}:{ftp_port}/")
        ensure_remote_dir(ftp, REMOTE_DIR)
        remove_if_present(ftp, TEMP_NAME)
        with open(ARTIFACT, "rb") as source:
            ftp.storbinary(f"STOR {TEMP_NAME}", source)
        remote_size = ftp.size(TEMP_NAME)
        if remote_size != local_size:
            remove_if_present(ftp, TEMP_NAME)
            raise RuntimeError(
                f"transfer verification failed: remote {remote_size} != local {local_size} bytes")
        print(f"uploaded {TEMP_NAME} ({remote_size} bytes), renaming to {REMOTE_NAME}")
        try:
            ftp.rename(TEMP_NAME, REMOTE_NAME)
        except ftplib.error_perm as error:
            # The built-in FTP server refuses to rename over an existing file.
            # The verified new copy is already on the SD card as TEMP_NAME, so
            # remove the old file and retry the atomic rename.
            if "553" not in str(error):
                raise
            print("destination exists; removing old exefs.nsp and retrying the rename")
            remove_if_present(ftp, REMOTE_NAME)
            ftp.rename(TEMP_NAME, REMOTE_NAME)
        print(f"deployed {REMOTE_DIR}/{REMOTE_NAME}")
    finally:
        try:
            ftp.quit()
        except (ftplib.error_temp, ftplib.error_perm, OSError):
            pass


def request_reboot(host: str, cmd_port: int, reboot_command: str) -> None:
    print(f"requesting {reboot_command} on {host}:{cmd_port}")
    with socket.create_connection((host, cmd_port), timeout=5) as sock:
        sock.settimeout(3)
        sock.sendall((reboot_command + "\r\n").encode("ascii"))
        try:
            line = sock.recv(256).decode("ascii", "replace").strip()
            if line:
                print(f"reboot response: {line}")
            else:
                print("no reboot response (empty); reboot NOT confirmed - "
                      "check the console manually")
        except OSError:
            print("no reboot response (timeout); reboot NOT confirmed - the "
                  "command port may be wedged; check the console manually")


def verify_build(host: str, cmd_port: int, timeout: float) -> bool:
    """Wait until the console answers a full command round-trip, then report the build.

    A bare TCP connect can succeed during the old process's shutdown window, so this
    retries until a getVersion + systemCapabilities round-trip completes.
    """
    sys.path.insert(0, REPO_ROOT)
    from client.sysagent import SysAgentClient
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        try:
            with SysAgentClient(host, cmd_port, timeout=10) as client:
                version = client.get_version()
                capabilities = client.system_capabilities()
            audio = capabilities.get("audio", "")
            print(f"verified sys-agent version={version}")
            if audio == EXPECTED_AUDIO_CAPABILITY:
                print(f"verified new build: systemCapabilities audio={audio}")
            elif audio:
                print(f"warning: installed build advertises audio={audio!r}, "
                      f"expected {EXPECTED_AUDIO_CAPABILITY!r}")
            else:
                print("warning: installed build does not advertise the audio capability; "
                      "the console may still run an older sys-agent")
            return True
        except OSError as error:
            last_error = error
            time.sleep(3)
    print(f"error: console did not answer within {timeout:.0f}s: {last_error}",
          file=sys.stderr)
    return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build sys-agent and update an installed copy on a running Switch, "
                    "then reboot into emuMMC. Requires sys-agent to already be running.")
    parser.add_argument("--host", default="switch", help="Switch host (default: switch)")
    parser.add_argument("--cmd-port", type=int, default=6000,
                        help="sys-agent command port (default: 6000)")
    parser.add_argument("--ftp-port", type=int, default=6001,
                        help="built-in FTP port (default: 6001)")
    parser.add_argument("--no-build", action="store_true",
                        help="skip the Docker build and deploy the existing artifact")
    parser.add_argument("--no-reboot", action="store_true",
                        help="upload and rename only; do not reboot the console")
    parser.add_argument("--normal-reboot", action="store_true",
                        help="reboot normally instead of directly into emuMMC")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip waiting for the console and checking the installed build")
    parser.add_argument("--verify-timeout", type=float, default=180.0,
                        help="seconds to wait for the console after reboot (default: 180)")
    parser.add_argument("--verify-only", action="store_true",
                        help="skip build/upload/reboot; just wait for the console and verify")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the planned steps without touching the Switch")
    args = parser.parse_args(argv)

    if args.verify_only:
        return 0 if verify_build(args.host, args.cmd_port, args.verify_timeout) else 1

    if not args.no_build:
        run_build()

    if not os.path.isfile(ARTIFACT) or os.path.getsize(ARTIFACT) == 0:
        print(f"error: build artifact missing or empty: {ARTIFACT}", file=sys.stderr)
        return 1
    local_size = os.path.getsize(ARTIFACT)
    print(f"artifact: {ARTIFACT} ({local_size} bytes)")

    if args.dry_run:
        print(f"plan: upload {REMOTE_NAME} to {REMOTE_DIR} via "
              f"ftp://{args.host}:{args.ftp_port} (temp {TEMP_NAME} + atomic rename)")
        if not args.no_reboot:
            reboot_command = "systemReboot" if args.normal_reboot else "systemRebootEmuMMC"
            print(f"plan: send {reboot_command} to {args.host}:{args.cmd_port}")
        if not args.no_reboot and not args.no_verify:
            print(f"plan: wait up to {args.verify_timeout:.0f}s and verify the installed build")
        return 0

    upload_and_rename(args.host, args.ftp_port, local_size)

    if not args.no_reboot:
        reboot_command = "systemReboot" if args.normal_reboot else "systemRebootEmuMMC"
        request_reboot(args.host, args.cmd_port, reboot_command)
        if not args.no_verify:
            print(f"waiting up to {args.verify_timeout:.0f}s for the console to answer ...")
            if not verify_build(args.host, args.cmd_port, args.verify_timeout):
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
