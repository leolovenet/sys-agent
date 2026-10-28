from __future__ import annotations

import os
import socketserver
import threading
import time
import unittest
from unittest.mock import patch

from client.sysagent import (SysAgentProtocolError, SysAgentClient, parse_response, require_ok,
                             text_runs)


def build_test_ticket() -> bytes:
    """Synthetic RSA-2048-SHA256 common ticket (0x2C0 bytes)."""
    ticket = bytearray(0x2C0)
    ticket[0:4] = (0x10004).to_bytes(4, "little")
    data = 0x140
    ticket[data + 0x141] = 0  # title key type: common
    ticket[data + 0x145] = 0x0B  # master key revision
    ticket[data + 0x40:data + 0x50] = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    ticket[data + 0x160:data + 0x170] = bytes.fromhex("01006F8002326800000000000000000B")
    return bytes(ticket)


class FakeState:
    def __init__(self) -> None:
        self.status_calls = 0
        self.cancelled = False
        self.search_active = False
        self.launch_fails = False
        self.last_command: list[str] = []
        self.commands: list[list[str]] = []
        self.addresses = [0x80000010, 0x80000120, 0x80000230]
        self.application_running = True
        self.processes = {
            0x1234: "01006F8002326000",
            0x6C: "010000000000001E",
        }
        self.late_diagnostic = False
        self.ticket_hex: str | None = None
        self.last_key_register: list[str] | None = None
        self.last_key_unregister: list[str] | None = None
        self.key_ops: list[str] = []
        self.poke_verify_fails = False
        self.peek_verify_unstable = False
        self.flaky_peek_remaining = 0
        self.repeat_support = True
        self.sequence_error = False
        self.trace_busy = False
        self.trace_target_addresses = [0x2B00001000]
        # An older sysmodule has only `configure <name> <value>` and stays silent for a
        # read (`configure <name>`), which is what makes the client hit its socket timeout.
        self.silent_configure = False
        # What `configure <name>` answers, like the sysmodule's own setting table.
        self.config_settings: dict[str, int] = {"controllerIdleReleaseMs": 1000,
                                               "freezeRate": 3,
                                               "keySleepTime": 30,
                                               "pollRate": 20}


class FakeHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        state: FakeState = self.server.state  # type: ignore[attr-defined]
        for raw in self.rfile:
            command = raw.decode("ascii").strip().split()
            if not command:
                continue
            state.last_command = command
            state.commands.append(command)
            if command[0] == "debug":
                if command[1] == "modules":
                    pid = int(command[2], 16)
                    if pid == 0x1234:
                        response = (
                            "OK pid=0x1234 count=2 reported=2 truncated=0 "
                            "modules=0=0x29083FE000+0x6000:build=AAAA,"
                            "1=0x2908406000+0x5A06000:"
                            "build=FF1D1C05670DB6021C85B624A710B963")
                    elif pid == 0x6C:
                        response = (
                            "OK pid=0x6C count=1 reported=1 truncated=0 "
                            "modules=0=0x30000000+0x4F8000:"
                            "build=C43E2FD4D46907A4033858194F552FCC5A2B089B")
                    else:
                        response = (
                            "OK pid=%016X count=1 reported=1 truncated=0 "
                            "modules=0=0x2B000000+0x100000:build=AABB" % pid)
                elif command[1] == "maps":
                    pid = int(command[2], 16)
                    if pid == 0x1234:
                        base, size = 0x2908406000, 0x5A06000
                    else:
                        base, size = 0x30000000, 0x4F8000
                    response = (
                        f"OK pid=0x{pid:X} count=1 limit={command[3]} "
                        f"truncated=0 dmntClosed=0 "
                        f"maps=0=0x{base:X}+0x{size:X}:t3/p5")
                elif command[1] == "trace":
                    if state.trace_busy:
                        response = "ERR code=TRACE_BUSY result=0x0 stage="
                    else:
                        response = (
                            "OK state=started pid=0000000000000071 address=0000002B00001000 "
                            "args=8 hits=1 ring=0 duration=60 ctxSlot=1 insnSlot=0 "
                            "dmntClosed=0"
                        )
                elif command[1] == "trace-status":
                    target_text = ",".join(
                        f"{index}:0x{address:X}"
                        for index, address in enumerate(
                            state.trace_target_addresses))
                    response = (
                        "OK active=1 armed=1 stale=0 memoryBlocked=1 dmntClosed=0 "
                        "pid=0000000000000071 address=0000002B00001000 "
                        f"targetCount={len(state.trace_target_addresses)} "
                        f"targets={target_text} maxHits=1 "
                        "hitCount=1 duration=60 startedTick=100 ring=0 ringCount=0 "
                        "overwritten=0 args=8 ctxSlot=1 insnSlot=0 lastSeq=1 "
                        "lastPc=0000002B00001000 lastLr=0000002B00001004 "
                        "lastSp=0000002B00002000 lastThread=0000000000000088 "
                        "lastCore=2 stage= lastError=0x0 hint="
                    )
                elif command[1] == "trace-read":
                    response = (
                        "OK seq=1 thread=0000000000000088 core=2 tick=200 "
                        "pc=0000002B00001000 lr=0000002B00001004 sp=0000002B00002000 "
                        "args=2 contextResult=0x0 coreResult=0x0 insnBytes=12 "
                        "insn=FD7BBFA9FD030091FD430091 x0=0000000000000001 "
                        "x1=0000000000000002"
                    )
                elif command[1] == "trace-stop":
                    response = "OK state=stopped hitCount=1 ringCount=0 lastError=0x0 stage=hitsReached"
                elif command[1] == "multitrace":
                    response = (
                        "OK state=started active=1 pid0=0000000000000071 "
                        "target0=0000000000001000 pid1=0000000000000085 "
                        "target1=0000000000002000 targetCount=4 "
                        "target2=0000000000003000 target3=0000000000004000 "
                        "targetProcesses=0,1,0,1 hits=64 ring=8 duration=30 "
                        "args=4 ctx=4,5 slots=0,1,2,3"
                    )
                elif command[1] == "multitrace-status":
                    response = (
                        "OK active=0 armed=0 everArmed=1 stale=1 memoryBlocked=0 "
                        "pid0=0000000000000071 target0=0000000000001000 "
                        "pid1=0000000000000085 target1=0000000000002000 hits=2,1 "
                        "targetCount=4 target2=0000000000003000 "
                        "target3=0000000000004000 targetProcesses=0,1,0,1 "
                        "hits2=1,1 "
                        "hitCount=3 maxHits=64 ring=8 ringCount=1 duration=30 "
                        "startedTick=100 args=4 ctx=4,5 slots=0,1,2,3 lastSeq=3 "
                        "lastPc=0000000000002000 lastLr=0000000000002004 "
                        "lastSp=0000000000003000 lastThread=0000000000000088 "
                        "lastCore=2 stage=hitsReached lastError=0x0"
                    )
                elif command[1] == "multitrace-read":
                    response = (
                        "OK seq=3 target=3 targetAddress=0000000000004000 "
                        "thread=0000000000000088 core=2 tick=300 "
                        "pc=0000000000004000 lr=0000000000004004 "
                        "sp=0000000000003000 args=2 contextResult=0x0 "
                        "coreResult=0x0 insnBytes=0 insn= snapshots=0 "
                        "x0=0000000000000000 x1=0000000000000001"
                    )
                elif command[1] == "multitrace-stop":
                    response = "OK state=stopped hitCount=3 ringCount=1"
                else:
                    response = "ERR code=UNKNOWN_COMMAND"
            elif command[0] == "searchCapabilities":
                response = "OK version=3 modes=bytes,u8,u16,u32,u64 regions=absolute,heap,main alignment=powerOfTwo,max256 endian=little maxPattern=256 chunk=0x40000 maxResults=65536 maxPage=256 refine=exact,changed,unchanged,increased,decreased persistent=runtime storage=sd cRegions=absolute,heap,main,alias,addressSpace"
            elif command[0] in {"searchStart", "searchStartRegion"}:
                state.search_active = True
                response = "OK session=7 state=queued"
            elif command[0] == "searchBegin":
                state.search_active = True
                response = "OK session=9223372036854775809 state=queued"
            elif command[0].startswith("searchRefine"):
                response = f"OK session={command[1]} state=queued"
            elif command[0] == "memoryBackend":
                if len(command) == 2 and state.search_active:
                    response = "ERR code=BUSY"
                else:
                    policy = command[1] if len(command) == 2 else "auto"
                    response = (
                        f"OK policy={policy} active=dmnt dmntAvailable=1 dmntAttached=1 "
                        "pid=0000000000001234 titleId=01006F8002326000 lastError=0x0"
                    )
            elif command[0] == "memoryBackendProbe":
                response = (
                    "OK policy=auto active=dmnt dmntAvailable=1 dmntAttached=1 "
                    "pid=0000000000001234 titleId=01006F8002326000 lastError=0x0"
                )
            elif command[0] == "systemCapabilities":
                response = "OK version=1 processPageMax=64 sensitiveData=serial,account,wifiPassphrase"
            elif command[0] in {"systemInfo", "systemTime", "powerStatus", "storageStatus",
                               "networkStatus", "networkProfile", "accountStatus"}:
                response = f"OK command={command[0]} sample=1 errors=field:0x0"
            elif command[0] == "applicationStatus":
                response = (f"OK command=applicationStatus "
                            f"running={1 if state.application_running else 0} "
                            "pid=0000000000001234 titleId=01006F8002326000 version=34 "
                            "main=0000002908406000 heap=0000002C1A400000 "
                            "buildId=FF1D1C05670DB6021C85B624A710B963 sample=1 "
                            "errors=field:0x0")
            elif command[0] == "processList":
                offset = int(command[1])
                count = int(command[2])
                entries = list(state.processes.items())
                page = entries[offset:offset + count]
                encoded = ",".join(
                    f"{pid:016X}:{title_id}" for pid, title_id in page)
                response = (
                    f"OK total={len(entries)} captured={len(page)} "
                    f"offset={offset} count={len(page)} processes={encoded}")
            elif command[0] in {"systemReboot", "systemRebootEmuMMC", "systemShutdown", "systemSleep",
                               "applicationTerminate"}:
                response = f"OK action={command[0]}"
            elif command[0] == "networkSet":
                response = f"OK wireless={1 if command[1] == 'enabled' else 0}"
            elif command[0] == "lockScreenStatus":
                response = "OK lockScreen=0"
            elif command[0] == "lockScreenSet":
                response = f"OK lockScreen={1 if command[1] == 'enabled' else 0}"
            elif command[0] == "audioVolume":
                volume = command[1] if len(command) == 2 else "65"
                response = f"OK volume={volume}"
            elif command[0] == "audioMute":
                mute = 1 if len(command) == 2 and command[1] == "enabled" else 0
                response = f"OK mute={mute} target=Tv"
            elif command[0] == "searchStatus":
                state.status_calls += 1
                status = "running" if state.status_calls == 1 else "done"
                response = (
                    f"OK session=7 state={status} start=0000000080000000 end=0000000080001000 "
                    f"scanned={2048 if status == 'running' else 4096} total=4096 matches=3 stored=3 "
                    "truncated=0 readErrors=1 error=0x0"
                    " type=bytes region=absolute base=0000000000000000 "
                    "regionOffset=0000000080000000 alignment=1 backend=dmnt"
                )
            elif command[0] == "searchResults":
                offset, count = int(command[2]), min(int(command[3]), 256)
                values = state.addresses[offset : offset + count]
                encoded = ",".join(f"{value:016X}" for value in values)
                response = f"OK session=7 offset={offset} count={len(values)} stored=3 addresses={encoded}"
            elif command[0] == "searchCancel":
                state.cancelled = True
                response = "OK session=7 cancel=requested"
            elif command[0] == "searchClose":
                state.search_active = False
                response = "OK session=7 state=closed"
            elif command[0] == "screenCapture":
                # Return a JPEG-like payload whose hex line exceeds the old
                # 1 MiB client cap, proving the raised response limit works.
                response = (b"\xFF\xD8" + b"\x00" * 600000 + b"\xFF\xD9").hex().upper()
            elif command[0] in {"peek", "peekAbsolute", "peekMain"}:
                if command[0] == "peek" and command[2] == "0x0":
                    response = ""
                elif state.flaky_peek_remaining > 0:
                    state.flaky_peek_remaining -= 1
                    response = ""
                else:
                    response = "DEADBEEF00"
            elif command[0] in {"peekMulti", "peekAbsoluteMulti", "peekMainMulti"}:
                response = "DEADBEEF00C0FFEE"
            elif command[0] in {"poke", "pokeAbsolute", "pokeMain", "pointerPoke"}:
                response = None
            elif command[0] in {"pokeVerified", "pokeAbsoluteVerified", "pokeMainVerified"}:
                if state.poke_verify_fails:
                    response = (f"ERR code=WRITE_VERIFY_FAILED addr={command[1]} size=2 "
                                "expected=AABB actual=0000")
                else:
                    response = f"OK addr={command[1]} size=2 verified=1"
            elif command[0] == "memoryQuery":
                response = ("OK addr=0x2C1A400000 base=0x2C1A400000 size=0x1000 "
                            "type=0x5 typeName=heap attr=0x0 perm=0x3 permName=rw")
            elif command[0] == "memoryHash":
                response = "OK addr=0x100 size=4 algo=fnv1a32 hash=0x1234ABCD"
            elif command[0] == "log":
                response = ("log 0 tick=100 cmd=getVersion\n"
                            "log 1 tick=200 cmd=peekAbsolute\n"
                            "END log")
            elif command[0] in {"peekVerified", "peekAbsoluteVerified", "peekMainVerified"}:
                if state.peek_verify_unstable:
                    response = "ERR code=READ_UNSTABLE addr=0x100 size=4 attempts=3"
                else:
                    response = "OK addr=0x100 size=4 attempts=3 data=DEADBEEF"
            elif command[0] == "pointer":
                response = "0000000080001000"
            elif command[0] in {"pointerAll", "pointerRelative"}:
                response = "0000000080002000"
            elif command[0] == "pointerPeek":
                response = "AABBCCDD"
            elif command[0] == "pointerPeekMulti":
                response = "AABBCCDDEEFF"
            elif command[0] in {"freeze", "unFreeze", "freezeClear", "freezePause",
                                "freezeUnpause"}:
                response = None
            elif command[0] == "freezeCount":
                response = "03"
            elif command[0] in {"press", "release", "click", "setStick", "clickCancel",
                                 "detachController", "touch", "touchHold", "touchDraw",
                                 "touchCancel", "key", "keyMod", "keyMulti",
                                 "screenOff", "screenOn"}:
                response = None
                if command[0] == "click" and state.late_diagnostic:
                    # The sysmodule reports a refused state write only once the click
                    # has run (its press/release sleep), i.e. after the client's short
                    # drain window: the line lands on the connection late.
                    time.sleep(0.05)
                    self._write("ERR controllerState result=0x1C24CA attached=1 takeover=1")
            elif command[0] == "configure":
                # A pre-read-path sysmodule reads the command and prints nothing.
                if state.silent_configure:
                    continue
                # `configure <name> <value>` is silent; the read paths answer with
                # `name=value` lines terminated by END, like the sysmodule does.
                if len(command) == 3:
                    response = None
                    if command[1] in state.config_settings:
                        state.config_settings[command[1]] = int(command[2])
                elif len(command) == 2:
                    if command[1] in state.config_settings:
                        response = (f"OK settings=1\n{command[1]}="
                                    f"{state.config_settings[command[1]]}\n"
                                    "END configure")
                    else:
                        response = (f"ERR configure unknown={command[1]} "
                                    "hint=controllerIdleReleaseMs\nEND configure")
                else:
                    settings = "\n".join(f"{name}={value}"
                                         for name, value in state.config_settings.items())
                    response = (f"OK settings={len(state.config_settings)}\n{settings}\n"
                                "END configure")
            elif command[0] == "clickSeqCapabilities":
                repeat = "repeat=paren-star" if state.repeat_support else "repeat=absent"
                response = ("OK version=1 syntax=click,press,release,wait,stick "
                            f"{repeat} maxCount=1000 maxDepth=4 maxTokens=1024 maxBytes=65536")
            elif command[0] == "clickSeq":
                if state.sequence_error:
                    response = "ERR code=BAD_SEQUENCE index=3 detail=bad-syntax"
                else:
                    tokens = len([token for token in command[1].split(",") if token])
                    response = f"OK tokens={tokens} estMs={tokens * 50}\ndone"
            elif command[0] == "game":
                if command[1] == "icon":
                    response = (b"\xFF\xD8" + b"ICON").hex().upper()
                elif command[1] == "version":
                    response = "1.6.0"
                elif command[1] == "rating":
                    response = "7"
                elif command[1] == "author":
                    response = "Nintendo"
                else:
                    response = "Animal Crossing"
            elif command[0] == "gameLaunchHeadless":
                if state.launch_fails:
                    response = ("ERR code=COMMAND_FAILED stage=launchProgram result=0xDFC7D802 "
                                "attempts=SdCard:0xDFC7D802,BuiltInUser:0xA5800A08,"
                                "GameCard:0x80000A08,None:0x80000A08")
                else:
                    response = "OK action=launched pid=0000000000001234 " \
                               f"titleId={command[1][2:].upper()} storage=SdCard"
            elif command[0] == "gameTicketRead":
                if state.ticket_hex is None:
                    response = "ERR code=COMMAND_FAILED stage=ticketData result=0x0"
                else:
                    response = (
                        "OK updateStorage=5 patchProgram=E10617820DB06889E1638499478DA0DE "
                        f"rightsId=01006F8002326800000000000000000B "
                        f"ticketSize={len(bytes.fromhex(state.ticket_hex))} "
                        f"ticket={state.ticket_hex}"
                    )
            elif command[0] == "gameExternalKeyProbe":
                response = ("OK updateStorage=5 patchProgram=E10617820DB06889E1638499478DA0DE "
                            "rightsId=01006F8002326800000000000000000B es14=0x0 es16=0x0")
            elif command[0] == "gameExternalKeyRegister":
                state.last_key_register = command[1:]
                state.key_ops.append("reg")
                response = f"OK action=externalKeyRegistered rightsId={command[1]}"
            elif command[0] == "gameExternalKeyPrepareCommon":
                state.last_key_register = command[1:3]
                state.key_ops.append("prepare")
                response = (
                    f"OK action=externalKeyPrepared mode=common rightsId={command[1]} "
                    "accessKey=00112233445566778899AABBCCDDEEFF"
                )
            elif command[0] == "gameExternalKeyUnregister":
                state.last_key_unregister = command[1:]
                state.key_ops.append("unreg")
                response = f"OK action=externalKeyUnregistered rightsId={command[1]}"
            elif command[0] == "getVersion":
                response = "2.7.0"
            elif command[0] == "charge":
                response = "85"
            elif command[0] == "fdCount":
                response = "3"
            elif command[0] == "getTitleID":
                response = "01006F8002326000"
            elif command[0] == "getTitleVersion":
                response = "0000000000100000"
            elif command[0] == "getSystemLanguage":
                response = "1"
            elif command[0] == "getBuildID":
                response = "0123456789ABCDEF"
            elif command[0] == "getHeapBase":
                response = "00000005ECA00000"
            elif command[0] == "getMainNsoBase":
                response = "00000005370606000"
            elif command[0] == "isProgramRunning":
                response = "1"
            elif command[0] == "controllerStatus":
                response = ("OK handle=0000000000000001 session=0000000000000002 attached=1 "
                            "deviceType=0x03 interface=1(bluetooth) bindNpad=-1 slot=0 "
                            "assignmentRc=0x0 lastStateError=0x0")
            elif command[0] in {"controllerDump", "controllerKick", "controllerReconnect",
                               "controllerPairedDevices"}:
                end_token = f"END {command[0]}"
                response = f"OK command={command[0]}\n  detail=1\n{end_token}"
            else:
                response = "ERR code=UNKNOWN_COMMAND"
            if response is None:
                continue
            # Split the response to verify that the client handles TCP fragmentation.
            self._write(response)

    def _write(self, response: str) -> None:
        """Sends one answer, tolerating a peer that the client already closed."""
        payload = (response + "\n").encode("ascii")
        try:
            midpoint = len(payload) // 2
            self.wfile.write(payload[:midpoint])
            self.wfile.flush()
            self.wfile.write(payload[midpoint:])
            self.wfile.flush()
        except OSError:
            pass


class FakeServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), FakeHandler)
        self.state = FakeState()


