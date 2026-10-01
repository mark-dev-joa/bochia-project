"""A3 print-at-100% tiles: one complete 250 mm chessboard square per sheet."""
from pathlib import Path
import json
import xml.etree.ElementTree as ET
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'output/pdf'
NAME = 'boccia-charuco-6x5-250mm-a3-tiles'
PW, PH = 420.0, 297.0
pdfmetrics.registerFont(TTFont('Korean', r'C:\Windows\Fonts\malgun.ttf'))
src = ET.parse(OUT / 'boccia-charuco-6x5-250mm.svg').getroot()
black = src.find('{http://www.w3.org/2000/svg}g')
rects = [tuple(float(el.attrib[k]) for k in ('x', 'y', 'width', 'height')) for el in black]
c = canvas.Canvas(str(OUT / f'{NAME}.pdf'), pagesize=(PW*mm, PH*mm), pageCompression=1)
c.setTitle('Boccia ChArUco - A3 100 percent - guide + 30 tiles')
c.setAuthor('Boccia calibration project')
c.setViewerPreference('PrintScaling', 'None')
c.setViewerPreference('Duplex', 'Simplex')

def text(x, y, value, size=11, color=0):
    c.setFillGray(color)
    c.setFont('Korean', size)
    c.drawString(x*mm, (PH-y)*mm, value)

def box(x, y, w, h, fill=None, stroke=None):
    if fill is not None:
        c.setFillGray(fill)
    if stroke is not None:
        c.setStrokeGray(stroke)
    c.rect(x*mm, (PH-y-h)*mm, w*mm, h*mm, fill=fill is not None, stroke=stroke is not None)

def line(x1, y1, x2, y2):
    c.line(x1*mm, (PH-y1)*mm, x2*mm, (PH-y2)*mm)

# Guide page: no printed target measurements should be taken from this page.
text(18, 23, 'ChArUco A3 분할 출력 · 조립 안내', 24)
text(18, 34, '원본 크기 유지 / 패턴 1500 × 1250 mm / 한 칸 250 mm / 마커 175 mm', 12, .25)
text(18, 51, '조립 배치: 가로 6장 × 세로 5장 = 패턴용 A3 30장', 13)
text(18, 59, '위에서 아래로 행 번호, 왼쪽에서 오른쪽으로 열 번호. 아래 도식은 축소 안내입니다.', 9, .3)
for r in range(5):
    for col in range(6):
        n = 2+r*6+col
        x, y = 18+col*29, 69+r*28
        shade = .14 if (r+col)%2 == 0 else .96
        box(x, y, 28, 27, fill=shade, stroke=.65)
        text(x+4, y+11, f'R{r+1}C{col+1}', 13, 1 if shade < .5 else 0)
        text(x+4, y+20, f'PDF {n:02d}', 10, .95 if shade < .5 else .2)
text(18, 220, '이 방향 그대로 조립하세요. 조각을 따로 회전하거나 뒤집지 마세요.', 10)
text(18, 230, '붙인 패턴: 1500 × 1250 mm', 12)
text(18, 239, '흰색 외곽 50 mm까지 확보한 받침판: 1600 × 1350 mm', 11)
text(18, 251, '30장에는 기존 원본의 사방 50 mm 흰 여백이 포함되지 않습니다.', 9, .25)
text(18, 258, '흰색 받침판 또는 무광 흰 종이로 외곽 여백을 별도로 확보하세요.', 9, .25)

steps = [
    (53, '1  인쇄 설정', ['A3 / 가로 / 단면 / 실제 크기 100%', '패턴만 출력하려면 PDF 2-31페이지를 선택합니다.', '용지 맞춤, 자동 축소, 소책자, 여러 쪽 모아 찍기는 끕니다.', '이미 나눠진 파일이므로 포스터 분할 기능도 끕니다.']),
    (95, '2  한 장 먼저 시험', ['PDF 3페이지(R1C2)를 먼저 출력합니다.', '가로·세로 확인 눈금이 각각 100 mm인지 측정합니다.', '마커 바깥 검은 테두리 한 변은 175 mm입니다.', '배율이 맞으면 나머지 장을 같은 설정으로 출력합니다.']),
    (137, '3  자르기', ['바깥 재단 표시를 연결하는 네 직선을 기준으로 자릅니다.', '남기는 부분은 정확히 250 × 250 mm 정사각형입니다.', '주변 10 mm는 인접 패턴과 겹치는 확인용 재단 여유입니다.', '설명·눈금·정렬 표시는 완성 패턴에 남기지 않습니다.']),
    (179, '4  조립', ['각 조각의 뒷면에 R행/C열 번호와 위쪽 방향을 적습니다.', '왼쪽 배치도대로 틈이나 겹침 없이 맞대어 붙입니다.', '10 mm 겹침 부분은 잘라 버리고, 최종 조각끼리는 겹치지 않습니다.', '평탄한 흰색 받침판에 주름과 들뜸 없이 고정합니다.']),
    (221, '5  확인', ['가로 6칸 = 1500 mm, 세로 5칸 = 1250 mm를 실측합니다.', '칼질 때문에 코너나 마커 테두리가 손상되지 않았는지 봅니다.', '기존 보드 JSON(0.250 m / 0.175 m)을 그대로 사용합니다.', '분할 출력은 현장 검출 시험용입니다. 정밀 캘은 제작 오차를 확인하세요.']),
]
for y, heading, paragraphs in steps:
    text(212, y, heading, 14)
    for j, s in enumerate(paragraphs):
        text(212, y+9+j*6, s, 9.5)
