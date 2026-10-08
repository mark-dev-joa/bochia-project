"""Selection checks protect hard examples, camera boundaries and human work."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from ball_labeler import Catalog, Project, atomic_json, export_dataset
from labeler_assist import encode_mask
from labeler_selection import SelectionFile, build_selection, compare_frames, features, match_boxes, selection_path


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        source = self.root / "images"
        source.mkdir()
        self.image = np.full((100, 120, 3), 80, np.uint8)
        cv2.circle(self.image, (50, 50), 10, (20, 20, 200), -1)
        for folder in ("a", "b"):
            (source / folder).mkdir()
            for i in range(5):
                cv2.imwrite(str(source / folder / f"{i}.png"), self.image)
        self.catalog = Catalog(source)
        self.project = Project(self.catalog, self.root / "project.json", self.root / "exports")
        mask = np.zeros((100, 120), np.uint8)
        cv2.circle(mask, (50, 50), 10, 1, -1)
        self.box = {"bbox": [40, 40, 21, 21], "segmentation": encode_mask(mask), "occluded": False}
        for rec in self.catalog.recordings:
            for i in range(5):
                self.project.state["frames"][f"{rec}/images/{i}"] = {
                    "recording": rec, "camera": "images", "index": i, "width": 120, "height": 100,
                    "boxes": [copy.deepcopy(self.box)], "reviewed": False, "human_touched": False}

    def tearDown(self):
        self.temp.cleanup()

    def test_static_selection_representatives_and_camera_isolation(self):
        before = copy.deepcopy(self.project.state)
        result = build_selection(self.catalog, before)
        self.assertEqual((result["total"], result["selected"], result["excluded"]), (10, 4, 6))
        for key, entry in result["frames"].items():
            if not entry["selected"]:
                self.assertEqual(key.rsplit("/", 1)[0], entry["representative"].rsplit("/", 1)[0])
                self.assertTrue(result["frames"][entry["representative"]]["selected"])
        self.assertEqual(self.project.state, before)
        # Periodic retention even in an entirely static sequence.
        periodic = build_selection(self.catalog, before, rules={"periodic_frames_without_timestamps": 2})
        self.assertEqual(periodic["selected"], 6)
        # Camera/recording endpoint samples always retained.
        self.assertTrue(all(result["frames"][f"{r}/images/{i}"]["selected"] for r in self.catalog.recordings for i in (0, 4)))

    def test_movement_occlusion_light_texture_and_unknown_ball_are_preserved(self):
        reference = features(self.image, [self.box])
        pairs = match_boxes([self.box], [self.box])
        self.assertTrue(compare_frames(reference, reference, pairs)[0])
        changed = self.image.copy(); changed[42:56, 46:57] = 80
        self.assertFalse(compare_frames(reference, features(changed, [self.box]), pairs)[0])
        changed = np.clip(self.image.astype(np.int16) + 15, 0, 255).astype(np.uint8)
        self.assertFalse(compare_frames(reference, features(changed, [self.box]), pairs)[0])
        # An unannotated new object/ball outside known boxes is scene motion.
        changed = self.image.copy(); cv2.circle(changed, (90, 75), 8, (200, 20, 20), -1)
        self.assertFalse(compare_frames(reference, features(changed, [self.box]), pairs)[0])
        occluded = {**self.box, "occluded": True}
        self.assertIsNone(match_boxes([self.box], [occluded]))
        # A draft box jitters by a few pixels on a ball that did not move; the pixels decide, not the box.
        jittered = {**self.box, "bbox": [46, 40, 21, 21]}
        self.assertEqual(len(match_boxes([self.box], [jittered])), 1)
        moved = {**self.box, "bbox": [50, 40, 21, 21]}
        self.assertIsNone(match_boxes([self.box], [moved]))
        self.assertIsNone(match_boxes([self.box], []))
        self.assertEqual(len(match_boxes([self.box, jittered], [jittered, self.box])), 2)

    def test_box_jitter_on_a_still_ball_is_a_duplicate_but_a_moved_ball_is_not(self):
        reference = features(self.image, [self.box])
        jittered = {**self.box, "bbox": [46, 40, 21, 21]}
        duplicate, reason, _ = compare_frames(reference, features(self.image, [jittered]), match_boxes([self.box], [jittered]))
        self.assertTrue(duplicate, reason)
        moved_image = np.full_like(self.image, 80)
        cv2.circle(moved_image, (56, 50), 10, (20, 20, 200), -1)
        moved_mask = np.zeros((100, 120), np.uint8)
        cv2.circle(moved_mask, (56, 50), 10, 1, -1)
        moved = {**jittered, "segmentation": encode_mask(moved_mask)}
        duplicate, reason, _ = compare_frames(reference, features(moved_image, [moved]), match_boxes([self.box], [moved]))
        self.assertFalse(duplicate, reason)

    def test_mask_shape_change_is_kept_even_with_identical_pixels(self):
        changed = copy.deepcopy(self.box)
        mask = np.zeros((100, 120), np.uint8); mask[40:61, 40:61] = 1
        changed["segmentation"] = encode_mask(mask)
        duplicate, reason, _ = compare_frames(features(self.image, [self.box]), features(self.image, [changed]), match_boxes([self.box], [changed]))
        self.assertFalse(duplicate)
        self.assertEqual(reason, "mask_change")

    def test_sidecar_human_override_reload_and_invalid_reference(self):
        result = build_selection(self.catalog, self.project.state)
        path = selection_path(self.project.path)
        atomic_json(path, result)
        key = next(k for k, v in result["frames"].items() if not v["selected"])
        self.assertFalse(self.project.summary()["frames"][key]["selected"])
        r, c, i = key.split("/")
        self.project.save_frame({"recording": r, "camera": c, "index": int(i), "boxes": [self.box],
                                 "reviewed": False, "expected_revision": 0})
        self.assertTrue(self.project.summary()["frames"][key]["selected"])
        self.assertEqual(self.project.summary()["selection"]["selected"], 5)
        # Sidecar installation/reload requires no annotation project mutation.
        self.assertEqual(self.project.state["revision"], 1)
        invalid = copy.deepcopy(result); invalid["source_signature"] = "different"
        atomic_json(path, invalid)
        with self.assertRaises(ValueError):
            self.project.summary()
        invalid = copy.deepcopy(result); invalid["frames"][key]["representative"] = next(k for k in result["frames"] if k.rsplit("/", 1)[0] != key.rsplit("/", 1)[0])
        atomic_json(path, invalid)
        with self.assertRaises(ValueError):
            SelectionFile(self.catalog, path).read()

    def test_export_subset_masks_and_explicit_human_inclusion(self):
        selection = build_selection(self.catalog, self.project.state)
        snapshot = copy.deepcopy(self.project.state)
        snapshot["frame_selection"] = selection
        snapshot["splits"] = {r: "train" for r in self.catalog.recordings}
        for item in snapshot["frames"].values():
            item["reviewed"] = True
        key = next(k for k, v in selection["frames"].items() if not v["selected"])
        snapshot["frames"][key]["human_touched"] = True
        result = export_dataset(self.catalog, snapshot, self.root / "exports", False)
        self.assertEqual(result["images"], selection["selected"] + 1)
        manifest = json.loads((Path(result["path"]) / "manifest.json").read_text(encoding="utf-8"))
        self.assertIn(key, [entry["key"] for entry in manifest["frames"]])
        coco = json.loads((Path(result["path"]) / "annotations/instances_train.json").read_text(encoding="utf-8"))
        self.assertTrue(all(entry["segmentation"] == self.box["segmentation"] for entry in coco["annotations"]))
        self.assertEqual(len(coco["images"]), 5)

    def test_project_export_uses_filter_and_preserves_existing_review(self):
        result = build_selection(self.catalog, self.project.state)
        atomic_json(selection_path(self.project.path), result)
        duplicate = next(k for k, v in result["frames"].items() if not v["selected"])
        self.project.state["frames"][duplicate]["reviewed"] = True
        self.assertTrue(self.project.summary()["frames"][duplicate]["selected"])
        self.assertEqual(self.project.summary()["export_frames"], 1)
        self.project.state["splits"] = {r: "train" for r in self.catalog.recordings}
        # Capture the snapshot used by the server's asynchronous exporter.
        from unittest.mock import patch
        captured = []
        with patch.object(self.project, "_export_job", side_effect=lambda snapshot, crops: captured.append(snapshot)):
            self.project.start_export(False)
            import time
            deadline = time.monotonic() + 2
            while not captured and time.monotonic() < deadline:
                time.sleep(.01)
        self.assertTrue(captured[0]["frame_selection"]["enabled"])
        self.assertTrue(captured[0]["frame_selection"]["frames"][duplicate]["selected"])


if __name__ == "__main__":
    unittest.main()
