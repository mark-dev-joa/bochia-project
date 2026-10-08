"""Conservative, within-camera representative-frame selection.

This changes the review/training subset, never images or annotations. Every
excluded frame points directly to a retained frame and records measured changes.
"""
from __future__ import annotations

import copy
import json
import math
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from labeler_assist import decode_mask


RULES = {
    # 8px of box movement is within the jitter of the automatic draft; the pixels decide whether the ball moved.
    "center_pixels": 8.0, "size_pixels": 4.0, "size_fraction": .12,
    "mask_iou": .85, "ball_mae": 5.0, "ball_correlation": .97,
    "scene_width": 720, "scene_mae": 2.5, "scene_changed_fraction": .0015,
    "scene_change_level": 12, "scene_component_pixels": 18, "scene_tile_mae": 4.0,
    "periodic_seconds": 2.0, "periodic_frames_without_timestamps": 60,
}


def selection_path(project_path: Path) -> Path:
    return project_path.with_name(project_path.stem + "-selection.json")


class SelectionFile:
    def __init__(self, catalog, path):
        self.catalog, self.path = catalog, path
        self.stamp = None
        self.value = None

    def read(self):
        stamp = self.path.stat().st_mtime_ns if self.path.exists() else None
        if stamp == self.stamp:
            return self.value
        if stamp is None:
            self.value, self.stamp = None, None
            return None
        value = json.loads(self.path.read_text(encoding="utf-8"))
        if value.get("version") != 1 or value.get("source_signature") != self.catalog.signature:
            raise ValueError("대표 프레임 필터와 촬영 데이터가 다릅니다.")
        decisions = value.get("frames")
        if not isinstance(decisions, dict):
            raise ValueError("대표 프레임 필터 형식이 올바르지 않습니다.")
        for key, item in decisions.items():
            recording, camera, index = key.split("/")
            self.catalog.frame_info(recording, camera, int(index))
            if not isinstance(item.get("selected"), bool):
                raise ValueError("대표 프레임 선택 상태가 올바르지 않습니다.")
            representative = item.get("representative", key)
            if not item["selected"]:
                if representative.rsplit("/", 1)[0] != key.rsplit("/", 1)[0] or not decisions.get(representative, {}).get("selected"):
                    raise ValueError("중복 프레임은 같은 촬영·카메라의 대표 프레임을 참조해야 합니다.")
        self.value, self.stamp = value, stamp
        return value

    def view(self, annotations):
        value = self.read()
        if not value:
            return {"enabled": False, "frames": {}}
        decisions = copy.deepcopy(value["frames"])
        # Human edits after selection must never silently vanish from training.
        for key, frame in annotations.items():
            if frame.get("human_touched") or frame.get("reviewed"):
                decisions[key] = {"selected": True, "representative": key, "reason": "human_preserved"}
        total = sum(len(cam["frames"]) for rec in self.catalog.recordings.values() for cam in rec["cameras"].values())
        excluded = sum(not item["selected"] for item in decisions.values())
        return {"enabled": True, "path": str(self.path), "created_at": value["created_at"],
                "algorithm": value["algorithm"], "rules": value["rules"], "total": total,
                "selected": total - excluded, "excluded": excluded,
                "coverage": len(value["frames"]), "frames": decisions}


def match_boxes(a, b, rules=RULES):
    if len(a) != len(b):
        return None
    edges = []
    for i, old in enumerate(a):
        x, y, w, h = old["bbox"]
        for j, new in enumerate(b):
            u, v, s, t = new["bbox"]
            distance = math.hypot(x + w / 2 - u - s / 2, y + h / 2 - v - t / 2)
            if distance <= rules["center_pixels"] and old.get("occluded", False) == new.get("occluded", False) and all(
                abs(before - after) <= max(rules["size_pixels"], before * rules["size_fraction"])
                for before, after in ((w, s), (h, t))):
                edges.append((distance, i, j))
    # Only a one-to-one correspondence is accepted. Ambiguous crowded balls
    # conservatively become representatives rather than force a match.
    pairs, used_a, used_b = [], set(), set()
    for distance, i, j in sorted(edges):
        if i not in used_a and j not in used_b:
            pairs.append((i, j, distance)); used_a.add(i); used_b.add(j)
    return pairs if len(pairs) == len(a) else None


def features(image, boxes, rules=RULES):
    height, width = image.shape[:2]
    scale = min(1.0, rules["scene_width"] / width)
    scene = cv2.resize(image, (round(width * scale), round(height * scale)), interpolation=cv2.INTER_AREA)
    # Balls are compared at the retained frame's position and masks in image coordinates, so a draft box that
    # only jittered on a still ball reads as the same ball.
    masks = [decode_mask(box["segmentation"], width, height).astype(bool) if "segmentation" in box else None for box in boxes]
    return {"scene": scene, "image": image, "boxes": boxes, "masks": masks}


def ball_patch(image, box):
    """Ball with context around the box center, blurred and resized so texture and occlusion differences remain."""
    x, y, w, h = box["bbox"]
    side = max(16, math.ceil(max(w, h) * 2))
    patch = cv2.getRectSubPix(image, (side, side), (float(x + w / 2), float(y + h / 2)))
    return cv2.resize(cv2.GaussianBlur(patch, (3, 3), 0), (48, 48), interpolation=cv2.INTER_AREA).astype(np.float32)


