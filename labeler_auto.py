"""Automatic draft annotations, with immutable human edits and resumable runs.

The existing native-pixel circle cache is authenticated against serial/id/time
in each raw frame. Stereo associations select likely balls; SAM segments them.
Similar neighboring crops may reuse a transformed mask until the next anchor.
These are pseudo labels, not ground truth or a measurement of detector accuracy.
"""
from __future__ import annotations

import copy
import json
import math
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from labeler_assist import BallAssist, decode_mask, encode_mask, overlap


def seed_bbox(seed, width, height):
    x, y, radius = seed["x"], seed["y"], seed["radius"]
    left, top = max(0, math.floor(x - radius)), max(0, math.floor(y - radius))
    right, bottom = min(width, math.ceil(x + radius)), min(height, math.ceil(y + radius))
    return [left, top, right - left, bottom - top]


class SeedCache:
    def __init__(self, catalog, root: Path):
        self.seeds = {}
        self.covered = set()
        self.method = "circle_candidates"
        by_name = {rec["name"]: rec for rec in catalog.recordings.values()}
        detections = {}
        path = root / "output/ball-detection-2026-10-02/full/detections.jsonl"
        if not path.is_file():
            return
        for line in path.open(encoding="utf-8"):
            entry = json.loads(line)
            rec = by_name.get(entry["session"])
            camera = f"camera-{entry['camera']}"
            if rec is None or camera not in rec["cameras"]:
                continue
            try:
                info = catalog.frame_info(rec["id"], camera, int(entry["frame"]))
                serial = rec["cameras"][camera].get("serial")
                if serial != entry["serial"] or int(info.get("host_ns", -1)) != int(entry["host_ns"]) or info.get("id") != entry["source_id"]:
                    continue
            except ValueError:
                continue
            key = (rec["id"], camera, int(entry["frame"]))
            detections[key] = entry["balls"]
            self.covered.add(key)
        for path in (root / "output/mono-color-2026-10-03/final-sessions").glob("*/matches.jsonl"):
            rec = by_name.get(path.parent.name)
            if rec is None:
                continue
            camera_by_serial = {cam.get("serial"): name for name, cam in rec["cameras"].items()}
            for line in path.open(encoding="utf-8"):
                entry = json.loads(line)
                if not entry.get("timing_accepted"):
                    continue
                for ball in entry["balls"]:
                    if ball.get("floor_warning"):
                        continue
                    for slot, xy in enumerate(ball["raw_pixels"]):
                        if xy is None:
                            continue
                        camera = camera_by_serial.get(entry["rig_serials"][slot])
                        if camera is None:
                            continue
                        key = (rec["id"], camera, int(entry["selected_frames"][slot]))
                        candidates = detections.get(key, [])
                        if not candidates:
                            continue
                        seed = min(candidates, key=lambda c: math.hypot(c["x"] - xy[0], c["y"] - xy[1]))
                        if math.hypot(seed["x"] - xy[0], seed["y"] - xy[1]) > .5:
                            continue
                        seeds = self.seeds.setdefault(key, [])
                        if not any(math.hypot(s["x"] - seed["x"], s["y"] - seed["y"]) < 2 for s in seeds):
                            seeds.append(copy.deepcopy(seed))
        if self.covered:
            self.method = "stereo_associated_native_circles"

    def get(self, recording, camera, index, image):
        key = (recording, camera, index)
        if key in self.covered:
            return self.seeds.get(key, []), "stereo_associated_native_circles"
        # A new input has no stereo cache; all of these still require human review.
        proposals = BallAssist.circles(image, 6, 45)[:12]
        return [{"x": b["bbox"][0] + b["bbox"][2] / 2, "y": b["bbox"][1] + b["bbox"][3] / 2,
                 "radius": max(b["bbox"][2:]) / 2} for b in proposals], "circle_candidates"


