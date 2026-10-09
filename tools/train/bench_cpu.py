"""CPU inference time per image of an exported ONNX detector at its own input size: ONNX Runtime with 4/8/16 threads, and
optionally PyTorch CPU for comparison.
python bench_cpu.py [model=D:/works/bochia/data/models/boccia/yolo26n-20261009.onnx] [weights=<best.pt for the PyTorch row>] [images=15]
"""
import glob
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

options = dict(item.split("=", 1) for item in sys.argv[1:])
model = options.get("model", "D:/works/bochia/data/models/boccia/yolo26n-20261009.onnx")
weights = options.get("weights"); count = int(options.get("images", 15))
probe = ort.InferenceSession(model, providers=["CPUExecutionProvider"]); shape = probe.get_inputs()[0].shape
size = shape[2] if isinstance(shape[2], int) else 1280
files = sorted(glob.glob("export/20261009-021405-bed880d6/images/test/*.png"))[:count]
images = []
for f in files:
    im = cv2.imread(f); h, w = im.shape[:2]; s = size / max(h, w); nw, nh = round(w * s), round(h * s)
    canvas = np.full((size, size, 3), 114, np.uint8); px, py = (size - nw) // 2, (size - nh) // 2
    canvas[py:py + nh, px:px + nw] = cv2.resize(im, (nw, nh))
    images.append(np.ascontiguousarray(canvas[:, :, ::-1].transpose(2, 0, 1)[None]).astype(np.float32) / 255)
print(f"{model} · input {size}")
for threads in (4, 8, 16):
    so = ort.SessionOptions(); so.intra_op_num_threads = threads
    session = ort.InferenceSession(model, so, providers=["CPUExecutionProvider"]); name = session.get_inputs()[0].name
    session.run(None, {name: images[0]}); session.run(None, {name: images[1]})
    started = time.perf_counter()
    for x in images:
        session.run(None, {name: x})
    print(f"ONNX Runtime CPU, {threads:2d} threads: {(time.perf_counter() - started) / len(images) * 1000:.0f} ms per image")

if weights:
    from ultralytics import YOLO

    yolo = YOLO(weights)
    yolo.predict(files[:2], imgsz=size, device="cpu", verbose=False)
    started = time.perf_counter()
    yolo.predict(files, imgsz=size, device="cpu", verbose=False)
    print(f"PyTorch CPU (ultralytics, incl. pre/post): {(time.perf_counter() - started) / len(files) * 1000:.0f} ms per image")
