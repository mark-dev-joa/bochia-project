"""Local boccia annotation server. Run: python ball_labeler.py --no-browser.

Reads the original lossless camera recordings without modifying them. Annotation
projects are atomically saved separately; reviewed frames export to COCO and YOLO.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import socket
import threading
import uuid
import webbrowser
from bisect import bisect_left
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
from urllib.error import URLError
from urllib.request import Request, urlopen

import cv2
import numpy as np
import zstandard as zstd
from labeler_assist import BallAssist, decode_mask, encode_mask, mask_contours, overlap, polygon_proposal
from labeler_auto import AutoLabelJob
from labeler_selection import SelectionFile, selection_path

ROOT = Path(__file__).resolve().parent
SPLITS = ("train", "val", "test")
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as handle:
            # Large mask projects contain many small RLE integers. One buffered
            # write avoids hundreds of thousands of text writes per checkpoint.
            handle.write(json.dumps(value, ensure_ascii=False, indent=2 if len(value.get("frames", {})) < 128 else None, allow_nan=False))
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def encode_png(image: np.ndarray) -> bytes:
    ok, encoded = cv2.imencode(".png", image)
    if not ok:
        raise ValueError("이미지를 PNG로 저장하지 못했습니다.")
    return encoded.tobytes()


def decode_raw(raw_path: Path, info: dict) -> np.ndarray:
    width, height = int(info["width"]), int(info["height"])
    if not (0 < width <= 20000 and 0 < height <= 20000) or info.get("status", 0) != 0:
        raise ValueError("유효하지 않은 원본 프레임입니다.")
    codec, predictor = int(info.get("codec", 0)), int(info.get("filter_stride", 0))
    size, stored_size = int(info["size"]), int(info.get("stored_size", info["size"]))
    if codec not in (0, 1) or not 0 <= predictor <= 8 or size <= 0 or stored_size <= 0:
        raise ValueError("지원하지 않는 원본 압축 형식입니다.")
    if codec == 0 and (predictor or stored_size != size):
        raise ValueError("압축하지 않은 원본의 크기가 맞지 않습니다.")
    with raw_path.open("rb") as handle:
        handle.seek(int(info["offset"]))
        blob = handle.read(stored_size)
    if len(blob) != stored_size:
        raise ValueError("원본 파일이 잘렸습니다.")
    if codec == 1:
        blob = zstd.ZstdDecompressor().decompress(blob, max_output_size=size)
    if len(blob) != size:
        raise ValueError("복원한 원본의 크기가 맞지 않습니다.")
    pixels = np.frombuffer(blob, dtype=np.uint8).copy()
    if predictor:
        for channel in range(predictor):
            pixels[channel::predictor] = np.cumsum(pixels[channel::predictor], dtype=np.uint64).astype(np.uint8)
    pixel_format = int(info["pixel_format"])
    channels = 3 if pixel_format in (0x02180014, 0x02180015) else 1
    row_size = width * channels + int(info.get("padding_x", 0))
    if row_size < width * channels or len(pixels) < row_size * height:
        raise ValueError("원본 영상의 행 크기가 맞지 않습니다.")
    image = pixels[:row_size * height].reshape(height, row_size)[:, :width * channels]
    if channels == 3:
        image = image.reshape(height, width, 3)
        if pixel_format == 0x02180014:  # RGB8 -> BGR
            image = cv2.cvtColor(image, cv2.COLOR_RGB2BGR)
    elif pixel_format == 0x01080001:  # Mono8: export three identical channels
        image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    elif pixel_format == 0x01080009:  # Physical RGGB mosaic (BayerRG8)
        image = cv2.cvtColor(image, cv2.COLOR_BayerRGGB2BGR)
    else:
        raise ValueError(f"지원하지 않는 픽셀 형식: {pixel_format}")
    return np.ascontiguousarray(image)


class Catalog:
    def __init__(self, source: Path):
        self.source = source.resolve()
        if not self.source.is_dir():
            raise ValueError(f"촬영 폴더가 없습니다: {self.source}")
        self.recordings: dict[str, dict] = {}
        signature = hashlib.sha256(str(self.source).encode())
        sessions = sorted(self.source.rglob("session.json"))
        for session_path in sessions:
            session = json.loads(session_path.read_text(encoding="utf-8-sig"))
            folder = session_path.parent
            relative = folder.relative_to(self.source).as_posix() or "."
            recording_id = hashlib.sha256(relative.encode()).hexdigest()[:16]
            cameras = {}
            for index_path in sorted(folder.glob("camera-*/frames.jsonl")):
                raw_path = index_path.with_name("frames.raw")
                if not raw_path.is_file():
                    continue
                index_bytes = index_path.read_bytes()
                frames = [json.loads(line) for line in index_bytes.decode("utf-8-sig").splitlines() if line.strip()]
                if not frames:
                    continue
                camera_id = index_path.parent.name
                camera_number = int(camera_id.split("-")[-1])
                descriptions = session.get("cameras", [])
                description = descriptions[camera_number] if camera_number < len(descriptions) else {}
                cameras[camera_id] = {
                    "id": camera_id, "name": description.get("name", camera_id),
                    "serial": description.get("serial"),
                    "kind": "모노" if frames[0]["pixel_format"] == 0x01080001 else "컬러",
                    "frames": frames, "raw_path": raw_path,
                    "times": [int(frame.get("host_ns", i)) for i, frame in enumerate(frames)],
                }
                signature.update(relative.encode() + camera_id.encode() + index_bytes)
                signature.update(str(raw_path.stat().st_size).encode())
            if cameras:
                self.recordings[recording_id] = {"id": recording_id, "name": folder.name,
                    "relative_path": relative, "complete": session.get("complete", False), "cameras": cameras}
        # A plain image folder is also useful for a separate day's evaluation set.
        if not sessions:
            by_folder: dict[Path, list[Path]] = {}
            for path in sorted(self.source.rglob("*")):
                if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS:
                    by_folder.setdefault(path.parent, []).append(path)
            for folder, paths in by_folder.items():
                relative = folder.relative_to(self.source).as_posix() or "."
                recording_id = hashlib.sha256(relative.encode()).hexdigest()[:16]
                frames = [{"image_path": path} for path in paths]
                self.recordings[recording_id] = {"id": recording_id, "name": folder.name,
                    "relative_path": relative, "complete": True, "cameras": {"images": {
                        "id": "images", "name": "이미지", "kind": "이미지", "frames": frames,
                        "times": list(range(len(frames))),
                    }}}
                for path in paths:
                    stat = path.stat()
                    signature.update(f"{path.relative_to(self.source)}:{stat.st_size}:{stat.st_mtime_ns}".encode())
        if not self.recordings:
            raise ValueError("session.json 촬영 데이터 또는 이미지 파일을 찾지 못했습니다.")
        self.signature = signature.hexdigest()

    def camera(self, recording: str, camera: str) -> dict:
        try:
            return self.recordings[recording]["cameras"][camera]
        except KeyError as exc:
            raise ValueError("촬영 또는 카메라를 찾지 못했습니다.") from exc

    def frame_info(self, recording: str, camera: str, index: int) -> dict:
        frames = self.camera(recording, camera)["frames"]
        if not 0 <= index < len(frames):
            raise ValueError("프레임 번호가 범위를 벗어났습니다.")
        return frames[index]

    def read(self, recording: str, camera: str, index: int) -> np.ndarray:
        stream = self.camera(recording, camera)
        info = self.frame_info(recording, camera, index)
        if "image_path" in info:
            image = cv2.imdecode(np.fromfile(info["image_path"], np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                raise ValueError("이미지 파일을 읽지 못했습니다.")
            return image
        return decode_raw(stream["raw_path"], info)

    def nearest(self, recording: str, camera: str, host_ns: int) -> int:
        times = self.camera(recording, camera)["times"]
        right = min(bisect_left(times, host_ns), len(times) - 1)
        left = max(0, right - 1)
        return left if abs(times[left] - host_ns) <= abs(times[right] - host_ns) else right

    def metadata(self) -> list[dict]:
        return [{"id": rec["id"], "name": rec["name"], "relative_path": rec["relative_path"],
            "complete": rec["complete"], "cameras": [{"id": cam["id"], "name": cam["name"],
                "kind": cam["kind"], "count": len(cam["frames"])} for cam in rec["cameras"].values()]}
            for rec in self.recordings.values()]


def validate_boxes(boxes: list, width: int, height: int) -> list[dict]:
    if not isinstance(boxes, list) or len(boxes) > 500:
        raise ValueError("공 박스 목록이 올바르지 않습니다.")
    validated = []
    for box in boxes:
        if not isinstance(box, dict) or not isinstance(box.get("bbox"), list) or len(box["bbox"]) != 4:
            raise ValueError("박스 좌표는 [x, y, 너비, 높이]여야 합니다.")
        if any(isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
               for value in box["bbox"]):
            raise ValueError("박스 좌표는 유한한 숫자여야 합니다.")
        x, y, w, h = box["bbox"]
        if x < 0 or y < 0 or w < 0.5 or h < 0.5 or x + w > width + 0.01 or y + h > height + 0.01:
            raise ValueError("박스는 이미지 안에 있어야 하고 크기가 0.5픽셀 이상이어야 합니다.")
        if not isinstance(box.get("occluded", False), bool):
            raise ValueError("가림 상태가 올바르지 않습니다.")
        item = {"bbox": [round(float(value), 4) for value in box["bbox"]], "occluded": box.get("occluded", False)}
        if "detection_bbox" in box:
            # Detection edge corrections are independent of the segmentation mask.
            item["detection_bbox"] = validate_boxes([{"bbox": box["detection_bbox"]}], width, height)[0]["bbox"]
        color = box.get("color", "unknown")
        if color not in ("unknown", "red", "blue", "white", "black"):
            raise ValueError("공 색상 속성이 올바르지 않습니다.")
        if color != "unknown":
            item["color"] = color
        if box.get("source") in ("manual", "manual_brush", "efficientsam", "circle", "tracking", "manual_polygon", "auto_sam", "auto_tracking", "auto_geometry"):
            item["source"] = box["source"]
        if "segmentation" in box:
            mask = decode_mask(box["segmentation"], width, height)
            if not np.allclose(cv2.boundingRect(mask), item["bbox"], atol=.01, rtol=0):
                raise ValueError("분할 윤곽과 박스 좌표가 다릅니다. 박스 수정 시 기존 분할 윤곽을 제거하세요.")
            item["segmentation"] = encode_mask(mask)
        validated.append(item)
    return validated


class Project:
    def __init__(self, catalog: Catalog, path: Path, export_root: Path):
        self.catalog, self.path, self.export_root = catalog, path.resolve(), export_root.resolve()
        plain_images = all("raw_path" not in camera for recording in catalog.recordings.values()
                           for camera in recording["cameras"].values())
        if plain_images and self.export_root.is_relative_to(catalog.source):
            raise ValueError("이미지 입력 폴더 밖에 --export-root를 지정하세요. 내보낸 이미지가 다시 입력에 포함되지 않아야 합니다.")
        self.lock = threading.RLock()
        self.job: dict = {"status": "idle"}
        self.selection = SelectionFile(catalog, selection_path(self.path))
        if self.path.exists():
            self.state = json.loads(self.path.read_text(encoding="utf-8"))
            if self.state.get("version") != 1 or self.state.get("source_signature") != catalog.signature:
                raise ValueError("기존 라벨 프로젝트와 촬영 데이터가 다릅니다. 입력 폴더에 촬영을 넣거나 뺐다면 원래대로 되돌리면 다시 열립니다. "
                                 "다른 촬영을 새로 시작하려면 --project로 다른 저장 파일을 지정하세요.")
        else:
            self.state = {"version": 1, "source": str(catalog.source), "source_signature": catalog.signature,
                "revision": 0, "splits": {key: "unassigned" for key in catalog.recordings}, "frames": {}}

    def summary(self) -> dict:
        with self.lock:
            selection = self.selection.view(self.state["frames"])
            frames = {key: {"reviewed": item["reviewed"], "count": len(item["boxes"]),
                           "review_status": item.get("review_status", "reviewed" if item["reviewed"] else "editing" if item.get("human_touched") else "draft"),
                           "automatic": bool(item.get("automatic")), "human_touched": item.get("human_touched", False),
                           "needs_attention": item.get("automatic", {}).get("needs_attention", False),
                           "selected": selection["frames"].get(key, {}).get("selected", True),
                           "representative": selection["frames"].get(key, {}).get("representative", key),
                           "selection_reason": selection["frames"].get(key, {}).get("reason", "unfiltered")}
                      for key, item in self.state["frames"].items()}
            return {"revision": self.state["revision"], "splits": dict(self.state["splits"]), "frames": frames,
                "reviewed": sum(item["reviewed"] for item in frames.values()),
                "boxes": sum(item["count"] for item in frames.values() if item["reviewed"]),
                "selection": {key: value for key, value in selection.items() if key != "frames"},
                "export_frames": sum(item["reviewed"] and item["selected"] for item in frames.values()),
                "export_boxes": sum(item["count"] for item in frames.values() if item["reviewed"] and item["selected"]),
                "auto_revision": self.state.get("auto_revision", 0),
                "automatic_frames": sum(item["automatic"] for item in frames.values()),
                "draft_boxes": sum(item["count"] for item in frames.values() if not item["reviewed"])}

    def commit_automatic(self, entries: list) -> int:
        # Background additions have their own revision, so they cannot conflict
        # with a browser's manual revision. Any saved frame wins over the batch.
        with self.lock:
            additions = {key: item for key, item in entries if key not in self.state["frames"]}
            if not additions:
                return 0
            replacement = dict(self.state)
            replacement["frames"] = {**self.state["frames"], **additions}
            replacement["auto_revision"] = replacement.get("auto_revision", 0) + 1
            atomic_json(self.path, replacement)
            self.state = replacement
            return len(additions)

    def mutate(self, expected_revision: int, callback) -> dict:
        with self.lock:
            if expected_revision != self.state["revision"]:
                raise ConflictError("다른 창에서 변경했습니다. 이 창을 새로고침한 후 다시 작업하세요.")
            replacement = copy.deepcopy(self.state)
            callback(replacement)
            replacement["revision"] += 1
            atomic_json(self.path, replacement)
            self.state = replacement
            return self.summary()

    def frame(self, recording: str, camera: str, index: int) -> dict:
        info = self.catalog.frame_info(recording, camera, index)
        if "width" in info:
            width, height = int(info["width"]), int(info["height"])
        else:
            height, width = self.catalog.read(recording, camera, index).shape[:2]
        key = f"{recording}/{camera}/{index}"
        with self.lock:
            annotation = copy.deepcopy(self.state["frames"].get(key, {"boxes": [], "reviewed": False}))
            for box in annotation["boxes"]:
                if "segmentation" in box:
                    box["contours"] = mask_contours(decode_mask(box["segmentation"], width, height))
            return {"key": key, "recording": recording, "camera": camera, "index": index,
                "width": width, "height": height, "host_ns": str(info.get("host_ns", index)),
                "revision": self.state["revision"], **annotation,
                "selection": self.selection.view(self.state["frames"])["frames"].get(key, {"selected": True, "reason": "unfiltered", "representative": key})}

    def assistance(self, assist: BallAssist, method: str, payload: dict) -> dict:
        recording, camera, index = payload["recording"], payload["camera"], int(payload["index"])
        frame = self.frame(recording, camera, index)
        if method == "polygon":
            return {"key": frame["key"], "proposals": [polygon_proposal(payload["points"], frame["width"], frame["height"])]}
        image = self.catalog.read(recording, camera, index)
        if method == "segment":
            try:
                box = assist.segment(image, self.catalog.signature + "/" + frame["key"], payload["points"],
                                     payload.get("radius", 14), payload.get("box"))
            except ValueError:
                raise
            except Exception as exc:
                raise ValueError(f"분할 실행에 실패했습니다: {str(exc)[:240]}") from exc
            return {"key": frame["key"], "proposals": [box]}
        if method == "circles":
            region = payload.get("region", [0, 0, frame["width"], frame["height"]])
            roi = validate_boxes([{"bbox": region}], frame["width"], frame["height"])[0]["bbox"]
            x, y, w, h = roi
            left, top = math.floor(x), math.floor(y)
            right, bottom = math.ceil(x + w), math.ceil(y + h)
            proposals = assist.circles(image[top:bottom, left:right], payload.get("minimum", 6), payload.get("maximum", 45))
            for box in proposals:
                box["bbox"][0] += left
                box["bbox"][1] += top
            proposals = [box for box in proposals if not any(overlap(box["bbox"], existing["bbox"]) > .4 for existing in frame["boxes"])]
            return {"key": frame["key"], "proposals": proposals}
        if method == "propagate":
            with self.lock:
                previous = [item for item in self.state["frames"].values() if item["recording"] == recording
                            and item["camera"] == camera and item["index"] < index and item["boxes"]]
                if not previous:
                    raise ValueError("이 카메라의 이전 프레임에 저장한 공이 없습니다. 먼저 한 프레임을 라벨링하세요.")
                prior = copy.deepcopy(max(previous, key=lambda item: item["index"]))
            old = self.catalog.read(recording, camera, prior["index"])
            return {"key": frame["key"], "previous_index": prior["index"],
                    "proposals": assist.propagate(old, image, prior["boxes"], frame["boxes"])}
        raise ValueError("지원하지 않는 보조 도구입니다.")

    def save_frame(self, payload: dict) -> dict:
        recording, camera, index = payload["recording"], payload["camera"], int(payload["index"])
        frame = self.frame(recording, camera, index)
        boxes = validate_boxes(payload["boxes"], frame["width"], frame["height"])
        reviewed = payload["reviewed"]
        if not isinstance(reviewed, bool):
            raise ValueError("검수 상태가 올바르지 않습니다.")
        review_status = payload.get("review_status", "reviewed" if reviewed else "editing")
        if review_status not in ("draft", "editing", "reviewed", "hold") or (review_status == "reviewed") != reviewed:
            raise ValueError("검수 상태와 완료 여부가 일치하지 않습니다.")
        if self.catalog.camera(recording, camera)["kind"] == "모노" and any(box.get("color", "unknown") != "unknown" for box in boxes):
            raise ValueError("모노 이미지의 색상은 미지정으로 유지하세요.")
        def update(state):
            previous = state["frames"].get(frame["key"], {})
            state["frames"][frame["key"]] = {"recording": recording, "camera": camera, "index": index,
                "width": frame["width"], "height": frame["height"], "boxes": boxes, "reviewed": reviewed, "review_status": review_status, "human_touched": True,
                **({"automatic": previous["automatic"]} if "automatic" in previous else {})}
            state.pop("native_split", None)
        return self.mutate(payload["expected_revision"], update)

    def save_splits(self, payload: dict) -> dict:
        splits = payload["splits"]
        if not isinstance(splits, dict) or any(key not in self.catalog.recordings or value not in (*SPLITS, "unassigned")
                                              for key, value in splits.items()):
            raise ValueError("촬영별 분할이 올바르지 않습니다.")
        return self.mutate(payload["expected_revision"], lambda state: state["splits"].update(splits))

    def start_export(self, crops: bool) -> dict:
        with self.lock:
            if self.job["status"] == "running":
                raise ValueError("이미 내보내는 중입니다.")
            snapshot = copy.deepcopy(self.state)
            snapshot["frame_selection"] = self.selection.view(self.state["frames"])
            reviewed = [item for _, item in selected_reviewed(snapshot)]
            if not reviewed:
                raise ValueError("검수 완료한 프레임이 없습니다. 공을 모두 표시한 뒤 검수 완료를 누르세요.")
            if any(snapshot["splits"].get(item["recording"]) not in SPLITS for item in reviewed):
                raise ValueError("검수한 촬영의 학습 / 검증 / 시험 분할을 먼저 지정하세요.")
            self.job = {"status": "running", "done": 0, "total": len(reviewed), "revision": snapshot["revision"]}
            threading.Thread(target=self._export_job, args=(snapshot, crops), daemon=True).start()
            return dict(self.job)

    def _export_job(self, snapshot: dict, crops: bool) -> None:
        try:
            result = export_dataset(self.catalog, snapshot, self.export_root, crops,
                                    lambda done: self._progress(done))
            with self.lock:
                self.job.update(status="complete", result=result)
        except Exception as exc:
            with self.lock:
                self.job.update(status="error", error=str(exc))

    def _progress(self, done: int) -> None:
        with self.lock:
            self.job["done"] = done


class ConflictError(ValueError):
    pass


def selected_reviewed(snapshot: dict):
    decisions = snapshot.get("frame_selection", {}).get("frames", {})
    return [(key, item) for key, item in snapshot["frames"].items()
            if item["reviewed"] and (item.get("human_touched") or decisions.get(key, {}).get("selected", True))]


def export_dataset(catalog: Catalog, snapshot: dict, export_root: Path, crops: bool = True,
                   progress=lambda done: None) -> dict:
    # Every export gets a new directory. No stale labels, overwrite, or split fallback.
    output = export_root / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    output.mkdir(parents=True, exist_ok=False)
    incomplete = output / "INCOMPLETE.txt"
    incomplete.write_text("내보내기가 끝나지 않았습니다. manifest.json이 있는 완료된 폴더를 사용하세요.\n", encoding="utf-8")
    coco = {split: {"info": {"description": "Boccia ball reviewed annotations", "version": "1.0"},
                   "licenses": [], "images": [], "annotations": [],
                   "categories": [{"id": 1, "name": "boccia_ball", "supercategory": "ball"}]} for split in SPLITS}
    detection_coco = copy.deepcopy(coco)
    for split in SPLITS:
        (output / "images" / split).mkdir(parents=True)
        (output / "labels" / split).mkdir(parents=True)
        if crops:
            (output / "crops" / split).mkdir(parents=True)
    (output / "annotations").mkdir()
    records, crop_records = [], []
    reviewed = sorted(selected_reviewed(snapshot), key=lambda pair: pair[0])
    annotation_id = 1
    try:
        for image_id, (key, item) in enumerate(reviewed, 1):
            recording, camera, index = item["recording"], item["camera"], item["index"]
            split = snapshot["splits"].get(recording)
            if split not in SPLITS:
                raise ValueError("검수한 촬영의 분할이 지정되지 않았습니다.")
            image = catalog.read(recording, camera, index)
            height, width = image.shape[:2]
            if (width, height) != (item["width"], item["height"]):
                raise ValueError("라벨링 이후 원본 크기가 변경되었습니다.")
            boxes = validate_boxes(item["boxes"], width, height)
            stem = f"{recording}_{camera}_{index:06d}"
            filename = stem + ".png"
            (output / "images" / split / filename).write_bytes(encode_png(image))
            coco[split]["images"].append({"id": image_id, "file_name": filename, "width": width, "height": height,
                "recording_id": recording, "camera_id": camera, "frame_index": index})
            detection_coco[split]["images"].append(copy.deepcopy(coco[split]["images"][-1]))
            yolo = []
            for box in boxes:
                x, y, w, h = box["bbox"]
                annotation = {"id": annotation_id, "image_id": image_id, "category_id": 1,
                    "bbox": [x, y, w, h], "area": w * h, "iscrowd": 0, "occluded": box["occluded"]}
                if "segmentation" in box:
                    annotation["segmentation"] = box["segmentation"]
                    annotation["area"] = sum(box["segmentation"]["counts"][1::2])
                coco[split]["annotations"].append(annotation)
                x, y, w, h = box.get("detection_bbox", box["bbox"])
                detection = {key: value for key, value in annotation.items() if key != "segmentation"}
                detection.update(bbox=[x, y, w, h], area=w * h)
                if "color" in box:
                    detection["color"] = box["color"]
                detection_coco[split]["annotations"].append(detection)
                yolo.append(f"0 {(x + w / 2) / width:.8f} {(y + h / 2) / height:.8f} {w / width:.8f} {h / height:.8f}")
                if crops:
                    left, top = math.floor(x), math.floor(y)
                    right, bottom = min(width, math.ceil(x + w)), min(height, math.ceil(y + h))
                    crop_name = f"{stem}_{annotation_id:06d}.png"
                    crop_path = f"crops/{split}/{crop_name}"
                    (output / crop_path).write_bytes(encode_png(image[top:bottom, left:right]))
                    crop_records.append({"file_name": crop_path, "source_image": f"images/{split}/{filename}",
                        "source_key": key, "bbox": [x, y, w, h], "crop_bounds": [left, top, right, bottom],
                        "occluded": box["occluded"]})
                annotation_id += 1
            (output / "labels" / split / (stem + ".txt")).write_text("\n".join(yolo) + ("\n" if yolo else ""), encoding="utf-8")
            info = catalog.frame_info(recording, camera, index)
            records.append({"key": key, "split": split, "file_name": f"images/{split}/{filename}",
                "recording_path": catalog.recordings[recording]["relative_path"], "camera": camera,
                "frame": index, "host_ns": str(info.get("host_ns", index)), "boxes": boxes})
            progress(image_id)
        for split, dataset in coco.items():
            atomic_json(output / "annotations" / f"instances_{split}.json", dataset)
            atomic_json(output / "annotations" / f"instances_detection_{split}.json", detection_coco[split])
        (output / "data.yaml").write_text(f"path: {json.dumps(output.as_posix(), ensure_ascii=False)}\n"
            "train: images/train\nval: images/val\ntest: images/test\nnames:\n  0: boccia_ball\n", encoding="utf-8")
        counts = {split: {"images": len(dataset["images"]), "boxes": len(dataset["annotations"])} for split, dataset in coco.items()}
        atomic_json(output / "manifest.json", {"version": 1, "complete": True,
            "created_at": datetime.now(timezone.utc).isoformat(), "source": str(catalog.source),
            "source_signature": catalog.signature, "project_revision": snapshot["revision"],
            "frame_selection": {key: value for key, value in snapshot.get("frame_selection", {"enabled": False}).items() if key != "frames"},
            "splits": snapshot["splits"], "native_split": snapshot.get("native_split"),
            "counts": counts, "frames": records, "crops": crop_records})
        incomplete.unlink()
        return {"path": str(output), "counts": counts, "images": len(records), "boxes": annotation_id - 1}
    except Exception as exc:
        incomplete.write_text(f"내보내기 실패: {exc}\n이 폴더를 학습에 사용하지 마세요.\n", encoding="utf-8")
        raise


class LabelServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = os.name != "nt"

    def server_bind(self):
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    def __init__(self, address, project: Project):
        super().__init__(address, Handler)
        self.project = project
        self.assist = BallAssist(ROOT / "data" / "models" / "efficientsam")
        self.auto_job = AutoLabelJob(project, ROOT)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        if args and str(args[1] if len(args) > 1 else "") not in ("200", "201"):
            super().log_message(format, *args)

    def respond(self, status: int, content: bytes, content_type: str):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; style-src 'self'; script-src 'self'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(content)

    def json_response(self, value: dict, status: int = 200):
        self.respond(status, json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8"), "application/json; charset=utf-8")

    def do_GET(self):
        try:
            parsed = urlparse(self.path)
            query = parse_qs(parsed.query)
            project = self.server.project
            if parsed.path == "/api/catalog":
                self.json_response({"recordings": project.catalog.metadata(), "source": str(project.catalog.source),
                    "project_path": str(project.path), "summary": project.summary()})
            elif parsed.path == "/api/native/catalog":
                from labeler_native import native_catalog
                self.json_response(native_catalog(project))
            elif parsed.path == "/api/native/thumb":
                from labeler_native import thumbnail
                self.respond(200, thumbnail(project, query["recording"][0], query["camera"][0], int(query["index"][0]),
                                            int(query.get("width", [128])[0])), "image/jpeg")
            elif parsed.path == "/api/native/color-fill":
                from labeler_native import color_fill_status
                self.json_response(color_fill_status(project))
            elif parsed.path == "/api/native/exports":
                from labeler_native import export_history
                self.json_response({"exports": export_history(project)})
            elif parsed.path in ("/api/frame", "/api/image"):
                recording, camera = query["recording"][0], query["camera"][0]
                index = int(query.get("index", [0])[0])
                if "host_ns" in query:
                    index = project.catalog.nearest(recording, camera, int(query["host_ns"][0]))
                if parsed.path == "/api/frame":
                    self.json_response(project.frame(recording, camera, index))
                else:
                    self.respond(200, encode_png(project.catalog.read(recording, camera, index)), "image/png")
            elif parsed.path == "/api/export-status":
                with project.lock:
                    self.json_response(dict(project.job))
            elif parsed.path == "/api/assist-status":
                self.json_response(self.server.assist.status())
            elif parsed.path == "/api/auto-status":
                self.json_response({"job": self.server.auto_job.status(), "summary": project.summary()})
            elif parsed.path in ("/", "/index.html", "/labeler.js", "/labeler.css"):
                filename = "index.html" if parsed.path == "/" else parsed.path[1:]
                types = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8"}
                path = ROOT / "labeler" / filename
                self.respond(200, path.read_bytes(), types[path.suffix])
            else:
                self.json_response({"error": "없는 주소입니다."}, 404)
        except (ValueError, KeyError, IndexError, OSError, zstd.ZstdError, cv2.error) as exc:
            self.json_response({"error": str(exc)}, 400)

    def do_POST(self):
        try:
            # Only our local page may send browser mutations; no external Origin/CORS.
            origin = self.headers.get("Origin")
            if origin and origin != f"http://{self.headers.get('Host')}":
                self.json_response({"error": "다른 사이트에서 저장할 수 없습니다."}, 403)
                return
            if not self.headers.get("Content-Type", "").startswith("application/json"):
                raise ValueError("JSON 요청이 필요합니다.")
            length = int(self.headers.get("Content-Length", 0))
            if not 0 < length <= 1_000_000:
                raise ValueError("요청 크기가 올바르지 않습니다.")
            payload = json.loads(self.rfile.read(length))
            project = self.server.project
            route = urlparse(self.path).path
            if route.startswith("/api/native/"):
                from labeler_native import native_action
                self.json_response(native_action(project, route.rsplit("/", 1)[-1], payload))
            elif route == "/api/annotation":
                self.json_response({"summary": project.save_frame(payload)})
            elif route == "/api/splits":
                self.json_response({"summary": project.save_splits(payload)})
            elif route == "/api/export":
                self.json_response(project.start_export(bool(payload.get("crops", True))))
            elif route in ("/api/assist/segment", "/api/assist/circles", "/api/assist/propagate", "/api/assist/polygon"):
                self.json_response(project.assistance(self.server.assist, route.rsplit("/", 1)[-1], payload))
            elif route == "/api/auto-start":
                self.json_response(self.server.auto_job.start(self.server.assist))
            elif route == "/api/auto-stop":
                self.json_response(self.server.auto_job.stop())
            else:
                self.json_response({"error": "없는 주소입니다."}, 404)
        except ConflictError as exc:
            self.json_response({"error": str(exc)}, 409)
        except (ValueError, KeyError, TypeError, OSError, cv2.error) as exc:
            self.json_response({"error": str(exc)}, 400)


def main() -> None:
    parser = argparse.ArgumentParser(description="보치아 공 로컬 라벨링 도구")
    parser.add_argument("--source", type=Path, default=ROOT / "data" / "imports")
    parser.add_argument("--project", type=Path, default=ROOT / "output" / "ball-labeler" / "project.json")
    parser.add_argument("--export-root", type=Path, default=ROOT / "output" / "ball-labeler" / "exports")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--auto", action="store_true", help="미처리 프레임의 자동 초안 생성 시작")
    args = parser.parse_args()
    try:
        catalog = Catalog(args.source)
        project = Project(catalog, args.project, args.export_root)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        parser.exit(1, str(exc) + "\n")
    try:
        server = LabelServer(("127.0.0.1", args.port), project)
    except OSError as exc:
        # Double-clicking the launcher again should reopen the running project.
        url = f"http://127.0.0.1:{args.port}/"
        if not args.no_browser:
            try:
                with urlopen(url + "api/catalog", timeout=2) as response:
                    running = json.load(response)
                if Path(running["source"]).resolve() == catalog.source and Path(running["project_path"]).resolve() == project.path:
                    if args.auto:
                        request = Request(url + "api/auto-start", data=b"{}", headers={"Content-Type": "application/json"})
                        try:
                            with urlopen(request, timeout=10):
                                pass
                        except URLError as auto_error:
                            print(f"Automatic labeling unavailable: {auto_error}", flush=True)
                    print(f"Existing Boccia labeler: {url}", flush=True)
                    webbrowser.open(url)
                    return
            except (URLError, OSError, ValueError, KeyError):
                pass
        parser.exit(1, str(exc) + "\n다른 --port를 지정하거나 실행 중인 서버를 종료하세요.\n")
    url = f"http://127.0.0.1:{server.server_port}/"
    print(f"Boccia labeler: {url}\nSource: {catalog.source}\nProject: {project.path}", flush=True)
    if args.auto:
        try:
            server.auto_job.start(server.assist)
        except ValueError as exc:
            print(f"Automatic labeling unavailable: {exc}", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
