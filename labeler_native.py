"""Native client's scene index and review-then-group-split operations.

The GUI uses the same Project and atomic annotation store as the browser.
No UI action writes the original recording files.
"""
import bisect
import copy
import hashlib
import json
from datetime import datetime

import cv2


def scene_index(catalog):
    """Match each source frame once; unmatched frames stay visible as sparse scenes."""
    scenes = []
    for recording_number, recording in enumerate(catalog.recordings.values(), 1):
        cameras = list(recording["cameras"].values())
        anchor = cameras[0]
        rows = [{"time": time, "members": {anchor["id"]: index}} for index, time in enumerate(anchor["times"])]
        times = anchor["times"]
        for camera in cameras[1:]:
            for index, time in enumerate(camera["times"]):
                pos = bisect.bisect_left(times, time)
                candidates = sorted({max(0, min(len(times) - 1, pos + offset)) for offset in (-1, 0, 1)},
                                    key=lambda number: abs(times[number] - time))
                match = next((number for number in candidates if abs(times[number] - time) <= 20_000_000
                              and camera["id"] not in rows[number]["members"]), None)
                if match is None:
                    rows.append({"time": time, "members": {camera["id"]: index}})
                else:
                    rows[match]["members"][camera["id"]] = index
        for row in sorted(rows, key=lambda row: row["time"]):
            members = []
            for camera in cameras:
                if camera["id"] not in row["members"]:
                    continue
                index = row["members"][camera["id"]]
                members.append({"key": f"{recording['id']}/{camera['id']}/{index}", "camera": camera["id"],
                                "name": camera["name"], "kind": camera["kind"], "index": index,
                                "host_ns": str(camera["times"][index]), "delta_ms": (camera["times"][index] - row["time"]) / 1e6})
            scenes.append({"id": f"{recording['id']}/{len(scenes)}", "recording": recording["id"],
                           "recording_number": recording_number, "index": row["members"].get(anchor["id"], members[0]["index"]),
                           "host_ns": str(row["time"]), "members": members})
    return scenes


def targets(project):
    selection = project.selection.view(project.state["frames"])["frames"]
    return [member for scene in scene_index(project.catalog) for member in scene["members"]
            if selection.get(member["key"], {}).get("selected", True)]


