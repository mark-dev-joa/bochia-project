"""Evaluate a trained detector on one split of a labeler export the way the research cares: balls found and missed, false
balls, and how far the predicted centres sit from the labelled ones, per camera.

Usage: python eval_centres.py <weights.pt> <export folder> [split=test] [conf=0.25] [imgsz=1280]
A prediction counts as a hit when its centre lies within the labelled ball's radius (half the shorter box side).
"""
import collections
import pathlib
import statistics
import sys

from ultralytics import YOLO

weights = sys.argv[1]
export = pathlib.Path(sys.argv[2])
options = dict(item.split("=", 1) for item in sys.argv[3:])
split = options.get("split", "test")
conf = float(options.get("conf", 0.25))
imgsz = int(options.get("imgsz", 1280))

images = sorted((export / "images" / split).glob("*.png"))
if not images:
    sys.exit(f"no images in {export / 'images' / split}")
model = YOLO(weights)

per_camera = collections.defaultdict(lambda: {"labels": 0, "hits": 0, "false": 0, "errors": [], "missed_frames": []})
for start in range(0, len(images), 16):
    batch = images[start:start + 16]
    results = model.predict([str(p) for p in batch], conf=conf, imgsz=imgsz, verbose=False)
    for image, result in zip(batch, results):
        camera = image.stem.split("_")[1]
        stats = per_camera[camera]
        height, width = result.orig_shape
        label_file = export / "labels" / split / (image.stem + ".txt")
        labels = []
        if label_file.exists():
            for line in label_file.read_text().split("\n"):
                parts = line.split()
                if len(parts) == 5:
                    _, cx, cy, w, h = map(float, parts)
                    labels.append((cx * width, cy * height, min(w * width, h * height) / 2))
        stats["labels"] += len(labels)
        predictions = [((x1 + x2) / 2, (y1 + y2) / 2) for x1, y1, x2, y2 in result.boxes.xyxy.tolist()]
        used = [False] * len(predictions)
        for lx, ly, r in labels:
            best, best_distance = -1, None
            for i, (px, py) in enumerate(predictions):
                if used[i]:
                    continue
                d = ((px - lx) ** 2 + (py - ly) ** 2) ** .5
                if d <= max(r, 4) and (best < 0 or d < best_distance):
                    best, best_distance = i, d
            if best >= 0:
                used[best] = True
                stats["hits"] += 1
                stats["errors"].append(best_distance)
            else:
                stats["missed_frames"].append(image.name)
        stats["false"] += sum(1 for u in used if not u)

print(f"split {split} · images {len(images)} · conf {conf} · imgsz {imgsz}")
print(f"{'camera':10} {'labels':>7} {'found':>7} {'missed':>7} {'false':>6} {'recall':>7} {'centre err px: median':>22} {'p95':>6} {'max':>6}")
total = {"labels": 0, "hits": 0, "false": 0, "errors": []}
for camera in sorted(per_camera):
    s = per_camera[camera]
    errors = s["errors"] or [0.0]
    recall = s["hits"] / s["labels"] if s["labels"] else 0
    p95 = sorted(errors)[int(len(errors) * .95) - 1] if len(errors) > 1 else errors[0]
    print(f"{camera:10} {s['labels']:7} {s['hits']:7} {s['labels'] - s['hits']:7} {s['false']:6} {recall:7.3f} {statistics.median(errors):22.2f} {p95:6.2f} {max(errors):6.2f}")
    for key in ("labels", "hits", "false"):
        total[key] += s[key]
    total["errors"] += s["errors"]
errors = total["errors"] or [0.0]
print(f"{'all':10} {total['labels']:7} {total['hits']:7} {total['labels'] - total['hits']:7} {total['false']:6} {total['hits'] / max(1, total['labels']):7.3f} {statistics.median(errors):22.2f}")
missed = [name for s in per_camera.values() for name in s["missed_frames"]]
if missed:
    print("missed in:", ", ".join(sorted(set(missed))[:12]), "..." if len(set(missed)) > 12 else "")
