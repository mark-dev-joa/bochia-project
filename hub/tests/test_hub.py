"""The hub server: tokens, registration, status, events, commands, files and the control API (design/hub/PLAN-1.md Task 1)."""
import json
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from hub.hub import Hub, classify

SAFE = "/?=&%@-._~:"
REPOSITORY = Path(__file__).resolve().parents[2]


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

    def fetch(self, path):
        """status, body, Content-Type and Cache-Control of a GET."""
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}{urllib.parse.quote(path, safe=SAFE)}")
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                return response.status, response.read(), response.headers.get("Content-Type", ""), response.headers.get("Cache-Control", "")
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers.get("Content-Type", ""), ""

    def restart(self, **options):
        self.hub.shutdown()
        self.hub = Hub(options.pop("storage", self.temp), **options)
        self.server = self.hub.serve(["127.0.0.1"], 0)
        self.port = self.server.server_port

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

    def test_control_api_refuses_addresses_not_allowed(self):
        self.assertTrue(self.hub.control_allowed("127.0.0.1") and self.hub.control_allowed("::1"))
        self.assertFalse(self.hub.control_allowed("100.64.0.5"))
        allowed = Hub(self.temp / "other", control_from=["100.64.0.5"])
        self.assertTrue(allowed.control_allowed("100.64.0.5") and allowed.control_allowed("127.0.0.1"))
        self.assertFalse(allowed.control_allowed("100.64.0.9"))
        self.hub.control_allowed = lambda address: False      # as if the request came from a worker PC
        self.assertEqual(self.call("GET", "/api/status")[0], 403)
        self.assertEqual(self.call("GET", "/ping")[0], 200)    # the app API is untouched

    def test_control_post_requires_json(self):
        self.assertEqual(self.call_raw("POST", "/api/invites", b'{"worker": "\xed\x99\x8d"}', None, "text/plain")[0], 415)
        self.assertEqual(self.call("POST", "/api/invites", {"worker": "홍길동"})[0], 200)

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

    # ---- stage 2: the screen (design/hub/PLAN-2.md Task 1)

    def test_classify_kinds(self):
        cases = {"장면 확인 · 촬영 03 · 프레임 0142 · 공 6개": "action", "확인 · 촬영 01 · 프레임 0003": "action", "이전 적용 켬": "action",
                 "전파 → · 반복 4장 · 초안 7장": "action", "공 수정 · 모노 A · 공 2 · 클릭 분할": "action", "되돌리기 · 모노 A": "action",
                 "오류 · 내보내기 · 디스크 공간 부족": "error", "초안 실패 · 촬영 04 · x": "error", "임포트 복사 실패 · x": "error",
                 "알림 · 원본과 저장 라벨 표시 중": "notice", "초안 다시 만들기 · 498장": "job", "시작 · D:\\x · CUDA 없음": "job", "확인한 라벨": "job"}
        self.assertEqual({text: classify(text) for text in cases}, cases)

    def test_events_carry_kind_and_worker_list_has_last_action(self):
        token = self.register()
        self.call("POST", "/events", {"events": [{"time": "2026-10-20 09:00:00.000", "text": "장면 확인 · 촬영 01 · 프레임 0001 · 공 2개"},
                                                 {"time": "2026-10-20 09:00:01.000", "text": "알림 · 확인했습니다"}]}, token)
        self.assertEqual([e["kind"] for e in self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]], ["action", "notice"])
        worker = self.call("GET", "/api/workers")[1][0]
        self.assertEqual(worker["last_action"]["text"], "장면 확인 · 촬영 01 · 프레임 0001 · 공 2개")
        self.assertEqual(worker["last_action"]["kind"], "action")
        self.assertTrue(worker["registered"])

    def test_events_fast_with_many(self):
        token = self.register()
        for chunk in range(20):
            self.call("POST", "/events", {"events": [{"time": f"2026-10-20 09:{chunk:02d}:{i // 1000:02d}.{i % 1000:03d}", "text": f"줄 {chunk}-{i}"} for i in range(1000)]}, token)
        last = self.call("GET", "/api/workers/홍길동@joonyeon/events?limit=1")[1][0]["received"]
        started = time.monotonic()
        for _ in range(20):
            self.assertEqual(self.call("GET", f"/api/workers/홍길동@joonyeon/events?since={last}&limit=0")[1], [])
        self.assertLess(time.monotonic() - started, 1.0)

    def test_cache_reloads_after_restart(self):
        token = self.register()
        lines = [{"time": "2026-10-20 09:00:00.000", "text": "장면 확인 · 촬영 01 · 프레임 0001 · 공 2개"},
                 {"time": "2026-10-20 09:00:02.000", "text": "오류 · 내보내기 · 디스크 공간 부족"}]
        self.call("POST", "/events", {"events": lines}, token)
        self.restart()
        self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, token)
        listed = self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]
        self.assertEqual([(e["text"], e["kind"]) for e in listed], [(lines[0]["text"], "action"), (lines[1]["text"], "error")])
        self.assertEqual(self.call("POST", "/events", {"events": lines}, token)[1]["stored"], 0)

    def test_static_ui_only_for_allowed(self):
        ui = self.temp / "ui"
        ui.mkdir()
        (ui / "index.html").write_text("<!doctype html><title>Boccia Hub</title>", "utf-8")
        (ui / "app.js").write_text("let x = 1;", "utf-8")
        self.restart(storage=self.temp / "store", ui=ui)
        status, body, kind, cache = self.fetch("/")
        self.assertEqual((status, kind.split(";")[0], cache), (200, "text/html", "no-store"))
        self.assertIn(b"Boccia Hub", body)
        status, body, kind, _ = self.fetch("/ui/app.js")
        self.assertEqual((status, kind.split(";")[0], body), (200, "text/javascript", b"let x = 1;"))
        self.assertEqual(self.fetch("/ui/none.js")[0], 404)
        self.assertIn(self.fetch("/ui/..%5chub.py")[0], (400, 404))
        self.hub.control_allowed = lambda address: False
        self.assertEqual(self.fetch("/")[0], 403)
        self.assertEqual(self.fetch("/ui/app.js")[0], 403)

    def test_fonts_fall_back_to_assets(self):
        font = REPOSITORY / "assets" / "fonts" / "NotoSansKR.ttf"
        status, body, kind, _ = self.fetch("/ui/fonts/NotoSansKR.ttf")
        self.assertEqual((status, kind, len(body)), (200, "font/ttf", font.stat().st_size))

    def test_shutdown_stops_the_hub(self):
        self.assertEqual(self.call("POST", "/api/shutdown", {}), (200, {"ok": True}))
        self.assertTrue(self.hub.stopped.wait(3))
        with self.assertRaises(urllib.error.URLError):
            urllib.request.urlopen(f"http://127.0.0.1:{self.port}/ping", timeout=3)

    def test_hub_log_written(self):
        self.register()
        self.assertIn("홍길동@joonyeon 접속", (self.temp / "hub.log").read_text("utf-8"))

    def test_status_has_address(self):
        self.assertIsInstance(self.call("GET", "/api/status")[1]["address"], str)

    # ---- final review fixes (design/hub/PLAN-2.md)

    def test_tailscale_runs_without_a_window(self):
        # Under pythonw every console child (tailscale.exe) would open a console window of its own.
        import hub.hub as module
        seen = []

        def fake_run(command, **options):
            seen.append(options.get("creationflags", 0))
            return subprocess.CompletedProcess(command, 0, stdout='{"Self": {"DNSName": "pc.example.ts.net."}}' if "status" in command else "100.64.0.7\n", stderr="")

        original = module.subprocess.run
        module.subprocess.run = fake_run
        try:
            self.assertEqual((module.tailscale_ip(), module.tailscale_name()), ("100.64.0.7", "pc.example.ts.net"))
        finally:
            module.subprocess.run = original
        self.assertTrue(seen and all(flags & 0x08000000 for flags in seen))   # CREATE_NO_WINDOW

    def test_rebind_adds_the_tailscale_address_later(self):
        # A hub started at login before Tailscale is up serves 127.0.0.1 only; it binds the Tailscale address once there is one.
        import hub.hub as module
        answers = {"ip": "", "name": ""}
        original = (module.tailscale_ip, module.tailscale_name)
        module.tailscale_ip, module.tailscale_name = (lambda: answers["ip"]), (lambda: answers["name"])
        try:
            module.rebind(self.hub, self.port)
            self.assertEqual((len(self.hub.servers), self.hub.public_name), (1, ""))
            answers.update(ip="127.0.0.2", name="pc.example.ts.net")
            module.rebind(self.hub, self.port)
            self.assertEqual(sorted(server.server_address[0] for server in self.hub.servers), ["127.0.0.1", "127.0.0.2"])
            self.assertEqual(self.hub.public_name, "pc.example.ts.net")
            module.rebind(self.hub, self.port)
            self.assertEqual(len(self.hub.servers), 2)
        finally:
            module.tailscale_ip, module.tailscale_name = original
        self.assertIn("127.0.0.2", (self.temp / "hub.log").read_text("utf-8"))

    def test_control_refuses_a_foreign_host_name(self):
        # DNS rebinding: a page whose name now points at 127.0.0.1 must not reach the screen or the control API.
        def get(path, host):
            request = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}")
            request.add_header("Host", host)
            try:
                with urllib.request.urlopen(request, timeout=10) as response:
                    return response.status
            except urllib.error.HTTPError as error:
                return error.code
        self.assertEqual(get("/api/status", f"attacker.example:{self.port}"), 403)
        self.assertEqual(get("/", f"attacker.example:{self.port}"), 403)
        self.assertEqual(get("/api/status", f"127.0.0.1:{self.port}"), 200)
        self.assertEqual(get("/api/status", f"localhost:{self.port}"), 200)
        self.hub.public_name = "pc.example.ts.net"
        self.assertEqual(get("/api/status", f"pc.example.ts.net:{self.port}"), 200)
        self.assertEqual(get("/ping", "attacker.example"), 200)   # the app API has its tokens


if __name__ == "__main__":
    unittest.main()
