"""The hub server: tokens, registration, status, events, commands, files and the control API (design/hub/PLAN-1.md Task 1)."""
import json
import shutil
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from hub.hub import Hub

SAFE = "/?=&%@-._~:"


class HubTest(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="boccia-hub-test-"))
        self.hub = Hub(self.temp)
        self.server = self.hub.serve(["127.0.0.1"], 0)
        self.port = self.server.server_port

    def tearDown(self):
        self.hub.shutdown()
        shutil.rmtree(self.temp, ignore_errors=True)

    def call_raw(self, method, path, data=None, token=None, content_type=None):
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}{urllib.parse.quote(path, safe=SAFE)}", data=data, method=method)
        if content_type:
            request.add_header("Content-Type", content_type)
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                raw, status, kind = response.read(), response.status, response.headers.get("Content-Type", "")
        except urllib.error.HTTPError as error:
            raw, status, kind = error.read(), error.code, error.headers.get("Content-Type", "")
        return status, (json.loads(raw) if kind.startswith("application/json") else raw)

    def call(self, method, path, body=None, token=None):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        return self.call_raw(method, path, data, token, "application/json" if data else None)

    def register(self, worker="홍길동", pc="joonyeon"):
        token = self.hub.invite(worker)
        status, _ = self.call("POST", "/register", {"app": "labeler", "worker": worker, "pc": pc, "version": "1"}, token)
        self.assertEqual(status, 200)
        return token

    def test_ping_needs_no_token(self):
        status, body = self.call("GET", "/ping")
        self.assertEqual(status, 200)
        self.assertEqual(body["hub"], "boccia")
        self.assertIn("time", body)

    def test_register_requires_token_and_matching_name(self):
        self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"})[0], 401)
        token = self.hub.invite("홍길동")
        self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "김민수", "pc": "joonyeon"}, token)[0], 403)
        status, body = self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon", "version": "1"}, token)
        self.assertEqual((status, body["id"], body["since"]), (200, "홍길동@joonyeon", ""))

    def test_same_name_two_pcs_are_two_workers(self):
        token = self.hub.invite("홍길동")
        for pc in ("joonyeon", "lab-pc-02"):
            self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": pc}, token)
        self.assertEqual(sorted(w["id"] for w in self.call("GET", "/api/workers")[1]), ["홍길동@joonyeon", "홍길동@lab-pc-02"])

    def test_events_dedupe_and_keep_received_time(self):
        token = self.register()
        one = {"time": "2026-10-19 14:31:07.112", "text": "확인 · 촬영 03 · 프레임 0142"}
        self.assertEqual(self.call("POST", "/events", {"events": [one, one]}, token)[1]["stored"], 1)
        self.assertEqual(self.call("POST", "/events", {"events": [one]}, token)[1]["stored"], 0)
        events = self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["text"], one["text"])
        self.assertIn("received", events[0])
        status, body = self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, token)
        self.assertEqual((status, body["since"]), (200, one["time"]))

    def test_events_listed_by_received_time_with_since_and_limit(self):
        token = self.register()
        first = [{"time": "2026-10-19 14:00:00.000", "text": f"줄 {i}"} for i in range(5)]
        self.call("POST", "/events", {"events": first}, token)
        time.sleep(0.01)
        later = [{"time": "2026-10-19 13:00:00.000", "text": "나중에 온 옛 줄"}]   # an older clock, received later
        self.call("POST", "/events", {"events": later}, token)
        events = self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]
        self.assertEqual([e["text"] for e in events][-1], "나중에 온 옛 줄")
        self.assertEqual(len(self.call("GET", "/api/workers/홍길동@joonyeon/events?limit=2")[1]), 2)
        since = events[2]["received"]
        self.assertEqual(len(self.call("GET", f"/api/workers/홍길동@joonyeon/events?since={since}")[1]), 3)

    def test_command_longpoll_is_delivered_once(self):
        token = self.register()
        self.assertEqual(self.call("GET", "/commands?wait=0", token=token)[1]["commands"], [])
        cid = self.call("POST", "/api/workers/홍길동@joonyeon/commands", {"name": "notify", "value": "쉬세요"})[1]["id"]
        got = self.call("GET", "/commands?wait=5", token=token)[1]["commands"]
        self.assertEqual([(c["id"], c["name"], c["value"]) for c in got], [(cid, "notify", "쉬세요")])
        self.assertEqual(self.call("GET", "/commands?wait=0", token=token)[1]["commands"], [])   # once only, even without a result
        self.assertEqual(self.call("GET", "/api/workers/홍길동@joonyeon/commands")[1][0]["state"], "sent")
        self.assertEqual(self.call("POST", f"/commands/{cid}/result", {"ok": True, "text": "띄움"}, token)[0], 204)
        listed = self.call("GET", "/api/workers/홍길동@joonyeon/commands")[1][0]
        self.assertEqual((listed["state"], listed["result"]), ("done", "띄움"))
        failed = self.call("POST", "/api/workers/홍길동@joonyeon/commands", {"name": "nope"})[1]["id"]
        self.call("GET", "/commands?wait=0", token=token)
        self.call("POST", f"/commands/{failed}/result", {"ok": False, "text": "모르는 명령"}, token)
        self.assertEqual(self.call("GET", "/api/workers/홍길동@joonyeon/commands")[1][1]["state"], "failed")

    def test_longpoll_wakes_when_a_command_arrives(self):
        token = self.register()

        def later():
            time.sleep(1.0)
            self.call("POST", "/api/workers/홍길동@joonyeon/commands", {"name": "ping"})

        threading.Thread(target=later, daemon=True).start()
        started = time.monotonic()
        got = self.call("GET", "/commands?wait=10", token=token)[1]["commands"]
        self.assertEqual([c["name"] for c in got], ["ping"])
        self.assertLess(time.monotonic() - started, 5)

    def test_file_upload_and_listing(self):
        token = self.register()
        status, body = self.call_raw("POST", "/files/log/labeler-2026-10-19.log", b"line\n", token, "application/octet-stream")
        self.assertEqual(status, 200)
        self.assertTrue(body["path"].endswith("labeler-2026-10-19.log"))
        listed = self.call("GET", "/api/workers/홍길동@joonyeon/files/log")[1]
        self.assertEqual([(f["name"], f["size"]) for f in listed], [("labeler-2026-10-19.log", 5)])
        self.assertEqual(self.call_raw("GET", "/api/workers/홍길동@joonyeon/files/log/labeler-2026-10-19.log"), (200, b"line\n"))
        self.assertEqual(self.call_raw("POST", "/files/log/..%5c..%5cevil", b"x", token)[0], 400)
        self.assertEqual(self.call_raw("POST", "/files/movie/a.bin", b"x", token)[0], 400)

    def test_file_download_returns_the_bytes(self):
        token = self.register()
        self.call_raw("POST", "/files/screen/14-31-09.png", b"\x89PNG", token, "image/png")
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}" + urllib.parse.quote("/api/workers/홍길동@joonyeon/files/screen/14-31-09.png", safe=SAFE))
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.read(), b"\x89PNG")

    def test_status_and_connected_flag(self):
        token = self.register()
        self.assertEqual(self.call("POST", "/status", {"page": "review", "scene": {"recording": "촬영 03", "index": 142}}, token)[0], 204)
        worker = self.call("GET", "/api/workers")[1][0]
        self.assertEqual((worker["worker"], worker["pc"], worker["connected"], worker["status"]["page"]), ("홍길동", "joonyeon", True, "review"))
        self.assertIn("received", worker["status"])
        self.hub.workers["홍길동@joonyeon"]["last_seen"] = "2026-10-19T10:00:00.000"
        self.assertFalse(self.call("GET", "/api/workers")[1][0]["connected"])
        summary = self.call("GET", "/api/status")[1]
        self.assertEqual((summary["workers"], summary["connected"]), (1, 0))

    def test_pc_header_picks_the_pc_when_one_token_serves_two(self):
        token = self.hub.invite("홍길동")
        for pc in ("joonyeon", "노트북"):
            self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": pc}, token)
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/status", data=b'{"page": "review"}', method="POST")
        request.add_header("Authorization", f"Bearer {token}")
        request.add_header("X-Boccia-Pc", urllib.parse.quote("joonyeon"))
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.status, 204)
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/status", data=b'{"page": "tools"}', method="POST")
        request.add_header("Authorization", f"Bearer {token}")
        request.add_header("X-Boccia-Pc", urllib.parse.quote("노트북"))
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.status, 204)
        pages = {worker["id"]: worker["status"].get("page") for worker in self.call("GET", "/api/workers")[1]}
        self.assertEqual(pages, {"홍길동@joonyeon": "review", "홍길동@노트북": "tools"})

    def test_newer_session_takes_over_the_poll(self):
        token = self.hub.invite("홍길동")
        self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon", "session": "a"}, token)

        def poll(session, wait):
            request = urllib.request.Request(f"http://127.0.0.1:{self.port}/commands?wait={wait}")
            request.add_header("Authorization", f"Bearer {token}")
            request.add_header("X-Boccia-Session", session)
            try:
                with urllib.request.urlopen(request, timeout=20) as response:
                    return response.status, json.loads(response.read())
            except urllib.error.HTTPError as error:
                return error.code, json.loads(error.read())

        stale = {}
        thread = threading.Thread(target=lambda: stale.update(result=poll("a", 10)), daemon=True)
        thread.start()
        time.sleep(0.3)
        started = time.monotonic()
        self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon", "session": "b"}, token)
        thread.join(5)
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual(stale["result"][0], 409)                          # the old poll ends at once, taking nothing
        cid = self.call("POST", "/api/workers/홍길동@joonyeon/commands", {"name": "ping"})[1]["id"]
        self.assertEqual(poll("a", 0)[0], 409)
        self.assertEqual([c["id"] for c in poll("b", 0)[1]["commands"]], [cid])
        self.assertEqual(poll("", 0)[0], 200)                              # no session header: as before

    def test_invite_again_replaces_token(self):
        old = self.hub.invite("홍길동")
        new = self.call("POST", "/api/invites", {"worker": "홍길동"})[1]["token"]
        self.assertNotEqual(old, new)
        self.assertEqual(len(new), 43)
        self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, old)[0], 401)
        self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, new)[0], 200)

    def test_state_survives_a_restart(self):
        token = self.register()
        self.call("POST", "/events", {"events": [{"time": "2026-10-19 14:31:07.112", "text": "확인"}]}, token)
        self.hub.shutdown()
        self.hub = Hub(self.temp)
        self.server = self.hub.serve(["127.0.0.1"], 0)
        self.port = self.server.server_port
        status, body = self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, token)
        self.assertEqual((status, body["since"]), (200, "2026-10-19 14:31:07.112"))
        self.assertEqual(self.call("POST", "/events", {"events": [{"time": "2026-10-19 14:31:07.112", "text": "확인"}]}, token)[1]["stored"], 0)


if __name__ == "__main__":
    unittest.main()
