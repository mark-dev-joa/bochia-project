"""Local annotation proposals: EfficientSAM clicks, circles and frame matching.

All outputs are proposals, never reviewed annotations. COCO RLE uses column-major
order and includes the leading background run (including a zero-length run).
"""
from __future__ import annotations

import math
import threading
from pathlib import Path

import cv2
import numpy as np


def encode_mask(mask: np.ndarray) -> dict:
    pixels = (mask > 0).astype(np.uint8).ravel(order="F")
    changes = np.flatnonzero(pixels[1:] != pixels[:-1]) + 1
    counts = np.diff(np.r_[0, changes, len(pixels)]).tolist()
    if pixels[0]:
        counts.insert(0, 0)
    return {"size": list(mask.shape), "counts": counts}


BALL_COLORS = ("red", "blue", "white", "black")


def ball_color(image: np.ndarray, mask: np.ndarray) -> str:
    """Red, blue, white or black from the pixels inside the mask of a colour image; unknown for a mono image.

    Thresholds come from the 2026-10-02 recordings, which are dim under warm light: a red ball has hue 5-6 and
    saturation about 190, the white jack hue 18 (the warm cast) and saturation about 100 at value 65, a black ball
    value about 11. Hue is OpenCV's 0-179.
    """
    if image.ndim != 3:
        return "unknown"
    inner = cv2.erode(mask.astype(np.uint8), np.ones((5, 5), np.uint8))   # the edge pixels mix with the floor
    if inner.sum() < 20:
        inner = mask.astype(np.uint8)
    pixels = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)[inner > 0]
    if len(pixels) < 10:
        return "unknown"
    saturated = pixels[pixels[:, 1] > 120]
    if len(saturated) >= len(pixels) * .4:
        hue = int(np.bincount(saturated[:, 0], minlength=180).argmax())
        if hue < 12 or hue > 165:
            return "red"
        if 95 <= hue <= 135:
            return "blue"
    return "black" if float(np.median(pixels[:, 2])) < 22 else "white"


def decode_mask(rle: dict, width: int, height: int) -> np.ndarray:
    if not isinstance(rle, dict) or rle.get("size") != [height, width]:
        raise ValueError("분할 마스크의 크기가 원본과 다릅니다.")
    counts = rle.get("counts")
    if not isinstance(counts, list) or not counts or len(counts) > width * height + 1:
        raise ValueError("분할 마스크의 RLE 형식이 올바르지 않습니다.")
    if any(type(value) is not int or value < 0 for value in counts) or sum(counts) != width * height:
        raise ValueError("분할 마스크의 RLE 길이가 올바르지 않습니다.")
    if not sum(counts[1::2]):
        raise ValueError("분할 마스크가 비어 있습니다.")
    flat = np.zeros(width * height, np.uint8)
    offset = 0
    for index, count in enumerate(counts):
        if index % 2:
            flat[offset:offset + count] = 1
        offset += count
    return flat.reshape((height, width), order="F")