class ClientTests(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FakeServer()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def client(self) -> SysAgentClient:
        return SysAgentClient("127.0.0.1", self.server.server_address[1], timeout=1)

    def test_complete_search_and_pagination(self) -> None:
        with self.client() as client:
            self.assertEqual(client.capabilities()["maxPattern"], "256")
            session = client.start(0x80000000, 0x80001000, bytes.fromhex("DEADBEEF"))
            self.assertEqual(session, 7)
            status = client.wait(session, poll_interval=0)
            self.assertEqual(status.state, "done")
            self.assertEqual(status.read_errors, 1)
            self.assertFalse(status.truncated)
            self.assertEqual(status.backend, "dmnt")
            self.assertEqual(list(client.iter_results(session, page_size=2)), self.server.state.addresses)
            client.close_session(session)

    def test_region_and_typed_start(self) -> None:
        with self.client() as client:
            self.assertEqual(client.capabilities()["endian"], "little")
            self.assertEqual(client.start_region("u32", "heap", 0x20, 0x1000, "0x12345678"), 7)
            self.assertEqual(client.start_region("bytes", "main", 0, 0x100, b"\xDE\xAD", 1), 7)
            with self.assertRaises(ValueError):
                client.start_region("u16", "invalid", 0, 16, 1)

    def test_backend_status_policy_and_probe(self) -> None:
        with self.client() as client:
            status = client.backend_status()
            self.assertEqual(status.active, "dmnt")
            self.assertTrue(status.dmnt_attached)
            self.assertEqual(status.title_id, 0x01006F8002326000)
            self.assertEqual(client.set_backend_policy("direct").policy, "direct")
            self.assertEqual(client.probe_backend().process_id, 0x1234)
            with self.assertRaises(ValueError):
                client.set_backend_policy("invalid")

    def test_system_queries_and_actions(self) -> None:
        with self.client() as client:
            self.assertEqual(client.system_capabilities()["processPageMax"], "64")
            self.assertEqual(client.system_query("network-profile")["sample"], "1")
            self.assertEqual(client.process_list(1, 2)["offset"], "1")
            self.assertEqual(client.set_wireless(False)["wireless"], "0")
            self.assertEqual(client.lock_screen_status()["lockScreen"], "0")
            self.assertEqual(client.set_lock_screen(True)["lockScreen"], "1")
            self.assertEqual(client.system_action("reboot")["action"], "systemReboot")
            self.assertEqual(self.server.state.last_command, ["systemReboot"])
            # Lock/unlock is server-side by default; the flag opts out.
            client.system_action("reboot", unlock=False)
            self.assertEqual(self.server.state.last_command, ["systemReboot", "noUnlock"])
            self.assertEqual(client.system_action("reboot-emummc")["action"], "systemRebootEmuMMC")
            client.system_action("reboot-emummc", unlock=False)
            self.assertEqual(self.server.state.last_command,
                             ["systemRebootEmuMMC", "noUnlock"])
            # --no-unlock means nothing for the other actions.
            with self.assertRaises(ValueError):
                client.system_action("sleep", unlock=False)
            with self.assertRaises(ValueError):
                client.process_list(0, 65)
            with self.assertRaises(ValueError):
                client.system_action("payload")

    def test_audio_commands(self) -> None:
        with self.client() as client:
            self.assertEqual(client.audio_volume()["volume"], "65")
            self.assertEqual(self.server.state.last_command, ["audioVolume"])
            self.assertEqual(client.audio_volume(0)["volume"], "0")
            self.assertEqual(client.audio_volume(100)["volume"], "100")
            self.assertEqual(client.audio_volume(30)["volume"], "30")
            self.assertEqual(self.server.state.last_command, ["audioVolume", "30"])
            self.assertEqual(client.audio_mute()["mute"], "0")
            self.assertEqual(client.audio_mute()["target"], "Tv")
            self.assertEqual(client.audio_mute("disabled")["mute"], "0")
            self.assertEqual(client.audio_mute("enabled")["mute"], "1")
            self.assertEqual(self.server.state.last_command, ["audioMute", "enabled"])
            with self.assertRaises(ValueError):
                client.audio_volume(-1)
            with self.assertRaises(ValueError):
                client.audio_volume(101)
            with self.assertRaises(ValueError):
                client.audio_mute("maybe")

    def test_audio_cli(self) -> None:
        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "audio", "volume"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "audio", "volume", "30"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "audio", "mute", "enabled"])
        self.assertEqual(code, 0)

    def test_poke_verified_is_default(self) -> None:
        with self.client() as client:
            result = client.poke_absolute_verified(0x100, b"\xAA\xBB")
            self.assertEqual(result["verified"], "1")
            self.assertEqual(self.server.state.last_command[0], "pokeAbsoluteVerified")

        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "memory", "poke-absolute", "0x100", "AABB"])
        self.assertEqual(code, 0)
        self.assertEqual(self.server.state.last_command[0], "pokeAbsoluteVerified")

    def test_poke_verified_heap_and_main_variants(self) -> None:
        with self.client() as client:
            self.assertEqual(client.poke_verified(0x20, b"\xAA\xBB")["verified"], "1")
            self.assertEqual(self.server.state.last_command[0], "pokeVerified")
            self.assertEqual(client.poke_main_verified(0x20, b"\xAA\xBB")["verified"], "1")
            self.assertEqual(self.server.state.last_command[0], "pokeMainVerified")

    def test_poke_no_verify_uses_legacy_command(self) -> None:
        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "memory", "poke-absolute", "0x100", "AABB", "--no-verify"])
        self.assertEqual(code, 0)
        self.assertEqual(self.server.state.last_command[0], "pokeAbsolute")

    def test_poke_verify_failure_is_reported(self) -> None:
        self.server.state.poke_verify_fails = True
        with self.client() as client:
            with self.assertRaisesRegex(SysAgentProtocolError, "WRITE_VERIFY_FAILED"):
                client.poke_absolute_verified(0x100, b"\xAA\xBB")

    def test_memory_query(self) -> None:
        with self.client() as client:
            info = client.memory_query(0x2C1A400000)
            self.assertEqual(info["typeName"], "heap")
            self.assertEqual(info["permName"], "rw")
            self.assertEqual(self.server.state.last_command[0], "memoryQuery")

    def test_debug_trace_cli(self) -> None:
        from client.sysagent import main
        base = ["--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1", "debug"]

        self.assertEqual(main(base + [
            "trace", "main+0x1000", "--pid", "0x71", "--hits", "2",
            "--ring", "16", "--duration", "30", "--args", "4"]), 0)
        self.assertEqual(self.server.state.last_command, [
            "debug", "trace", "main+0x1000", "pid=0x71", "hits", "2",
            "ring", "16", "duration", "30", "args", "4"])

        self.assertEqual(main(base + [
            "trace", "main+0x1000", "0x2000", "--snapshot",
            "x0+0x10:0x20", "--snapshot", "lr:8"]), 0)
        self.assertEqual(self.server.state.last_command, [
            "debug", "trace", "main+0x1000", "0x2000",
            "snap=x0+0x10:0x20", "snap=lr:8"])

        self.assertEqual(main(base + ["trace-status"]), 0)
        self.assertEqual(self.server.state.last_command,
                         ["debug", "trace-status"])
        self.assertEqual(main(base + ["trace-read", "0"]), 0)
        self.assertEqual(self.server.state.last_command,
                         ["debug", "trace-read", "0"])
        self.assertEqual(main(base + ["trace-stop"]), 0)
        self.assertEqual(self.server.state.last_command,
                         ["debug", "trace-stop"])

        self.assertEqual(main(base + ["trace", "0x1000", "--ring", "65"]), 2)
        self.assertEqual(main(base + ["trace", "0x1000", "--hits", "0"]), 2)
        self.assertEqual(main(base + [
            "trace", "0x1000", "0x2000", "0x3000", "0x4000", "0x5000"]), 2)
        self.assertEqual(
            main(base + ["trace", "0x1000", "--ring", "8", "--hits", "0"]),
            0)
        self.server.state.trace_busy = True
        self.assertEqual(main(base + ["trace", "0x1000"]), 2)

    def test_debug_trace_dump(self) -> None:
        import json
        import tempfile
        from client.sysagent import main

        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "trace.jsonl")
            code = main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "trace-dump", "--output", output,
            ])
            self.assertEqual(code, 0)
            with open(output, encoding="utf-8") as handle:
                metadata = json.loads(handle.readline())
                event = json.loads(handle.readline())
            self.assertEqual(metadata["record"], "meta")
            self.assertEqual(metadata["schemaVersion"], 1)
            self.assertIn("sessionId", metadata)
            self.assertEqual(len(metadata["targets"]), 1)
            self.assertEqual(event["record"], "event")
            self.assertEqual(event["pc"], "0000002B00001000")
            self.assertNotIn("ok", event)

    def test_debug_trace_dump_preserves_multiple_targets(self) -> None:
        import json
        import tempfile
        from client.sysagent import main

        self.server.state.trace_target_addresses = [
            0x2B00001000,
            0x2B00002000,
            0x2B00003000,
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "trace.jsonl")
            self.assertEqual(main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "trace-dump", "--output", output,
            ]), 0)
            with open(output, encoding="utf-8") as handle:
                metadata = json.loads(handle.readline())
            self.assertEqual(len(metadata["targets"]), 3)
            self.assertEqual(
                metadata["targets"][2]["address"], "0x2B00003000")

    def test_debug_multitrace_cli_and_dump(self) -> None:
        import json
        import tempfile
        from client.sysagent import main

        base = ["--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1", "debug"]
        self.assertEqual(main(base + [
            "multitrace", "0x71", "main+0x10", "0x85", "0x2000",
            "--extra-target1", "main+0x20",
            "--extra-target2", "main+0x30",
            "--ring", "8", "--duration", "30", "--args", "4",
            "--snapshot", "x1:0x100"]), 0)
        self.assertEqual(self.server.state.last_command, [
            "debug", "multitrace", "0x71", "main+0x10", "0x85", "0x2000",
            "also", "main+0x20", "main+0x30",
            "ring", "8", "duration", "30", "args", "4", "snap=x1:0x100"])
        self.assertEqual(main(base + [
            "multitrace", "0x71", "0x10", "0x85", "0x20",
            "--extra-target1", "0x30"]), 2)
        self.assertEqual(main(base + ["multitrace-status"]), 0)
        self.assertEqual(main(base + ["multitrace-read", "0"]), 0)
        self.assertEqual(main(base + ["multitrace-stop"]), 0)

        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "multitrace.jsonl")
            self.assertEqual(main(base + [
                "multitrace-dump", "--output", output]), 0)
            with open(output, encoding="utf-8") as handle:
                metadata = json.loads(handle.readline())
                event = json.loads(handle.readline())
            self.assertEqual(metadata["kind"], "multitrace")
            self.assertEqual(len(metadata["targets"]), 4)
            self.assertEqual(metadata["targets"][2]["processIndex"], 0)
            self.assertEqual(metadata["targets"][3]["processIndex"], 1)
            self.assertEqual(event["target"], "3")

    def test_trace_profile_plan_run_and_report(self) -> None:
        import json
        import tempfile
        from client.sysagent import (load_trace_profile, main,
                                     report_trace_with_profile,
                                     resolve_trace_profile)

        profile_data = {
            "schemaVersion": 1,
            "name": "client-server-entry-return",
            "processes": [
                {
                    "id": "client",
                    "foreground": True,
                    "expect": {
                        "titleId": "01006F8002326000",
                        "buildId": "FF1D1C05670DB6021C85B624A710B963",
                    },
                },
                {
                    "id": "server",
                    "titleId": "010000000000001E",
                    "mainModuleIndex": 0,
                    "expect": {
                        "buildId": "C43E2FD4D46907A4033858194F552FCC5A2B089B",
                    },
                },
            ],
            "targets": [
                {"id": "client-entry", "process": "client",
                 "offset": "0x3DB65C0"},
                {"id": "server-entry", "process": "server",
                 "offset": "0x162620"},
                {"id": "client-return", "process": "client",
                 "offset": "0x3BA05D4"},
                {"id": "server-return", "process": "server",
                 "offset": "0x3C2FC"},
            ],
            "pairs": [
                {"entry": "client-entry", "return": "client-return"},
                {"entry": "server-entry", "return": "server-return"},
            ],
            "trace": {
                "hits": 16,
                "ring": 16,
                "duration": 20,
                "args": 8,
                "snapshots": ["x1:0x100"],
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            profile_path = os.path.join(directory, "profile.json")
            plan_path = os.path.join(directory, "plan.json")
            with open(profile_path, "w", encoding="utf-8") as handle:
                json.dump(profile_data, handle)
            profile = load_trace_profile(profile_path)
            with self.client() as client:
                plan = resolve_trace_profile(client, profile)
            self.assertEqual(plan["mode"], "multitrace")
            self.assertEqual([target["id"] for target in plan["targets"]], [
                "client-entry", "server-entry", "client-return", "server-return"])
            self.assertEqual(plan["targets"][0]["address"], "0x290C1BC5C0")
            self.assertEqual(plan["targets"][1]["address"], "0x30162620")

            base = ["--host", "127.0.0.1", "--port",
                    str(self.server.server_address[1]), "--timeout", "1",
                    "debug", "trace-plan", profile_path]
            self.assertEqual(main(base + ["--out", plan_path]), 0)
            with open(plan_path, encoding="utf-8") as handle:
                self.assertEqual(json.load(handle)["kind"], "trace-plan")

            run_base = ["--host", "127.0.0.1", "--port",
                        str(self.server.server_address[1]), "--timeout", "1",
                        "debug", "trace-run", profile_path]
            self.assertEqual(main(run_base), 0)
            self.assertEqual(self.server.state.last_command[:7], [
                "debug", "multitrace", "0x1234", "0x290C1BC5C0",
                "0x6C", "0x30162620", "also"])
            self.assertIn("snap=x1:0x100", self.server.state.last_command)
            trace_output = os.path.join(directory, "profile-trace.jsonl")
            self.assertEqual(main(run_base + ["--output", trace_output]), 0)
            with open(trace_output, encoding="utf-8") as handle:
                exported_metadata = json.loads(handle.readline())
            self.assertEqual(exported_metadata["kind"], "multitrace")
            self.assertEqual(exported_metadata["targets"][0]["id"], "client-entry")
            self.assertEqual(
                exported_metadata["profile"]["name"],
                "client-server-entry-return")

            client_process = {
                "pid": "0000000000001234",
                "titleId": "01006F8002326000",
                "moduleBase": "0x2908406000",
                "buildId": profile_data["processes"][0]["expect"]["buildId"],
            }
            server_process = {
                "pid": "000000000000006C",
                "titleId": "010000000000001E",
                "moduleBase": "0x30000000",
                "buildId": profile_data["processes"][1]["expect"]["buildId"],
            }
            metadata = {
                "record": "meta",
                "schemaVersion": 1,
                "sessionId": "profile-test",
                "targets": [
                    {"index": index, "address": target["address"],
                     "processIndex": 0 if process is client_process else 1,
                     "process": process}
                    for index, target, process in zip(
                        range(4), plan["targets"],
                        (client_process, server_process,
                         client_process, server_process))
                ],
            }
            events = [
                {"seq": "1", "tick": "100", "thread": "AA", "target": "0",
                 "targetAddress": plan["targets"][0]["address"],
                 "pc": plan["targets"][0]["address"], "x0": "0"},
                {"seq": "2", "tick": "200", "thread": "AA", "target": "2",
                 "targetAddress": plan["targets"][2]["address"],
                 "pc": plan["targets"][2]["address"], "x0": "1234"},
                {"seq": "3", "tick": "300", "thread": "BB", "target": "1",
                 "targetAddress": plan["targets"][1]["address"],
                 "pc": plan["targets"][1]["address"], "x0": "5678"},
                {"seq": "4", "tick": "400", "thread": "BB", "target": "3",
                 "targetAddress": plan["targets"][3]["address"],
                 "pc": plan["targets"][3]["address"], "x0": "167B"},
            ]
            report = report_trace_with_profile(
                {"metadata": metadata, "events": events, "path": "trace.jsonl"},
                profile)
            self.assertEqual(report["pairs"][0]["entryId"], "client-entry")
            self.assertEqual(report["pairs"][1]["firstNonZeroX0"], "167B")
            self.assertEqual(report["pairs"][1]["calls"][0]["x0NonZero"], True)

            trace_path = os.path.join(directory, "trace.jsonl")
            with open(trace_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps(metadata) + "\n")
                for event in events:
                    handle.write(json.dumps({
                        "record": "event", "schemaVersion": 1, **event,
                    }) + "\n")
            self.assertEqual(main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "trace-report", trace_path, profile_path]), 0)

            single_profile = {
                "schemaVersion": 1,
                "name": "single-process",
                "processes": [{"id": "client", "foreground": True}],
                "targets": [
                    {"id": "a", "process": "client", "offset": "0x3DB65C0"},
                    {"id": "b", "process": "client", "offset": "0x3DB65C4"},
                    {"id": "c", "process": "client", "offset": "0x3DB65C8"},
                ],
                "trace": {"hits": 4, "ring": 4, "duration": 5, "args": 2},
            }
            single_path = os.path.join(directory, "single.json")
            with open(single_path, "w", encoding="utf-8") as handle:
                json.dump(single_profile, handle)
            self.assertEqual(main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "trace-run", single_path]), 0)
            self.assertEqual(self.server.state.last_command[:6], [
                "debug", "trace", "0x290C1BC5C0", "0x290C1BC5C4",
                "0x290C1BC5C8", "pid=0x1234"])

    def test_hipc_cmif_decode_and_trace_scan(self) -> None:
        import json
        import tempfile
        from client.sysagent import (decode_hipc_cmif, decode_trace_ipc,
                                     load_trace_jsonl, main)

        request = bytearray(40)
        request[0:4] = (4).to_bytes(4, "little")
        request[4:8] = (8).to_bytes(4, "little")
        request[16:32] = b"".join((
            (0x49434653).to_bytes(4, "little"),
            (0).to_bytes(4, "little"),
            (8).to_bytes(4, "little"),
            (0xDEADBEEF).to_bytes(4, "little"),
        ))
        decoded_request = decode_hipc_cmif(bytes(request))
        self.assertEqual(decoded_request["cmif"]["mode"], "request")
        self.assertEqual(decoded_request["cmif"]["header"]["commandId"], 8)
        self.assertEqual(decoded_request["cmif"]["header"]["token"], 0xDEADBEEF)

        response = bytearray(request)
        response[16:20] = (0x4F434653).to_bytes(4, "little")
        response[24:28] = (0x167B).to_bytes(4, "little")
        decoded_response = decode_hipc_cmif(bytes(response))
        self.assertEqual(decoded_response["cmif"]["mode"], "response")
        self.assertEqual(decoded_response["cmif"]["header"]["result"], "0x167B")

        metadata = {
            "record": "meta",
            "schemaVersion": 1,
            "sessionId": "ipc-test",
            "profile": {
                "name": "ipc-pair",
                "pairs": [{
                    "entry": "account-entry",
                    "return": "account-return",
                }],
            },
            "targets": [
                {
                    "index": 0,
                    "id": "account-entry",
                    "process": {"pid": "000000000000006C"},
                },
                {
                    "index": 1,
                    "id": "account-return",
                    "process": {"pid": "000000000000006C"},
                },
            ],
        }
        request_event = {
            "record": "event",
            "schemaVersion": 1,
            "seq": "1",
            "tick": "100",
            "thread": "AA",
            "target": "0",
            "targetAddress": "0000000000001000",
            "snapshots": "1",
            "snap0Address": "0000000000002000",
            "snap0Data": request.hex().upper(),
        }
        response_event = {
            **request_event,
            "seq": "2",
            "tick": "200",
            "target": "1",
            "targetAddress": "0000000000001004",
            "snap0Address": "0000000000002000",
            "snap0Data": response.hex().upper(),
        }
        report = decode_trace_ipc([{
            "metadata": metadata,
            "events": [request_event, response_event],
            "path": "trace.jsonl",
        }])
        self.assertEqual(report["summary"]["ipcCount"], 2)
        self.assertEqual(report["summary"]["transactionCount"], 1)
        self.assertEqual(report["records"][0]["targetId"], "account-entry")
        self.assertEqual(report["transactions"][0]["commandId"], 8)
        self.assertEqual(report["transactions"][0]["result"], "0x167B")

        with tempfile.TemporaryDirectory() as directory:
            trace_path = os.path.join(directory, "trace.jsonl")
            with open(trace_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps(metadata) + "\n")
                handle.write(json.dumps(request_event) + "\n")
                handle.write(json.dumps(response_event) + "\n")
            report = decode_trace_ipc(load_trace_jsonl([trace_path]))
            self.assertEqual(report["records"][0]["cmif"]["header"]["commandId"], 8)
            self.assertEqual(main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "ipc-decode", request.hex()]), 0)
            self.assertEqual(main([
                "--host", "127.0.0.1", "--port",
                str(self.server.server_address[1]), "--timeout", "1",
                "debug", "trace-ipc", trace_path]), 0)

    def test_trace_analyze_and_diff(self) -> None:
        import json
        import tempfile
        from client.sysagent import (analyze_trace_documents, diff_trace_summaries,
                                     load_trace_jsonl, main)

        with tempfile.TemporaryDirectory() as directory:
            left_path = os.path.join(directory, "left.jsonl")
            right_path = os.path.join(directory, "right.jsonl")
            metadata = {
                "record": "meta", "schemaVersion": 1, "sessionId": "left",
                "status": {"pid": "0000000000000088"},
                "process": {"pid": "0000000000000088",
                            "titleId": "01006F8002326000",
                            "buildId": "AABB"},
            }
            events = [
                {"record": "event", "schemaVersion": 1, "seq": "1",
                 "tick": "100", "thread": "AA", "pc": "0000000000001000",
                 "targetAddress": "0000000000001000", "x0": "1",
                 "contextResult": "0x1"},
                {"record": "event", "schemaVersion": 1, "seq": "2",
                 "tick": "200", "thread": "AA", "pc": "0000000000002000",
                 "targetAddress": "0000000000002000", "x0": "0000000000000123"},
                {"record": "event", "schemaVersion": 1, "seq": "3",
                 "tick": "300", "thread": "AA", "pc": "0000000000001000",
                 "targetAddress": "0000000000001000", "x0": "2"},
            ]
            with open(left_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps(metadata) + "\n")
                for event in events:
                    handle.write(json.dumps(event) + "\n")
            with open(right_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps({**metadata, "sessionId": "right"}) + "\n")
                handle.write(json.dumps(events[0]) + "\n")

            report = analyze_trace_documents(
                load_trace_jsonl([left_path]), [(0x1000, 0x2000)])
            pair = report["pairs"][0]
            self.assertEqual(pair["matched"], 1)
            self.assertEqual(pair["unmatchedEntries"], 1)
            self.assertEqual(pair["calls"][0]["x0"], "0000000000000123")
            self.assertEqual(
                report["summary"]["processIdentities"]["0000000000000088"]["buildId"],
                "AABB")
            self.assertEqual(report["summary"]["snapshotErrors"], 0)

            diff = diff_trace_summaries(report, analyze_trace_documents(
                load_trace_jsonl([right_path])))
            self.assertEqual(diff["delta"]["eventCount"], -2)

            self.assertEqual(main([
                "debug", "trace-analyze", left_path,
                "--pair", "0x1000:0x2000", "--json"]), 0)
            self.assertEqual(main([
                "debug", "trace-diff", left_path, right_path, "--json"]), 0)

            legacy_path = os.path.join(directory, "legacy.jsonl")
            with open(legacy_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps({
                    "seq": "1", "tick": "1", "thread": "AA",
                    "pc": "0000000000001000",
                }) + "\n")
            legacy = load_trace_jsonl([legacy_path])[0]["metadata"]
            self.assertTrue(legacy["legacy"])

            invalid_path = os.path.join(directory, "invalid.jsonl")
            with open(invalid_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps({
                    "record": "meta", "schemaVersion": 2,
                    "sessionId": "invalid",
                }) + "\n")
            with self.assertRaisesRegex(SysAgentProtocolError, "unsupported trace schema"):
                load_trace_jsonl([invalid_path])

            multi_path = os.path.join(directory, "multi.jsonl")
            multi_meta = {
                "record": "meta", "schemaVersion": 1, "sessionId": "multi",
                "status": {},
                "targets": [
                    {"index": 0, "process": {"pid": "0000000000000071"}},
                    {"index": 1, "process": {"pid": "0000000000000085"}},
                ],
            }
            with open(multi_path, "w", encoding="utf-8") as handle:
                handle.write(json.dumps(multi_meta) + "\n")
                handle.write(json.dumps({
                    "record": "event", "schemaVersion": 1, "seq": "1",
                    "tick": "1", "thread": "AA", "target": "1",
                    "pc": "0000000000002000",
                    "targetAddress": "0000000000002000",
                }) + "\n")
            multi_report = analyze_trace_documents(load_trace_jsonl([multi_path]))
            self.assertEqual(
                multi_report["summary"]["processCounts"],
                {"0000000000000085": 1})

    def test_peek_verified(self) -> None:
        with self.client() as client:
            self.assertEqual(client.peek_absolute_verified(0x100, 4)["data"], "DEADBEEF")
            self.assertEqual(self.server.state.last_command[0], "peekAbsoluteVerified")

        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "memory", "peek-absolute-verified", "0x100", "4"])
        self.assertEqual(code, 0)
        self.assertEqual(self.server.state.last_command[0], "peekAbsoluteVerified")

    def test_peek_verified_unstable_translated(self) -> None:
        self.server.state.peek_verify_unstable = True
        with self.client() as client:
            with self.assertRaisesRegex(SysAgentProtocolError, "disagreed"):
                client.peek_absolute_verified(0x100, 4)

    def test_memory_hash(self) -> None:
        with self.client() as client:
            self.assertEqual(client.memory_hash(0x100, 4)["algo"], "fnv1a32")
            self.assertEqual(self.server.state.last_command[0], "memoryHash")

    def test_typed_memory_info(self) -> None:
        with self.client() as client:
            info = client.memory_info(0x2C1A400000)
            self.assertEqual(info.type_name, "heap")
            self.assertEqual(info.perm_name, "rw")
            self.assertEqual(info.size, 0x1000)

    def test_typed_game_status(self) -> None:
        with self.client() as client:
            status = client.game_status()
            self.assertTrue(status.running)
            self.assertEqual(status.title_id, 0x01006F8002326000)
            self.assertEqual(status.main_base, 0x2908406000)
            self.assertEqual(status.raw["running"], "1")

    def test_operation_log(self) -> None:
        with self.client() as client:
            entries = client.operation_log(count=2)
            self.assertEqual(len(entries), 2)
            self.assertEqual(entries[0]["cmd"], "getVersion")
            self.assertEqual(entries[1]["cmd"], "peekAbsolute")

        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "system", "log", "--count", "2", "--json"])
        self.assertEqual(code, 0)

    def test_memory_dump_writes_file(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            path = os.path.join(temp, "dump.bin")
            with self.client() as client:
                written = client.dump(0x100, 8, path, chunk=4)
            self.assertEqual(written, 8)
            with open(path, "rb") as handle:
                self.assertEqual(handle.read(), bytes.fromhex("DEADBEEF" * 2))

    def test_json_and_out(self) -> None:
        import json
        import tempfile
        from client.sysagent import main
        with tempfile.TemporaryDirectory() as temp:
            path = os.path.join(temp, "out.json")
            code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                         "--timeout", "1", "memory", "query", "0x2C1A400000",
                         "--json", "--out", path])
            self.assertEqual(code, 0)
            with open(path, encoding="utf-8") as handle:
                parsed = json.load(handle)
            self.assertEqual(parsed["typeName"], "heap")

    def test_image_diff(self) -> None:
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        import tempfile
        from client.sysagent import image_diff
        with tempfile.TemporaryDirectory() as temp:
            baseline = os.path.join(temp, "a.png")
            current = os.path.join(temp, "b.png")
            Image.new("RGB", (4, 4), (0, 0, 0)).save(baseline)
            changed = Image.new("RGB", (4, 4), (0, 0, 0))
            changed.putpixel((1, 1), (255, 255, 255))
            changed.save(current)
            report = image_diff(baseline, current)
            self.assertEqual(report["changed_pixels"], 1)
            self.assertEqual(report["bbox"], [1, 1, 2, 2])

    def test_read_retries_on_empty_response(self) -> None:
        self.server.state.flaky_peek_remaining = 2
        with self.client() as client:
            self.assertEqual(client.peek_absolute(0x100, 4), "DEADBEEF00")
        self.assertEqual(self.server.state.flaky_peek_remaining, 0)

    def test_write_does_not_retry(self) -> None:
        self.server.state.poke_verify_fails = True
        before = len(self.server.state.commands)
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError):
                client.poke_absolute_verified(0x100, b"\xAA\xBB")
        self.assertEqual(len(self.server.state.commands) - before, 1)

    def test_wait_for_game(self) -> None:
        with self.client() as client:
            self.assertTrue(client.wait_for_game(timeout=2, poll=0.05))
        self.server.state.application_running = False
        with self.client() as client:
            self.assertFalse(client.wait_for_game(timeout=0.2, poll=0.05))

    def test_wait_for_value(self) -> None:
        with self.client() as client:
            self.assertTrue(client.wait_for_value(
                0x100, 4, bytes.fromhex("DEADBEEF"), timeout=2, poll=0.05))
            self.assertFalse(client.wait_for_value(
                0x100, 4, bytes.fromhex("00000000"), timeout=0.2, poll=0.05))

    def test_error_code_translation(self) -> None:
        with self.assertRaisesRegex(SysAgentProtocolError, "could not be read"):
            require_ok(parse_response("ERR code=READ_FAILED result=0x10801"))
        with self.assertRaisesRegex(SysAgentProtocolError, "verification read-back failed"):
            require_ok(parse_response("ERR code=READBACK_FAILED result=0x10801"))
        with self.assertRaisesRegex(SysAgentProtocolError, "debug handle unavailable"):
            require_ok(parse_response("ERR code=WEIRD result=0x10801"))
        with self.assertRaisesRegex(SysAgentProtocolError, "active watch/trace"):
            require_ok(parse_response("ERR code=DEBUG_BUSY result=0x10201"))
        with self.assertRaisesRegex(SysAgentProtocolError, "bluetooth"):
            require_ok(parse_response("ERR code=WEIRD result=0x2F4471"))

    def test_unknown_search_commands_and_status_fields(self) -> None:
        with self.client() as client:
            session = client.begin_unknown("u32", "addressSpace", 0x20, 0x1000,
                                           alignment=4, pause=True)
            self.assertEqual(session, 0x8000000000000001)
            self.assertEqual(self.server.state.last_command[-2:], ["4", "1"])
            client.refine(session, "changed", pause=False)
            self.assertEqual(self.server.state.last_command[0], "searchRefineChanged")
            client.refine(session, "exact", "0x1234", pause=True)
            self.assertEqual(self.server.state.last_command[-1], "1")
            with self.assertRaises(ValueError):
                client.begin_unknown("f32", "heap", 0, 16)
            with self.assertRaises(ValueError):
                client.refine(session, "changed", 1)

        from client.sysagent import SearchStatus
        status = SearchStatus.from_response(parse_response(
            "OK session=9223372036854775809 state=done start=0000000000001000 "
            "end=0000000000002000 scanned=4096 total=4096 matches=1024 stored=1024 "
            "truncated=0 readErrors=0 error=0x0 type=u32 region=heap base=1000 "
            "regionOffset=0 alignment=4 backend=dmnt kind=unknown generation=2 "
            "candidates=1024 operation=unchanged diskBytes=5000 pause=1 committed=1 "
            "resumable=1 failure=NONE"
        ))
        self.assertEqual(status.kind, "unknown")
        self.assertEqual(status.generation, 2)
        self.assertTrue(status.committed)
        self.assertTrue(status.resumable)

    def test_backend_policy_change_reports_busy_during_search(self) -> None:
        with self.client() as client:
            client.start(0x80000000, 0x80001000, b"\x00")
            with self.assertRaisesRegex(SysAgentProtocolError, "BUSY"):
                client.set_backend_policy("direct")

    def test_cancel(self) -> None:
        with self.client() as client:
            client.cancel(7)
        self.assertTrue(self.server.state.cancelled)

    def test_screenshot_large_response(self) -> None:
        expected = b"\xFF\xD8" + b"\x00" * 600000 + b"\xFF\xD9"
        with self.client() as client:
            data = client.screenshot()
            self.assertEqual(data, expected)
            self.assertEqual(self.server.state.last_command, ["screenCapture"])

    def test_screenshot_cli_writes_file(self) -> None:
        import os
        import tempfile
        from client.sysagent import main

        expected = b"\xFF\xD8" + b"\x00" * 600000 + b"\xFF\xD9"
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "screen.jpg")
            code = main(["--host", "127.0.0.1",
                         "--port", str(self.server.server_address[1]),
                         "--timeout", "1", "screen", "capture", "--output", output])
            self.assertEqual(code, 0)
            with open(output, "rb") as image:
                self.assertEqual(image.read(), expected)

    def test_legacy_memory_read_and_write(self) -> None:
        with self.client() as client:
            self.assertEqual(client.peek(0x100, 4), "DEADBEEF00")
            self.assertEqual(client.peek_absolute(0x80000000, 4), "DEADBEEF00")
            self.assertEqual(client.peek_main(0x20, 4), "DEADBEEF00")
            self.assertEqual(client.peek_multi([(0x100, 4), (0x200, 2)]), "DEADBEEF00C0FFEE")
            self.assertEqual(client.peek_absolute_multi([(0x100, 4)]), "DEADBEEF00C0FFEE")
            self.assertEqual(client.peek_main_multi([(0x100, 4)]), "DEADBEEF00C0FFEE")
            self.assertEqual(client.pointer([0x10, 0x20]), "0000000080001000")
            self.assertEqual(client.pointer_all([0x10], 0x20), "0000000080002000")
            self.assertEqual(client.pointer_relative([0x10], 0x20), "0000000080002000")
            self.assertEqual(client.pointer_peek(4, [0x10], 0x20), "AABBCCDD")
            self.assertEqual(
                client.pointer_peek_multi([(4, [0x10], 0x20), (2, [0x30], 0x40)]),
                "AABBCCDDEEFF")
            self.assertEqual(
                self.server.state.last_command,
                ["pointerPeekMulti", "0x4", "0x10", "0x20", "*", "0x2", "0x30", "0x40"])
            client.poke(0x100, b"\xDE\xAD")
            client.poke_absolute(0x80000000, b"\xBE\xEF")
            client.poke_main(0x20, b"\x01\x02")
            client.pointer_poke(b"\xAA", [0x10], 0x20)
            self.assertEqual(client.freeze_count(), "03")
            self.assertEqual(self.server.state.commands[-2][0], "pointerPoke")

    def test_empty_bare_response_raises(self) -> None:
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError):
                client.peek(0x100, 0)

    def test_freeze_commands(self) -> None:
        with self.client() as client:
            client.freeze(0x45097552, b"\x00\x64")
            client.unfreeze(0x45097552)
            client.freeze_clear()
            client.freeze_pause()
            client.freeze_unpause()
            self.assertEqual(client.freeze_count(), "03")
            self.assertEqual(self.server.state.last_command, ["freezeCount"])

    def test_input_and_screen_commands(self) -> None:
        with self.client() as client:
            client.press("A")
            client.release("A")
            client.click("A")
            client.set_stick("LEFT", 0x7FFF, 0)
            client.click_cancel()
            client.detach_controller()
            client.touch([(100, 200), (300, 400)])
            client.touch_hold(100, 200, 500)
            client.touch_draw([(100, 200), (300, 400)])
            client.touch_cancel()
            client.key([11, 8])
            client.key_mod([(4, 1), (5, 2)])
            client.key_multi([224, 226])
            client.screen_off()
            client.screen_on()
            self.assertEqual(client.freeze_count(), "03")
            self.assertEqual(self.server.state.commands[-2][0], "screenOn")
            with self.assertRaises(ValueError):
                client.set_stick("UP", 0, 0)
            with self.assertRaises(ValueError):
                client.set_stick("LEFT", 0x8000, 0)

    def test_text_runs_are_one_command_per_run(self) -> None:
        """Unshifted characters share a report list; a shifted one is its own report
        (shift plus the key), because the software keyboard ignores the autopilot's
        own modifier byte -- measured on the console, which is why `keyMod` is not
        what this uses."""
        self.assertEqual(text_runs("ab%9:"),
                         [("keys", [0x04, 0x05]), ("shift", [0x22]),
                          ("keys", [0x26]), ("shift", [0x33])])
        self.assertEqual(text_runs("Az"), [("shift", [0x04]), ("keys", [0x1D])])
        self.assertEqual(text_runs(""), [])

    def test_text_run_refuses_what_the_layout_cannot_type(self) -> None:
        """Chinese needs the console's input method, a newline is Enter: both are
        refused before the first key goes out, so a half-typed field cannot be
        mistaken for a complete one."""
        with self.assertRaises(SysAgentProtocolError):
            text_runs("已安装")
        with self.assertRaises(SysAgentProtocolError):
            text_runs("a\nb")

    def test_type_text_sends_key_runs_and_shift_pairs(self) -> None:
        with self.client() as client:
            client.type_text("Az5", per_char_ms=0)
            commands = self.server.state.commands
            key_commands = [command for command in commands
                            if command[0] in {"key", "keyMulti"}]
            self.assertEqual(key_commands, [["keyMulti", "225", "4"],  # shift + A
                                            ["key", "29", "34"]])      # z, 5

    def test_type_text_raises_and_restores_key_sleep(self) -> None:
        """The console's 25 ms default drops keys in a game's chat field (ACNH 3.0.3
        was measured at 25/30 ms losing characters).  type_text raises keySleepTime
        to the text floor for the call and puts the original value back afterwards."""
        with self.client() as client:
            client.type_text("Az5", per_char_ms=0)
            commands = self.server.state.commands
            key_sleep_sets = [command for command in commands
                              if command[0] == "configure" and len(command) == 3
                              and command[1] == "keySleepTime"]
            self.assertEqual(key_sleep_sets, [["configure", "keySleepTime", "50"],
                                              ["configure", "keySleepTime", "30"]])
            self.assertEqual(self.server.state.config_settings["keySleepTime"], 30)

    def test_type_text_leaves_a_slow_key_sleep_alone(self) -> None:
        self.server.state.config_settings["keySleepTime"] = 60
        with self.client() as client:
            client.type_text("a", per_char_ms=0)
            key_sleep_sets = [command for command in self.server.state.commands
                              if command[0] == "configure" and len(command) == 3
                              and command[1] == "keySleepTime"]
            self.assertEqual(key_sleep_sets, [])

    def test_type_text_survives_a_console_without_key_sleep_setting(self) -> None:
        """Old sysmodules do not answer `configure keySleepTime`; typing still goes
        through, paced by the compiled-in fallback, and no setting is written."""
        self.server.state.config_settings = {"controllerIdleReleaseMs": 1000, "freezeRate": 3}
        with self.client() as client:
            client.type_text("a")
            commands = self.server.state.commands
            key_sleep_sets = [command for command in commands
                              if command[0] == "configure" and len(command) == 3
                              and command[1] == "keySleepTime"]
            self.assertEqual(key_sleep_sets, [])
            self.assertIn(["key", "4"], commands)

    def test_type_text_restores_key_sleep_when_a_key_command_fails(self) -> None:
        """The restore lives in `finally`: a typing error must not leave the console
        at the raised 50 ms value."""
        with self.client() as client:
            with patch.object(client, "key_multi",
                              side_effect=SysAgentProtocolError("boom")):
                with self.assertRaises(SysAgentProtocolError):
                    client.type_text("A", per_char_ms=0)
            commands = self.server.state.commands
            key_sleep_sets = [command for command in commands
                              if command[0] == "configure" and len(command) == 3
                              and command[1] == "keySleepTime"]
            self.assertEqual(key_sleep_sets, [["configure", "keySleepTime", "50"],
                                              ["configure", "keySleepTime", "30"]])
            self.assertEqual(self.server.state.config_settings["keySleepTime"], 30)

    def test_type_text_falls_back_once_when_configure_reads_are_silent(self) -> None:
        """A sysmodule with only `configure <name> <value>` never answers a read.  The
        first probe times out; the client drops the connection and uses the compiled-in
        defaults for the rest of the call instead of waiting once per setting."""
        self.server.state.silent_configure = True
        with self.client() as client:
            client.type_text("a")
            commands = self.server.state.commands
            self.assertEqual(commands.count(["configure", "keySleepTime"]), 1)
            self.assertNotIn(["configure", "pollRate"], commands)
            self.assertIn(["key", "4"], commands)

    def test_text_pacing_comes_from_the_console_settings(self) -> None:
        """keySleepTime + pollRate is what the console spends per character, so the
        client asks the console instead of hard-coding a pace: the fake answers
        30 + 20, which is neither default."""
        with self.client() as client:
            self.assertEqual(client.text_per_char_ms(), 50.0)
            self.assertEqual(self.server.state.commands[-2], ["configure", "keySleepTime"])
            self.assertEqual(self.server.state.commands[-1], ["configure", "pollRate"])

    def test_text_pacing_falls_back_without_console_settings(self) -> None:
        """A console that does not report these settings still gets paced typing
        instead of an unpaced run, using the compiled-in defaults (25 + 17)."""
        self.server.state.config_settings = {"controllerIdleReleaseMs": 1000, "freezeRate": 3}
        with self.client() as client:
            self.assertEqual(client.text_per_char_ms(), 42.0)

    def test_click_seq_done(self) -> None:
        with self.client() as client:
            result = client.click_seq("A,W100,B")
            self.assertEqual(result.done, True)
            self.assertEqual(result.tokens, 3)
            self.assertEqual(result.estimated_ms, 150)
            self.assertEqual(self.server.state.last_command, ["clickSeq", "A,W100,B"])

    def test_click_seq_repeat_syntax(self) -> None:
        """The repeat syntax goes over the wire verbatim, and the console's own
        token count tells the caller how much it expanded."""
        with self.client() as client:
            result = client.click_seq("(DDOWN,W100)*3")
            self.assertEqual(self.server.state.last_command, ["clickSeq", "(DDOWN,W100)*3"])
            self.assertEqual(result.tokens, 2)  # the fake counts written tokens
            capabilities = client.sequence_capabilities()
            self.assertEqual(capabilities["repeat"], "paren-star")
            self.assertEqual(capabilities["maxTokens"], "1024")

    def test_click_seq_repeat_needs_console_support(self) -> None:
        """A sysmodule without the expander must be reported, not fed a sequence
        it would read as unknown tokens (which send nothing and still answer done)."""
        self.server.state.repeat_support = False
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError):
                client.click_seq("(DDOWN,W100)*3")
            self.assertEqual(self.server.state.last_command, ["clickSeqCapabilities"])

    def test_click_seq_rejects_whitespace(self) -> None:
        """The sysmodule splits the command line on spaces: a sequence with one
        would arrive as several arguments and be dropped."""
        with self.client() as client:
            with self.assertRaises(ValueError):
                client.click_seq("(DDOWN, W100)*3")

    def test_click_seq_syntax_error_is_raised(self) -> None:
        self.server.state.sequence_error = True
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError) as raised:
                client.click_seq("(A*2")
            self.assertIn("BAD_SEQUENCE", str(raised.exception))

    def test_late_diagnostic_does_not_desync_next_command(self) -> None:
        """A diagnostic printed after a silent command must not become the answer."""
        self.server.state.late_diagnostic = True
        with self.client() as client:
            client.click("A")
            status = client.controller_status()
            self.assertEqual(status["attached"], "1")
            self.assertEqual(self.server.state.last_command, ["controllerStatus"])

    def test_controller_diagnostics_and_recovery(self) -> None:
        with self.client() as client:
            status = client.controller_status()
            self.assertEqual(status["attached"], "1")
            self.assertEqual(status["slot"], "0")
            self.assertEqual(status["lastStateError"], "0x0")

            dump = client.controller_dump()
            self.assertTrue(dump.startswith("OK "))
            self.assertTrue(dump.endswith("END controllerDump"))
            self.assertEqual(self.server.state.last_command, ["controllerDump"])

            kicked = client.controller_kick(2)
            self.assertTrue(kicked.endswith("END controllerKick"))
            self.assertEqual(self.server.state.last_command, ["controllerKick", "2"])

            pulled = client.controller_reconnect()
            self.assertTrue(pulled.endswith("END controllerReconnect"))
            self.assertEqual(self.server.state.last_command, ["controllerReconnect", "5000"])

            pulled_addr = client.controller_reconnect(3000, "98:41:5C:65:A6:FD")
            self.assertTrue(pulled_addr.endswith("END controllerReconnect"))
            self.assertEqual(self.server.state.last_command,
                             ["controllerReconnect", "3000", "98:41:5C:65:A6:FD"])

            paired = client.controller_paired_devices()
            self.assertTrue(paired.endswith("END controllerPairedDevices"))

            with self.assertRaises(ValueError):
                client.controller_kick(9)
            with self.assertRaises(ValueError):
                client.controller_reconnect(0)

    def test_utility_commands(self) -> None:
        with self.client() as client:
            self.assertEqual(client.get_version(), "2.7.0")
            self.assertEqual(client.get_title_id(), "01006F8002326000")
            self.assertEqual(client.get_title_version(), "0000000000100000")
            self.assertEqual(client.get_system_language(), "1")
            self.assertEqual(client.get_build_id(), "0123456789ABCDEF")
            self.assertEqual(client.get_heap_base(), "00000005ECA00000")
            self.assertEqual(client.get_main_nso_base(), "00000005370606000")
            self.assertEqual(client.is_program_running(0x01006F8002326000), "1")
            self.assertEqual(client.charge(), "85")
            self.assertEqual(client.fd_count(), "3")
            self.assertEqual(client.game("name"), "Animal Crossing")
            self.assertEqual(client.game("author"), "Nintendo")
            self.assertEqual(client.game("rating"), "7")
            self.assertEqual(client.game("version"), "1.6.0")
            self.assertEqual(client.game("icon"), (b"\xFF\xD8" + b"ICON").hex().upper())

    def test_version_prints_client_and_host(self) -> None:
        import contextlib
        import io
        from client.sysagent import CLIENT_VERSION, main

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                         "--timeout", "1", "version"])
        self.assertEqual(code, 0)
        lines = output.getvalue().splitlines()
        self.assertEqual(lines[0], f"client    {CLIENT_VERSION}")
        self.assertEqual(
            lines[1],
            f"sys-agent 2.7.0  (127.0.0.1:{self.server.server_address[1]})")

    def test_version_flag_prints_the_client_only(self) -> None:
        import contextlib
        import io
        from client.sysagent import CLIENT_VERSION, main

        output = io.StringIO()
        with self.assertRaises(SystemExit) as exit_code:
            with contextlib.redirect_stdout(output):
                main(["--version"])
        self.assertEqual(exit_code.exception.code, 0)
        self.assertEqual(output.getvalue().strip(), f"sysagent.py {CLIENT_VERSION}")

    def test_version_reports_an_unreachable_host(self) -> None:
        import contextlib
        import io
        from client.sysagent import CLIENT_VERSION, main

        output = io.StringIO()
        errors = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = main(["--host", "127.0.0.1", "--port", "1", "--timeout", "1",
                         "--retries", "0", "version"])
        self.assertEqual(code, 2)
        self.assertEqual(output.getvalue().splitlines(), [f"client    {CLIENT_VERSION}"])
        self.assertTrue(errors.getvalue().startswith("error: "), errors.getvalue())

    def test_game_icon_cli_writes_file(self) -> None:
        import os
        import tempfile
        from client.sysagent import main

        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "icon.bin")
            code = main(["--host", "127.0.0.1",
                         "--port", str(self.server.server_address[1]),
                         "--timeout", "1", "game", "icon", "--output", output])
            self.assertEqual(code, 0)
            with open(output, "rb") as image:
                self.assertEqual(image.read(), b"\xFF\xD8" + b"ICON")

    def test_game_group_commands(self) -> None:
        import contextlib
        import io
        import os
        import tempfile
        from contextlib import redirect_stdout
        from client.sysagent import main

        with self.client() as client:
            launched = client.game_launch_headless(0x01006F8002326000)
            self.assertEqual(launched["action"], "launched")
            self.assertEqual(launched["titleId"], "01006F8002326000")
            self.assertEqual(self.server.state.last_command,
                             ["gameLaunchHeadless", "0x01006F8002326000"])
            forced = client.game_launch_headless(0x01006F8002326000, storage="BuiltInUser")
            self.assertEqual(forced["storage"], "SdCard")
            self.assertEqual(self.server.state.last_command,
                             ["gameLaunchHeadless", "0x01006F8002326000", "BuiltInUser"])
            self.assertEqual(client.system_query("application")["command"],
                             "applicationStatus")
            self.assertEqual(client.system_action("terminate-application")["action"],
                             "applicationTerminate")
            with self.assertRaises(ValueError):
                client.game_launch_headless(0)
            with self.assertRaises(ValueError):
                client.game_launch_headless(0x01006F8002326000, storage="NAND")

        base = ["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                "--timeout", "1"]
        for path in (["game", "status"], ["game", "terminate"], ["game", "name"],
                     ["game", "author"], ["game", "rating"], ["game", "version"],
                     ["game", "launch-headless", "0x01006F8002326000"]):
            with io.StringIO() as output:
                with redirect_stdout(output):
                    self.assertEqual(main([*base, *path]), 0)
                self.assertNotEqual(output.getvalue(), "")
        self.server.state.application_running = False
        for path in (["game", "status"], ["game", "name"], ["game", "icon"]):
            with io.StringIO() as output:
                with contextlib.redirect_stderr(io.StringIO()) as error:
                    with redirect_stdout(output):
                        self.assertEqual(main([*base, *path]), 2)
                self.assertEqual(output.getvalue(), "")
                self.assertIn("requires a running game", error.getvalue())
            self.assertEqual(self.server.state.last_command, ["applicationStatus"])
        self.server.state.application_running = True
        self.assertEqual(main([*base, "game", "launch-headless", "01006F8002326000"]), 0)
        self.assertEqual(self.server.state.last_command,
                         ["gameLaunchHeadless", "0x01006F8002326000"])
        self.assertEqual(main([*base, "game", "launch-headless",
                               "01006F8002326000", "GameCard"]), 0)
        self.assertEqual(self.server.state.last_command,
                         ["gameLaunchHeadless", "0x01006F8002326000", "GameCard"])
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "icon.bin")
            self.assertEqual(main([*base, "game", "icon", "--output", output]), 0)
            with open(output, "rb") as image:
                self.assertEqual(image.read(), b"\xFF\xD8" + b"ICON")
        with self.assertRaises(SystemExit):
            main([*base, "game", "launch-headless", "not-a-title-id"])

    def test_game_launch_headless_failure_lists_attempts(self) -> None:
        self.server.state.launch_fails = True
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError) as ctx:
                client.game_launch_headless(0x01006F8002326000)
        message = str(ctx.exception)
        self.assertIn("stage=launchProgram", message)
        self.assertIn("result=0xDFC7D802", message)
        self.assertIn("attempts=SdCard:0xDFC7D802", message)
        self.assertIn("BuiltInUser:0xA5800A08", message)

    def test_load_titlekey_from_titlekeys(self) -> None:
        import tempfile
        from client.sysagent import load_titlekey_from_titlekeys
        real_key = "6A3DC21743110E3DD19073F33263690B"
        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as keys:
            keys.write("# comment\n")
            keys.write("00000000000000000000000000000000 = 00000000000000000000000000000000\n")
            keys.write(f"01006F8002326800000000000000000B = {real_key}\n")
            path = keys.name
        try:
            found = load_titlekey_from_titlekeys(
                path, "01006F8002326800000000000000000B")
            self.assertEqual(found.hex().upper(), real_key)
            self.assertIsNone(load_titlekey_from_titlekeys(
                path, "01006F8002326800000000000000000C"))
        finally:
            import os
            os.unlink(path)

    def test_load_titlekey_block_from_blocks(self) -> None:
        import tempfile
        from client.sysagent import load_titlekey_block_from_blocks

        with tempfile.NamedTemporaryFile("w", suffix=".blocks", delete=False) as blocks:
            blocks.write("# comment\n")
            blocks.write(
                "01006F8002326800000000000000000B = "
                "44FDFC7D7F789693C24E5AA64112658E 11\n")
            path = blocks.name
        try:
            found = load_titlekey_block_from_blocks(
                path, "01006F8002326800000000000000000B")
            self.assertEqual(found, ("44FDFC7D7F789693C24E5AA64112658E", 11))
            self.assertIsNone(load_titlekey_block_from_blocks(
                path, "01006F8002326800000000000000000C"))
        finally:
            import os
            os.unlink(path)

    def test_game_launch_headless_auto_registers_titlekeys(self) -> None:
        import contextlib
        import io
        import os
        import tempfile
        from contextlib import redirect_stdout
        from client.sysagent import main

        real_key = "6A3DC21743110E3DD19073F33263690B"
        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as keys:
            keys.write(f"01006F8002326800000000000000000B = {real_key}\n")
            titlekeys_path = keys.name
        base = ["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                "--timeout", "1"]
        try:
            with self.client() as client:
                result = client.game_launch_headless_auto(
                    0x01006F8002326000, titlekeys_path=titlekeys_path)
            self.assertEqual(result["externalKey"], "title.keys")
            self.assertEqual(result["rightsId"],
                             "01006F8002326800000000000000000B")
            self.assertEqual(self.server.state.last_key_register,
                             ["01006F8002326800000000000000000B", real_key])
            self.assertEqual(self.server.state.last_key_unregister,
                             ["01006F8002326800000000000000000B"])
            self.assertEqual(self.server.state.last_command,
                             ["gameLaunchHeadless", "0x01006F8002326000"])

            with io.StringIO() as output:
                with redirect_stdout(output):
                    self.assertEqual(main([*base, "game", "launch-headless",
                                           "0x01006F8002326000",
                                           "--titlekeys", titlekeys_path]), 0)
                self.assertIn("externalKey=title.keys", output.getvalue())
        finally:
            os.unlink(titlekeys_path)

    def test_game_launch_headless_auto_unregisters_on_failure(self) -> None:
        import os
        import tempfile
        from client.sysagent import SysAgentProtocolError

        real_key = "6A3DC21743110E3DD19073F33263690B"
        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as keys:
            keys.write(f"01006F8002326800000000000000000B = {real_key}\n")
            titlekeys_path = keys.name
        self.server.state.launch_fails = True
        try:
            with self.client() as client:
                with self.assertRaises(SysAgentProtocolError):
                    client.game_launch_headless_auto(
                        0x01006F8002326000, titlekeys_path=titlekeys_path)
            self.assertEqual(self.server.state.key_ops,
                             ["unreg", "reg", "unreg"])
        finally:
            os.unlink(titlekeys_path)

    def test_game_launch_headless_auto_uses_titlekey_block_source(self) -> None:
        import os
        import tempfile

        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as blocks:
            blocks.write(
                "# comment\n"
                "01006F8002326800000000000000000B = "
                "44FDFC7D7F789693C24E5AA64112658E 11\n")
            blocks_path = blocks.name
        try:
            with self.client() as client:
                result = client.game_launch_headless_auto(
                    0x01006F8002326000, titlekeys_path=blocks_path)
            self.assertEqual(result["externalKey"], "titlekey.block+spl")
            self.assertEqual(result["rightsId"],
                             "01006F8002326800000000000000000B")
            self.assertEqual(self.server.state.last_key_register,
                             ["01006F8002326800000000000000000B",
                              "44FDFC7D7F789693C24E5AA64112658E"])
            self.assertEqual(self.server.state.last_command,
                             ["gameLaunchHeadless", "0x01006F8002326000"])
        finally:
            os.unlink(blocks_path)

    def test_load_titlekey_block_from_blocks_plain_key_returns_none(self) -> None:
        import os
        import tempfile
        from client.sysagent import load_titlekey_block_from_blocks

        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as blocks:
            blocks.write(
                "01006F8002326800000000000000000B = "
                "6A3DC21743110E3DD19073F33263690B\n")
            path = blocks.name
        try:
            self.assertIsNone(load_titlekey_block_from_blocks(
                path, "01006F8002326800000000000000000B"))
        finally:
            os.unlink(path)

    def test_aes128_fips_vector_and_ticket_parsing(self) -> None:
        from client.sysagent import _aes128_ecb_decrypt_block, parse_common_ticket
        key = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
        ciphertext = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
        self.assertEqual(
            _aes128_ecb_decrypt_block(key, ciphertext),
            bytes.fromhex("00112233445566778899aabbccddeeff"))
        rights_id, encrypted, key_gen = parse_common_ticket(build_test_ticket())
        self.assertEqual(rights_id.hex().upper(), "01006F8002326800000000000000000B")
        self.assertEqual(encrypted.hex().upper(), "69C4E0D86A7B0430D8CDB78070B4C55A")
        self.assertEqual(key_gen, 0x0B)
        personalized = bytearray(build_test_ticket())
        personalized[0x140 + 0x141] = 1
        with self.assertRaises(SysAgentProtocolError):
            parse_common_ticket(bytes(personalized))

    def test_launch_headless_with_keys_registers_external_key(self) -> None:
        import os
        import tempfile

        ticket = build_test_ticket()
        self.server.state.ticket_hex = ticket.hex().upper()
        with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as keys_file:
            keys_file.write("titlekek_0b = 000102030405060708090a0b0c0d0e0f\n")
            keys_path = keys_file.name
        try:
            with self.client() as client:
                result = client.game_launch_headless_with_keys(
                    0x01006F8002326000, keys_path=keys_path)
            self.assertEqual(result["externalKey"], "ok")
            self.assertEqual(result["rightsId"], "01006F8002326800000000000000000B")
            self.assertEqual(
                self.server.state.last_key_register,
                ["01006F8002326800000000000000000B",
                 "00112233445566778899AABBCCDDEEFF"])
            self.assertEqual(
                self.server.state.last_command,
                ["gameLaunchHeadless", "0x01006F8002326000"])
        finally:
            os.unlink(keys_path)

    def test_launch_headless_with_keys_missing_ticket_fails(self) -> None:
        with self.client() as client:
            with self.assertRaises(SysAgentProtocolError) as ctx:
                client.game_launch_headless_with_keys(
                    0x01006F8002326000, keys_path="/nonexistent/keys")
        self.assertIn("ticketData", str(ctx.exception))

    def test_configure_and_raw(self) -> None:
        from client.sysagent import main
        with self.client() as client:
            client.configure("freezeRate", 10)
            self.assertEqual(client.raw_command("getVersion"), "2.7.0")
            self.assertEqual(self.server.state.commands[-2], ["configure", "freezeRate", "10"])
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "config", "set", "notAParam", "1"])
        self.assertEqual(code, 2)

    def test_config_get_and_list(self) -> None:
        from client.sysagent import main
        with self.client() as client:
            listing = client.config_list()
            self.assertEqual(self.server.state.last_command, ["configure"])
            self.assertEqual(listing["freezeRate"], "3")
            self.assertEqual(listing["controllerIdleReleaseMs"], "1000")

            self.assertEqual(client.config_get("controllerIdleReleaseMs"), "1000")
            self.assertEqual(self.server.state.last_command,
                             ["configure", "controllerIdleReleaseMs"])

            # The seconds name is a write alias: reading it reports the millisecond
            # setting rather than a lossy division.
            self.assertEqual(client.config_get("controllerIdleRelease"), "1000")
            self.assertEqual(self.server.state.last_command,
                             ["configure", "controllerIdleReleaseMs"])

            with self.assertRaises(SysAgentProtocolError):
                client.config_get("notAParam")

        base = ["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                "--timeout", "1"]
        self.assertEqual(main([*base, "config", "list"]), 0)
        self.assertEqual(main([*base, "config", "get", "freezeRate"]), 0)
        self.assertEqual(main([*base, "config", "get", "notAParam"]), 2)

    def test_begin_and_refine_cli(self) -> None:
        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "search", "begin", "u32", "heap", "0x0", "0x1000",
                     "--alignment", "4", "--pause"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "search", "refine", "9223372036854775809", "changed"])
        self.assertEqual(code, 0)

    def test_legacy_cli_and_raw(self) -> None:
        from client.sysagent import main
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "utility", "heap-base"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "memory", "poke", "0x100", "DEADBEEF"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "input", "click-seq", "A,W10", "--no-wait"])
        self.assertEqual(code, 0)
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "raw", "getVersion"])
        self.assertEqual(code, 0)

    def test_every_subcommand_has_help(self) -> None:
        import contextlib
        import io
        from client.sysagent import COMMANDS, CommandGroup, build_parser
        paths = []
        for item in COMMANDS:
            if isinstance(item, CommandGroup):
                for child in item.children:
                    paths.append([item.name, child.name])
            else:
                paths.append([item.name])
        for path in paths:
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    build_parser().parse_args([*path, "--help"])
            self.assertEqual(raised.exception.code, 0, " ".join(path))

    def test_missing_command_prints_full_help(self) -> None:
        import contextlib
        import io
        from client.sysagent import build_parser

        def stderr_of(argv: list[str]) -> tuple[str, int]:
            with contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit) as raised:
                    build_parser().parse_args(argv)
            return error.getvalue(), raised.exception.code  # type: ignore[union-attr]

        top, code = stderr_of([])
        self.assertEqual(code, 2)
        self.assertIn("error: the following arguments are required: COMMAND", top)
        self.assertIn("usage:", top)
        self.assertIn("commands:", top)
        self.assertIn("Launch, close, or inspect the running game", top)

        game, code = stderr_of(["game"])
        self.assertEqual(code, 2)
        self.assertIn("game: error: the following arguments are required: COMMAND", game)
        self.assertIn("usage:", game)
        self.assertIn("game [-h] COMMAND", game)
        self.assertIn("launch-headless", game)

    def test_rejects_malformed_response(self) -> None:
        with self.assertRaises(SysAgentProtocolError):
            parse_response("not-a-response")
        with self.assertRaises(SysAgentProtocolError):
            parse_response("OK duplicate=1 duplicate=2")

    def test_error_response_with_result_field(self) -> None:
        response = parse_response("ERR code=COMMAND_FAILED stage=getLastOpenedUser result=0x2F01")
        self.assertEqual(response["code"], "COMMAND_FAILED")
        self.assertEqual(response["result"], "0x2F01")
        with self.assertRaisesRegex(
            SysAgentProtocolError,
            r"sys-agent error: COMMAND_FAILED \(stage=getLastOpenedUser, result=0x2F01\)",
        ):
            require_ok(response)

    def test_page_size_validation(self) -> None:
        with self.client() as client:
            with self.assertRaises(ValueError):
                list(client.iter_results(7, page_size=257))

    def test_truncated_status_parsing(self) -> None:
        response = parse_response(
            "OK session=9 state=done start=0000000000001000 end=0000000000002000 "
            "scanned=4096 total=4096 matches=70000 stored=65536 truncated=1 "
            "readErrors=0 error=0x0"
        )
        from client.sysagent import SearchStatus

        status = SearchStatus.from_response(response)
        self.assertTrue(status.truncated)
        self.assertEqual(status.matches, 70000)
        self.assertEqual(status.stored, 65536)


if __name__ == "__main__":
    unittest.main()
