"""Predictions of a trained D-FINE checkpoint on one split of the labeler export, as COCO results (image_id, bbox xywh,
score), so eval_centres_coco.py and the COCO mAP can score it like the other models.

Usage (from C:/boccia-train): python dfine_predict.py <config.yml> <checkpoint.pth> <export folder> <split> <out.json> [size=1280]
"""
import json
import os
import pathlib
import sys

import torch
import torchvision.transforms as T
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "D-FINE"))
from src.core import YAMLConfig  # noqa: E402

config, checkpoint, export, split, out = sys.argv[1:6]
export = pathlib.Path(export)
options = dict(item.split("=", 1) for item in sys.argv[6:]); size = int(options.get("size", 1280))
device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

cfg = YAMLConfig(config, resume=checkpoint)
if "HGNetv2" in cfg.yaml_cfg:
    cfg.yaml_cfg["HGNetv2"]["pretrained"] = False
state = torch.load(checkpoint, map_location="cpu", weights_only=False)
state = state["ema"]["module"] if "ema" in state else state["model"]
cfg.model.load_state_dict(state)
model = cfg.model.deploy().to(device).eval()
postprocessor = cfg.postprocessor.deploy().to(device).eval()
transform = T.Compose([T.Resize((size, size)), T.ToTensor()])

data = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
results = []
with torch.no_grad():
    for info in data["images"]:
        image = Image.open(export / "images" / split / info["file_name"]).convert("RGB")
        width, height = image.size
        tensor = transform(image).unsqueeze(0).to(device)
        labels, boxes, scores = postprocessor(model(tensor), torch.tensor([[width, height]]).to(device))
        for box, score, label in zip(boxes[0].tolist(), scores[0].tolist(), labels[0].tolist()):
            if score < 0.05:
                continue
            x1, y1, x2, y2 = box
            results.append({"image_id": info["id"], "category_id": int(label), "bbox": [x1, y1, x2 - x1, y2 - y1], "score": float(score)})
json.dump(results, open(out, "w"))
print(f"{len(results)} predictions over {len(data['images'])} images -> {out}")