def mask_contours(mask: np.ndarray) -> list:
    contours, _ = cv2.findContours(mask, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    return [contour.reshape(-1, 2).tolist() for contour in contours if len(contour) >= 3]


def overlap(a: list, b: list) -> float:
    x, y = max(a[0], b[0]), max(a[1], b[1])
    w, h = max(0, min(a[0] + a[2], b[0] + b[2]) - x), max(0, min(a[1] + a[3], b[1] + b[3]) - y)
    area = w * h
    return area / max(1, a[2] * a[3] + b[2] * b[3] - area)


def bounded_number(value, low: float, high: float, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{name}: {low:g}~{high:g} 사이의 숫자를 입력하세요.")
    return float(value)


class BallAssist:
    def __init__(self, model_dir: Path):
        self.model_dir = model_dir
        self.lock = threading.RLock()
        self.encoder = self.decoder = None
        self.cache = None

    def status(self) -> dict:
        files = all((self.model_dir / f"efficientsam_ti_{part}.onnx").is_file() for part in ("encoder", "decoder"))
        try:
            import onnxruntime  # noqa: F401
            runtime = True
        except ImportError:
            runtime = False
        return {"ready": files and runtime, "model": "EfficientSAM-Ti", "device": "CPU",
                "license": "Apache-2.0", "model_files": files, "runtime": runtime}

    def _sessions(self):
        if self.encoder is not None:
            return
        if not self.status()["ready"]:
            raise ValueError("분할 모델이 준비되지 않았습니다. requirements-labeler.txt 설치 후 python tools/setup_labeler_assist.py를 실행하세요.")
        import onnxruntime as ort
        options = ort.SessionOptions()
        options.intra_op_num_threads = 4
        options.inter_op_num_threads = 1
        try:
            encoder = ort.InferenceSession(str(self.model_dir / "efficientsam_ti_encoder.onnx"), options, providers=["CPUExecutionProvider"])
            decoder = ort.InferenceSession(str(self.model_dir / "efficientsam_ti_decoder.onnx"), options, providers=["CPUExecutionProvider"])
        except Exception as exc:
            raise ValueError("분할 모델을 읽지 못했습니다. 모델 설치 스크립트로 파일을 다시 확인하세요.") from exc
        self.encoder, self.decoder = encoder, decoder

    def segment(self, image: np.ndarray, key: str, points: list, radius=14, seed_bbox=None) -> dict:
        height, width = image.shape[:2]
        radius = bounded_number(radius, 3, 300, "예상 공 반지름")
        if not isinstance(points, list) or not 1 <= len(points) <= 32:
            raise ValueError("분할 클릭은 1~32개여야 합니다.")
        coords, labels = [], []
        for point in points:
            if not isinstance(point, dict) or point.get("label") not in (0, 1) or isinstance(point.get("label"), bool):
                raise ValueError("포함/제외 클릭 형식이 올바르지 않습니다.")
            coords.append([bounded_number(point.get("x"), 0, width - 1, "클릭 X"),
                           bounded_number(point.get("y"), 0, height - 1, "클릭 Y")])
            labels.append(point["label"])
        if labels[0] != 1:
            raise ValueError("첫 클릭은 공 내부의 포함 클릭이어야 합니다.")
        # A local crop gives the model enough pixels for the small distant balls.
        cx, cy = coords[0]
        half = int(max(32, radius * 3.5))
        left, top = max(0, int(cx) - half), max(0, int(cy) - half)
        right, bottom = min(width, int(cx) + half + 1), min(height, int(cy) + half + 1)
        if any(not (left <= x < right and top <= y < bottom) for x, y in coords):
            raise ValueError("추가 클릭은 첫 공 주변에서 해주세요. 새 공은 현재 제안을 추가/취소한 뒤 클릭하세요.")
        crop = image[top:bottom, left:right]
        prompt_coords, prompt_labels = list(coords), list(labels)
        if seed_bbox is not None:
            if not isinstance(seed_bbox, list) or len(seed_bbox) != 4:
                raise ValueError("자동 분할의 시작 박스가 올바르지 않습니다.")
            x, y, w, h = [bounded_number(value, 0, max(width, height), "시작 박스") for value in seed_bbox]
            if w < 1 or h < 1 or x + w > width or y + h > height:
                raise ValueError("자동 분할의 시작 박스는 이미지 안에 있어야 합니다.")
            prompt_coords.extend([[x, y], [min(width - 1, x + w), min(height - 1, y + h)]])
            prompt_labels.extend([2, 3])  # EfficientSAM box corners, plus the positive center.
        cache_key = (key, left, top, right, bottom)
        with self.lock:
            self._sessions()
            if self.cache is None or self.cache[0] != cache_key:
                rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
                tensor = rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255.0
                embeddings = self.encoder.run(None, {"batched_images": tensor})[0]
                self.cache = (cache_key, embeddings)
            local_coords = np.array(prompt_coords, np.float32) - [left, top]
            logits, ious, _ = self.decoder.run(None, {
                "image_embeddings": self.cache[1],
                "batched_point_coords": local_coords.astype(np.float32)[None, None],
                "batched_point_labels": np.array(prompt_labels, np.float32)[None, None],
                "orig_im_size": np.array(crop.shape[:2], np.int64),
            })
        candidates = []
        for logit, iou in zip(logits[0, 0], ious[0, 0]):
            mask = (logit >= 0).astype(np.uint8)
            _, components = cv2.connectedComponents(mask)
            anchor = components[int(cy) - top, int(cx) - left]
            if anchor == 0:
                continue
            mask = (components == anchor).astype(np.uint8)
            if any(bool(mask[int(y) - top, int(x) - left]) != bool(label)
                   for (x, y), label in zip(coords, labels)):
                continue
            x, y, w, h = cv2.boundingRect(mask)
            area = int(mask.sum())
            if area < 4 or area > mask.size * .7 or max(w, h) > 2 * half * .95:
                continue
            candidates.append((float(iou), mask, [left + x, top + y, w, h], area))
        if not candidates:
            raise ValueError("공 영역을 분리하지 못했습니다. 공 중심을 다시 클릭하거나 반지름을 조정하고, Shift+클릭으로 주변을 제외하세요.")
        quality, local_mask, bbox, area = max(candidates, key=lambda candidate: candidate[0])
        mask = np.zeros((height, width), np.uint8)
        mask[top:bottom, left:right] = local_mask
        return {"bbox": bbox, "occluded": False, "segmentation": encode_mask(mask),
                "contours": mask_contours(mask), "source": "efficientsam", "quality": round(quality, 4), "area": area}

    @staticmethod
    def circles(image: np.ndarray, minimum=6, maximum=45, existing=()) -> list[dict]:
        minimum = int(bounded_number(minimum, 2, 300, "최소 반지름"))
        maximum = int(bounded_number(maximum, minimum, 300, "최대 반지름"))
        height, width = image.shape[:2]
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        gray = cv2.createCLAHE(clipLimit=2, tileGridSize=(8, 8)).apply(gray)
        gray = cv2.GaussianBlur(gray, (5, 5), 1)
        found = cv2.HoughCircles(gray, cv2.HOUGH_GRADIENT, dp=1.2, minDist=minimum * 2,
                                 param1=90, param2=22, minRadius=minimum, maxRadius=maximum)
        proposals = []
        if found is not None:
            for cx, cy, radius in found[0]:
                left, top = max(0, math.floor(cx - radius)), max(0, math.floor(cy - radius))
                right, bottom = min(width, math.ceil(cx + radius)), min(height, math.ceil(cy + radius))
                bbox = [left, top, right - left, bottom - top]
                if any(overlap(bbox, box["bbox"]) > .4 for box in existing):
                    continue
                proposals.append({"bbox": bbox, "occluded": False, "source": "circle"})
                if len(proposals) >= 100:
                    break
        return proposals

    @staticmethod
    def propagate(previous: np.ndarray, image: np.ndarray, boxes: list, existing=(), search=64) -> list[dict]:
        if previous.shape != image.shape:
            raise ValueError("이전 프레임과 이미지 크기가 다릅니다.")
        height, width = image.shape[:2]
        old = cv2.cvtColor(previous, cv2.COLOR_BGR2GRAY)
        new = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        proposals = []
        for box in boxes:
            x, y, w, h = box["bbox"]
            left, top = max(0, math.floor(x) - 3), max(0, math.floor(y) - 3)
            right, bottom = min(width, math.ceil(x + w) + 3), min(height, math.ceil(y + h) + 3)
            template = old[top:bottom, left:right]
            if min(template.shape) < 2 or float(template.std()) < 3:
                continue
            sx, sy = max(0, left - search), max(0, top - search)
            ex, ey = min(width, right + search), min(height, bottom + search)
            response = cv2.matchTemplate(new[sy:ey, sx:ex], template, cv2.TM_CCOEFF_NORMED)
            _, quality, _, location = cv2.minMaxLoc(response)
            if quality < .7:
                continue
            dx, dy = sx + location[0] - left, sy + location[1] - top
            bbox = [max(0, min(width - w, x + dx)), max(0, min(height - h, y + dy)), w, h]
            if any(overlap(bbox, item["bbox"]) > .4 for item in (*existing, *proposals)):
                continue
            # A mask from another frame would be inaccurate; only propose the box.
            proposals.append({"bbox": bbox, "occluded": box.get("occluded", False),
                              "source": "tracking", "quality": round(quality, 4)})
        return proposals


def polygon_proposal(points: list, width: int, height: int) -> dict:
    if not isinstance(points, list) or not 3 <= len(points) <= 2048:
        raise ValueError("수동 윤곽은 꼭짓점이 3~2048개여야 합니다.")
    coords = []
    for point in points:
        if not isinstance(point, list) or len(point) != 2:
            raise ValueError("윤곽 좌표는 [x, y]여야 합니다.")
        coords.append([round(bounded_number(point[0], 0, width - 1, "윤곽 X")),
                       round(bounded_number(point[1], 0, height - 1, "윤곽 Y"))])
    polygon = np.array(coords, np.int32)
    if abs(cv2.contourArea(polygon)) < 1:
        raise ValueError("윤곽의 면적이 너무 작습니다. 공 둘레를 따라 점을 찍으세요.")
    mask = np.zeros((height, width), np.uint8)
    cv2.fillPoly(mask, [polygon], 1)
    return {"bbox": list(cv2.boundingRect(mask)), "occluded": False, "source": "manual_polygon",
            "segmentation": encode_mask(mask), "contours": mask_contours(mask)}

