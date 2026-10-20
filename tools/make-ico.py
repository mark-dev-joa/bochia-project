"""make-ico.py <icon.svg> <icon.ico> [<icon.png>]: render an SVG icon with headless Edge at the Windows icon sizes and pack
the PNGs into one .ico (PNG entries, Windows Vista and newer); the 256 px PNG is written too when asked.
Standard library only (headless Edge through hub/tests/edge.py)."""
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from hub.tests.edge import HeadlessEdge  # noqa: E402

SIZES = (16, 24, 32, 48, 64, 128, 256)


def main(svg, ico, png=None):
    svg, ico = Path(svg).resolve(), Path(ico)
    edge = HeadlessEdge()
    images = []
    try:
        with tempfile.TemporaryDirectory() as folder:
            for size in SIZES:
                page = Path(folder) / f"icon-{size}.html"
                page.write_text(f'<!doctype html><html><body style="margin:0;background:transparent">'
                                f'<img src="{svg.as_uri()}" width="{size}" height="{size}" style="display:block"></body></html>', "utf-8")
                shot = Path(folder) / f"icon-{size}.png"
                edge.capture(page.as_uri(), shot, width=size, height=size, transparent=True)
                images.append((size, shot.read_bytes()))
    finally:
        edge.close()
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        side = 0 if size >= 256 else size   # 0 means 256 in an icon directory
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    ico.write_bytes(header + entries + blobs)
    if png:
        Path(png).write_bytes(images[-1][1])
    print(f"{ico}: {', '.join(str(size) for size, _ in images)} px")


if __name__ == "__main__":
    main(*sys.argv[1:])
