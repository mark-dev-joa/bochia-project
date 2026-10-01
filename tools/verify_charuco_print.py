"""Verify the rendered PDF, not just the generator's in-memory pattern."""
from pathlib import Path
import json
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tmp/pdfs/python-deps'))
import cv2
import numpy as np
from pypdf import PdfReader

OUT = ROOT / 'output/pdf'
NAME = 'boccia-charuco-6x5-250mm'
cfg = json.loads((OUT / f'{NAME}.json').read_text())
dictionary = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)
board = cv2.aruco.CharucoBoard((6, 5), .250, .175, dictionary)
board.setLegacyPattern(False)
im = cv2.imread(str(ROOT / 'tmp/pdfs/charuco-render.png'), cv2.IMREAD_GRAYSCALE)
assert im is not None
corners, ids, marker_corners, marker_ids = cv2.aruco.CharucoDetector(board).detectBoard(im)
assert marker_ids is not None and sorted(marker_ids.flatten().tolist()) == cfg['marker_ids']
assert ids is not None and sorted(ids.flatten().tolist()) == list(range(20))
observed = corners.reshape(-1, 2)
expected = (board.getChessboardCorners()[ids.flatten(), :2] * 1000 + 50) * 2
errors = np.linalg.norm(observed-expected, axis=1)
assert float(errors.max()) < 1.25, errors

reader = PdfReader(OUT / f'{NAME}.pdf')
assert len(reader.pages[0].images) == 0, 'Expected a vector-only PDF'
assert reader.trailer['/Root']['/ViewerPreferences']['/PrintScaling'] == '/None'
svg = ET.parse(OUT / f'{NAME}.svg').getroot()
assert svg.attrib['width'] == '1600mm' and svg.attrib['height'] == '1350mm'
assert not svg.findall('.//{http://www.w3.org/2000/svg}image')

preview = cv2.resize(im, (1280, round(im.shape[0]*1280/im.shape[1])), interpolation=cv2.INTER_AREA)
cv2.imwrite(str(ROOT / 'tmp/pdfs/charuco-preview.png'), preview)
report = {'render_dpi': 50.8, 'render_size_px': list(im.shape[::-1]),
          'marker_ids_detected': sorted(marker_ids.flatten().tolist()),
          'charuco_corner_ids_detected': sorted(ids.flatten().tolist()),
          'max_corner_error_px': float(errors.max()),
          'pdf_vector_only': True, 'svg_vector_only': True,
          'pdf_print_scaling': 'None', 'svg_size_mm': [1600, 1350],
          'scope': 'Digital geometry and detection only; physical print and field performance require verification.'}
(ROOT / 'tmp/pdfs/verification.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
