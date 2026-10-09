"""CPU inference time of the exported yolo26n at 1280: ONNX Runtime with 4/8/16 threads, and PyTorch CPU for comparison.
python bench_cpu.py  (from C:/boccia-train; the GPU is left alone)
"""
import glob
import time

import cv2
import numpy as np
import onnxruntime as ort

files = sorted(glob.glob("export/20261009-021405-bed880d6/images/test/*.png"))[:15]
images = []
for f in files:
    im = cv2.imread(f); h, w = im.shape[:2]; s = 1280 / max(h, w); nw, nh = round(w * s), round(h * s)
    canvas = np.full((1280, 1280, 3), 114, np.uint8); px, py = (1280 - nw) // 2, (1280 - nh) // 2
    canvas[py:py + nh, px:px + nw] = cv2.resize(im, (nw, nh))
    images.append(np.ascontiguousarray(canvas[:, :, ::-1].transpose(2, 0, 1)[None]).astype(np.float32) / 255)
model = "D:/works/bochia/data/models/boccia/yolo26n-20261009.onnx"
for threads in (4, 8, 16):
    so = ort.SessionOptions(); so.intra_op_num_threads = threads
    session = ort.InferenceSession(model, so, providers=["CPUExecutionProvider"]); name = session.get_inputs()[0].name
    session.run(None, {name: images[0]}); session.run(None, {name: images[1]})
    started = time.perf_counter()
    for x in images:
        session.run(None, {name: x})
    print(f"ONNX Runtime CPU, {threads:2d} threads: {(time.perf_counter() - started) / len(images) * 1000:.0f} ms per image")

from ultralytics import YOLO  # noqa: E402

yolo = YOLO("runs/detect/runs/yolo26n-e40/weights/best.pt")
yolo.predict(files[:2], imgsz=1280, device="cpu", verbose=False)
started = time.perf_counter()
yolo.predict(files, imgsz=1280, device="cpu", verbose=False)
print(f"PyTorch CPU (ultralytics, incl. pre/post): {(time.perf_counter() - started) / len(files) * 1000:.0f} ms per image")
