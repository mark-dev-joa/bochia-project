"""COCO mAP of a results file against a split of the labeler export: python coco_map.py <export> <split> <results.json>"""
import pathlib
import sys

from faster_coco_eval import COCO, COCOeval_faster

export = pathlib.Path(sys.argv[1]); split = sys.argv[2]
gt = COCO(str(export / "annotations" / f"instances_detection_{split}.json"))
dt = gt.loadRes(sys.argv[3])
evaluator = COCOeval_faster(gt, dt, "bbox", print_function=lambda *a, **k: None)
evaluator.evaluate(); evaluator.accumulate(); evaluator.summarize()
print(f"split {split} · mAP50 {evaluator.stats[1]:.4f} · mAP50-95 {evaluator.stats[0]:.4f}")
