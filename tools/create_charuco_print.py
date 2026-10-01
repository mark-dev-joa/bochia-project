"""Create the full-size calibration target using OpenCV's actual board definition."""
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tmp/pdfs/python-deps'))
import cv2
import numpy as np
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm
from pypdf import PdfReader

OUT = ROOT / 'output/pdf'
TMP = ROOT / 'tmp/pdfs'
OUT.mkdir(parents=True, exist_ok=True)
TMP.mkdir(parents=True, exist_ok=True)
NAME = 'boccia-charuco-6x5-250mm'
SX, SY, SQUARE, MARKER, MARGIN = 6, 5, 250.0, 175.0, 50.0
WIDTH, HEIGHT = SX * SQUARE + 2*MARGIN, SY * SQUARE + 2*MARGIN
dictionary = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)
board = cv2.aruco.CharucoBoard((SX, SY), SQUARE/1000, MARKER/1000, dictionary)
board.setLegacyPattern(False)
ids = board.getIds().flatten().tolist()
reference = board.generateImage((3600, 3000), marginSize=0, borderBits=1)
rects = []
for row in range(SY):
    for col in range(SX):
        # Read square polarity from OpenCV rather than assuming board orientation.
        if reference[row*600+5, col*600+5] == 0:
            rects.append((MARGIN+col*SQUARE, MARGIN+row*SQUARE, SQUARE, SQUARE))

for marker_id, corners in zip(ids, board.getObjPoints()):
    # Snap the float32 OpenCV geometry onto the exact specified metric grid.
    cx = int(round(float(corners[:, 0].mean())*1000/SQUARE - 0.5))
    cy = int(round(float(corners[:, 1].mean())*1000/SQUARE - 0.5))
    x0 = MARGIN + cx*SQUARE + (SQUARE-MARKER)/2
    y0 = MARGIN + cy*SQUARE + (SQUARE-MARKER)/2
    bits = cv2.aruco.generateImageMarker(dictionary, marker_id, 6, borderBits=1)
    module = MARKER/6
    for y in range(6):
        x = 0
        while x < 6:
            if bits[y, x] != 0:
                x += 1
                continue
            start = x
            while x < 6 and bits[y, x] == 0:
                x += 1
            rects.append((x0+start*module, y0+y*module, (x-start)*module, module))

pdf = OUT / f'{NAME}.pdf'
c = canvas.Canvas(str(pdf), pagesize=(WIDTH*mm, HEIGHT*mm), pageCompression=1)
c.setTitle('Boccia ChArUco 6x5 - square 250 mm - marker 175 mm - print at 100%')
c.setAuthor('Boccia calibration project')
c.setSubject('DICT_4X4_50; IDs 0-14; borderBits=1; 1600 x 1350 mm; OpenCV legacyPattern=False')
c.setViewerPreference('PrintScaling', 'None')
c.setFillColorRGB(1, 1, 1)
c.rect(0, 0, WIDTH*mm, HEIGHT*mm, fill=1, stroke=0)
c.setFillColorRGB(0, 0, 0)
for x, y, w, h in rects:
    c.rect(x*mm, (HEIGHT-y-h)*mm, w*mm, h*mm, fill=1, stroke=0)
c.showPage()
c.save()

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH:g}mm" height="{HEIGHT:g}mm" viewBox="0 0 {WIDTH:g} {HEIGHT:g}">',
       '<title>Boccia ChArUco 6x5, DICT_4X4_50, square 250 mm, marker 175 mm</title>',
       f'<rect width="{WIDTH:g}" height="{HEIGHT:g}" fill="white"/>', '<g fill="black" stroke="none">']
svg += [f'<rect x="{x:.9f}" y="{y:.9f}" width="{w:.9f}" height="{h:.9f}"/>' for x,y,w,h in rects]
svg += ['</g>', '</svg>']
(OUT / f'{NAME}.svg').write_text('\n'.join(svg)+'\n', encoding='utf-8')

