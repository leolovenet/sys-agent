from __future__ import annotations

import socketserver
import threading
import unittest

from client.sysbot_search import SysBotProtocolError, SysBotSearchClient, parse_response


class FakeState:
    def __init__(self) -> None:
        self.status_calls = 0
        self.cancelled = False
        self.search_active = False
        self.last_command: list[str] = []
        self.addresses = [0x80000010, 0x80000120, 0x80000230]


class FakeHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        state: FakeState = self.server.state  # type: ignore[attr-defined]
        for raw in self.rfile:
            command = raw.decode("ascii").strip().split()
            if not command:
                continue
            state.last_command = command
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
            else:
                response = "ERR code=UNKNOWN_COMMAND"
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

    def client(self) -> SysBotSearchClient:
        return SysBotSearchClient("127.0.0.1", self.server.server_address[1], timeout=1)

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

        from client.sysbot_search import SearchStatus
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
            with self.assertRaisesRegex(SysBotProtocolError, "BUSY"):
                client.set_backend_policy("direct")

    def test_cancel(self) -> None:
        with self.client() as client:
            client.cancel(7)
        self.assertTrue(self.server.state.cancelled)

    def test_rejects_malformed_response(self) -> None:
        with self.assertRaises(SysBotProtocolError):
            parse_response("not-a-response")
        with self.assertRaises(SysBotProtocolError):
            parse_response("OK duplicate=1 duplicate=2")

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
        from client.sysbot_search import SearchStatus

        status = SearchStatus.from_response(response)
        self.assertTrue(status.truncated)
        self.assertEqual(status.matches, 70000)
        self.assertEqual(status.stored, 65536)


if __name__ == "__main__":
    unittest.main()
