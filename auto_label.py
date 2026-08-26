"""고전 검출기로 YOLO 학습 데이터셋을 자동 생성한다.

고전 파이프라인이 이미 코트 위에서는 정확하므로, 그 결과를 그대로 라벨로 쓴다
(teacher-student). 사람은 전수 라벨링 대신 review/ 이미지만 훑어보고 틀린 것을
지우면 된다.

    # 1) 자동 라벨 생성 (영상 1개는 검증용으로 홀드아웃)
    python auto_label.py --holdout temp-07282026184602.mp4

    # 2) review/ 를 눈으로 확인하고 틀린 프레임은 지운다
    python auto_label.py --prune dataset

    # 3) 학습
    yolo detect train data=dataset/data.yaml model=yolo11n.pt imgsz=1280 epochs=100
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from ball_detector import DetectorConfig, detect_balls, draw_balls, find_ankles
from ball_finder import load_config
from video_player import DEFAULT_POSE_WEIGHT, find_local_pose_weight, find_video_files

CLASS_NAMES = ["ball"]


def to_yolo_line(ball, width: int, height: int, padding: float = 1.15) -> str:
    box = ball.radius * 2 * padding
    return (
        f"0 {ball.x / width:.6f} {ball.y / height:.6f} "
        f"{min(1.0, box / width):.6f} {min(1.0, box / height):.6f}"
    )


def prepare_directories(root: Path) -> None:
    for split in ("train", "val"):
        (root / "images" / split).mkdir(parents=True, exist_ok=True)
        (root / "labels" / split).mkdir(parents=True, exist_ok=True)
    (root / "review").mkdir(parents=True, exist_ok=True)


def write_data_yaml(root: Path) -> None:
    (root / "data.yaml").write_text(
        f"path: {root.resolve()}\n"
        "train: images/train\n"
        "val: images/val\n"
        f"nc: {len(CLASS_NAMES)}\n"
        f"names: {CLASS_NAMES}\n"
    )


def load_pose_model(enabled: bool):
    if not enabled:
        return None
    from ultralytics import YOLO

    weight = find_local_pose_weight()
    return YOLO(str(weight) if weight else DEFAULT_POSE_WEIGHT)


def label_video(
    video: Path,
    root: Path,
    split: str,
    interval: int,
    min_balls: int,
    max_balls: int,
    pose_model,
    review_every: int,
) -> dict:
    import cv2

    court, config = load_config(video.name)
    if court is None:
        print(f"  ! {video.name}: 저장된 코트가 없습니다. ball_finder.py 에서 지정 후 Save Cfg 하세요.")
        return {"video": video.name, "kept": 0, "skipped": 0, "reason": "no court"}

    capture = cv2.VideoCapture(str(video))
    total = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    kept = skipped = 0
    reviewed: list[str] = []

    for index in range(0, total, interval):
        capture.set(cv2.CAP_PROP_POS_FRAMES, index)
        ok, frame = capture.read()
        if not ok:
            break

        ankles = find_ankles(frame, pose_model) if pose_model else ()
        balls, _counts = detect_balls(frame, court_polygon=court, ankles=ankles, config=config)

        if not (min_balls <= len(balls) <= max_balls):
            skipped += 1
            continue

        height, width = frame.shape[:2]
        stem = f"{video.stem}_{index:06d}"
        cv2.imwrite(str(root / "images" / split / f"{stem}.jpg"), frame,
                    [cv2.IMWRITE_JPEG_QUALITY, 92])
        (root / "labels" / split / f"{stem}.txt").write_text(
            "\n".join(to_yolo_line(ball, width, height) for ball in balls) + "\n"
        )
        if kept % review_every == 0:
            cv2.imwrite(str(root / "review" / f"{stem}.jpg"),
                        draw_balls(frame, balls, court))
            reviewed.append(stem)
        kept += 1

    capture.release()
    print(f"  {video.name} [{split}]: {kept} 장 생성, {skipped} 장 제외")
    return {
        "video": video.name,
        "split": split,
        "kept": kept,
        "skipped": skipped,
        "reviewed": reviewed,
    }


def prune(root: Path) -> None:
    """review/ 에서 사람이 지운 프레임을 데이터셋에서도 제거한다.

    review 이미지가 생성된 프레임만 검수 대상이다. 그 중 사람이 삭제한 것
    (= review 에 더 이상 없는 것) 을 images/labels 에서도 지운다.
    """
    manifest_path = root / "reviewed.json"
    if not manifest_path.exists():
        print(f"검수 대상 목록이 없습니다: {manifest_path}")
        return

    reviewed = set(json.loads(manifest_path.read_text()))
    surviving = {path.stem for path in (root / "review").glob("*.jpg")}
    rejected = reviewed - surviving
    removed = 0

    for split in ("train", "val"):
        for stem in rejected:
            image_path = root / "images" / split / f"{stem}.jpg"
            label_path = root / "labels" / split / f"{stem}.txt"
            if image_path.exists():
                image_path.unlink()
                label_path.unlink(missing_ok=True)
                removed += 1

    manifest_path.write_text(json.dumps(sorted(surviving), indent=2))
    print(f"검수 대상 {len(reviewed)}장 중 {len(rejected)}장 반려 -> {removed}장 삭제")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path("dataset"))
    parser.add_argument("--interval", type=int, default=40, help="샘플링 간격 (프레임)")
    parser.add_argument("--holdout", default=None, help="검증용으로 뺄 영상 파일명")
    parser.add_argument("--min-balls", type=int, default=1)
    parser.add_argument("--max-balls", type=int, default=7)
    parser.add_argument("--no-pose", action="store_true", help="발목 veto 끄기 (빠르지만 신발 오검출 증가)")
    parser.add_argument("--review-every", type=int, default=1, help="N장마다 1장 review 이미지 생성")
    parser.add_argument("--prune", type=Path, default=None, help="review 검수 결과를 데이터셋에 반영")
    args = parser.parse_args()

    if args.prune:
        prune(args.prune)
        return 0

    videos = find_video_files(Path.cwd())
    if not videos:
        print("영상 파일을 찾지 못했습니다.")
        return 1

    prepare_directories(args.root)
    pose_model = load_pose_model(not args.no_pose)
    print(f"자동 라벨링 시작: {len(videos)}개 영상, 간격 {args.interval}프레임")

    summary = []
    for video in videos:
        split = "val" if args.holdout and video.name == args.holdout else "train"
        summary.append(
            label_video(video, args.root, split, args.interval,
                        args.min_balls, args.max_balls, pose_model, args.review_every)
        )

    write_data_yaml(args.root)
    reviewed = sorted(stem for item in summary for stem in item.get("reviewed", []))
    (args.root / "reviewed.json").write_text(json.dumps(reviewed, indent=2))
    (args.root / "summary.json").write_text(
        json.dumps([{k: v for k, v in item.items() if k != "reviewed"} for item in summary],
                   indent=2, ensure_ascii=False)
    )

    total_kept = sum(item.get("kept", 0) for item in summary)
    print(f"\n총 {total_kept}장 생성 -> {args.root}")
    print(f"review/ 를 확인하고 틀린 라벨은 삭제한 뒤 학습하세요.")
    print(f"  yolo detect train data={args.root}/data.yaml model=yolo11n.pt imgsz=1280 epochs=100")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
