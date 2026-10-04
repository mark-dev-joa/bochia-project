"""Build read-only sample data for the labeler v2 UI mockup.

Reads the existing recording and annotation project. Only files beside this script
are written. The annotation project is never saved.
"""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
import cv2
from ball_labeler import Catalog
from labeler_assist import decode_mask, mask_contours

HERE = Path(__file__).resolve().parent
IMAGES = HERE / "img"
IMAGES.mkdir(exist_ok=True)
RECORDING_NUMBER = 5
MAIN_INDEX = 22
ROW_RANGE = range(12, 37)
CAMERAS = ["camera-0", "camera-1", "camera-2"]
LABELS = {"camera-0": ("C", "컬러"), "camera-1": ("A", "모노"), "camera-2": ("B", "모노")}

project_path = ROOT / "output/ball-labeler/project.json"
source_bytes = project_path.read_bytes()
project = json.loads(source_bytes)
selection = json.loads((ROOT / "output/ball-labeler/project-selection.json").read_text(encoding="utf-8"))["frames"]
catalog = Catalog(Path(project["source"]))
recordings = list(catalog.recordings.values())
recording = recordings[RECORDING_NUMBER - 1]
recording_id = recording["id"]
start_ns = catalog.frame_info(recording_id, "camera-0", 0)["host_ns"]


def scene_frames(reference_index: int) -> list[tuple[str, int, dict]]:
    reference = catalog.frame_info(recording_id, "camera-0", reference_index)["host_ns"]
    frames = []
    for camera in CAMERAS:
        index = catalog.nearest(recording_id, camera, reference)
        info = catalog.frame_info(recording_id, camera, index)
        if abs(info["host_ns"] - reference) > 20_000_000:
            raise ValueError("Cameras are outside the 20 ms receive-time window")
        frames.append((camera, index, info))
    return frames


main = {"index": MAIN_INDEX, "frames": []}
main_reference = catalog.frame_info(recording_id, "camera-0", MAIN_INDEX)["host_ns"]
for position, (camera, index, info) in enumerate(scene_frames(MAIN_INDEX)):
    annotation = project["frames"][f"{recording_id}/{camera}/{index}"]
    image = catalog.read(recording_id, camera, index)
    file_name = f"cam{position}.png"
    if not cv2.imwrite(str(IMAGES / file_name), image):
        raise ValueError("Could not save sample image")
    boxes = []
    for box in annotation["boxes"]:
        mask = decode_mask(box["segmentation"], annotation["width"], annotation["height"]) if box.get("segmentation") else None
        boxes.append({"bbox": box["bbox"], "contours": mask_contours(mask) if mask is not None else [],
                      "source": box.get("source", "manual"), "occluded": box.get("occluded", False)})
    boxes.sort(key=lambda item: item["bbox"][0])
    main["frames"].append({"key": LABELS[camera][0], "kind": LABELS[camera][1], "file": f"img/{file_name}",
                           "width": annotation["width"], "height": annotation["height"], "index": index,
                           "delta_ms": round((info["host_ns"] - main_reference) / 1e6, 2), "boxes": boxes})

rows = []
for reference_index in ROW_RANGE:
    counts, selected = [], []
    for camera, index, _ in scene_frames(reference_index):
        key = f"{recording_id}/{camera}/{index}"
        counts.append(len(project["frames"][key]["boxes"]))
        selected.append(selection.get(key, {}).get("selected", True))
    thumb = cv2.resize(catalog.read(recording_id, "camera-0", reference_index), (128, 96), interpolation=cv2.INTER_AREA)
    file_name = f"t{reference_index:04d}.jpg"
    cv2.imwrite(str(IMAGES / file_name), thumb, [cv2.IMWRITE_JPEG_QUALITY, 88])
    seconds = (catalog.frame_info(recording_id, "camera-0", reference_index)["host_ns"] - start_ns) / 1e9
    rows.append({"index": reference_index, "seconds": round(seconds, 2), "counts": counts,
                 "selected": selected, "thumb": f"img/{file_name}"})

from datetime import datetime
recs = []
for number, item in enumerate(recordings, 1):
    folder = catalog.source / item["relative_path"]
    session = json.loads((folder / "session.json").read_text(encoding="utf-8-sig"))
    times = item["cameras"]["camera-0"]["times"]
    keys = [key for key, frame in project["frames"].items() if frame["recording"] == item["id"]]
    chosen = [key for key in keys if selection.get(key, {}).get("selected", True)]
    scenes = {project["frames"][key]["index"] for key in chosen}
    started = datetime.fromtimestamp(int(item["name"].split("-")[-1]) / 1000)
    recs.append({"number": number, "name": item["name"], "group": item["relative_path"].split("/")[0],
                 "started": started.strftime("%m-%d %H:%M:%S"), "seconds": round((times[-1] - times[0]) / 1e9, 1),
                 "frames": [len(camera["frames"]) for camera in item["cameras"].values()],
                 "megabytes": round(session.get("bytes", 0) / 1e6), "complete": bool(session.get("complete")),
                 "gaps": sum(session.get("gaps", [])), "invalid": sum(session.get("invalid", [])),
                 "images": len(keys), "boxes": sum(len(project["frames"][key]["boxes"]) for key in keys),
                 "empty": sum(1 for key in keys if not project["frames"][key]["boxes"]),
                 "selected": len(chosen), "scenes": len(scenes),
                 "reviewed": sum(1 for key in chosen if project["frames"][key]["reviewed"]),
                 "sboxes": sum(len(project["frames"][key]["boxes"]) for key in chosen),
                 "sempty": sum(1 for key in chosen if not project["frames"][key]["boxes"]),
                 "ssmall": sum(1 for key in chosen for box in project["frames"][key]["boxes"] if max(box["bbox"][2:]) < 32),
                 "split": project["splits"].get(item["id"], "unassigned")})
cameras = [{"name": camera["name"], "kind": camera["kind"], "serial": camera.get("serial")} for camera in recordings[0]["cameras"].values()]
frame_counts = [sum(1 for frame in project["frames"].values() if frame["recording"] == item["id"] and frame["camera"] == "camera-0")
                for item in recordings]
payload = {"recording_number": RECORDING_NUMBER, "recording_scenes": frame_counts[RECORDING_NUMBER - 1],
           "next_recording_scenes": frame_counts[RECORDING_NUMBER] if RECORDING_NUMBER < len(frame_counts) else 0,
           "recordings": len(recordings), "main": main, "rows": rows, "recs": recs, "cameras": cameras,
           "source": str(catalog.source).replace("\\", "/"),
           "totals": {"images": len(project["frames"]), "selected": sum(1 for value in selection.values() if value.get("selected", True)),
                      "boxes": sum(len(frame["boxes"]) for frame in project["frames"].values())}}
(HERE / "mock-data.js").write_text("const MOCK = " + json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + ";\n", encoding="utf-8")
assert project_path.read_bytes() == source_bytes, "Source annotations changed during sample extraction"
print(json.dumps({"rows": len(rows), "main_boxes": [len(frame["boxes"]) for frame in main["frames"]],
                  "deltas_ms": [frame["delta_ms"] for frame in main["frames"]], "totals": payload["totals"],
                  "source_annotation_sha256": hashlib.sha256(source_bytes).hexdigest()}, ensure_ascii=False))
