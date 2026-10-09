"""The overlapping balls of a few consecutive frames, camera by camera: a crop around the cluster with the labelled boxes
(green; the missed ones thick) and the detector's predictions at any score (red, score written), one row per frame.

python show_cluster.py <export folder> <split> <predictions.json> <out.png> frames=95,96,97,98,99,100 [prefix=a9fa9c1628aee107] [conf=0.25] [min=0.05]
"""
import collections
import json
import math
import pathlib
import sys

import cv2
import numpy as np

export, split, pred_path, out = sys.argv[1:5]
export = pathlib.Path(export)
options = dict(item.split("=", 1) for item in sys.argv[5:])
frames = [int(f) for f in options["frames"].split(",")]
prefix = options.get("prefix", ""); conf = float(options.get("conf", 0.25)); min_score = float(options.get("min", 0.05))
gt = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
by_name = {im["file_name"]: im for im in gt["images"]}
gts = collections.defaultdict(list)
for a in gt["annotations"]:
    gts[a["image_id"]].append(a["bbox"])
preds = collections.defaultdict(list)
for p in json.load(open(pred_path)):
    if p["score"] >= min_score:
        preds[p["image_id"]].append((p["bbox"], p["score"]))


def iou(a, b):
    ix = max(0.0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])); iy = max(0.0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    inter = ix * iy; union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


TILE = 300; BAND = 22
cameras = [0, 1, 2]
centres = {}
rows = []
for frame in frames:
    row = []
    for cam in cameras:
        name = f"{prefix}_camera-{cam}_{frame:06d}.png"
        tile = np.full((TILE + BAND, TILE, 3), 255, np.uint8)
        info = by_name.get(name)
        if info is None:
            cv2.putText(tile, f"cam{cam} f{frame}: not in split", (4, 16), cv2.FONT_HERSHEY_SIMPLEX, .45, (0, 0, 0), 1)
            row.append(tile); continue
        boxes = gts[info["id"]]
        cluster = [b for b in boxes if any(iou(b, o) > 0 for o in boxes if o is not b)]
        if cluster:
            cx = sum(b[0] + b[2] / 2 for b in cluster) / len(cluster); cy = sum(b[1] + b[3] / 2 for b in cluster) / len(cluster)
            half = max(70, int(1.6 * max(max(b[2], b[3]) for b in cluster)))
            centres[cam] = (cx, cy, half)
        elif cam in centres:
            cx, cy, half = centres[cam]
        else:
            cv2.putText(tile, f"cam{cam} f{frame}: no cluster", (4, 16), cv2.FONT_HERSHEY_SIMPLEX, .45, (0, 0, 0), 1)
            row.append(tile); continue
        image = cv2.imread(str(export / "images" / split / name), cv2.IMREAD_COLOR)
        H, W = image.shape[:2]
        x0, y0 = max(0, int(cx - half)), max(0, int(cy - half)); x1, y1 = min(W, int(cx + half)), min(H, int(cy + half))
        crop = image[y0:y1, x0:x1].copy()
        scale = TILE / max(crop.shape[0], crop.shape[1])
        crop = cv2.resize(crop, (int(crop.shape[1] * scale), int(crop.shape[0] * scale)), interpolation=cv2.INTER_CUBIC)

        def rect(b, colour, thick):
            p1 = (int((b[0] - x0) * scale), int((b[1] - y0) * scale)); p2 = (int((b[0] + b[2] - x0) * scale), int((b[1] + b[3] - y0) * scale))
            cv2.rectangle(crop, p1, p2, colour, thick)
            return p1

        matched = set()   # one prediction answers for one ball, as in overlap_stats.py
        for pb, s in sorted(preds[info["id"]], key=lambda t: -t[1]):
            if s < conf:
                continue
            best, best_iou = None, 0.0
            for i, b in enumerate(boxes):
                if i in matched:
                    continue
                v = iou(b, pb)
                if v > best_iou:
                    best, best_iou = i, v
            if best is None:
                continue
            b = boxes[best]; pcx, pcy = pb[0] + pb[2] / 2, pb[1] + pb[3] / 2
            if best_iou >= .5 or (best_iou >= .25 and b[0] <= pcx <= b[0] + b[2] and b[1] <= pcy <= b[1] + b[3]):
                matched.add(best)
        found = 0; missed = 0
        for i, b in enumerate(boxes):
            if b[0] + b[2] < x0 or b[0] > x1 or b[1] + b[3] < y0 or b[1] > y1:
                continue
            hit = i in matched
            rect(b, (0, 200, 0), 1 if hit else 3)
            found += hit; missed += not hit
        for pb, s in preds[info["id"]]:
            if pb[0] + pb[2] < x0 or pb[0] > x1 or pb[1] + pb[3] < y0 or pb[1] > y1:
                continue
            colour = (0, 0, 255) if s >= conf else (0, 140, 255)
            p1 = rect(pb, colour, 1)
            cv2.putText(crop, f"{s:.2f}", (p1[0], max(10, p1[1] - 3)), cv2.FONT_HERSHEY_SIMPLEX, .4, colour, 1)
        tile[BAND:BAND + crop.shape[0], :crop.shape[1]] = crop
        label = f"cam{cam} f{frame}  balls {len(cluster) if cluster else 0} overlapping  found {found}  missed {missed}"
        cv2.putText(tile, label, (4, 16), cv2.FONT_HERSHEY_SIMPLEX, .45, (0, 0, 0) if not missed else (0, 0, 200), 1)
        print(label, "| preds:", ", ".join(f"{s:.2f}" for pb, s in preds[info["id"]] if not (pb[0] + pb[2] < x0 or pb[0] > x1 or pb[1] + pb[3] < y0 or pb[1] > y1)))
        row.append(tile)
    rows.append(np.hstack(row))
canvas = np.vstack(rows)
cv2.imwrite(out, canvas)
print("->", out, canvas.shape)
