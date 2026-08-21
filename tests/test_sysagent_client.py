from __future__ import annotations

import socketserver
import threading
import unittest

from client.sysagent import SysAgentProtocolError, SysAgentClient, parse_response, require_ok


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
        self.ticket_hex: str | None = None
        self.last_key_register: list[str] | None = None
        self.last_key_unregister: list[str] | None = None
        self.key_ops: list[str] = []


class FakeHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        state: FakeState = self.server.state  # type: ignore[attr-defined]
        for raw in self.rfile:
            command = raw.decode("ascii").strip().split()
            if not command:
                continue
            state.last_command = command
            state.commands.append(command)
            if command[0] == "searchCapabilities":
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
                            "sample=1 errors=field:0x0")
            elif command[0] == "processList":
                response = f"OK total=3 offset={command[1]} count=1 processes=1:2"
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
                else:
                    response = "DEADBEEF00"
            elif command[0] in {"peekMulti", "peekAbsoluteMulti", "peekMainMulti"}:
                response = "DEADBEEF00C0FFEE"
            elif command[0] in {"poke", "pokeAbsolute", "pokeMain", "pointerPoke"}:
                response = None
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
                                 "screenOff", "screenOn", "configure"}:
                response = None
            elif command[0] == "clickSeq":
                response = "done"
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
                response = "2.6"
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
            else:
                response = "ERR code=UNKNOWN_COMMAND"
            if response is None:
                continue
            # Split the response to verify that the client handles TCP fragmentation.
            payload = (response + "\n").encode("ascii")
            midpoint = len(payload) // 2
            self.wfile.write(payload[:midpoint])
            self.wfile.flush()
            self.wfile.write(payload[midpoint:])
            self.wfile.flush()


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
            self.assertEqual(client.system_action("reboot-emummc")["action"], "systemRebootEmuMMC")
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

    def test_click_seq_done(self) -> None:
        with self.client() as client:
            line = client.click_seq("A,W100,B")
            self.assertEqual(line, "done")
            self.assertEqual(self.server.state.last_command, ["clickSeq", "A,W100,B"])

    def test_utility_commands(self) -> None:
        with self.client() as client:
            self.assertEqual(client.get_version(), "2.6")
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
            self.assertEqual(client.raw_command("getVersion"), "2.6")
            self.assertEqual(self.server.state.commands[-2], ["configure", "freezeRate", "10"])
        code = main(["--host", "127.0.0.1", "--port", str(self.server.server_address[1]),
                     "--timeout", "1", "config", "set", "notAParam", "1"])
        self.assertEqual(code, 2)

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