text(18, 283, 'DICT_4X4_50 · IDs 0-14 · borderBits=1 · legacyPattern=False', 9, .35)
text(348, 283, '안내 1 / 전체 31페이지', 9, .35)
c.showPage()

manifest = []
for r in range(5):
    for col in range(6):
        n = 2+r*6+col
        # Core in page top-left coordinates: x=85, y=23.5, size=250.
        # Ten millimetres of context on each side; all annotation is outside it.
        wx, wy = 40+col*250, 40+r*250
        for x, y, w, h in rects:
            x1, y1 = max(x, wx), max(y, wy)
            x2, y2 = min(x+w, wx+270), min(y+h, wy+270)
            if x2 > x1 and y2 > y1:
                box(75+x1-wx, 13.5+y1-wy, x2-x1, y2-y1, fill=0)
        c.setStrokeGray(0)
        c.setLineWidth(.45)
        for y in (23.5, 273.5):
            line(68, y, 73, y)
            line(347, y, 352, y)
        for x in (85, 335):
            line(x, 7.5, x, 11.5)
            line(x, 285.5, x, 289.5)

        text(10, 44, f'R{r+1}C{col+1}', 23)
        text(10, 57, f'PDF {n:02d} / 31', 12)
        text(10, 72, f'{r+1}행 {col+1}열', 12)
        text(10, 87, '위쪽 방향 ↑', 12)
        text(10, 107, 'A3 가로 · 단면', 10)
        text(10, 115, '실제 크기 100%', 10)
        text(10, 130, '재단 후 크기', 10)
        text(10, 138, '250 × 250 mm', 10)
        text(10, 156, '바깥 표시를 이어', 9)
        text(10, 163, '네 변을 자르세요.', 9)
        text(10, 180, '이 설명 영역은', 9)
        text(10, 187, '잘라 버립니다.', 9)
        text(10, 204, '재단 전 뒷면에', 9)
        text(10, 211, '위치와 방향 기록', 9)

        text(354, 44, '배율 확인', 12)
        text(354, 54, '두 축 모두 100 mm', 9)
        text(354, 177, '세로 100 mm', 9)
        line(375, 65, 375, 165)
        for k in range(11):
            tick = 3 if k in (0, 5, 10) else 1.5
            line(375-tick, 65+k*10, 375+tick, 65+k*10)
        text(380, 67, '0', 8)
        text(380, 117, '50', 8)
        text(380, 167, '100', 8)
        text(354, 186, '코너 표시 기준', 9)
        text(354, 194, '중앙 정사각형만', 9)
        text(354, 202, '남기세요.', 9)
        line(160, 8, 260, 8)
        for k in range(11):
            tick = 1.5 if k in (0, 5, 10) else .8
            line(160+k*10, 8-tick, 160+k*10, 8+tick)
        text(265, 10, '가로 100 mm', 8)
        c.showPage()
        manifest.append({'pdf_page': n, 'row': r+1, 'column': col+1,
                         'core_page_mm': [85, 23.5, 250, 250],
                         'pattern_origin_mm': [col*250, r*250]})
c.save()
reader = PdfReader(OUT / f'{NAME}.pdf')
assert len(reader.pages) == 31
for page in reader.pages:
    assert abs(float(page.mediabox.width)/mm - PW) < .001
    assert abs(float(page.mediabox.height)/mm - PH) < .001
    assert len(page.images) == 0
(ROOT / 'tmp/pdfs/a3-tile-manifest.json').write_text(json.dumps(manifest, indent=2))
print(json.dumps({'pdf': str(OUT/f'{NAME}.pdf'), 'pages': 31,
                  'tile_pages': '2-31', 'print_size_mm': [PW, PH],
                  'assembly_pattern_mm': [1500, 1250], 'tiles': [6, 5]}, indent=2))
