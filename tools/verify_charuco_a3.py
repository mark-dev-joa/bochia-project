from pathlib import Path
import sys
import json

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tmp/pdfs/python-deps'))
import cv2
import numpy as np
from PIL import Image, ImageOps, ImageDraw

TMP = ROOT / 'tmp/pdfs'
manifest = json.loads((TMP / 'a3-tile-manifest.json').read_text())
assembled = np.full((2700, 3200), 255, dtype=np.uint8)
for tile in manifest:
    im = cv2.imread(str(TMP / f'a3-render-{tile["pdf_page"]:02d}.png'), 0)
    assert im is not None
    core = im[47:547, 170:670]
    assert core.shape == (500, 500)
    x = 100 + (tile['column']-1)*500
    y = 100 + (tile['row']-1)*500
    assembled[y:y+500, x:x+500] = core

dictionary = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)
board = cv2.aruco.CharucoBoard((6, 5), .250, .175, dictionary)
board.setLegacyPattern(False)
corners, ids, marker_corners, marker_ids = cv2.aruco.CharucoDetector(board).detectBoard(assembled)
assert sorted(marker_ids.flatten().tolist()) == list(range(15))
assert sorted(ids.flatten().tolist()) == list(range(20))
expected = (board.getChessboardCorners()[ids.flatten(), :2]*1000+50)*2
err = np.linalg.norm(corners.reshape(-1, 2)-expected, axis=1)
assert err.max() < 1.25
original = cv2.imread(str(TMP/'charuco-render.png'), 0)[:2700, :3200]
different = float(np.mean((original < 128) != (assembled < 128)))
assert different < .001, different
cv2.imwrite(str(TMP/'a3-assembled-preview.png'), cv2.resize(assembled, (1280, 1080)))

thumb_w, thumb_h = 336, 255
contact = Image.new('RGB', (thumb_w*4, thumb_h*8), '#dddddd')
draw = ImageDraw.Draw(contact)
for i in range(31):
    thumb = Image.open(TMP/f'a3-render-{i+1:02d}.png').convert('RGB')
    thumb.thumbnail((thumb_w-8, thumb_h-22))
    x, y = (i%4)*thumb_w, (i//4)*thumb_h
    contact.paste(thumb, (x+4, y+18))
    draw.text((x+5, y+3), f'PDF {i+1:02d}', fill='black')
contact.save(TMP/'a3-contact.png')
report = {'tile_pages': 30, 'assembled_marker_ids': sorted(marker_ids.flatten().tolist()),
          'assembled_charuco_corners': len(ids), 'max_corner_error_px': float(err.max()),
          'binarized_difference_from_full_size_pdf_fraction': different,
          'verification': 'All rendered tile cores assembled at 100%; labels and crop marks excluded.'}
(TMP/'a3-verification.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
