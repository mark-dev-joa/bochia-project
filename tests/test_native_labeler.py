"""Native scene, mask-preserving box export and review-before-split contracts."""
import copy
import json
import threading
import time
import unittest
from urllib.request import Request, urlopen

import cv2
import numpy as np

from ball_labeler import LabelServer, export_dataset, validate_boxes
from labeler_assist import encode_mask, decode_mask
from labeler_native import native_action, native_catalog, scene_index, split_valid
import test_ball_labeler as fixtures


class NativeWorkflowTests(unittest.TestCase):
    setUp = fixtures.LabelerTests.setUp
    tearDown = fixtures.LabelerTests.tearDown
    save = fixtures.LabelerTests.save
    splits = fixtures.LabelerTests.splits

    def review_all(self):
        for scene in scene_index(self.catalog):
            native_action(self.project, "review", {"keys": [member["key"] for member in scene["members"]],
                          "status": "reviewed", "expected_revision": self.project.state["revision"]})

    def confirm(self):
        plan = native_action(self.project, "split-preview", {})
        return native_action(self.project, "split-confirm", {**plan, "expected_revision": self.project.state["revision"]})

    def test_scene_index_covers_each_frame_once_and_retains_unmatched(self):
        scenes = scene_index(self.catalog)
        keys = [member["key"] for scene in scenes for member in scene["members"]]
        self.assertEqual(len(keys), 18)
        self.assertEqual(len(set(keys)), 18)
        self.assertTrue(all(len(scene["members"]) == 2 for scene in scenes))
        self.catalog.camera(self.ids[0], "camera-1")["times"] = [1_000_000_000 + value for value in (1000, 1100, 1200)]
        scenes = scene_index(self.catalog)
        self.assertEqual(len({member["key"] for scene in scenes for member in scene["members"]}), 18)
        self.assertTrue(any(len(scene["members"]) == 1 for scene in scenes))

    def test_detection_override_keeps_mask_and_both_coco_coordinate_contracts(self):
        mask = np.zeros((6, 8), np.uint8)
        mask[1:5, 2:5] = 1
        mask[2, 3] = 0
        rle = encode_mask(mask)
        box = {"bbox": [2, 1, 3, 4], "segmentation": rle, "detection_bbox": [1, 0, 5, 6], "source": "manual_brush"}
        self.save(boxes=[box])
        saved = self.project.frame(self.ids[0], "camera-0", 0)["boxes"][0]
        np.testing.assert_array_equal(decode_mask(saved["segmentation"], 8, 6), mask)
        self.splits()
        result = export_dataset(self.catalog, self.project.state, self.root / "export-native")
        from pathlib import Path
        output = Path(result["path"])
        segmentation = json.loads((output / "annotations/instances_train.json").read_text())["annotations"][0]
        detection = json.loads((output / "annotations/instances_detection_train.json").read_text())["annotations"][0]
        self.assertEqual(segmentation["bbox"], [2, 1, 3, 4])
        self.assertEqual(segmentation["area"], 11)
        self.assertEqual(detection["bbox"], [1, 0, 5, 6])
        self.assertNotIn("segmentation", detection)
        yolo = next((output / "labels/train").glob("*.txt")).read_text().split()
        self.assertAlmostEqual(float(yolo[3]), 5 / 8)
        self.assertAlmostEqual(float(yolo[4]), 1)
        with self.assertRaises(ValueError):
            validate_boxes([{**box, "detection_bbox": [-1, 0, 5, 6]}], 8, 6)

    def test_review_all_then_confirm_and_label_edit_invalidates_confirmation(self):
        with self.assertRaises(ValueError):
            self.confirm()
        self.review_all()
        native = native_catalog(self.project)
        self.assertEqual(native["target_images"], 18)
        self.assertEqual(native["reviewed_target_images"], 18)
        confirmed = self.confirm()
        self.assertTrue(split_valid(self.project))
        fingerprint = self.project.state["native_split"]["fingerprint"]
        self.assertEqual(confirmed["fingerprint"], fingerprint)
        self.assertEqual(native_catalog(self.project)["fingerprint"], fingerprint)
        restored = type(self.project)(self.catalog, self.project_path, self.root / "exports")
        self.assertTrue(split_valid(restored))
        self.save(reviewed=False)
        self.assertFalse(split_valid(self.project))
        self.assertIsNone(native_catalog(self.project)["fingerprint"])
        with self.assertRaises(ValueError):
            native_action(self.project, "export", {"crops": False})

    def test_groups_never_cross_splits_and_insufficient_groups_rejected(self):
        self.review_all()
        plan = native_action(self.project, "split-preview", {})
        self.assertEqual(set(plan["splits"].values()), {"train", "val", "test"})
        self.assertEqual(len(plan["splits"]), 3)
        with self.assertRaises(ValueError):
            native_action(self.project, "split-preview", {"groups": dict.fromkeys(self.ids, "one-scene")})
        prior = copy.deepcopy(self.project.state)
        groups = {self.ids[0]: "same", self.ids[1]: "same", self.ids[2]: "other"}
        with self.assertRaises(ValueError):
            native_action(self.project, "split-confirm", {"groups": groups, "splits": plan["splits"], "expected_revision": prior["revision"]})
        self.assertEqual(self.project.state, prior)

    def test_confirmed_native_export_contains_all_reviewed_images_and_fixed_groups(self):
        self.save(reviewed=False)
        self.review_all()
        self.confirm()
        native_action(self.project, "export", {"crops": False})
        deadline = time.monotonic() + 5
        while self.project.job["status"] == "running" and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(self.project.job["status"], "complete", self.project.job)
        result = self.project.job["result"]
        self.assertEqual(result["images"], 18)
        self.assertEqual(result["boxes"], 1)
        from pathlib import Path
        output = Path(result["path"])
        manifest = json.loads((output / "manifest.json").read_text())
        self.assertEqual(manifest["splits"], self.project.state["splits"])
        self.assertEqual(manifest["native_split"], self.project.state["native_split"])
        for recording in self.ids:
            self.assertEqual({frame["split"] for frame in manifest["frames"] if frame["recording_path"] == self.catalog.recordings[recording]["relative_path"]}, {self.project.state["splits"][recording]})
        self.assertTrue(all((output / f"annotations/instances_detection_{split}.json").exists() for split in ("train", "val", "test")))

    def test_atomic_scene_review_hold_and_mono_color_guard(self):
        scene = scene_index(self.catalog)[0]
        keys = [member["key"] for member in scene["members"]]
        native_action(self.project, "review", {"keys": keys, "status": "hold", "expected_revision": 0})
        self.assertTrue(all(self.project.state["frames"][key]["review_status"] == "hold" for key in keys))
        prior = copy.deepcopy(self.project.state)
        with self.assertRaises(ValueError):
            native_action(self.project, "review", {"keys": [keys[0], f"{self.ids[0]}/camera-1/2"], "expected_revision": prior["revision"]})
        self.assertEqual(self.project.state, prior)
        with self.assertRaises(ValueError):
            self.save(boxes=[{"bbox": [2, 1, 3, 4], "color": "red"}])

    def test_thumbnail_is_a_small_cached_jpeg(self):
        from labeler_native import thumbnail
        first = thumbnail(self.project, self.ids[0], "camera-0", 0, 32)
        image = cv2.imdecode(np.frombuffer(first, np.uint8), cv2.IMREAD_COLOR)
        self.assertEqual(image.shape[:2], (24, 32))
        self.assertIs(thumbnail(self.project, self.ids[0], "camera-0", 0, 32), first)
        with self.assertRaises(ValueError):
            thumbnail(self.project, self.ids[0], "camera-0", 0, 4000)

    def test_catalog_reports_attention_and_recording_statistics(self):
        self.save(boxes=[{"bbox": [2, 1, 3, 4]}, {"bbox": [0, 0, 2, 2]}])
        native = native_catalog(self.project)
        stats = native["stats"][self.ids[0]]
        self.assertEqual((stats["number"], stats["images"], stats["balls"], stats["empty"], stats["small"], stats["reviewed"]), (1, 6, 2, 5, 2, 1))
        self.assertIsNone(stats["day"])
        self.assertEqual(stats["frames"], [3, 3])
        self.assertTrue(all("attention" in member for scene in native["scenes"] for member in scene["members"]))

    def test_segment_passes_the_dragged_box_as_seed(self):
        calls = []

        class Assist:
            def segment(self, image, key, points, radius=14, seed_bbox=None):
                calls.append(seed_bbox)
                return {"bbox": [1, 1, 2, 2]}

        payload = {"recording": self.ids[0], "camera": "camera-0", "index": 0,
                   "points": [{"x": 3, "y": 2, "label": 1}], "box": [1, 1, 4, 3]}
        self.project.assistance(Assist(), "segment", payload)
        self.project.assistance(Assist(), "segment", {**payload, "box": None})
        self.assertEqual(calls, [[1, 1, 4, 3], None])

    def test_export_history_lists_only_completed_exports(self):
        from labeler_native import export_history
        self.assertEqual(export_history(self.project), [])
        self.review_all()
        self.confirm()
        native_action(self.project, "export", {"crops": False})
        deadline = time.monotonic() + 5
        while self.project.job["status"] == "running" and time.monotonic() < deadline:
            time.sleep(.01)
        broken = self.project.export_root / "20990101-000000-broken"
        broken.mkdir(parents=True)
        (broken / "manifest.json").write_text("{}")
        (broken / "INCOMPLETE.txt").write_text("x")
        history = export_history(self.project)
        self.assertEqual(len(history), 1)
        self.assertEqual(history[0]["fingerprint"], self.project.state["native_split"]["fingerprint"])
        self.assertEqual(sum(item["images"] for item in history[0]["counts"].values()), 18)

    def test_native_http_catalog_and_review_use_same_project(self):
        server = LabelServer(("127.0.0.1", 0), self.project)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            with urlopen(base + "/api/native/catalog") as response:
                value = json.load(response)
            self.assertEqual(value["api_version"], 2)
            self.assertEqual(value["target_images"], 18)
            keys = [member["key"] for member in value["scenes"][0]["members"]]
            body = json.dumps({"keys": keys, "expected_revision": 0}).encode()
            with urlopen(Request(base + "/api/native/review", data=body, headers={"Content-Type": "application/json"})) as response:
                saved = json.load(response)
            self.assertEqual(saved["summary"]["reviewed"], 2)
            with urlopen(base + f"/api/native/thumb?recording={self.ids[0]}&camera=camera-0&index=0&width=32") as response:
                self.assertEqual(response.headers["Content-Type"], "image/jpeg")
                self.assertGreater(len(response.read()), 100)
            with urlopen(base + "/api/native/exports") as response:
                self.assertEqual(json.load(response), {"exports": []})
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
