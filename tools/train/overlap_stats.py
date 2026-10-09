"""How often the labelled balls overlap each other in a split of the labeler export, and how a detector's predictions
(COCO results from onnx_predict.py / dfine_predict.py) do on the overlapping balls compared with the isolated ones.

python overlap_stats.py <export folder> <split> [predictions.json] [conf=0.25] [list=8]
A ball counts as found when a prediction above conf has IoU >= 0.5 with it, or IoU >= 0.25 with its centre inside the
ball's box (an occluded ball's box is uncertain). Misses are written next to the predictions as *.misses.json.
"""
import collections
import json
import pathlib
import sys

export = pathlib.Path(sys.argv[1]); split = sys.argv[2]
options = {}; pred_path = None
for item in sys.argv[3:]:
    if "=" in item:
        key, value = item.split("=", 1); options[key] = value
    else:
        pred_path = item
conf = float(options.get("conf", 0.25)); listing = int(options.get("list", 8))
gt = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
images = {im["id"]: im for im in gt["images"]}
balls = collections.defaultdict(list)
for a in gt["annotations"]:
    balls[a["image_id"]].append(a)


def iou(a, b):
    ix = max(0.0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])); iy = max(0.0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    inter = ix * iy; union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


def camera(name):
    i = name.find("camera-"); return name[i + 7] if i >= 0 else "?"


def bucket(m):
    if m <= 0:
        return "isolated"
    if m < 0.1:
        return "touching <0.1"
    if m < 0.3:
        return "overlap 0.1-0.3"
    return "heavy >=0.3"


order = ["isolated", "touching <0.1", "overlap 0.1-0.3", "heavy >=0.3"]
kind = {}
for image_id, items in balls.items():
    for a in items:
        m = max([iou(a["bbox"], b["bbox"]) for b in items if b is not a] or [0.0])
        kind[a["id"]] = (bucket(m), m)
total = collections.Counter(k for k, m in kind.values())
per_camera = collections.defaultdict(collections.Counter)
for image_id, items in balls.items():
    for a in items:
        per_camera[camera(images[image_id]["file_name"])][kind[a["id"]][0]] += 1
n = sum(total.values())
print(f"split {split}: {len(images)} images, {n} balls")
for k in order:
    print(f"  {k:16s} {total[k]:5d} ({100 * total[k] / max(n, 1):5.1f}%)  " + "  ".join(f"cam{c} {per_camera[c][k]}" for c in sorted(per_camera)))
images_with = sum(1 for items in balls.values() if any(kind[a["id"]][0] != "isolated" for a in items))
print(f"  images with an overlapping ball: {images_with} ({100 * images_with / max(len(images), 1):.1f}%)")
if not pred_path:
    sys.exit()
preds = collections.defaultdict(list)
for p in json.load(open(pred_path)):
    if p["score"] >= conf:
        preds[p["image_id"]].append(p)
hits = collections.Counter(); missed = []
for image_id, items in balls.items():
    matched = set()
    for p in sorted(preds.get(image_id, []), key=lambda p: -p["score"]):
        best, best_iou = None, 0.0
        for a in items:
            if a["id"] in matched:
                continue
            v = iou(p["bbox"], a["bbox"])
            if v > best_iou:
                best, best_iou = a, v
        if best is None:
            continue
        cx, cy = p["bbox"][0] + p["bbox"][2] / 2, p["bbox"][1] + p["bbox"][3] / 2
        inside = best["bbox"][0] <= cx <= best["bbox"][0] + best["bbox"][2] and best["bbox"][1] <= cy <= best["bbox"][1] + best["bbox"][3]
        if best_iou >= 0.5 or (best_iou >= 0.25 and inside):
            matched.add(best["id"])
    for a in items:
        k = kind[a["id"]][0]
        if a["id"] in matched:
            hits[k] += 1
        else:
            missed.append((k, kind[a["id"]][1], images[image_id]["file_name"], [round(v) for v in a["bbox"]]))
print(f"found at conf {conf} by overlap ({pred_path}):")
for k in order:
    print(f"  {k:16s} {hits[k]:5d}/{total[k]:5d} = {100 * hits[k] / max(total[k], 1):5.1f}%   missed {total[k] - hits[k]}")
missed.sort(key=lambda m: -m[1])
for k, m, name, box in missed[:listing]:
    print(f"  miss {k:16s} iou {m:.2f} {name} {box}")
json.dump([{"kind": k, "iou": m, "file": name, "bbox": box} for k, m, name, box in missed], open(pathlib.Path(pred_path).with_suffix(".misses.json"), "w"), indent=1)
