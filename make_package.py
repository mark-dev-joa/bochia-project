"""배포용 압축 파일을 만든다.

    python make_package.py                 # 소스 + 영상 전부
    python make_package.py --no-videos     # 소스만 (수 MB)
    python make_package.py --dist dist/BochiaBallFinder   # 빌드된 exe 폴더 포함

영상(mp4)은 이미 압축된 포맷이라 다시 압축해도 줄지 않는다. 그래서 저장(STORED)
방식으로 담는다 - 압축 시간만 낭비하지 않고 크기는 같다.
"""
from __future__ import annotations

import argparse
import zipfile
from pathlib import Path

SOURCE_FILES = [
    "hough_viewer.py",
    "ball_detector.py",
    "ball_finder.py",
    "auto_label.py",
    "video_player.py",
    "requirements.txt",
    "hough_viewer.spec",
]
OPTIONAL_FILES = ["hough_viewer_settings.json", "ball_finder_config.json"]
VIDEO_SUFFIXES = {".mp4", ".avi", ".mov", ".mkv"}


def human(size: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f}{unit}"
        size /= 1024
    return f"{size:.1f}TB"


def add(archive: zipfile.ZipFile, path: Path, arcname: str, store: bool = False) -> int:
    archive.write(path, arcname,
                  compress_type=zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED)
    return path.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("BochiaBallFinder.zip"))
    parser.add_argument("--no-videos", action="store_true", help="영상 제외")
    parser.add_argument("--dist", type=Path, default=None, help="빌드된 exe 폴더")
    args = parser.parse_args()

    root = Path.cwd()
    raw = 0

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in SOURCE_FILES + OPTIONAL_FILES:
            path = root / name
            if path.exists():
                raw += add(archive, path, f"BochiaBallFinder/{name}")
                print(f"  + {name}")

        if args.dist and args.dist.exists():
            for path in sorted(args.dist.rglob("*")):
                if path.is_file():
                    raw += add(archive, path,
                               f"BochiaBallFinder/app/{path.relative_to(args.dist)}", store=True)
            print(f"  + {args.dist}/  (exe 폴더)")

        if not args.no_videos:
            videos = sorted(p for p in root.iterdir()
                            if p.is_file() and p.suffix.lower() in VIDEO_SUFFIXES)
            for path in videos:
                raw += add(archive, path, f"BochiaBallFinder/{path.name}", store=True)
                print(f"  + {path.name}  ({human(path.stat().st_size)})")

        archive.writestr("BochiaBallFinder/README.txt",
                         "보치아 공 검출기\n\n"
                         "exe 로 실행하는 경우: app/BochiaBallFinder.exe 를 실행하세요.\n"
                         "영상 파일은 exe 와 같은 폴더에 두어야 목록에 나타납니다.\n\n"
                         "파이썬으로 실행하는 경우:\n"
                         "  pip install -r requirements.txt pillow\n"
                         "  python hough_viewer.py --width 1600\n")

    packed = args.out.stat().st_size
    print(f"\n{args.out}  {human(packed)}  (원본 합계 {human(raw)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
