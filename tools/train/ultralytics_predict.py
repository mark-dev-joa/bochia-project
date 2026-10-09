"""Predictions of an Ultralytics checkpoint (YOLO or RT-DETR, chosen by the weights' path) on one split of the labeler
export, as COCO results (image_id = the export's image id, bbox xywh, score) for coco_map.py, eval_centres_coco.py and
overlap_stats.py.

python ultralytics_predict.py <weights.pt> <export folder> <split> <out.json> [conf=0.05] [imgsz=1280] [device=cpu] [threads=8] [batch=4]
"""
import json
import pathlib
import sys
import time

import torch

weights, export, split, out = sys.argv[1:5]
export = pathlib.Path(export)
options = dict(item.split("=", 1) for item in sys.argv[5:])
conf = float(options.get("conf", 0.05)); imgsz = int(options.get("imgsz", 1280)); device = options.get("device", "cpu")
batch = int(options.get("batch", 4)); torch.set_num_threads(int(options.get("threads", 8)))

from ultralytics import RTDETR, YOLO  # noqa: E402

model = RTDETR(weights) if "rtdetr" in weights.lower() else YOLO(weights)
gt = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
infos = gt["images"]; paths = [str(export / "images" / split / info["file_name"]) for info in infos]
results = []; started = time.time()
for i in range(0, len(paths), batch):
    chunk = paths[i:i + batch]
    for info, r in zip(infos[i:i + batch], model.predict(chunk, imgsz=imgsz, conf=conf, device=device, batch=batch, verbose=False)):
        assert pathlib.Path(r.path).name == info["file_name"], (r.path, info["file_name"])
        for (x1, y1, x2, y2), s in zip(r.boxes.xyxy.tolist(), r.boxes.conf.tolist()):
            results.append({"image_id": info["id"], "category_id": 1, "bbox": [x1, y1, x2 - x1, y2 - y1], "score": float(s)})
    if (i // batch) % 50 == 0:
        print(f"{i + len(chunk)}/{len(paths)} images, {time.time() - started:.0f} s", flush=True)
json.dump(results, open(out, "w"))
print(f"{len(results)} predictions over {len(paths)} images in {time.time() - started:.0f} s -> {out}")
