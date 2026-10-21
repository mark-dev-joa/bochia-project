"""Labeler versions the hub keeps for the launcher (design/hub/PLAN-LAUNCHER.md Task 1 and 2)."""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path

from hub.hub import Hub
from hub.release import blob, current, register, release_files, version_id

REPOSITORY = Path(__file__).resolve().parents[2]
FILES = {
    "labeler/boccia_labeler.exe": b"exe v1",
    "labeler/a.dll": b"same bytes",
    "labeler/data/x": b"labels",
    "labeler/logs/y": b"log",
    "labeler/boccia_labeler-previous.exe": b"old exe",
    "labeler/BocciaLauncher.exe": b"launcher",
    "models/boccia/m.onnx": b"model",
    "cuda/c.dll": b"same bytes",
}


def tree(root, files):
    for relative, data in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="boccia-release-"))
        self.root = self.temp / "release"
        self.storage = self.temp / "storage"
        tree(self.root, FILES)

    def tearDown(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def index(self):
        return json.loads((self.storage / "releases" / "labeler" / "index.json").read_text("utf-8"))

    def test_register_leaves_out_worker_files(self):
        paths = [f["path"] for f in register(self.storage, "labeler", self.root, "v1")["manifest"]["files"]]
        for left_out in ("labeler/data/x", "labeler/logs/y", "labeler/boccia_labeler-previous.exe", "labeler/BocciaLauncher.exe"):
            self.assertNotIn(left_out, paths)
        for kept in ("BocciaLauncher.exe", "labeler/a.dll", "models/boccia/m.onnx", "cuda/c.dll"):
            self.assertIn(kept, paths)
        self.assertEqual(paths, sorted(paths))

    def test_same_content_is_stored_once(self):
        manifest = register(self.storage, "labeler", self.root, "v1")["manifest"]
        blobs = [path for path in (self.storage / "releases" / "files").iterdir() if path.is_file()]
        self.assertEqual(len(blobs), len(manifest["files"]) - 1)   # labeler/a.dll and cuda/c.dll hold the same bytes
        for entry in manifest["files"]:
            stored = blob(self.storage, entry["sha256"])
            self.assertIsNotNone(stored)
            self.assertEqual(hashlib.sha256(stored.read_bytes()).hexdigest(), entry["sha256"])
            self.assertEqual(stored.stat().st_size, entry["size"])

    def test_register_sets_current_and_keeps_versions(self):
        register(self.storage, "labeler", self.root, "v1")
        (self.root / "labeler" / "a.dll").write_bytes(b"changed")
        register(self.storage, "labeler", self.root, "v2")
        index = self.index()
        self.assertEqual(index["current"], version_id("v2"))
        self.assertEqual([version["id"] for version in index["versions"]], [version_id("v1"), version_id("v2")])
        for name in ("v1", "v2"):
            self.assertTrue((self.storage / "releases" / "labeler" / f"{version_id(name)}.json").is_file())
        self.assertEqual(current(self.storage, "labeler")["version"], "v2")

    def test_same_version_again_replaces_it(self):
        register(self.storage, "labeler", self.root, "v1")
        register(self.storage, "labeler", self.root, "v1")
        self.assertEqual(len(self.index()["versions"]), 1)

    def test_unchanged_files_are_not_hashed_again(self):
        first = register(self.storage, "labeler", self.root, "v1")
        second = register(self.storage, "labeler", self.root, "v2")
        self.assertGreater(first["hashed"], 0)
        self.assertEqual((second["hashed"], second["added_bytes"]), (0, 0))

    def test_version_id(self):
        self.assertEqual(version_id("2026-10-21 10:12 · 2039bd5"), "2026-10-21-10-12-2039bd5")

    def test_unknown_app_is_refused(self):
        with self.assertRaises(ValueError):
            release_files(self.root, "tracker")

    def test_blob_refuses_bad_names(self):
        register(self.storage, "labeler", self.root, "v1")
        for name in ("abc", "0" * 64, "../files", "A" * 64):
            self.assertIsNone(blob(self.storage, name))

    def test_no_release_yet(self):
        self.assertIsNone(current(self.storage, "labeler"))

    def test_cli_release(self):
        result = subprocess.run([sys.executable, str(REPOSITORY / "hub" / "cli.py"), "release", "labeler", str(self.root),
                                 "--storage", str(self.storage), "--version", "v1"],
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("labeler v1 · 파일 5개", result.stdout)
        self.assertEqual(current(self.storage, "labeler")["id"], "v1")


class ReleaseServeTest(unittest.TestCase):
    """What the launcher asks the hub for: the current version's list and the files, with an invite code only."""
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="boccia-release-serve-"))
        self.root = self.temp / "release"
        tree(self.root, FILES)
        self.hub = Hub(self.temp / "storage")
        self.server = self.hub.serve(["127.0.0.1"], 0)
        self.token = self.hub.invite("점검")

    def tearDown(self):
        self.hub.shutdown()
        shutil.rmtree(self.temp, ignore_errors=True)

    def get(self, path, token=None):
        request = urllib.request.Request(f"http://127.0.0.1:{self.server.server_port}{path}")
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.status, response.read(), response.headers
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers

    def registered(self, version="v1"):
        return register(self.hub.storage, "labeler", self.root, version)["manifest"]

    def test_release_needs_a_token(self):
        self.registered()
        self.assertEqual(self.get("/releases/labeler")[0], 401)
        self.assertEqual(self.get("/releases/labeler", "nope")[0], 401)

    def test_no_register_needed(self):
        manifest = self.registered()
        status, body, _ = self.get("/releases/labeler", self.token)
        self.assertEqual(status, 200)
        answer = json.loads(body)
        self.assertEqual((answer["id"], answer["files"]), (version_id("v1"), manifest["files"]))

    def test_no_release_is_404(self):
        self.assertEqual(self.get("/releases/labeler", self.token)[0], 404)
        self.registered()
        self.assertEqual(self.get("/releases/tracker", self.token)[0], 404)

    def test_release_file_bytes(self):
        entry = next(f for f in self.registered()["files"] if f["path"] == "labeler/a.dll")
        status, body, headers = self.get(f"/releases/files/{entry['sha256']}", self.token)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).hexdigest(), entry["sha256"])
        self.assertEqual(headers["Content-Length"], str(entry["size"]))

    def test_unknown_file_is_404(self):
        self.registered()
        for name in ("0" * 64, "abc"):
            self.assertEqual(self.get(f"/releases/files/{name}", self.token)[0], 404)

    def test_big_file_streams(self):
        big = bytes(range(256)) * (24 * 1024 * 4)   # 24 MiB
        (self.root / "cuda" / "big.dll").write_bytes(big)
        entry = next(f for f in self.registered()["files"] if f["path"] == "cuda/big.dll")
        status, body, _ = self.get(f"/releases/files/{entry['sha256']}", self.token)
        self.assertEqual((status, len(body)), (200, len(big)))
        self.assertEqual(hashlib.sha256(body).hexdigest(), hashlib.sha256(big).hexdigest())


if __name__ == "__main__":
    unittest.main()
