"""Predictions of an exported ONNX detector on one split of the labeler export, as COCO results (image_id, bbox xywh,
score), run the way the calibration app runs it: letterbox to the model size with grey (114) padding, CPU.

python onnx_predict.py <model.onnx> <export folder> <split> <out.json> [conf=0.05] [iou=0.5]
Handles the float input ([1,3,S,S], /255 done here) and the byte input of the -u8 variants ([1,S,S,3] RGB), the NMS
export ([1,N,6]: x1 y1 x2 y2 conf cls) and the raw export ([1,4+nc,N], greedy NMS at `iou`).
"""
import json
import pathlib
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

model, export, split, out = sys.argv[1:5]
export = pathlib.Path(export)
options = dict(item.split("=", 1) for item in sys.argv[5:])
conf = float(options.get("conf", 0.05)); iou_nms = float(options.get("iou", 0.5))
session = ort.InferenceSession(model, providers=["CPUExecutionProvider"])
inp = session.get_inputs()[0]; bytes_in = inp.type == "tensor(uint8)"
size = inp.shape[1] if bytes_in else inp.shape[2]
size = size if isinstance(size, int) else 1280
gt = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))


def nms(boxes, scores, thr):
    order = np.argsort(-scores); keep = []
    while order.size:
        i = order[0]; keep.append(int(i))
        if order.size == 1:
            break
        rest = order[1:]
        xx1 = np.maximum(boxes[i, 0], boxes[rest, 0]); yy1 = np.maximum(boxes[i, 1], boxes[rest, 1])
        xx2 = np.minimum(boxes[i, 2], boxes[rest, 2]); yy2 = np.minimum(boxes[i, 3], boxes[rest, 3])
        inter = np.clip(xx2 - xx1, 0, None) * np.clip(yy2 - yy1, 0, None)
        area_i = (boxes[i, 2] - boxes[i, 0]) * (boxes[i, 3] - boxes[i, 1])
        area_r = (boxes[rest, 2] - boxes[rest, 0]) * (boxes[rest, 3] - boxes[rest, 1])
        order = rest[inter / (area_i + area_r - inter + 1e-9) < thr]
    return keep


results = []; started = time.time()
for info in gt["images"]:
    image = cv2.imread(str(export / "images" / split / info["file_name"]), cv2.IMREAD_COLOR)
    h, w = image.shape[:2]; scale = min(size / w, size / h); nw, nh = round(w * scale), round(h * scale)
    canvas = np.full((size, size, 3), 114, np.uint8); px, py = (size - nw) // 2, (size - nh) // 2
    canvas[py:py + nh, px:px + nw] = cv2.resize(image, (nw, nh), interpolation=cv2.INTER_LINEAR)
    rgb = np.ascontiguousarray(canvas[:, :, ::-1])
    blob = rgb[None] if bytes_in else np.ascontiguousarray(rgb.transpose(2, 0, 1)[None]).astype(np.float32) / 255
    output = session.run(None, {inp.name: blob})[0]
    if output.ndim == 3 and output.shape[2] == 6:
        rows = output[0]; boxes = rows[:, :4]; scores = rows[:, 4]
        keep = scores >= conf; boxes, scores = boxes[keep], scores[keep]
    else:
        rows = output[0].T; cxcywh = rows[:, :4]; scores = rows[:, 4:].max(axis=1)
        keep = scores >= conf; cxcywh, scores = cxcywh[keep], scores[keep]
        boxes = np.stack([cxcywh[:, 0] - cxcywh[:, 2] / 2, cxcywh[:, 1] - cxcywh[:, 3] / 2,
                          cxcywh[:, 0] + cxcywh[:, 2] / 2, cxcywh[:, 1] + cxcywh[:, 3] / 2], axis=1)
        if len(scores):
            idx = nms(boxes, scores, iou_nms); boxes, scores = boxes[idx], scores[idx]
    for (x1, y1, x2, y2), s in zip(boxes.tolist(), scores.tolist()):
        x1 = (x1 - px) / scale; x2 = (x2 - px) / scale; y1 = (y1 - py) / scale; y2 = (y2 - py) / scale
        results.append({"image_id": info["id"], "category_id": 1, "bbox": [x1, y1, x2 - x1, y2 - y1], "score": s})
json.dump(results, open(out, "w"))
print(f"{len(results)} predictions over {len(gt['images'])} images in {time.time() - started:.0f} s -> {out}")