def fingerprint(project, state=None):
    state = state or project.state
    selection = project.selection.view(state["frames"])["frames"]
    snapshot = {"source": project.catalog.signature, "frames": state["frames"],
                "selected": sorted(key for key, value in selection.items() if value["selected"])}
    return hashlib.sha256(json.dumps(snapshot, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def split_valid(project):
    confirmation = project.state.get("native_split", {})
    return bool(confirmation and confirmation.get("fingerprint") == fingerprint(project)
                and confirmation.get("splits") == project.state["splits"])


def recording_stats(project):
    """Per-recording counts of the review targets for the data and split tables."""
    stats = {}
    for number, (recording_id, recording) in enumerate(project.catalog.recordings.items(), 1):
        stamp = recording["name"].rsplit("-", 1)[-1]
        started = datetime.fromtimestamp(int(stamp) / 1000) if stamp.isdigit() and len(stamp) == 13 else None
        times = next(iter(recording["cameras"].values()))["times"]
        stats[recording_id] = {"number": number, "name": recording["name"],
                               "day": started.strftime("%Y-%m-%d") if started else None,
                               "started": started.strftime("%m-%d %H:%M:%S") if started else None,
                               "seconds": round((times[-1] - times[0]) / 1e9, 1) if started else None,
                               "frames": [len(camera["frames"]) for camera in recording["cameras"].values()],
                               "images": 0, "balls": 0, "empty": 0, "small": 0, "reviewed": 0}
    for member in targets(project):
        item = stats[member["key"].split("/")[0]]
        frame = project.state["frames"].get(member["key"], {})
        boxes = frame.get("boxes", [])
        item["images"] += 1
        item["balls"] += len(boxes)
        item["empty"] += not boxes
        item["small"] += sum(max(box.get("detection_bbox", box["bbox"])[2:]) < 32 for box in boxes)
        item["reviewed"] += bool(frame.get("reviewed"))
    return stats


def thumbnail(project, recording, camera, index, width=128):
    """Small JPEG of one source frame for the scene list. Never written to disk."""
    width = int(width)
    if not 16 <= width <= 512:
        raise ValueError("썸네일 폭이 올바르지 않습니다.")
    cache = project.__dict__.setdefault("thumbnails", {})
    key = (recording, camera, int(index), width)
    if key not in cache:
        image = project.catalog.read(recording, camera, int(index))
        height = max(1, round(image.shape[0] * width / image.shape[1]))
        ok, encoded = cv2.imencode(".jpg", cv2.resize(image, (width, height), interpolation=cv2.INTER_AREA),
                                   [cv2.IMWRITE_JPEG_QUALITY, 85])
        if not ok:
            raise ValueError("썸네일을 만들지 못했습니다.")
        cache[key] = encoded.tobytes()
    return cache[key]


def export_history(project):
    """Completed export folders, newest first."""
    entries = []
    if not project.export_root.is_dir():
        return entries
    for folder in sorted(project.export_root.iterdir(), reverse=True):
        manifest = folder / "manifest.json"
        if not manifest.is_file() or (folder / "INCOMPLETE.txt").exists():
            continue
        try:
            value = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        entries.append({"name": folder.name, "path": str(folder), "created_at": value.get("created_at"),
                        "counts": value.get("counts", {}),
                        "fingerprint": (value.get("native_split") or {}).get("fingerprint")})
    return entries


def native_catalog(project):
    with project.lock:
        summary = project.summary()
        scenes = scene_index(project.catalog)
        count = reviewed = 0
        for scene in scenes:
            for member in scene["members"]:
                entry = summary["frames"].get(member["key"], {})
                member.update(selected=entry.get("selected", True), reviewed=entry.get("reviewed", False),
                              status=entry.get("review_status", "draft"), count=entry.get("count", 0),
                              attention=entry.get("needs_attention", False))
                if member["selected"]:
                    count += 1
                    reviewed += member["reviewed"]
        # 2: recording stats, thumbnails, export history and the data version that the v2 screens need.
        return {"api_version": 2, "source": str(project.catalog.source), "project_path": str(project.path),
                "recordings": project.catalog.metadata(), "summary": summary, "scenes": scenes,
                "target_images": count, "reviewed_target_images": reviewed,
                "split_confirmed": split_valid(project), "groups": project.state.get("native_split", {}).get("groups", {}),
                "stats": recording_stats(project),
                # The data version: set only while the confirmed split still matches the labels.
                "fingerprint": project.state["native_split"]["fingerprint"] if split_valid(project) else None}


def group_plan(project, groups):
    recordings = project.catalog.recordings
    if not isinstance(groups, dict) or set(groups) != set(recordings) or any(not isinstance(group, str) or not group or len(group) > 128 for group in groups.values()):
        raise ValueError("촬영 그룹은 모든 촬영에 지정해야 합니다.")
    counts = dict.fromkeys(recordings, 0)
    for member in targets(project):
        counts[member["key"].split("/")[0]] += 1
    grouped = {}
    for recording, group in groups.items():
        grouped[group] = grouped.get(group, 0) + counts[recording]
    usable = [group for group, count in grouped.items() if count]
    if len(usable) < 3:
        raise ValueError("학습·검증·최종 평가에는 독립적인 촬영 그룹이 최소 3개 필요합니다.")
    # Reserve the groups nearest the 10% target; keep every group intact.
    remaining = set(usable)
    assignment = dict.fromkeys(grouped, "train")
    goal = sum(grouped.values()) * .1
    for split in ("test", "val"):
        chosen = min(remaining, key=lambda group: (abs(grouped[group] - goal), group))
        assignment[chosen] = split
        remaining.remove(chosen)
    splits = {recording: assignment[group] for recording, group in groups.items()}
    return splits, counts


def native_action(project, action, payload):
    if action == "review":
        keys = payload.get("keys")
        status = payload.get("status", "reviewed")
        if not isinstance(keys, list) or not 1 <= len(keys) <= 3 or len(set(keys)) != len(keys) or status not in ("reviewed", "hold"):
            raise ValueError("현재 장면의 검수 상태 요청이 올바르지 않습니다.")
        with project.lock:
            entries = []
            for key in keys:
                recording, camera, index = key.split("/")
                frame = project.frame(recording, camera, int(index))
                entries.append((key, frame))
            if len({frame["recording"] for _, frame in entries}) != 1:
                raise ValueError("한 장면의 촬영만 함께 검수할 수 있습니다.")
            if not any(set(keys).issubset({member["key"] for member in scene["members"]}) for scene in scene_index(project.catalog)):
                raise ValueError("서로 다른 시점의 프레임을 한 장면으로 검수할 수 없습니다.")
            def update(state):
                for key, frame in entries:
                    annotation = {name: copy.deepcopy(value) for name, value in frame.items()
                                  if name in ("recording", "camera", "index", "width", "height", "boxes", "automatic")}
                    # The frame response includes transient contours; never store them.
                    for box in annotation["boxes"]:
                        box.pop("contours", None)
                    annotation.update(reviewed=status == "reviewed", review_status=status, human_touched=True)
                    state["frames"][key] = annotation
                state.pop("native_split", None)
            return {"summary": project.mutate(payload["expected_revision"], update)}
    if action == "split-preview":
        with project.lock:
            groups = payload.get("groups", {recording: recording for recording in project.catalog.recordings})
            splits, counts = group_plan(project, groups)
            return {"groups": groups, "splits": splits, "counts": counts}
    if action == "split-confirm":
        with project.lock:
            unresolved = [member["key"] for member in targets(project)
                          if not project.state["frames"].get(member["key"], {}).get("reviewed", False)]
            if unresolved:
                raise ValueError(f"검수 대상 {len(unresolved)}장이 미완료입니다. 전체 검수를 먼저 완료하세요.")
            groups = payload["groups"]
            group_plan(project, groups)
            splits = payload["splits"]
            if not isinstance(splits, dict) or set(splits) != set(project.catalog.recordings) or any(value not in ("train", "val", "test") for value in splits.values()):
                raise ValueError("모든 촬영을 학습·검증·최종 평가로 지정하세요.")
            for group in set(groups.values()):
                if len({splits[recording] for recording in groups if groups[recording] == group}) != 1:
                    raise ValueError("동일 촬영 그룹을 서로 다른 분할로 나눌 수 없습니다.")
            if {splits[member["key"].split("/")[0]] for member in targets(project)} != {"train", "val", "test"}:
                raise ValueError("학습·검증·최종 평가에 검수 이미지가 각각 있어야 합니다.")
            def update(state):
                state["splits"] = dict(splits)
                state["native_split"] = {"groups": dict(groups), "splits": dict(splits),
                                         "fingerprint": fingerprint(project, state)}
            summary = project.mutate(payload["expected_revision"], update)
            return {"summary": summary, "split_confirmed": True, "fingerprint": project.state["native_split"]["fingerprint"]}
    if action == "export":
        with project.lock:
            if not split_valid(project):
                raise ValueError("전체 검수 후 데이터 분할을 확정하세요. 라벨 수정 후에는 다시 확정해야 합니다.")
            return project.start_export(bool(payload.get("crops", True)))
    raise ValueError("지원하지 않는 네이티브 작업입니다.")