config = {
    'dictionary': 'DICT_4X4_50', 'squares_x': SX, 'squares_y': SY,
    'square_length_m': SQUARE/1000, 'marker_length_m': MARKER/1000,
    'marker_border_bits': 1, 'marker_ids': ids, 'legacy_pattern': False,
    'opencv_generator_version': cv2.__version__,
    'pattern_size_mm': [SX*SQUARE, SY*SQUARE], 'page_size_mm': [WIDTH, HEIGHT],
    'outer_margin_mm': MARGIN, 'print_scale_percent': 100,
    'board_coordinates': 'OpenCV CharucoBoard coordinates; page margin excluded; object points supplied below.',
    'charuco_corner_ids': list(range(len(board.getChessboardCorners()))),
    'charuco_corners_m': np.round(board.getChessboardCorners().astype(float), 8).tolist(),
    'marker_object_points_m': np.round(np.array(board.getObjPoints()).astype(float), 8).tolist(),
}
(OUT / f'{NAME}.json').write_text(json.dumps(config, indent=2)+'\n', encoding='utf-8')
(OUT / 'PRINT-INSTRUCTIONS-KO.md').write_text('''# 보치아 ChArUco 출력 안내

## 납품 파일
- boccia-charuco-6x5-250mm.pdf: 실제 크기 벡터 PDF, 1페이지. 인쇄용 권장.
- boccia-charuco-6x5-250mm.svg: 동일한 실제 크기의 벡터 원본.
- boccia-charuco-6x5-250mm.json: 검출/캘리브레이션 프로그램용 보드 정의.

## 제작 규격
- 전체 페이지: 1600 x 1350 mm
- 패턴 영역: 1500 x 1250 mm
- 칸 개수: 가로 6칸 x 세로 5칸 (내부 ChArUco 코너 5 x 4 = 20개)
- 한 칸: 250 x 250 mm
- ArUco 마커: 175 x 175 mm (검은 테두리 포함)
- 사방 흰색 여백: 50 mm
- Dictionary: DICT_4X4_50, 마커 ID 0-14, borderBits=1
- OpenCV legacyPattern=False. SVG/PDF에는 텍스트와 보조선을 넣지 않았습니다.

## 출력 업체 전달 사항
실제 크기/배율 100%로 출력해주세요. 용지 맞춤, 자동 축소, 임의 확대,
가로세로 비율 변경, 패턴 잘라내기를 적용하지 마세요.
흰 바탕/검정 패턴, 무광 표면, 평탄한 강성 판재와 적절한 후면 보강을 사용해주세요.
PDF는 벡터 원본입니다. 화면 캡처나 미리보기 이미지를 인쇄 원본으로 사용하지 마세요.

## 사용 전 확인
1. 가로/세로 여러 곳의 칸 간격과 전체 패턴 크기를 실측합니다.
2. 출력 배율이 두 축에서 다른지, 판이 휘었는지 확인합니다.
3. 카메라의 가장 먼 관측 위치와 사용할 기울기에서 ID/코너 검출을 시험합니다.
4. 종이에 분할 출력해 이어 붙이는 방식은 가독성 시험용으로만 사용하고,
   정밀 캘리브레이션용 판은 일체형으로 제작하는 것을 권장합니다.

현재 크기는 사전 협의한 시험 후보이며, 실제 원거리 검출 성능을 보장하는 것은 아닙니다.
외곽 여백 50 mm는 인쇄 페이지의 여백이고 OpenCV 보드 좌표에 포함되지 않습니다.
''', encoding='utf-8')

reader = PdfReader(pdf)
page = reader.pages[0]
size = [float(page.mediabox.width)/mm, float(page.mediabox.height)/mm]
assert len(reader.pages) == 1
assert np.allclose(size, [WIDTH, HEIGHT], atol=0.001), size
assert len(ids) == 15 and len(board.getChessboardCorners()) == 20
print(json.dumps({'pdf': str(pdf), 'page_size_mm': size, 'marker_count': len(ids),
                  'charuco_corners': 20, 'opencv': cv2.__version__}, indent=2))
