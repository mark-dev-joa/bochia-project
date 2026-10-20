"""The hub command line against a live hub (design/hub/PLAN-1.md Task 2)."""
import contextlib
import io
import json
import shutil
import tempfile
import threading
import time
import unittest
import urllib.request
from pathlib import Path

from hub import cli
from hub.hub import Hub


class CliTest(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="boccia-hub-cli-"))
        self.hub = Hub(self.temp)
        self.server = self.hub.serve(["127.0.0.1"], 0)
        self.address = f"127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.hub.shutdown()
        shutil.rmtree(self.temp, ignore_errors=True)

    def run_cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(["--hub", self.address, *args])
        return code, out.getvalue()

    def app(self, method, path, body=None, token=None, raw=None):
        data = raw if raw is not None else (json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None)
        request = urllib.request.Request(f"http://{self.address}{path}", data=data, method=method)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        with urllib.request.urlopen(request, timeout=10) as response:
            payload = response.read()
            return json.loads(payload) if payload else None

    def register(self, worker="홍길동", pc="joonyeon"):
        token = self.hub.invite(worker)
        self.app("POST", "/register", {"app": "labeler", "worker": worker, "pc": pc, "version": "10-19 · 3b8a166"}, token)
        return token

    def test_status_line(self):
        code, out = self.run_cli("status")
        self.assertEqual(code, 0)
        self.assertEqual(out.strip(), f"허브 {self.address} · 붙음 0 · 전체 0 · 저장소 {self.temp}")

    def test_workers_table(self):
        self.register()
        code, out = self.run_cli("workers")
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertTrue(lines[0].startswith("이름"))
        self.assertIn("홍길동", lines[1])
        self.assertIn("joonyeon", lines[1])
        self.assertIn("붙음", lines[1])
        self.hub.workers["홍길동@joonyeon"]["last_seen"] = "2026-10-19T14:02:33.000"
        self.assertIn("떨어짐 14:02", self.run_cli("workers")[1])

    def test_send_prints_id_then_result(self):
        token = self.register()

        def worker_side():
            commands = self.app("GET", "/commands?wait=5", token=token)["commands"]
            self.app("POST", f"/commands/{commands[0]['id']}/result", {"ok": True, "text": "띄움"}, token)

        threading.Thread(target=worker_side, daemon=True).start()
        code, out = self.run_cli("send", "홍길동", "notify", "쉬세요", "지금")
        self.assertEqual(code, 0)
        first, second = out.splitlines()
        self.assertRegex(first, r"^c-[0-9a-f]{6} 보냄$")
        self.assertEqual(second, "완료 · 띄움")
        listed = self.app("GET", "/api/workers/" + urllib.parse.quote("홍길동@joonyeon") + "/commands")
        self.assertEqual((listed[0]["name"], listed[0]["value"], listed[0]["state"]), ("notify", "쉬세요 지금", "done"))

    def test_ambiguous_name_lists_workers_and_exits_2(self):
        self.register("홍길동", "joonyeon")
        self.register("홍길동", "lab-pc-02")
        code, out = self.run_cli("send", "홍길동", "ping")
        self.assertEqual(code, 2)
        self.assertEqual(out.splitlines(), ["홍길동@joonyeon", "홍길동@lab-pc-02"])
        self.assertEqual(self.run_cli("events", "없는사람")[0], 1)

    def test_events_prints_time_and_text(self):
        token = self.register()
        self.app("POST", "/events", {"events": [{"time": "2026-10-19 14:31:07.112", "text": "확인 · 촬영 03 · 프레임 0142"},
                                                {"time": "2026-10-19 14:31:09.500", "text": "저장"}]}, token)
        code, out = self.run_cli("events", "홍길동", "--limit", "1")
        self.assertEqual((code, out.splitlines()), (0, ["14:31:09  저장"]))
        self.assertEqual(self.run_cli("events", "홍길동@joonyeon")[1].splitlines()[0], "14:31:07  확인 · 촬영 03 · 프레임 0142")

    def test_invite_prints_a_token_that_registers(self):
        code, out = self.run_cli("invite", "김민수")
        self.assertEqual(code, 0)
        token = out.strip()
        self.assertEqual(len(token), 43)
        self.assertEqual(self.app("POST", "/register", {"app": "labeler", "worker": "김민수", "pc": "lab-pc-02"}, token)["id"], "김민수@lab-pc-02")

    def test_logs_prints_the_latest_log_file(self):
        token = self.register()
        self.app("POST", "/files/log/labeler-2026-10-18.log", token=token, raw="옛 줄\n".encode("utf-8"))
        time.sleep(0.02)
        self.app("POST", "/files/log/labeler-2026-10-19.log", token=token, raw="새 줄 하나\n새 줄 둘\n".encode("utf-8"))
        code, out = self.run_cli("logs", "홍길동")
        self.assertEqual((code, out), (0, "새 줄 하나\n새 줄 둘\n"))
        self.assertEqual(self.run_cli("logs", "홍길동@joonyeon")[0], 0)

    def test_stop(self):
        self.assertEqual(self.run_cli("stop"), (0, "허브 끔\n"))
        self.assertTrue(self.hub.stopped.wait(3))

    def test_unreachable_hub(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(["--hub", "127.0.0.1:1", "status"])
        self.assertEqual((code, out.getvalue().strip()), (1, "허브에 연결할 수 없음 · 127.0.0.1:1"))


if __name__ == "__main__":
    unittest.main()
