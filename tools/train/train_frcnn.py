"""Faster R-CNN (torchvision, ResNet-50 FPN v2, COCO-pretrained) fine-tuned on the labeler's export, for the comparison
with YOLO26, RT-DETR and D-FINE. Trains on train, picks the epoch by val, and writes the test predictions as COCO results
so eval_centres_coco.py can score them like the others.

Usage: python train_frcnn.py <export folder> <out dir> [epochs=12] [max_size=1280] [batch=4]
Everything but the dataset, collate and helper functions lives in main(): on Windows the DataLoader workers re-import
this module, so module-level training code would start again in every worker.
"""
import json
import pathlib
import sys
import time

import torch
import torchvision
from torchvision.models.detection import fasterrcnn_resnet50_fpn_v2, FasterRCNN_ResNet50_FPN_V2_Weights
from torchvision.models.detection.faster_rcnn import FastRCNNPredictor
from torchvision.io import decode_image


class Split(torch.utils.data.Dataset):
    """One split of the export: the image and its boxes from the COCO detection annotations (label 1 = ball)."""
    def __init__(self, export, split):
        data = json.load(open(export / "annotations" / f"instances_detection_{split}.json", encoding="utf-8"))
        self.images = data["images"]
        self.boxes = {}
        for a in data["annotations"]:
            x, y, w, h = a["bbox"]
            self.boxes.setdefault(a["image_id"], []).append([x, y, x + w, y + h])
        self.folder = export / "images" / split

    def __len__(self):
        return len(self.images)

    def __getitem__(self, index):
        info = self.images[index]
        image = decode_image(str(self.folder / info["file_name"]), mode=torchvision.io.ImageReadMode.RGB).float() / 255
        boxes = torch.tensor(self.boxes.get(info["id"], []), dtype=torch.float32).reshape(-1, 4)
        target = {"boxes": boxes, "labels": torch.ones((len(boxes),), dtype=torch.int64), "image_id": info["id"]}
        return image, target


def collate(items):
    return tuple(zip(*items))


def loader(export, split, batch, shuffle):
    return torch.utils.data.DataLoader(Split(export, split), batch_size=batch, shuffle=shuffle, num_workers=4, collate_fn=collate, persistent_workers=True)


@torch.no_grad()
def predict(model, export, split, batch, device, threshold=0.05):
    """COCO results (image_id, bbox xywh, score, category 1) for a split."""
    model.eval()
    results = []
    for images, targets in loader(export, split, batch, False):
        outputs = model([image.to(device) for image in images])
        for target, output in zip(targets, outputs):
            for box, score in zip(output["boxes"].tolist(), output["scores"].tolist()):
                if score < threshold:
                    continue
                x1, y1, x2, y2 = box
                results.append({"image_id": target["image_id"], "category_id": 1, "bbox": [x1, y1, x2 - x1, y2 - y1], "score": score})
    return results


def coco_map(export, split, results):
    from faster_coco_eval import COCO, COCOeval_faster
    gt = COCO(str(export / "annotations" / f"instances_detection_{split}.json"))
    if not results:
        return 0.0, 0.0
    dt = gt.loadRes(results)
    evaluator = COCOeval_faster(gt, dt, "bbox", print_function=lambda *a, **k: None)
    evaluator.evaluate(); evaluator.accumulate(); evaluator.summarize()
    return float(evaluator.stats[1]), float(evaluator.stats[0])   # mAP50, mAP50-95


def main():
    export = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
    options = dict(item.split("=", 1) for item in sys.argv[3:])
    epochs = int(options.get("epochs", 12)); max_size = int(options.get("max_size", 1280)); batch = int(options.get("batch", 4))
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    model = fasterrcnn_resnet50_fpn_v2(weights=FasterRCNN_ResNet50_FPN_V2_Weights.DEFAULT, min_size=max_size * 3 // 4, max_size=max_size)
    model.roi_heads.box_predictor = FastRCNNPredictor(model.roi_heads.box_predictor.cls_score.in_features, 2)   # background + ball
    model.to(device)
    optimizer = torch.optim.SGD([p for p in model.parameters() if p.requires_grad], lr=0.01, momentum=0.9, weight_decay=1e-4)
    schedule = torch.optim.lr_scheduler.MultiStepLR(optimizer, milestones=[int(epochs * .7), int(epochs * .9)], gamma=0.1)
    scaler = torch.amp.GradScaler("cuda", enabled=device.type == "cuda")

    history = []
    best = (-1.0, None)
    train_loader = loader(export, "train", batch, True)
    for epoch in range(1, epochs + 1):
        model.train()
        started = time.time(); total = 0.0; steps = 0
        for images, targets in train_loader:
            images = [image.to(device) for image in images]
            targets = [{"boxes": t["boxes"].to(device), "labels": t["labels"].to(device)} for t in targets]
            with torch.autocast("cuda", enabled=device.type == "cuda"):
                losses = model(images, targets)
                loss = sum(losses.values())
            optimizer.zero_grad(set_to_none=True)
            scaler.scale(loss).backward()
            scaler.step(optimizer); scaler.update()
            total += float(loss); steps += 1
            if steps % 100 == 0:
                print(f"epoch {epoch} step {steps}/{len(train_loader)} loss {total / steps:.3f} {time.time() - started:.0f} s", flush=True)
        schedule.step()
        map50, map5095 = coco_map(export, "val", predict(model, export, "val", batch, device))
        history.append({"epoch": epoch, "loss": total / max(1, steps), "val_mAP50": map50, "val_mAP50-95": map5095, "seconds": time.time() - started})
        print(json.dumps(history[-1]), flush=True)
        if map5095 > best[0]:
            best = (map5095, epoch)
            torch.save(model.state_dict(), out / "best.pt")
        json.dump(history, open(out / "history.json", "w"), indent=1)

    model.load_state_dict(torch.load(out / "best.pt", map_location=device))
    test_results = predict(model, export, "test", batch, device)
    json.dump(test_results, open(out / "test_results.json", "w"))
    map50, map5095 = coco_map(export, "test", test_results)
    summary = {"best_epoch": best[1], "val_mAP50-95": best[0], "test_mAP50": map50, "test_mAP50-95": map5095, "epochs": epochs, "max_size": max_size}
    json.dump(summary, open(out / "summary.json", "w"), indent=1)
    print("summary", json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