def appearance(image, seed):
    radius = seed["radius"]
    patch = cv2.getRectSubPix(image, (max(8, round(radius * 2.4)),) * 2, (float(seed["x"]), float(seed["y"])))
    return cv2.resize(patch, (40, 40), interpolation=cv2.INTER_AREA).astype(np.float32)


def similar(a, b):
    centered_a, centered_b = a - a.mean(), b - b.mean()
    denominator = np.linalg.norm(centered_a) * np.linalg.norm(centered_b)
    return denominator > 1 and float(np.sum(centered_a * centered_b) / denominator) >= .96 and float(np.mean(np.abs(a - b))) <= 6


def transfer_mask(old, seed, shape):
    previous = old["seed"]
    ratio = seed["radius"] / previous["radius"]
    mask = decode_mask(old["box"]["segmentation"], shape[1], shape[0])
    transform = np.float32([[ratio, 0, seed["x"] - ratio * previous["x"]],
                            [0, ratio, seed["y"] - ratio * previous["y"]]])
    mask = cv2.warpAffine(mask, transform, (shape[1], shape[0]), flags=cv2.INTER_NEAREST)
    if not mask.any():
        raise ValueError("추적 마스크가 원본 밖으로 이동했습니다.")
    return {"bbox": list(cv2.boundingRect(mask)), "segmentation": encode_mask(mask),
            "occluded": False, "source": "auto_tracking"}