def compare_frames(a, b, pairs, rules=RULES):
    if a["scene"].shape != b["scene"].shape:
        return False, "image_dimensions", {}
    difference = cv2.absdiff(a["scene"], b["scene"]).max(axis=2)
    changed = (difference > rules["scene_change_level"]).astype(np.uint8)
    _, _, stats, _ = cv2.connectedComponentsWithStats(changed, connectivity=8)
    component = int(stats[1:, cv2.CC_STAT_AREA].max()) if len(stats) > 1 else 0
    tile_mae = max(float(difference[y:y + 30, x:x + 30].mean())
                   for y in range(0, difference.shape[0], 30) for x in range(0, difference.shape[1], 30))
    metrics = {"center_max": round(max((p[2] for p in pairs), default=0), 3),
               "scene_mae": round(float(difference.mean()), 4),
               "scene_changed_fraction": round(float(changed.mean()), 6),
               "scene_component": component, "scene_tile_mae": round(tile_mae, 4)}
    if metrics["scene_mae"] > rules["scene_mae"] or metrics["scene_changed_fraction"] > rules["scene_changed_fraction"] or component > rules["scene_component_pixels"] or tile_mae > rules["scene_tile_mae"]:
        return False, "scene_change", metrics
    max_mae, min_correlation, min_iou = 0.0, 1.0, 1.0
    for i, j, _ in pairs:
        old, new = ball_patch(a["image"], a["boxes"][i]), ball_patch(b["image"], a["boxes"][i])   # the same spot in both frames
        mae = float(np.abs(old - new).mean())
        centered_a, centered_b = old - old.mean(), new - new.mean()
        denominator = float(np.linalg.norm(centered_a) * np.linalg.norm(centered_b))
        correlation = float(np.sum(centered_a * centered_b) / denominator) if denominator > 1 else (1.0 if mae < 1 else 0.0)
        max_mae, min_correlation = max(max_mae, mae), min(min_correlation, correlation)
        old_mask, new_mask = a["masks"][i], b["masks"][j]
        if (old_mask is None) != (new_mask is None):
            return False, "mask_change", metrics
        if old_mask is not None:
            iou = float(np.logical_and(old_mask, new_mask).sum() / max(1, np.logical_or(old_mask, new_mask).sum()))
            min_iou = min(min_iou, iou)
    metrics.update(ball_mae=round(max_mae, 4), ball_correlation=round(min_correlation, 5), mask_iou=round(min_iou, 5))
    if max_mae > rules["ball_mae"] or min_correlation < rules["ball_correlation"]:
        return False, "ball_appearance", metrics
    if min_iou < rules["mask_iou"]:
        return False, "mask_change", metrics
    return True, "near_duplicate", metrics


def build_selection(catalog, snapshot, progress=lambda count, total, camera: None, rules=None):
    rules = {**RULES, **(rules or {})}
    if snapshot.get("source_signature") != catalog.signature:
        raise ValueError("라벨 프로젝트와 원본 촬영이 다릅니다.")
    total = sum(len(cam["frames"]) for rec in catalog.recordings.values() for cam in rec["cameras"].values())
    decisions, counts, done = {}, [], 0
    for recording, rec in catalog.recordings.items():
        for camera, stream in rec["cameras"].items():
            anchor, anchor_features, kept = None, None, 0
            for index, info in enumerate(stream["frames"]):
                key = f"{recording}/{camera}/{index}"
                item = snapshot["frames"].get(key)
                reason, metrics, selected = "first_frame", {}, True
                image_features = None
                if not item:
                    reason = "unlabeled"
                elif item.get("human_touched") or item.get("reviewed"):
                    reason = "human_preserved"
                elif index == len(stream["frames"]) - 1:
                    reason = "last_frame"
                elif item.get("automatic", {}).get("notes"):
                    reason = "uncertain_annotation"
                elif anchor is not None:
                    old_key, old_index, old_item = anchor
                    old_info = stream["frames"][old_index]
                    if "host_ns" in info and "host_ns" in old_info:
                        periodic = int(info["host_ns"]) - int(old_info["host_ns"]) >= rules["periodic_seconds"] * 1e9
                    else:
                        periodic = index - old_index >= rules["periodic_frames_without_timestamps"]
                    pairs = match_boxes(old_item["boxes"], item["boxes"], rules)
                    if periodic:
                        reason = "periodic_static_sample"
                    elif pairs is None:
                        reason = "ball_geometry"
                    else:
                        image_features = features(catalog.read(recording, camera, index), item["boxes"], rules)
                        if anchor_features is None:
                            anchor_features = features(catalog.read(recording, camera, old_index), old_item["boxes"], rules)
                        duplicate, reason, metrics = compare_frames(anchor_features, image_features, pairs, rules)
                        selected = not duplicate
                decisions[key] = {"selected": selected, "representative": key if selected else anchor[0],
                                  "reason": reason, **({"metrics": metrics} if metrics else {})}
                if selected:
                    kept += 1
                    anchor = (key, index, item) if item else None
                    anchor_features = image_features
                done += 1
                if done % 200 == 0:
                    progress(done, total, rec["name"] + "/" + camera)
            counts.append({"recording": recording, "name": rec["name"], "camera": camera,
                           "total": len(stream["frames"]), "selected": kept, "excluded": len(stream["frames"]) - kept})
            progress(done, total, rec["name"] + "/" + camera)
    return {"version": 1, "algorithm": "within-camera-geometry-mask-appearance-v2",
            "created_at": datetime.now(timezone.utc).isoformat(), "source_signature": catalog.signature,
            "project_revision": snapshot.get("revision", 0), "rules": rules,
            "total": total, "selected": sum(c["selected"] for c in counts),
            "excluded": sum(c["excluded"] for c in counts), "cameras": counts, "frames": decisions}
