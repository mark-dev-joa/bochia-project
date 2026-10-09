"""GPU time per image of the four detectors on the same 50 test images: from the decoded image to boxes (each model's own
preprocessing, forward pass and postprocessing), batch 1, fp32, 5 warm-up runs. python bench_gpu.py  (from C:/boccia-train)
"""
import glob
import os
import sys
import time

import cv2
import numpy as np
import torch
import torch.nn.functional as F

files = sorted(glob.glob("export/20261009-021405-bed880d6/images/test/*.png"))[:50]
bgr = [cv2.imread(f) for f in files]
device = torch.device("cuda")


def timed(name, run):
    n = len(bgr)
    for i in range(5):
        run(i % n)
    torch.cuda.synchronize(); started = time.perf_counter()
    for i in range(n):
        run(i)
    torch.cuda.synchronize(); print(f"{name}: {(time.perf_counter() - started) / n * 1000:.1f} ms per image", flush=True)


from ultralytics import RTDETR, YOLO  # noqa: E402

yolo = YOLO("runs/detect/runs/yolo26n-e40/weights/best.pt")
timed("yolo26n (ultralytics, 1280)", lambda i: yolo.predict(bgr[i], imgsz=1280, device=0, verbose=False))
yolos = YOLO("runs/detect/runs/yolo26s-e40/weights/best.pt")
timed("yolo26s (ultralytics, 1280)", lambda i: yolos.predict(bgr[i], imgsz=1280, device=0, verbose=False))
rt = RTDETR("runs/detect/runs/rtdetr-l-e30/weights/best.pt")
timed("RT-DETR-L (ultralytics, 1280)", lambda i: rt.predict(bgr[i], imgsz=1280, device=0, verbose=False))

from torchvision.models.detection import fasterrcnn_resnet50_fpn_v2  # noqa: E402
from torchvision.models.detection.faster_rcnn import FastRCNNPredictor  # noqa: E402

fr = fasterrcnn_resnet50_fpn_v2(weights=None, weights_backbone=None, min_size=960, max_size=1280)
fr.roi_heads.box_predictor = FastRCNNPredictor(fr.roi_heads.box_predictor.cls_score.in_features, 2)
fr.load_state_dict(torch.load("runs/frcnn/best.pt", map_location="cpu")); fr.to(device).eval()
rgb = [torch.from_numpy(np.ascontiguousarray(im[:, :, ::-1])).permute(2, 0, 1).float().div(255) for im in bgr]
with torch.no_grad():
    timed("Faster R-CNN R50-FPN v2 (1280)", lambda i: fr([rgb[i].to(device)]))

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "D-FINE"))
from src.core import YAMLConfig  # noqa: E402

checkpoint = "runs/dfine-s/best_stg2.pth"
cfg = YAMLConfig("D-FINE/configs/dfine/custom/dfine_hgnetv2_s_boccia_b4.yml", resume=checkpoint)
if "HGNetv2" in cfg.yaml_cfg:
    cfg.yaml_cfg["HGNetv2"]["pretrained"] = False
state = torch.load(checkpoint, map_location="cpu", weights_only=False)
state = state["ema"]["module"] if "ema" in state else state["model"]
cfg.model.load_state_dict(state)
dfine = cfg.model.deploy().to(device).eval(); post = cfg.postprocessor.deploy().to(device).eval()
sizes = torch.tensor([[bgr[0].shape[1], bgr[0].shape[0]]]).to(device)


def dfine_run(i):
    x = F.interpolate(rgb[i].to(device)[None], size=(1280, 1280), mode="bilinear", align_corners=False)
    return post(dfine(x), sizes)


with torch.no_grad():
    timed("D-FINE-S (1280)", dfine_run)
