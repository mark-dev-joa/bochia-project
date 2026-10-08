"""Draw the labels (green) and the model's predictions (red, with confidence) on chosen test images, cropped around the
balls, so a miss can be looked at. Usage: python show_misses.py <weights.pt> <export> <out dir> <image stem> [stem ...]"""
import pathlib
import sys

import cv2
from ultralytics import YOLO

weights, export, out = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
model = YOLO(weights)
for stem in sys.argv[4:]:
    image_path = export / "images" / "test" / (stem + ".png")
    image = cv2.imread(str(image_path))
    height, width = image.shape[:2]
    labels = []
    label_file = export / "labels" / "test" / (stem + ".txt")
    for line in label_file.read_text().split("\n") if label_file.exists() else []:
        parts = line.split()
        if len(parts) == 5:
            _, cx, cy, w, h = map(float, parts)
            labels.append((cx * width, cy * height, w * width, h * height))
    result = model.predict(str(image_path), conf=0.05, imgsz=1280, verbose=False)[0]
    for cx, cy, w, h in labels:
        cv2.rectangle(image, (int(cx - w / 2), int(cy - h / 2)), (int(cx + w / 2), int(cy + h / 2)), (0, 220, 0), 2)
    for (x1, y1, x2, y2), conf in zip(result.boxes.xyxy.tolist(), result.boxes.conf.tolist()):
        colour = (0, 0, 255) if conf >= 0.25 else (0, 165, 255)
        cv2.rectangle(image, (int(x1), int(y1)), (int(x2), int(y2)), colour, 1)
        cv2.putText(image, f"{conf:.2f}", (int(x1), int(y1) - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.45, colour, 1, cv2.LINE_AA)
    xs = [l[0] for l in labels] + [(b[0] + b[2]) / 2 for b in result.boxes.xyxy.tolist()]
    ys = [l[1] for l in labels] + [(b[1] + b[3]) / 2 for b in result.boxes.xyxy.tolist()]
    if xs:
        x0, x1 = max(0, int(min(xs) - 160)), min(width, int(max(xs) + 160))
        y0, y1 = max(0, int(min(ys) - 120)), min(height, int(max(ys) + 120))
        image = image[y0:y1, x0:x1]
    cv2.imwrite(str(out / (stem + ".png")), image)
    print(stem, "labels", len(labels), "predictions", [round(c, 2) for c in result.boxes.conf.tolist()])
