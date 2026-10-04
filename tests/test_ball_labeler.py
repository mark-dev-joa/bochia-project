"""Data-integrity tests for the local labeling workflow; no model needed."""
import copy
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import cv2
import numpy as np
import zstandard as zstd

from ball_labeler import Catalog, ConflictError, LabelServer, Project, decode_raw, encode_png, export_dataset, main, validate_boxes
from labeler_assist import BallAssist, decode_mask, encode_mask, polygon_proposal
from labeler_auto import AutoLabelJob, transfer_mask


class LabelerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="boccia-labeler-test-")
        self.root = Path(self.temp.name)
        self.source = self.root / "recordings"
        self.original = np.arange(48, dtype=np.uint8).reshape(6, 8)
        for session in ("capture-a", "capture-b", "capture-c"):
            folder = self.source / session
            folder.mkdir(parents=True)
            (folder / "session.json").write_text(json.dumps({"complete": True, "cameras": [{"name": "Mono A"}, {"name": "Mono B"}]}))
            for camera in ("camera-0", "camera-1"):
                stream = folder / camera
                stream.mkdir()
                previous = self.original.tobytes()
                predicted = np.frombuffer(previous, np.uint8).copy()
                predicted[1:] = predicted[1:] - predicted[:-1]
                compressed = zstd.ZstdCompressor().compress(predicted.tobytes())
                infos = []
                for index in range(3):
                    infos.append({"width": 8, "height": 6, "padding_x": 0, "padding_y": 0,
                        "pixel_format": 0x01080001, "size": 48, "stored_size": len(compressed),
                        "offset": index * len(compressed), "codec": 1, "filter_stride": 1, "status": 0,
                        "host_ns": 1000 + index * 100 + (5 if camera == "camera-1" else 0)})
                (stream / "frames.raw").write_bytes(compressed * 3)
                (stream / "frames.jsonl").write_text("\n".join(json.dumps(info) for info in infos))
        self.catalog = Catalog(self.source)
        self.ids = list(self.catalog.recordings)
        self.project_path = self.root / "project.json"
        self.project = Project(self.catalog, self.project_path, self.root / "exports")

    def tearDown(self):
        self.temp.cleanup()

    def save(self, recording=0, camera="camera-0", index=0, reviewed=True, boxes=None):
        payload = {"recording": self.ids[recording], "camera": camera, "index": index, "reviewed": reviewed,
                   "boxes": boxes if boxes is not None else [{"bbox": [2, 1, 3, 4], "occluded": True}],
                   "expected_revision": self.project.state["revision"]}
        return self.project.save_frame(payload)

    def splits(self):
        return self.project.save_splits({"expected_revision": self.project.state["revision"],
            "splits": dict(zip(self.ids, ("train", "val", "test")))})

    def test_lossless_compressed_mono_and_nearest_camera(self):
        image = self.catalog.read(self.ids[0], "camera-0", 2)
        self.assertEqual(image.shape, (6, 8, 3))
        for channel in range(3):
            np.testing.assert_array_equal(image[:, :, channel], self.original)
        self.assertEqual(self.catalog.nearest(self.ids[0], "camera-1", 1198), 2)
        self.assertEqual(self.catalog.nearest(self.ids[0], "camera-1", 0), 0)
        self.assertEqual(self.catalog.nearest(self.ids[0], "camera-1", 9999), 2)

    def test_bayer_channel_order_padding_and_truncation(self):
        mosaic = np.zeros((12, 14), np.uint8)
        mosaic[:, 12:] = 99
        mosaic[0::2, 0:12:2] = 240  # red
        mosaic[0::2, 1:12:2] = 120
        mosaic[1::2, 0:12:2] = 120
        mosaic[1::2, 1:12:2] = 15  # blue
        path = self.root / "bayer.raw"
        path.write_bytes(mosaic.tobytes())
        info = {"width": 12, "height": 12, "padding_x": 2, "pixel_format": 0x01080009,
                "offset": 0, "size": 168}
        image = decode_raw(path, info)
        np.testing.assert_array_equal(image[6, 6], [15, 120, 240])
        path.write_bytes(b"short")
        with self.assertRaises(ValueError):
            decode_raw(path, info)

    def test_save_resume_and_conflict_do_not_overwrite(self):
        self.save()
        resumed = Project(self.catalog, self.project_path, self.root / "exports")
        self.assertTrue(resumed.frame(self.ids[0], "camera-0", 0)["reviewed"])
        before = self.project_path.read_bytes()
        with self.assertRaises(ConflictError):
            self.project.save_splits({"expected_revision": 0, "splits": {self.ids[0]: "train"}})
        self.assertEqual(self.project_path.read_bytes(), before)
        with patch("ball_labeler.atomic_json", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.save(index=1)
        self.assertEqual(self.project.summary()["reviewed"], 1)

    def test_invalid_coordinates_and_missing_split_rejected(self):
        for bbox in ([-1, 0, 2, 2], [7, 0, 3, 2], [0, 0, 0, 2], [float("nan"), 0, 2, 2]):
            with self.assertRaises(ValueError):
                self.save(boxes=[{"bbox": bbox}])
        self.save()
        with self.assertRaises(ValueError):
            self.project.start_export(True)

    def test_mask_persistence_export_and_bbox_agree(self):
        mask = np.zeros((6, 8), np.uint8)
        mask[1:5, 2:5] = 1
        mask[2, 3] = 0  # A hole must survive RLE, unlike a filled polygon.
        rle = encode_mask(mask)
        np.testing.assert_array_equal(decode_mask(rle, 8, 6), mask)
        self.splits()
        self.save(boxes=[{"bbox": [2, 1, 3, 4], "segmentation": rle, "source": "efficientsam", "occluded": True}])
        restored = Project(self.catalog, self.project_path, self.root / "exports").frame(self.ids[0], "camera-0", 0)["boxes"][0]
        self.assertEqual(restored["segmentation"], rle)
        self.assertTrue(restored["contours"])
        self.assertEqual(restored["source"], "efficientsam")
        output = Path(export_dataset(self.catalog, self.project.state, self.root / "exports")["path"])
        annotation = json.loads((output / "annotations/instances_train.json").read_text())["annotations"][0]
        self.assertEqual(annotation["area"], 11)
        self.assertEqual(annotation["segmentation"], rle)
        with self.assertRaises(ValueError):
            self.save(boxes=[{"bbox": [1, 1, 3, 4], "segmentation": rle}])
        for bad in ({"size": [8, 6], "counts": rle["counts"]}, {"size": [6, 8], "counts": [47]},
                    {"size": [6, 8], "counts": [48]}, {"size": [6, 8], "counts": [True, 47]}):
            with self.assertRaises(ValueError):
                decode_mask(bad, 8, 6)
        filled = np.ones((6, 8), np.uint8)
        self.assertEqual(encode_mask(filled)["counts"], [0, 48])
        np.testing.assert_array_equal(decode_mask(encode_mask(filled), 8, 6), filled)

    def test_assist_proposals_do_not_mutate_or_cross_cameras(self):
        assist = BallAssist(self.root / "missing-model")
        self.assertFalse(assist.status()["ready"])
        with self.assertRaisesRegex(ValueError, "준비"):
            assist.segment(np.zeros((80, 80, 3), np.uint8), "test", [{"x": 40, "y": 40, "label": 1}], 14)
        self.save(camera="camera-1", index=0)
        payload = {"recording": self.ids[0], "camera": "camera-0", "index": 1}
        with self.assertRaisesRegex(ValueError, "이전 프레임"):
            self.project.assistance(assist, "propagate", payload)
        self.save(camera="camera-0", index=0)
        before = copy.deepcopy(self.project.state)
        with patch.object(assist, "propagate", return_value=[{"bbox": [2, 1, 3, 4], "occluded": False}]) as propagate:
            result = self.project.assistance(assist, "propagate", payload)
        self.assertEqual(result["previous_index"], 0)
        self.assertEqual(result["key"], f"{self.ids[0]}/camera-0/1")
        self.assertEqual(self.project.state, before)

        self.assertEqual(propagate.call_args.args[2][0]["bbox"], [2., 1., 3., 4.])
        with patch.object(assist, "circles", return_value=[{"bbox": [1, 1, 2, 2], "occluded": False}]) as circles:
            result = self.project.assistance(assist, "circles", {**payload, "region": [2, 1, 5, 5]})
        self.assertEqual(circles.call_args.args[0].shape, (5, 5, 3))
        self.assertEqual(result["proposals"][0]["bbox"], [3, 2, 2, 2])
        self.assertEqual(self.project.state, before)

    def test_manual_polygon_mask_and_rejected_coordinates(self):
        proposal = polygon_proposal([[2, 1], [4, 1], [4, 4], [2, 4]], 8, 6)
        self.assertEqual(proposal["bbox"], [2, 1, 3, 4])
        self.save(boxes=[proposal])
        restored = self.project.frame(self.ids[0], "camera-0", 0)["boxes"][0]
        self.assertEqual(restored["source"], "manual_polygon")
        self.assertEqual(int(decode_mask(restored["segmentation"], 8, 6).sum()), 12)
        for points in ([[0, 0], [1, 1]], [[0, 0], [1, 1], [2, 2]], [[-1, 0], [4, 1], [4, 4]]):
            with self.assertRaises(ValueError):
                polygon_proposal(points, 8, 6)

    def test_background_writes_do_not_conflict_or_overwrite_human_edits(self):
        auto = {"recording": self.ids[0], "camera": "camera-0", "index": 1, "width": 8, "height": 6,
                "boxes": [], "reviewed": False, "automatic": {"needs_attention": True}, "human_touched": False}
        key = f"{self.ids[0]}/camera-0/1"
        self.assertEqual(self.project.commit_automatic([(key, auto)]), 1)
        self.assertEqual(self.project.state["revision"], 0)
        self.assertEqual(self.project.state["auto_revision"], 1)
        self.save(index=1)
        before = copy.deepcopy(self.project.state["frames"][key])
        self.assertTrue(before["human_touched"])
        self.assertEqual(self.project.commit_automatic([(key, auto)]), 0)
        self.assertEqual(self.project.state["frames"][key], before)
        self.assertTrue(before["reviewed"])
        self.assertTrue(before["automatic"]["needs_attention"])

    def test_complete_auto_job_resume_and_mask_tracking(self):
        self.save(index=0)
        manual = copy.deepcopy(self.project.state["frames"][f"{self.ids[0]}/camera-0/0"])
        class Seeds:
            method = "test_stereo"
            def get(self, recording, camera, index, image):
                return [{"x": 3.5, "y": 3., "radius": 1.5}], self.method
        class Assist:
            def status(self):
                return {"ready": True}
            def segment(self, image, key, points, radius, bounds):
                mask = np.zeros((6, 8), np.uint8); mask[1:5, 2:5] = 1
                return {"bbox": [2, 1, 3, 4], "occluded": False, "segmentation": encode_mask(mask), "area": 12}
        job = AutoLabelJob(self.project, self.root)
        with patch("labeler_auto.SeedCache", return_value=Seeds()):
            job.start(Assist())
            deadline = time.monotonic() + 10
            while job.status()["status"] == "running" and time.monotonic() < deadline:
                time.sleep(.02)
        result = job.status()
        self.assertEqual(result["status"], "complete", result)
        self.assertEqual(result["done"], 18)
        self.assertEqual(result["written"], 17)
        self.assertEqual(result["preserved"], 1)
        self.assertGreater(result["tracked_masks"], 0)
        self.assertEqual(self.project.state["revision"], 1)  # Manual revision is independent.
        self.assertEqual(self.project.state["frames"][f"{self.ids[0]}/camera-0/0"], manual)
        for key, item in self.project.state["frames"].items():
            if item.get("automatic"):
                self.assertFalse(item["reviewed"])
                self.assertFalse(item["human_touched"])
                validate_boxes(item["boxes"], 8, 6)
        with patch("labeler_auto.SeedCache", return_value=Seeds()), patch.object(self.catalog, "read") as read:
            job.start(Assist())
            deadline = time.monotonic() + 10
            while job.status()["status"] == "running" and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertEqual(job.status()["written"], 0)
            self.assertEqual(job.status()["preserved"], 18)
            read.assert_not_called()

    def test_export_group_splits_review_filter_and_coordinate_agreement(self):
        self.splits()
        self.save()  # train
        self.save(camera="camera-1", index=2)  # same recording, same split
        self.save(recording=1, boxes=[])  # explicitly verified negative, val
        self.save(recording=2, reviewed=False)  # draft, excluded
        result = export_dataset(self.catalog, copy.deepcopy(self.project.state), self.root / "exports")
        output = Path(result["path"])
        self.assertEqual(result["images"], 3)
        self.assertEqual(result["boxes"], 2)
        train = json.loads((output / "annotations/instances_train.json").read_text())
        val = json.loads((output / "annotations/instances_val.json").read_text())
        test = json.loads((output / "annotations/instances_test.json").read_text())
        self.assertEqual({image["recording_id"] for image in train["images"]}, {self.ids[0]})
        self.assertEqual(len(val["images"]), 1)
        self.assertEqual(val["annotations"], [])
        self.assertEqual(test["images"], [])  # no copying train into empty evaluation split
        filename = train["images"][0]["file_name"]
        image = cv2.imdecode(np.fromfile(output / "images/train" / filename, np.uint8), cv2.IMREAD_COLOR)
        np.testing.assert_array_equal(image[:, :, 0], self.original)
        yolo = (output / "labels/train" / (Path(filename).stem + ".txt")).read_text().split()
        np.testing.assert_allclose([float(v) for v in yolo[1:]], [3.5 / 8, 3 / 6, 3 / 8, 4 / 6])
        manifest = json.loads((output / "manifest.json").read_text())
        self.assertTrue(manifest["complete"])
        self.assertFalse((output / "INCOMPLETE.txt").exists())
        crop = manifest["crops"][0]
        crop_image = cv2.imdecode(np.fromfile(output / crop["file_name"], np.uint8), cv2.IMREAD_COLOR)
        np.testing.assert_array_equal(crop_image[:, :, 0], self.original[1:5, 2:5])
        self.assertEqual(crop["bbox"], train["annotations"][0]["bbox"])
        self.assertTrue(crop["occluded"])
        self.assertEqual(len(list((output / "labels/val").glob("*.txt"))), 1)
        self.assertEqual(next((output / "labels/val").glob("*.txt")).read_text(), "")

    def test_failed_export_is_marked_incomplete(self):
        self.splits(); self.save()
        with patch.object(self.catalog, "read", side_effect=OSError("source unavailable")):
            with self.assertRaises(OSError):
                export_dataset(self.catalog, self.project.state, self.root / "exports")
        output = next((self.root / "exports").iterdir())
        self.assertTrue((output / "INCOMPLETE.txt").exists())
        self.assertFalse((output / "manifest.json").exists())

    def test_source_change_rejected_and_plain_unicode_images_supported(self):
        self.save()
        raw = self.source / "capture-a/camera-0/frames.raw"
        raw.write_bytes(raw.read_bytes() + b"extra")
        with self.assertRaises(ValueError):
            Project(Catalog(self.source), self.project_path, self.root / "exports")
        photos = self.root / "사진/다른날"
        photos.mkdir(parents=True)
        (photos / "공.png").write_bytes(encode_png(self.catalog.read(self.ids[1], "camera-0", 0)))
        plain = Catalog(photos.parent)
        rec = next(iter(plain.recordings))
        np.testing.assert_array_equal(plain.read(rec, "images", 0)[:, :, 0], self.original)
        with self.assertRaises(ValueError):
            Project(plain, self.root / "photos.json", photos / "exports")

    def test_http_image_frame_save_and_external_origin_rejection(self):
        server = LabelServer(("127.0.0.1", 0), self.project)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}"
        try:
            with urlopen(url + "/api/catalog") as response:
                self.assertEqual(len(json.load(response)["recordings"]), 3)
            with urlopen(url + "/api/assist-status") as response:
                self.assertEqual(json.load(response)["model"], "EfficientSAM-Ti")
            payload = {"recording": self.ids[0], "camera": "camera-0", "index": 0, "points": [{"x": 3, "y": 3, "label": 1}]}
            with patch.object(server.assist, "segment", return_value={"bbox": [2, 1, 3, 4], "occluded": False}):
                request = Request(url + "/api/assist/segment", data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"})
                with urlopen(request) as response:
                    self.assertEqual(json.load(response)["proposals"][0]["bbox"], [2, 1, 3, 4])
            self.assertEqual(self.project.state["revision"], 0)  # Suggestions are not annotations.
            with patch("sys.argv", ["ball_labeler.py", "--source", str(self.source), "--project", str(self.project_path),
                                    "--port", str(server.server_port)]), patch("ball_labeler.webbrowser.open") as open_browser, patch("builtins.print"):
                main()
                open_browser.assert_called_once_with(url + "/")
            query = f"?recording={self.ids[0]}&camera=camera-0&index=0"
            with urlopen(url + "/api/image" + query) as response:
                np.testing.assert_array_equal(cv2.imdecode(np.frombuffer(response.read(), np.uint8), cv2.IMREAD_COLOR)[:, :, 0], self.original)
            payload = {"recording": self.ids[0], "camera": "camera-0", "index": 0, "boxes": [], "reviewed": True, "expected_revision": 0}
            request = Request(url + "/api/annotation", data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"})
            with urlopen(request) as response:
                self.assertEqual(json.load(response)["summary"]["reviewed"], 1)
            request.add_header("Origin", "https://outside.example")
            with self.assertRaises(HTTPError) as raised:
                urlopen(request)
            self.assertEqual(raised.exception.code, 403)
            self.assertEqual(self.project.state["revision"], 1)
        finally:
            server.shutdown(); server.server_close(); thread.join(timeout=2)


class AssistTests(unittest.TestCase):
    def test_tracking_mask_translation_and_scale_matches_bbox(self):
        mask = np.zeros((60, 80), np.uint8); mask[20:31, 30:41] = 1
        previous = {"seed": {"x": 35., "y": 25., "radius": 5.},
                    "box": {"segmentation": encode_mask(mask)}}
        box = transfer_mask(previous, {"x": 45., "y": 28., "radius": 5.}, (60, 80, 3))
        self.assertEqual(box["bbox"], [40, 23, 11, 11])
        self.assertEqual(box["source"], "auto_tracking")
        validate_boxes([box], 80, 60)
    def test_circle_candidates_and_moving_box_matching(self):
        image = np.zeros((180, 240, 3), np.uint8)
        cv2.circle(image, (80, 90), 14, (80, 130, 240), -1)
        cv2.circle(image, (80, 90), 14, (240, 240, 240), 2)
        cv2.line(image, (75, 85), (83, 95), (40, 40, 40), 2)
        boxes = BallAssist.circles(image, 10, 18)
        self.assertTrue(any(abs(box["bbox"][0] - 65) < 6 and abs(box["bbox"][1] - 75) < 6 for box in boxes))
        moved = cv2.warpAffine(image, np.float32([[1, 0, 12], [0, 1, -7]]), (240, 180))
        previous_box = {"bbox": [65, 75, 31, 31], "occluded": True, "segmentation": {"old": "mask"}}
        propagated = BallAssist.propagate(image, moved, [previous_box])
        self.assertEqual(len(propagated), 1)
        self.assertEqual(propagated[0]["bbox"], [77, 68, 31, 31])
        self.assertNotIn("segmentation", propagated[0])
        self.assertTrue(propagated[0]["occluded"])
        self.assertEqual(BallAssist.propagate(image, moved, [previous_box], existing=propagated), [])
        self.assertEqual(BallAssist.propagate(image, np.zeros_like(image), [previous_box]), [])

    def test_prompt_crop_rgb_conversion_cache_and_negative_click(self):
        class Encoder:
            calls = 0
            def run(self, _, inputs):
                self.calls += 1
                tensor = inputs["batched_images"]
                np.testing.assert_allclose(tensor[0, :, 0, 0], [200 / 255, 40 / 255, 10 / 255])
                return [np.zeros((1, 256, 64, 64), np.float32)]
        class Decoder:
            def run(self, _, inputs):
                h, w = inputs["orig_im_size"]
                logits = np.full((1, 1, 3, h, w), -5., np.float32)
                for index in range(3):
                    cv2.circle(logits[0, 0, index], (int(w // 2), int(h // 2)), 10, 5., -1)
                return logits, np.array([[[.9, .8, .7]]], np.float32), np.zeros(1)
        assist = BallAssist(Path("missing"))
        assist.encoder, assist.decoder = Encoder(), Decoder()
        image = np.full((140, 180, 3), [10, 40, 200], np.uint8)
        positive = {"x": 90, "y": 70, "label": 1}
        first = assist.segment(image, "same-frame", [positive], 14)
        second = assist.segment(image, "same-frame", [positive, {"x": 112, "y": 70, "label": 0}], 14)
        self.assertEqual(assist.encoder.calls, 1)
        self.assertEqual(first["bbox"], second["bbox"])
        self.assertEqual(validate_boxes([first], 180, 140)[0]["segmentation"], first["segmentation"])
        with self.assertRaisesRegex(ValueError, "분리하지"):
            assist.segment(image, "same-frame", [positive, {"x": 90, "y": 70, "label": 0}], 14)
        with self.assertRaises(ValueError):
            assist.segment(image, "same-frame", [{"x": -1, "y": 0, "label": 1}])
        with self.assertRaises(ValueError):
            assist.segment(image, "same-frame", [positive], float("nan"))


if __name__ == "__main__":
    unittest.main()
