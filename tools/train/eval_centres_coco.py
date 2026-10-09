"""Score COCO-format predictions (image_id, bbox xywh, score) against a split of the labeler export the way the research
cares: balls found and missed, false balls, centre error per camera. The same rule as eval_centres.py: a prediction hits
when its centre lies within the labelled ball's radius.

Usage: python eval_centres_coco.py <export folder> <split> <results.json> [conf=0.25]
"""
import collections
import json
import pathlib
import statistics
import sys

export = pathlib.Path(sys.argv[1]); split = sys.argv[2]; results = json.load(open(sys.argv[3]))
options = dict(item.split("=", 1) for item in sys.argv[4:]); conf = float(options.get("conf", 0.25))
data = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
images = {i["id"]: i for i in data["images"]}
labels = collections.defaultdict(list)
for a in data["annotations"]:
    x, y, w, h = a["bbox"]; labels[a["image_id"]].append((x + w / 2, y + h / 2, min(w, h) / 2))
predictions = collections.defaultdict(list)
for r in results:
    if r["score"] >= conf:
        x, y, w, h = r["bbox"]; predictions[r["image_id"]].append((x + w / 2, y + h / 2))
per = collections.defaultdict(lambda: {"labels": 0, "hits": 0, "false": 0, "errors": []})
for image_id, info in images.items():
    camera = info.get("camera_id", info["file_name"].split("_")[1])
    s = per[camera]; preds = predictions.get(image_id, []); used = [False] * len(preds)
    s["labels"] += len(labels.get(image_id, []))
    for lx, ly, r in labels.get(image_id, []):
        best, bd = -1, None
        for i, (px, py) in enumerate(preds):
            if used[i]: continue
            d = ((px - lx) ** 2 + (py - ly) ** 2) ** .5
            if d <= max(r, 4) and (best < 0 or d < bd): best, bd = i, d
        if best >= 0: used[best] = True; s["hits"] += 1; s["errors"].append(bd)
    s["false"] += sum(1 for u in used if not u)
print(f"split {split} · images {len(images)} · conf {conf}")
print(f"{'camera':10} {'labels':>7} {'found':>7} {'missed':>7} {'false':>6} {'recall':>7} {'centre err median':>18} {'p95':>6}")
total = {"labels": 0, "hits": 0, "false": 0, "errors": []}
for camera in sorted(per):
    s = per[camera]; e = s["errors"] or [0.0]; p95 = sorted(e)[max(0, int(len(e) * .95) - 1)]
    print(f"{camera:10} {s['labels']:7} {s['hits']:7} {s['labels'] - s['hits']:7} {s['false']:6} {s['hits'] / max(1, s['labels']):7.3f} {statistics.median(e):18.2f} {p95:6.2f}")
    for k in ("labels", "hits", "false"): total[k] += s[k]
    total["errors"] += s["errors"]
e = total["errors"] or [0.0]
print(f"{'all':10} {total['labels']:7} {total['hits']:7} {total['labels'] - total['hits']:7} {total['false']:6} {total['hits'] / max(1, total['labels']):7.3f} {statistics.median(e):18.2f}")
