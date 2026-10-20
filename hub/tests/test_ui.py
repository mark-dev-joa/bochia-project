"""The hub screen (hub/ui) against a seeded hub, read back with headless Edge (design/hub/PLAN-2.md Task 4)."""
import json
import shutil
import tempfile
import unittest
import urllib.parse
import urllib.request
from pathlib import Path

from hub.hub import Hub
from hub.tests.edge import EDGE, HeadlessEdge

WORKER = urllib.parse.quote("홍길동@joonyeon")
STATUS = {"page": "review", "text": "검수 · 촬영 03 · 프레임 0142", "device": "GPU", "apply_previous": True,
          "scene": {"recording": "촬영 03", "index": 142}, "job": None, "error": "",
          "progress": {"target": 3869, "reviewed": 1214, "hold": 12, "propagated": 1302, "today": 412, "last10": 12,
                       "recordings": [{"name": "촬영 01", "reviewed": 612, "target": 612}, {"name": "촬영 02", "reviewed": 4, "target": 604}],
                       "days": [{"date": f"10-{day}", "count": count} for day, count in zip(range(14, 21), (0, 388, 520, 0, 410, 384, 412))]}}


@unittest.skipIf(EDGE is None, "Microsoft Edge is not installed")
class ScreenTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = Path(tempfile.mkdtemp(prefix="boccia-hub-ui-"))
        cls.edge = HeadlessEdge()
        cls.hub = Hub(cls.temp / "store")
        cls.port = cls.hub.serve(["127.0.0.1"], 0).server_port
        cls.token = cls.hub.invite("홍길동")
        cls.app("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon", "version": "2026-10-20 09:12 · 8bab00d",
                                      "device": {"gpu": True, "cuda": True, "name": "NVIDIA GeForce RTX 4090 Laptop GPU"},
                                      "summary": {"project": "경기장 테스트"}}, cls.token)
        cls.app("POST", "/status", STATUS, cls.token)
        cls.app("POST", "/events", {"events": [{"time": "2026-10-20 14:30:00.000", "text": "알림 · 확인했습니다"},
                                               {"time": "2026-10-20 14:30:30.000", "text": "오류 · 내보내기 · 디스크 공간 부족"},
                                               {"time": "2026-10-20 14:31:07.112", "text": "장면 확인 · 촬영 03 · 프레임 0142 · 공 6개"}]}, cls.token)

    @classmethod
    def tearDownClass(cls):
        cls.edge.close()
        cls.hub.shutdown()
        shutil.rmtree(cls.temp, ignore_errors=True)

    @classmethod
    def app(cls, method, path, body, token):
        request = urllib.request.Request(f"http://127.0.0.1:{cls.port}{path}", data=json.dumps(body, ensure_ascii=False).encode("utf-8"), method=method)
        request.add_header("Content-Type", "application/json")
        request.add_header("Authorization", f"Bearer {token}")
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.read()

    def dump(self, route, wait_for):
        """The text the screen shows at route, once it shows wait_for (8 s at most). The worker reports again first, so it
        counts as connected (a report within 15 s)."""
        self.app("POST", "/status", STATUS, self.token)
        return self.edge.text(f"http://127.0.0.1:{self.port}/{route}", wait_for=wait_for, timeout=8)

    def test_list_shows_workers_from_the_hub(self):
        dom = self.dump("#/workers", "홍길동")
        for text in ("Boccia Hub", "홍길동", "joonyeon", "경기장 테스트", "1,214 / 3,869", "412", "12장 / 10분", "촬영 03 · 프레임 0142"):
            self.assertIn(text, dom)

    def test_list_takes_the_project_from_the_status(self):
        # A labeler registers when it starts, before a project is open: the project comes with its status.
        token = self.hub.invite("김민수")
        self.app("POST", "/register", {"app": "labeler", "worker": "김민수", "pc": "lab-pc-02", "version": "2026-10-20 09:12 · 8bab00d",
                                      "device": {"gpu": False}, "summary": {}}, token)
        self.app("POST", "/status", {"page": "review", "project": "사진 10-15", "progress": {"target": 120, "reviewed": 97}}, token)
        dom = self.dump("#/workers", "김민수")
        self.assertIn("사진 10-15", dom)

    def test_activity_lists_events_and_hides_notices(self):
        dom = self.dump(f"#/workers/{WORKER}/activity", "장면 확인")
        self.assertIn("장면 확인", dom)
        self.assertIn("디스크 공간 부족", dom)
        self.assertIn("RTX 4090", dom)
        self.assertNotIn("확인했습니다", dom)

    def test_progress_shows_recordings_and_days(self):
        dom = self.dump(f"#/workers/{WORKER}/progress", "촬영 01")
        for text in ("촬영 01", "612 / 612", "4 / 604", "412", "1,302", "520"):
            self.assertIn(text, dom)

    def test_settings_has_shutdown_and_storage(self):
        dom = self.dump("#/settings", str(self.temp / "store"))
        self.assertIn("허브 끄기", dom)
        self.assertIn(str(self.temp / "store"), dom)


if __name__ == "__main__":
    unittest.main()
