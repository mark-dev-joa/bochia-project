"""Select representative labeler frames without deleting any source or labels."""
import argparse
import json
import sys
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ball_labeler import Catalog, atomic_json
from labeler_selection import build_selection, selection_path


def main():
    parser = argparse.ArgumentParser(description="정지 장면의 중복 프레임 선별")
    parser.add_argument("--source", type=Path, default=ROOT / "data/imports")
    parser.add_argument("--project", type=Path, default=ROOT / "output/ball-labeler/project.json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    started = time.monotonic()
    catalog = Catalog(args.source)
    snapshot = json.loads(args.project.read_text(encoding="utf-8"))
    result = build_selection(catalog, snapshot, lambda done, total, camera: print(f"{done}/{total} {camera}", flush=True))
    result["elapsed_seconds"] = round(time.monotonic() - started, 2)
    result["reasons"] = dict(Counter(item["reason"] for item in result["frames"].values()))
    output = args.output or selection_path(args.project)
    atomic_json(output, result)
    print(json.dumps({"path": str(output.resolve()), "total": result["total"], "selected": result["selected"],
                      "excluded": result["excluded"], "reasons": result["reasons"], "elapsed_seconds": result["elapsed_seconds"]}, ensure_ascii=True), flush=True)


if __name__ == "__main__":
    main()