class AutoLabelJob:
    def __init__(self, project, root: Path):
        self.project, self.root = project, root
        self.lock = threading.RLock()
        self.state = {"status": "idle"}
        self.stop_event = threading.Event()
        self.status_path = project.path.with_name(project.path.stem + "-auto-status.json")
        status_path = self.status_path if self.status_path.exists() else project.path.with_name("auto-status.json")
        if status_path.exists():
            previous = json.loads(status_path.read_text(encoding="utf-8"))
            if previous.get("project_path") == str(project.path):
                self.state = previous
                if self.state.get("status") == "running":
                    self.state.update(status="interrupted", message="중단된 작업입니다. 자동 초안을 다시 시작하면 미처리 프레임부터 이어갑니다.")

    def status(self):
        with self.lock:
            return copy.deepcopy(self.state)

    def start(self, assist):
        if not assist.status()["ready"]:
            raise ValueError("자동 분할 모델을 먼저 설치하세요.")
        with self.lock:
            if self.state.get("status") == "running":
                return self.status()
            run_id = datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
            total = sum(len(cam["frames"]) for rec in self.project.catalog.recordings.values() for cam in rec["cameras"].values())
            self.state = {"status": "running", "run_id": run_id, "project_path": str(self.project.path),
                          "source_signature": self.project.catalog.signature, "total": total, "done": 0,
                          "written": 0, "preserved": 0, "boxes": 0, "sam_masks": 0, "tracked_masks": 0,
                          "fallback_boxes": 0, "failed_frames": 0, "started_at": datetime.now(timezone.utc).isoformat()}
            self.stop_event.clear()
            threading.Thread(target=self._run, args=(assist,), daemon=True).start()
            return self.status()

    def stop(self):
        self.stop_event.set()
        return self.status()

    def persist(self):
        from ball_labeler import atomic_json
        with self.lock:
            atomic_json(self.status_path, self.state)

    def _run(self, assist):
        from ball_labeler import validate_boxes
        started = time.monotonic()
        try:
            cache = SeedCache(self.project.catalog, self.root)
            with self.lock:
                self.state["seed_method"] = cache.method
            local = threading.local()
            def camera_task(recording, rec, camera, stream):
                pending = []
                if not hasattr(local, "assist"):
                    local.assist = BallAssist(assist.model_dir) if type(assist) is BallAssist else assist
                local_assist = local.assist
                previous = []
                for index in range(len(stream["frames"])):
                    if self.stop_event.is_set():
                        break
                    key = f"{recording}/{camera}/{index}"
                    with self.project.lock:
                        preserved = key in self.project.state["frames"]
                    if preserved:
                        with self.lock:
                            self.state["preserved"] += 1
                            self.state["done"] += 1
                        continue
                    image = self.project.catalog.read(recording, camera, index)
                    height, width = image.shape[:2]
                    seeds, method = cache.get(recording, camera, index, image)
                    boxes, next_previous, notes = [], [], []
                    for seed in seeds:
                        bounds = seed_bbox(seed, width, height)
                        patch = appearance(image, seed)
                        prior = min(previous, key=lambda p: math.hypot(p["seed"]["x"] - seed["x"], p["seed"]["y"] - seed["y"]), default=None)
                        box = None
                        if prior and index - prior["anchor"] < 24 and abs(seed["radius"] / prior["seed"]["radius"] - 1) < .15 and math.hypot(prior["seed"]["x"] - seed["x"], prior["seed"]["y"] - seed["y"]) < max(20, seed["radius"] * 2) and similar(patch, prior["patch"]):
                            box = transfer_mask(prior, seed, image.shape)
                            anchor = prior["anchor"]
                            with self.lock:
                                self.state["tracked_masks"] += 1
                        if box is None:
                            try:
                                box = local_assist.segment(image, key, [{"x": seed["x"], "y": seed["y"], "label": 1}], seed["radius"], bounds)
                                if overlap(box["bbox"], bounds) < .4 or box["area"] < math.pi * seed["radius"] ** 2 * .25:
                                    raise ValueError("분할 영역이 시작 후보와 다릅니다.")
                                box["source"] = "auto_sam"
                                anchor = index
                                with self.lock:
                                    self.state["sam_masks"] += 1
                            except ValueError as exc:
                                box = {"bbox": bounds, "occluded": False, "source": "auto_geometry"}
                                notes.append(str(exc))
                                with self.lock:
                                    self.state["fallback_boxes"] += 1
                        if any(overlap(box["bbox"], b["bbox"]) > .6 for b in boxes):
                            continue
                        boxes.append(box)
                        if "segmentation" in box:
                            next_previous.append({"box": box, "seed": seed, "patch": patch, "anchor": anchor})
                    previous = next_previous
                    boxes = validate_boxes(boxes, width, height)
                    pending.append((key, {"recording": recording, "camera": camera, "index": index,
                        "width": width, "height": height, "boxes": boxes, "reviewed": False, "human_touched": False,
                        "automatic": {"run_id": self.state["run_id"], "seed_method": method, "seed_count": len(seeds),
                                      "needs_attention": bool(notes) or not boxes or method == "circle_candidates",
                                      "notes": notes[:10]}}))
                    with self.lock:
                        self.state["done"] += 1
                        self.state["boxes"] += len(boxes)
                        self.state["current"] = f"{rec['name']} / {camera} / {index}"
                        self.state["elapsed_seconds"] = round(time.monotonic() - started, 1)
                    if len(pending) >= 32:
                        written = self.project.commit_automatic(pending)
                        pending = []
                        with self.lock:
                            self.state["written"] += written
                        self.persist()
                if pending:
                    written = self.project.commit_automatic(pending)
                    with self.lock:
                        self.state["written"] += written
                    self.persist()
            workers = min(3, max(1, (os.cpu_count() or 4) // 4))
            with self.lock:
                self.state["workers"] = workers
            with ThreadPoolExecutor(max_workers=workers) as executor:
                futures = [executor.submit(camera_task, recording, rec, camera, stream)
                           for recording, rec in self.project.catalog.recordings.items()
                           for camera, stream in rec["cameras"].items()]
                for future in as_completed(futures):
                    try:
                        future.result()
                    except Exception:
                        self.stop_event.set()
                        raise
            with self.lock:
                self.state.update(status="stopped" if self.stop_event.is_set() else "complete",
                                  elapsed_seconds=round(time.monotonic() - started, 1))
        except Exception as exc:
            with self.lock:
                self.state.update(status="error", error=str(exc))
        self.persist()
