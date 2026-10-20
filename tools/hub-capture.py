"""hub-capture.py <port> <worker id> <folder>: picture a running hub's screen (workers list, the worker's activity and
progress) at 1440x900 through headless Edge, once each page shows its data. Writes hub-list.png, hub-activity.png and
hub-progress.png into the folder."""
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from hub.tests.edge import HeadlessEdge  # noqa: E402


def main(port, worker, folder):
    out = Path(folder)
    out.mkdir(parents=True, exist_ok=True)
    edge = HeadlessEdge()
    try:
        link = urllib.parse.quote(worker)
        for route, name, wait in (("#/workers", "list", worker.split("@")[0]), (f"#/workers/{link}/activity", "activity", "장면 확인"),
                                  (f"#/workers/{link}/progress", "progress", "날짜별 확인")):
            edge.capture(f"http://127.0.0.1:{port}/{route}", out / f"hub-{name}.png", wait_for=wait)
            print(out / f"hub-{name}.png")
    finally:
        edge.close()


if __name__ == "__main__":
    main(*sys.argv[1:])
