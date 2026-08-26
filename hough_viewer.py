"""허프 서클 뷰어 - 이미지 프로세싱 과정을 단계별로 보고 파라미터를 조정한다.

좌측  처리 5단계 패널 (원본/그레이/블러/캐니/허프) + 조작 버튼
중앙  좌측에서 고른 단계를 크게. 어떤 단계를 골라도 원은 그 위에 그린다.
우측  파라미터 슬라이더. 처리 순서대로 그룹으로 묶여 있다.

    python hough_viewer.py
    python hough_viewer.py --width 1600
    python hough_viewer.py --export out.mp4
"""
from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from pathlib import Path


# --------------------------------------------------------------------------
# video_player.py 에서 가져온 부분. 이 파일 하나로 실행되도록 인라인했다.
# --------------------------------------------------------------------------
SUPPORTED_VIDEO_EXTENSIONS = {".mp4", ".avi", ".mov", ".mkv"}
DEFAULT_POSE_WEIGHT = "yolov8n-pose.pt"
YOLO_POSE_EDGES = [
    (5, 6),
    (5, 7),
    (7, 9),
    (6, 8),
    (8, 10),
    (5, 11),
    (6, 12),
    (11, 12),
    (11, 13),
    (13, 15),
    (12, 14),
    (14, 16),
    (0, 1),
    (0, 2),
    (1, 3),
    (2, 4),
]


def find_video_files(directory: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in directory.iterdir()
            if path.is_file() and path.suffix.lower() in SUPPORTED_VIDEO_EXTENSIONS
        ),
        key=lambda path: path.name.lower(),
    )


def find_local_pose_weight(search_roots: Sequence[Path] | None = None) -> Path | None:
    if search_roots is None:
        home = Path.home()
        search_roots = [
            Path.cwd(),
            home / "Downloads",
            home / ".cache" / "ultralytics",
            home / ".config" / "Ultralytics",
            home / "Library" / "Application Support" / "Ultralytics",   # macOS
            home / "AppData" / "Roaming" / "Ultralytics",               # Windows
            home / "work",
        ]

    candidates: list[Path] = []
    for root in search_roots:
        if not root.exists():
            continue
        root_depth = len(root.parts)
        for directory, subdirectories, files in os.walk(root):
            directory_path = Path(directory)
            if len(directory_path.parts) - root_depth >= 5:
                subdirectories[:] = []
            for filename in files:
                lowercase_name = filename.lower()
                if (
                    lowercase_name.endswith(".pt")
                    and (
                        "pose" in lowercase_name
                        or lowercase_name.startswith("yolov8")
                        or lowercase_name.startswith("yolo11")
                    )
                ):
                    candidates.append(directory_path / filename)

    def weight_priority(path: Path) -> tuple[int, str]:
        name = path.name.lower()
        if "n-pose" in name:
            return (0, name)
        if "s-pose" in name:
            return (1, name)
        if "m-pose" in name:
            return (2, name)
        if "pose" in name:
            return (3, name)
        return (4, name)

    unique_candidates = sorted(set(candidates), key=weight_priority)
    pose_candidates = [
        candidate
        for candidate in unique_candidates
        if "pose" in candidate.name.lower()
    ]
    if pose_candidates:
        return pose_candidates[0]
    if unique_candidates:
        return unique_candidates[0]
    return None


SETTINGS_PATH = Path("hough_viewer_settings.json")
STAGE_COUNT = 5
BLUR_TYPES = ["없음", "미디언", "가우시안", "바이래터럴", "박스"]
BUTTONS = [("play_pause", "재생 / 정지"), ("prev", "< 이전 프레임"),
           ("next", "다음 프레임 >"), ("roi", "ROI 그리기"),
           ("save", "설정 저장"), ("quit", "닫기")]

BUTTON_TOP_MARGIN = 12
BUTTON_PITCH = 42
INFO_BLOCK_HEIGHT = 196
CONTROLS_WIDTH = 260
CONTROLS_TOP = 46
SLIDER_PITCH = 28
CHECK_PITCH = 24
CHECK_BOX = 13
GROUP_HEADER_HEIGHT = 26
SLIDER_MARGIN = 14
HELP_BOX_HEIGHT = 92
ROI_POINT_COUNT = 4         # 4점 클릭하면 자동으로 닫힌다
ROI_COLOR = (255, 200, 0)
SKELETON_EDGE = (255, 0, 255)
SKELETON_POINT = (0, 255, 255)
WRIST_COLOR = (0, 165, 255)     # 주황 - 손목 (공이 손에서 굴러 나감)
WRIST_KEYPOINTS = (9, 10)      # YOLO 포즈: 9=왼손목, 10=오른손목
PICKED_COLOR = (0, 255, 255)        # 학습용으로 고른 공
MANUAL_BOX_COLOR = (255, 255, 255)  # 직접 그리는 중인 박스
MAIN_TOOLBAR_HEIGHT = 44
DATASET_DIR = Path("dataset_manual")
WINDOW_NAME = "Hough Viewer"
DATASET_STATS_TOP = 46      # 통계 4줄 시작
DATASET_STATS_PITCH = 22
DATASET_FORMAT_TOP = DATASET_STATS_TOP + DATASET_STATS_PITCH * 4 + 10   # 144
DATASET_EXPORT_TOP = DATASET_FORMAT_TOP + 36                            # 180
DATASET_ROWS_TOP = DATASET_EXPORT_TOP + 32 + 44                         # 256
SEG_POLYGON_POINTS = 24     # 원을 다각형으로 근사할 때 쓰는 점 개수
MAIN_TOOLS = [("open", "영상 열기"), ("label_mode", "학습 데이터 선택 / 해제"),
              ("clear_pick", "이 프레임 전체 해제")]
MENU_ROW_HEIGHT = 34
SCRUB_HEIGHT = 36
LABEL_LIMIT = 30            # 원이 이보다 많으면 인라이어 라벨은 생략

SELECTED_COLOR = (0, 220, 0)
PANEL_BG = (26, 26, 26)
HOUGH_COLOR = (0, 220, 0)       # 초록 - 허프 원본
RANSAC_COLOR = (0, 0, 255)      # 빨강 - RANSAC 재피팅

KOREAN_FONTS = [
    # Windows
    r"C:\Windows\Fonts\malgun.ttf",
    r"C:\Windows\Fonts\malgunsl.ttf",
    r"C:\Windows\Fonts\gulim.ttc",
    # macOS
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "/System/Library/Fonts/Supplemental/AppleGothic.ttf",
    # Linux
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]


# --------------------------------------------------------------------------
# 한글 렌더링 - OpenCV 기본 폰트는 한글을 못 그리므로 PIL 로 한 번에 그린다.
# --------------------------------------------------------------------------
class TextLayer:
    def __init__(self) -> None:
        self.fonts: dict[int, object] = {}
        self.pending: list[tuple[str, tuple[int, int], int, tuple[int, int, int]]] = []
        self.available = True
        try:
            from PIL import ImageFont  # noqa: F401
        except ImportError:
            self.available = False

    def font(self, size: int):
        from PIL import ImageFont

        if size not in self.fonts:
            for path in KOREAN_FONTS:
                if Path(path).exists():
                    self.fonts[size] = ImageFont.truetype(path, size)
                    break
            else:
                self.fonts[size] = ImageFont.load_default()
        return self.fonts[size]

    def add(self, text: str, position, size: int, color) -> None:
        self.pending.append((text, position, size, color))

    def measure(self, text: str, size: int) -> int:
        if not self.available:
            return len(text) * size // 2
        font = self.font(size)
        return int(font.getbbox(text)[2])

    def wrap(self, text: str, max_width: int, size: int, limit: int = 3) -> list[str]:
        lines, current = [], ""
        for word in text.split():
            trial = f"{current} {word}".strip()
            if self.measure(trial, size) <= max_width:
                current = trial
            else:
                if current:
                    lines.append(current)
                current = word
        if current:
            lines.append(current)
        return lines[:limit]

    def flush(self, canvas):
        """모아둔 텍스트를 한 번의 변환으로 캔버스에 그린다."""
        if not self.pending:
            return canvas
        if not self.available:
            self.pending.clear()
            return canvas

        import numpy as np
        from PIL import Image, ImageDraw

        image = Image.fromarray(canvas[:, :, ::-1])       # BGR -> RGB
        draw = ImageDraw.Draw(image)
        for text, (x, y), size, color in self.pending:
            draw.text((x, y), text, font=self.font(size), fill=(color[2], color[1], color[0]))
        self.pending.clear()
        return np.ascontiguousarray(np.array(image)[:, :, ::-1])


@dataclass
class Slider:
    key: str
    label: str
    minimum: int
    maximum: int
    value: int
    group: str
    kind: str = "slider"        # "slider" 또는 "check"

    @property
    def is_check(self) -> bool:
        return self.kind == "check"


def default_sliders() -> list[Slider]:
    """이미지 프로세싱 순서대로 그룹핑한다."""
    return [
        # ---- 화면 표시 -----------------------------------------------------
        Slider("show_hough", "허프 원 (초록)", 0, 1, 1, "표시", "check"),
        Slider("show_ransac", "RANSAC 원 (빨강)", 0, 1, 1, "표시", "check"),

        # ---- 3단계: 블러 ---------------------------------------------------
        Slider("blur_type", "블러 종류", 0, len(BLUR_TYPES) - 1, 1, "3. 블러"),
        Slider("ksize", "커널 크기", 1, 31, 5, "3. 블러"),
        Slider("gauss_sigma10", "가우시안 시그마 x10", 0, 300, 0, "3. 블러"),
        Slider("bilat_d", "바이래터럴 지름", 1, 25, 9, "3. 블러"),
        Slider("bilat_color", "밝기 허용치", 1, 300, 75, "3. 블러"),
        Slider("bilat_space", "거리 허용치", 1, 300, 75, "3. 블러"),

        # ---- 4단계: 캐니 ---------------------------------------------------
        Slider("canny_low", "약한 엣지 기준", 0, 400, 90, "4. 캐니"),
        Slider("canny_high", "강한 엣지 기준", 1, 400, 180, "4. 캐니"),

        # ---- 5단계: 허프 ---------------------------------------------------
        Slider("dp10", "누산기 해상도 x10", 1, 40, 10, "5. 허프"),
        Slider("min_dist", "중심 최소 간격", 1, 200, 18, "5. 허프"),
        Slider("param1", "내부 캐니 상한", 1, 400, 180, "5. 허프"),
        Slider("param2", "투표 임계값", 1, 200, 14, "5. 허프"),
        Slider("min_radius", "최소 반지름", 0, 200, 4, "5. 허프"),
        Slider("max_radius", "최대 반지름", 1, 300, 53, "5. 허프"),

        # ---- 6단계: ROI 필터 -----------------------------------------------
        Slider("roi_on", "ROI 안쪽만 통과", 0, 1, 1, "6. ROI 필터", "check"),

        # ---- 7단계: RANSAC -------------------------------------------------
        Slider("ransac_on", "RANSAC 사용", 0, 1, 1, "7. RANSAC", "check"),
        Slider("ransac_reach10", "엣지 수집 범위 x10", 10, 40, 15, "7. RANSAC"),
        Slider("ransac_tol10", "인라이어 허용오차 x10", 5, 60, 20, "7. RANSAC"),
        Slider("ransac_iters", "샘플 반복 횟수", 8, 200, 60, "7. RANSAC"),
        Slider("min_inlier_pct", "최소 인라이어 비율 %", 0, 100, 50, "7. RANSAC"),

        # ---- 8단계: YOLO 스켈레톤 -------------------------------------------
        Slider("skeleton_on", "스켈레톤 표시", 0, 1, 0, "8. 스켈레톤", "check"),
        Slider("skeleton_conf", "키포인트 신뢰도 %", 5, 95, 30, "8. 스켈레톤"),
    ]


SLIDER_HELP = {
    "show_hough": "허프가 찾은 원본 원을 초록색으로 그린다.",
    "show_ransac": "엣지에 다시 맞춘 원을 빨간색으로 그린다.",
    "blur_type": "0 없음 / 1 미디언 / 2 가우시안 / 3 바이래터럴 / 4 박스",
    "ksize": "블러 커널 크기. 홀수만 쓰며 클수록 많이 뭉갠다.",
    "gauss_sigma10": "가우시안 퍼짐 정도. 0이면 커널 크기에서 자동으로 정해진다.",
    "bilat_d": "참조할 이웃 픽셀 지름. 클수록 느려진다.",
    "bilat_color": "밝기가 이만큼 달라도 섞는다. 높이면 엣지까지 뭉갠다.",
    "bilat_space": "이만큼 떨어진 픽셀까지 섞는다.",
    "canny_low": "약한 엣지 기준. 4번 패널 표시용이며 허프 결과에는 영향이 없다.",
    "canny_high": "강한 엣지 기준. 4번 패널 표시용이며 허프 결과에는 영향이 없다.",
    "dp10": "누산기 해상도. 1.0은 원본 크기, 2.0은 절반이라 빠르지만 거칠다.",
    "min_dist": "원 중심 사이 최소 간격. 작으면 같은 물체를 겹쳐서 여러 번 잡는다.",
    "param1": "허프 내부 캐니의 상한값. 실제 검출을 좌우하는 값이다.",
    "param2": "원으로 인정할 투표 수. 낮출수록 원이 많이 나온다.",
    "min_radius": "허프가 찾을 최소 반지름(px).",
    "max_radius": "허프가 찾을 최대 반지름(px).",
    "roi_on": "ROI 다각형 바깥에 중심이 있는 원은 버린다. ROI 그리기 버튼으로 점을 찍는다.",
    "ransac_on": "허프 원을 실제 캐니 엣지 픽셀에 다시 맞춘다.",
    "ransac_reach10": "반지름의 몇 배 범위까지 엣지 픽셀을 모을지 정한다.",
    "ransac_tol10": "원 둘레에서 이 거리 안에 있으면 인라이어로 센다(px).",
    "ransac_iters": "3점 랜덤 샘플을 몇 번 시도할지. 많을수록 안정적이지만 느리다.",
    "min_inlier_pct": "원 둘레의 이 비율만큼 실제 엣지여야 통과시킨다.",
    "skeleton_on": "YOLOv8 포즈 모델로 사람 골격을 찾아 그린다. 첫 실행 때 모델을 불러온다.",
    "skeleton_conf": "이 신뢰도 미만인 키포인트는 그리지 않는다.",
}


class Params:
    def __init__(self, sliders: list[Slider]) -> None:
        self.by_key = {slider.key: slider for slider in sliders}

    def raw(self, key: str) -> int:
        return self.by_key[key].value

    @property
    def blur_type(self) -> int:
        return self.raw("blur_type")

    @property
    def odd_ksize(self) -> int:
        k = max(1, self.raw("ksize"))
        return k if k % 2 == 1 else k + 1

    @property
    def canny_low(self) -> int:
        return self.raw("canny_low")

    @property
    def canny_high(self) -> int:
        return max(self.canny_low + 1, self.raw("canny_high"))

    @property
    def min_radius(self) -> int:
        return self.raw("min_radius")

    @property
    def max_radius(self) -> int:
        return max(self.min_radius + 1, self.raw("max_radius"))

    def blur_label(self) -> str:
        name = BLUR_TYPES[self.blur_type]
        if name == "없음":
            return "3. 블러 없음"
        if name == "미디언":
            return f"3. 미디언 k={self.odd_ksize}"
        if name == "가우시안":
            return f"3. 가우시안 k={self.odd_ksize} s={self.raw('gauss_sigma10') / 10:.1f}"
        if name == "바이래터럴":
            return f"3. 바이래터럴 d={self.raw('bilat_d')}"
        return f"3. 박스 k={self.odd_ksize}"


def apply_blur(gray, params: Params):
    import cv2

    name = BLUR_TYPES[params.blur_type]
    k = params.odd_ksize
    if name == "없음":
        return gray.copy()
    if name == "미디언":
        return cv2.medianBlur(gray, k)
    if name == "가우시안":
        return cv2.GaussianBlur(gray, (k, k), params.raw("gauss_sigma10") / 10)
    if name == "바이래터럴":
        return cv2.bilateralFilter(gray, max(1, params.raw("bilat_d")),
                                   params.raw("bilat_color"), params.raw("bilat_space"))
    return cv2.blur(gray, (k, k))


def find_circles(blurred, params: Params):
    import cv2
    import numpy as np

    circles = cv2.HoughCircles(
        blurred, cv2.HOUGH_GRADIENT,
        dp=max(0.1, params.raw("dp10") / 10),
        minDist=max(1, params.raw("min_dist")),
        param1=max(1, params.raw("param1")),
        param2=max(1, params.raw("param2")),
        minRadius=params.min_radius,
        maxRadius=params.max_radius,
    )
    if circles is None:
        return []
    return [tuple(int(value) for value in circle) for circle in np.rint(circles[0])]


def circle_from_three(points):
    """세 점을 지나는 원(외접원). 일직선이면 None."""
    (ax, ay), (bx, by), (cx, cy) = points
    d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-9:
        return None
    a2, b2, c2 = ax * ax + ay * ay, bx * bx + by * by, cx * cx + cy * cy
    ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    return ux, uy, ((ax - ux) ** 2 + (ay - uy) ** 2) ** 0.5


def fit_circle_least_squares(xs, ys):
    """Kasa 대수적 원 피팅 - 인라이어 전체로 마무리 보정."""
    import numpy as np

    if len(xs) < 3:
        return None
    A = np.column_stack([xs, ys, np.ones(len(xs))])
    b = xs ** 2 + ys ** 2
    try:
        solution, *_ = np.linalg.lstsq(A, b, rcond=None)
    except np.linalg.LinAlgError:
        return None
    cx, cy = solution[0] / 2, solution[1] / 2
    radius_squared = solution[2] + cx * cx + cy * cy
    if radius_squared <= 0:
        return None
    return float(cx), float(cy), float(radius_squared ** 0.5)


def ransac_refit(circle, edge_xs, edge_ys, reach, tolerance, iterations, rng):
    """캐니 엣지 픽셀만 가지고 원을 다시 맞춘다.

    허프는 누산기 격자에 양자화된 값을 주므로 중심/반지름이 거칠다.
    반지름의 reach 배 안에 있는 엣지 픽셀을 모아 RANSAC 으로 재피팅한 뒤,
    인라이어 전체로 최소자승 보정을 한 번 더 한다.
    """
    import numpy as np

    x, y, r = circle
    window = r * reach
    nearby = (np.abs(edge_xs - x) <= window) & (np.abs(edge_ys - y) <= window)
    xs, ys = edge_xs[nearby].astype(np.float64), edge_ys[nearby].astype(np.float64)
    if len(xs) < 12:
        return None

    inside = np.hypot(xs - x, ys - y) <= window
    xs, ys = xs[inside], ys[inside]
    if len(xs) < 12:
        return None

    best_inliers, best_circle = 0, None
    for _ in range(iterations):
        picks = rng.choice(len(xs), 3, replace=False)
        candidate = circle_from_three([(xs[i], ys[i]) for i in picks])
        if candidate is None:
            continue
        cx, cy, cr = candidate
        if cr < r * 0.5 or cr > r * 1.8:
            continue
        residual = np.abs(np.hypot(xs - cx, ys - cy) - cr)
        count = int(np.count_nonzero(residual < tolerance))
        if count > best_inliers:
            best_inliers, best_circle = count, (cx, cy, cr)

    if best_circle is None or best_inliers < 10:
        return None

    cx, cy, cr = best_circle
    mask = np.abs(np.hypot(xs - cx, ys - cy) - cr) < tolerance
    refined = fit_circle_least_squares(xs[mask], ys[mask])
    if refined is None:
        return None
    fx, fy, fr = refined
    if fr < r * 0.5 or fr > r * 1.8:
        return None

    final_residual = np.abs(np.hypot(xs - fx, ys - fy) - fr)
    inliers = int(np.count_nonzero(final_residual < tolerance))
    # 커버리지 = 인라이어 / 이상적인 원둘레 픽셀 수 (캐니는 1px 두께).
    coverage = inliers / max(1.0, 2 * np.pi * fr)
    return int(round(fx)), int(round(fy)), int(round(fr)), inliers, min(1.0, coverage)


def inside_polygon(polygon, width: int, height: int):
    """다각형 내부 마스크. 점 개수 제한 없음 (3점 이상이면 된다)."""
    import cv2
    import numpy as np

    mask = np.zeros((height, width), np.uint8)
    cv2.fillPoly(mask, [np.asarray(polygon, np.int32)], 255)
    return mask


def build_stages(frame, params: Params, roi_polygon=None):
    import cv2
    import numpy as np

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    blurred = apply_blur(gray, params)
    canny = cv2.Canny(blurred, params.canny_low, params.canny_high)
    circles = find_circles(blurred, params)

    # ROI 필터는 허프 직후에 건다. 뒤의 RANSAC 이 훨씬 적은 후보만 처리하게 된다.
    if params.raw("roi_on") and roi_polygon and len(roi_polygon) >= 3:
        height, width = gray.shape
        roi_mask = inside_polygon(roi_polygon, width, height)
        circles = [c for c in circles
                   if 0 <= c[0] < width and 0 <= c[1] < height and roi_mask[c[1], c[0]]]

    refined = []
    if params.raw("ransac_on") and circles:
        edge_ys, edge_xs = np.nonzero(canny)
        rng = np.random.default_rng(0)          # 프레임마다 같은 결과가 나오도록 고정
        reach = params.raw("ransac_reach10") / 10
        tolerance = params.raw("ransac_tol10") / 10
        iterations = params.raw("ransac_iters")
        minimum_coverage = params.raw("min_inlier_pct") / 100
        for circle in circles:
            fitted = ransac_refit(circle, edge_xs, edge_ys, reach, tolerance, iterations, rng)
            if fitted is not None and fitted[4] >= minimum_coverage:
                refined.append(fitted)

    label = f"5. 허프 {len(circles)}"
    if params.raw("ransac_on"):
        label += f" / RANSAC {len(refined)}"

    return [
        ("1. 원본", frame),
        ("2. 그레이스케일", gray),
        (params.blur_label(), blurred),
        (f"4. 캐니 {params.canny_low}/{params.canny_high}", canny),
        (label, frame),
    ], circles, refined


def with_circles(image, circles, refined=(), show_hough=True, show_ransac=True,
                 roi_points=(), roi_drawing=False):
    """고른 단계 이미지 위에 원을 그린다. 허프는 초록, RANSAC 은 빨강."""
    import cv2

    canvas = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR) if image.ndim == 2 else image.copy()

    if show_hough:
        for x, y, r in circles:
            cv2.circle(canvas, (x, y), r, HOUGH_COLOR, 2)
            cv2.circle(canvas, (x, y), 2, HOUGH_COLOR, -1)

    if show_ransac:
        show_labels = len(refined) <= LABEL_LIMIT
        for x, y, r, inliers, coverage in refined:
            cv2.circle(canvas, (x, y), r, RANSAC_COLOR, 2)
            cv2.circle(canvas, (x, y), 2, RANSAC_COLOR, -1)
            if show_labels:
                cv2.putText(canvas, f"{inliers} / {coverage * 100:.0f}%",
                            (x + r + 5, y - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.5, RANSAC_COLOR, 1)

    if roi_points:
        import numpy as np

        closed = len(roi_points) >= ROI_POINT_COUNT
        if len(roi_points) >= 2:
            cv2.polylines(canvas, [np.asarray(roi_points, np.int32)], closed, ROI_COLOR, 2)
        for order, (px, py) in enumerate(roi_points, start=1):
            cv2.circle(canvas, (px, py), 7, ROI_COLOR, -1)
            cv2.putText(canvas, str(order), (px + 10, py - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, ROI_COLOR, 2)
        if roi_drawing and not closed:
            cv2.putText(canvas, f"ROI {len(roi_points)}/{ROI_POINT_COUNT}",
                        (14, 78), cv2.FONT_HERSHEY_SIMPLEX, 0.8, ROI_COLOR, 2)
    return canvas


def load_settings(sliders: list[Slider]) -> None:
    if not SETTINGS_PATH.exists():
        return
    try:
        stored = json.loads(SETTINGS_PATH.read_text())
    except (json.JSONDecodeError, OSError):
        return
    for slider in sliders:
        if slider.key in stored:
            try:
                value = int(stored[slider.key])
            except (TypeError, ValueError):
                continue
            slider.value = max(slider.minimum, min(slider.maximum, value))


def draw_skeleton(canvas, people, confidence_threshold: float):
    """YOLO 포즈 키포인트를 뼈대로 이어 그린다."""
    import cv2

    for person in people:
        for start, end in YOLO_POSE_EDGES:
            if start >= len(person) or end >= len(person):
                continue
            sx, sy, sc = person[start]
            ex, ey, ec = person[end]
            if sc < confidence_threshold or ec < confidence_threshold:
                continue
            cv2.line(canvas, (int(sx), int(sy)), (int(ex), int(ey)), SKELETON_EDGE, 2)
        for index, (kx, ky, kc) in enumerate(person):
            if kc < confidence_threshold:
                continue
            if index in WRIST_KEYPOINTS:
                continue                                  # 손목은 아래에서 따로 그린다
            cv2.circle(canvas, (int(kx), int(ky)), 4, SKELETON_POINT, -1)

        # 손목은 공이 출발하는 지점이라 눈에 띄게 표시한다.
        for index in WRIST_KEYPOINTS:
            if index >= len(person):
                continue
            kx, ky, kc = person[index]
            if kc < confidence_threshold:
                continue
            center = (int(kx), int(ky))
            cv2.circle(canvas, center, 11, WRIST_COLOR, 2)
            cv2.circle(canvas, center, 3, WRIST_COLOR, -1)
            cv2.line(canvas, (center[0] - 16, center[1]), (center[0] - 13, center[1]), WRIST_COLOR, 2)
            cv2.line(canvas, (center[0] + 13, center[1]), (center[0] + 16, center[1]), WRIST_COLOR, 2)
            cv2.line(canvas, (center[0], center[1] - 16), (center[0], center[1] - 13), WRIST_COLOR, 2)
            cv2.line(canvas, (center[0], center[1] + 13), (center[0], center[1] + 16), WRIST_COLOR, 2)
            cv2.putText(canvas, "L" if index == 9 else "R",
                        (center[0] + 14, center[1] - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, WRIST_COLOR, 1)
    return canvas


def detect_label(cx: int, cy: int, r: int, width: int, height: int) -> str:
    """YOLO 검출 라벨: class cx cy w h (정규화)."""
    box = r * 2 * 1.15
    return (f"0 {cx / width:.6f} {cy / height:.6f} "
            f"{min(1.0, box / width):.6f} {min(1.0, box / height):.6f}")


def segment_label(cx: int, cy: int, r: int, width: int, height: int) -> str:
    """YOLO 세그멘테이션 라벨: class x1 y1 x2 y2 ... (정규화).

    공은 원이므로 중심과 반지름만으로 마스크를 정확히 만들 수 있다.
    추가 라벨링 없이 같은 클릭에서 세그멘테이션 데이터가 나온다.
    """
    import math

    points = []
    for step in range(SEG_POLYGON_POINTS):
        angle = 2 * math.pi * step / SEG_POLYGON_POINTS
        px = min(max(cx + r * math.cos(angle), 0), width - 1)
        py = min(max(cy + r * math.sin(angle), 0), height - 1)
        points.append(f"{px / width:.6f} {py / height:.6f}")
    return "0 " + " ".join(points)


def load_roi(viewer) -> None:
    if not SETTINGS_PATH.exists():
        return
    try:
        stored = json.loads(SETTINGS_PATH.read_text()).get("_roi_points") or []
    except (json.JSONDecodeError, OSError):
        return
    if isinstance(stored, list) and len(stored) == ROI_POINT_COUNT:
        viewer.roi_points = [(int(px), int(py)) for px, py in stored]


def save_settings(sliders: list[Slider], selected: int, roi_points=()) -> None:
    payload = {slider.key: slider.value for slider in sliders}
    payload["_selected_stage"] = selected
    payload["_roi_points"] = [list(point) for point in roi_points]
    try:
        SETTINGS_PATH.write_text(json.dumps(payload, indent=2, ensure_ascii=False))
        print(f"설정 저장: {SETTINGS_PATH}")
    except OSError as exc:
        print(f"설정 저장 실패: {exc}")


class HoughViewer:
    def __init__(self, video_path: Path, width: int = 1600) -> None:
        import cv2

        self.cv2 = cv2
        self.video_path = video_path
        self.capture = cv2.VideoCapture(str(video_path))
        if not self.capture.isOpened():
            raise RuntimeError(f"영상을 열 수 없습니다: {video_path}")

        self.total_frames = int(self.capture.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = self.capture.get(cv2.CAP_PROP_FPS) or 30
        self.delay_ms = max(1, round(1000 / fps))

        self.sliders = default_sliders()
        load_settings(self.sliders)
        self.params = Params(self.sliders)
        self.text = TextLayer()

        # 좌측 사이드바 높이와 본문 높이가 같아지도록 역산한다.
        chrome = BUTTON_TOP_MARGIN + len(BUTTONS) * BUTTON_PITCH + INFO_BLOCK_HEIGHT
        usable = width - CONTROLS_WIDTH
        self.sidebar_width = max(140, int((usable - chrome * 4 / 3) / 6))
        self.panel_width = self.sidebar_width
        self.panel_height = int(self.panel_width * 3 / 4)

        self.main_height = STAGE_COUNT * self.panel_height + chrome
        self.main_width = int(self.main_height * 4 / 3)
        self.main_left = self.sidebar_width
        self.controls_left = self.sidebar_width + self.main_width
        self.width = self.controls_left + CONTROLS_WIDTH
        self.height = self.main_height

        self.slider_rows, self.group_rows = self.layout_controls()
        self.dragging: int | None = None
        self.hover: int | None = None

        self.frame = None
        self.frame_index = -1
        self.paused = True
        self.selected = STAGE_COUNT - 1
        self.should_quit = False
        self.circle_count = 0
        self.status = "S 키로도 저장"
        self.status_ticks = 0
        self.roi_points: list[tuple[int, int]] = []
        self.roi_drawing = False
        self.labeling = False
        self.picked: list[tuple[int, int, int]] = []      # 현재 프레임에서 고른 공
        self.datasets: dict[str, dict[int, list[tuple[int, int, int]]]] = {}
        self.videos = find_video_files(Path.cwd())
        self.menu_open = False
        self.scrubbing = False
        self.box_start = None
        self.box_now = None
        self.export_seg = False
        self._stage_cache = None
        self._stage_key = None
        self.last_refined: list = []
        self.panel_tab = "params"                          # params | dataset
        self.dataset_rows: list[tuple[int, int, int]] = []
        self.hover_frame: int | None = None
        self.pose_model = None
        self.pose_weight_name = ""
        self.skeleton_people = []
        self.skeleton_frame_index = None
        load_roi(self)
        self.window = WINDOW_NAME

    # ---- layout -------------------------------------------------------
    def layout_controls(self):
        """그룹 헤더를 감안해 슬라이더 y 좌표를 미리 계산한다."""
        rows, groups = [], []
        y = CONTROLS_TOP
        current = None
        for slider in self.sliders:
            if slider.group != current:
                current = slider.group
                groups.append((current, y))
                y += GROUP_HEADER_HEIGHT
            if slider.is_check:
                rows.append(y + CHECK_PITCH // 2)
                y += CHECK_PITCH
            else:
                rows.append(y + 18)
                y += SLIDER_PITCH
        return rows, groups

    def button_boxes(self):
        margin, gap, height = 10, 8, 34
        top = STAGE_COUNT * self.panel_height + BUTTON_TOP_MARGIN
        return [
            (action, text, margin, top + index * (height + gap),
             self.sidebar_width - margin, top + index * (height + gap) + height)
            for index, (action, text) in enumerate(BUTTONS)
        ]

    def slider_track(self, index: int) -> tuple[int, int, int]:
        return (self.controls_left + SLIDER_MARGIN,
                self.width - SLIDER_MARGIN,
                self.slider_rows[index])

    def slider_at(self, x: int, y: int) -> int | None:
        if x < self.controls_left:
            return None
        for index, track_y in enumerate(self.slider_rows):
            reach = CHECK_PITCH // 2 if self.sliders[index].is_check else 14
            if track_y - reach <= y <= track_y + (reach - 2 if self.sliders[index].is_check else 11):
                return index
        return None

    def set_slider_from_x(self, index: int, x: int) -> None:
        slider = self.sliders[index]
        left, right, _y = self.slider_track(index)
        ratio = min(1.0, max(0.0, (x - left) / max(1, right - left)))
        slider.value = int(round(slider.minimum + ratio * (slider.maximum - slider.minimum)))

    def pose_keypoints(self):
        """현재 프레임의 키포인트. 같은 프레임이면 다시 추론하지 않는다."""
        if self.skeleton_frame_index == self.frame_index:
            return self.skeleton_people

        if self.pose_model is None:
            from ultralytics import YOLO

            weight = find_local_pose_weight()
            weight = str(weight) if weight else DEFAULT_POSE_WEIGHT
            print(f"YOLO 포즈 모델 로딩: {weight}")
            self.pose_model = YOLO(weight)
            self.pose_weight_name = Path(weight).name
            self.status = f"모델 {self.pose_weight_name}"
            self.status_ticks = 120

        results = self.pose_model.predict(self.frame, verbose=False)
        people = []
        if results and results[0].keypoints is not None and results[0].keypoints.data is not None:
            people = results[0].keypoints.data.cpu().numpy()
        self.skeleton_people = people
        self.skeleton_frame_index = self.frame_index
        return people

    def main_tool_boxes(self):
        """본문 상단 툴바. 화면 좌표 기준."""
        gap, height, top = 8, 28, 8
        boxes, left = [], self.main_left + 14
        for action, text in MAIN_TOOLS:
            width = int(self.text.measure(text, 12)) + 22
            boxes.append((action, text, left, top, left + width, top + height))
            left += width + gap
        return boxes

    def main_tool_at(self, x: int, y: int) -> str | None:
        for action, _t, left, top, right, bottom in self.main_tool_boxes():
            if left <= x <= right and top <= y <= bottom:
                return action
        return None

    def pick_at(self, frame_point) -> bool:
        """클릭 지점의 항목을 고르거나 해제한다.

        이미 고른 것(수동 박스 포함)이 먼저다. 그래야 직접 그린 박스도 지울 수 있다.
        """
        fx, fy = frame_point

        for cx, cy, r in list(self.picked):
            half = int(r * 1.15)
            if abs(fx - cx) <= half and abs(fy - cy) <= half:
                self.picked.remove((cx, cy, r))
                self.autocommit()
                self.status = "선택 해제"
                self.status_ticks = 60
                return True

        for circle in self.last_refined:
            cx, cy, r = circle[0], circle[1], circle[2]
            if (fx - cx) ** 2 + (fy - cy) ** 2 <= (r + 6) ** 2:
                self.picked.append((cx, cy, r))
                self.autocommit()
                self.status = f"선택 {len(self.picked)}개"
                self.status_ticks = 60
                return True
        return False

    def add_manual_box(self, start, end) -> bool:
        """드래그한 사각형을 공 하나로 등록한다. RANSAC 이 놓친 공을 직접 넣을 때 쓴다."""
        (x0, y0), (x1, y1) = start, end
        size = max(abs(x1 - x0), abs(y1 - y0))
        if size < 8:
            return False
        # 내보낼 때 r*2*1.15 로 박스를 만드므로, 그린 그대로 나오도록 역산한다.
        radius = max(2, int(round(size / 2 / 1.15)))
        self.picked.append(((x0 + x1) // 2, (y0 + y1) // 2, radius))
        self.autocommit()
        self.status = f"직접 추가 (총 {len(self.picked)}개)"
        self.status_ticks = 90
        return True

    @property
    def dataset(self):
        """영상마다 따로 모은다. 영상을 바꿔도 이전 작업이 섞이지 않는다."""
        return self.datasets.setdefault(self.video_path.stem, {})

    def open_video(self, path: Path) -> None:
        import cv2

        capture = cv2.VideoCapture(str(path))
        if not capture.isOpened():
            self.status = "영상을 열 수 없습니다"
            self.status_ticks = 120
            return

        self.capture.release()
        self.capture = capture
        self.video_path = path
        self.total_frames = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = capture.get(cv2.CAP_PROP_FPS) or 30
        self.delay_ms = max(1, round(1000 / fps))
        self.frame_index = -1
        self.paused = True
        self.menu_open = False
        self.skeleton_frame_index = None
        self.seek(0)
        try:
            cv2.setWindowTitle(WINDOW_NAME, f"Hough Viewer - {path.name}")
        except cv2.error:
            pass
        self.status = path.name
        self.status_ticks = 120

    def scrub_track(self) -> tuple[int, int, int]:
        return (self.main_left + 16, self.controls_left - 16,
                self.main_height - SCRUB_HEIGHT // 2)

    def scrub_at(self, x: int, y: int) -> bool:
        left, right, _track_y = self.scrub_track()
        return (left - 8 <= x <= right + 8
                and self.main_height - SCRUB_HEIGHT <= y < self.main_height)

    def seek_from_x(self, x: int) -> None:
        left, right, _track_y = self.scrub_track()
        ratio = min(1.0, max(0.0, (x - left) / max(1, right - left)))
        self.seek(int(round(ratio * (self.total_frames - 1))))

    def menu_boxes(self):
        if not self.menu_open:
            return []
        left = self.main_left + 14
        right = min(self.controls_left - 14, left + 520)
        top = MAIN_TOOLBAR_HEIGHT + 6
        return [(index, path, left, top + index * MENU_ROW_HEIGHT,
                 right, top + (index + 1) * MENU_ROW_HEIGHT)
                for index, path in enumerate(self.videos)]

    def menu_at(self, x: int, y: int):
        for index, path, left, top, right, bottom in self.menu_boxes():
            if left <= x <= right and top <= y <= bottom:
                return path
        return None

    def autocommit(self) -> None:
        """선택 상태를 그대로 목록에 반영한다. 비면 해당 프레임을 목록에서 뺀다."""
        if self.picked:
            self.dataset[self.frame_index] = list(self.picked)
        else:
            self.dataset.pop(self.frame_index, None)

    def sync_picks_for_frame(self) -> None:
        """프레임이 바뀌면 선택을 초기화한다.

        이미 저장해둔 프레임이면 그때 고른 것을 되살려서 바로 고칠 수 있게 한다.
        """
        self.picked = list(self.dataset.get(self.frame_index, []))

    def export_dataset(self) -> None:
        """고른 공을 YOLO 형식으로 내보낸다. 원본 프레임을 다시 읽어 오버레이 없이 저장한다."""
        import cv2

        pending = {stem: frames for stem, frames in self.datasets.items() if frames}
        if not pending:
            self.status = "담긴 프레임이 없습니다"
            self.status_ticks = 90
            return

        for split in ("train", "val"):
            (DATASET_DIR / "images" / split).mkdir(parents=True, exist_ok=True)
            (DATASET_DIR / "labels" / split).mkdir(parents=True, exist_ok=True)

        by_stem = {path.stem: path for path in self.videos}
        by_stem[self.video_path.stem] = self.video_path
        counts = {"train": 0, "val": 0}
        boxes = 0

        for stem, frames in sorted(pending.items()):
            source = by_stem.get(stem)
            if source is None:
                print(f"  영상을 찾을 수 없어 건너뜀: {stem}")
                continue
            capture = cv2.VideoCapture(str(source))
            indices = sorted(frames)
            for order, index in enumerate(indices):
                capture.set(cv2.CAP_PROP_POS_FRAMES, index)
                ok, frame = capture.read()
                if not ok:
                    continue
                # 5장 중 1장을 검증용으로. 순서 기준이라 매번 같은 분할이 나온다.
                split = "val" if len(indices) >= 5 and order % 5 == 4 else "train"
                height, width = frame.shape[:2]
                name = f"{stem}_{index:06d}"
                cv2.imwrite(str(DATASET_DIR / "images" / split / f"{name}.jpg"),
                            frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
                make_label = segment_label if self.export_seg else detect_label
                lines = [make_label(cx, cy, r, width, height) for cx, cy, r in frames[index]]
                (DATASET_DIR / "labels" / split / f"{name}.txt").write_text("\n".join(lines) + "\n")
                counts[split] += 1
                boxes += len(frames[index])
            capture.release()

        (DATASET_DIR / "data.yaml").write_text(
            f"path: {DATASET_DIR.resolve()}\n"
            "train: images/train\n"
            f"val: images/{'val' if counts['val'] else 'train'}\n"
            "nc: 1\n"
            "names: ['ball']\n"
        )
        self.status = f"train {counts['train']} / val {counts['val']}"
        self.status_ticks = 150
        task = "segment" if self.export_seg else "detect"
        model = "yolo11n-seg.pt" if self.export_seg else "yolo11n.pt"
        print(f"내보내기 완료: {DATASET_DIR}  train {counts['train']}장 / "
              f"val {counts['val']}장 / 객체 {boxes}개 / 형식 {task}")
        print(f"  학습:  yolo {task} train data={DATASET_DIR}/data.yaml "
              f"model={model} imgsz=1280 epochs=100")

    def main_to_frame(self, x: int, y: int) -> tuple[int, int] | None:
        """본문 화면 좌표를 원본 프레임 좌표로 되돌린다."""
        if self.frame is None:
            return None
        # 상단 툴바와 하단 스크러버는 영상 위에 겹쳐 그린 UI 다.
        # 여기를 이미지 좌표로 바꾸면 버튼 클릭이 라벨링 드래그로 새어 나간다.
        if not (self.main_left <= x < self.controls_left
                and MAIN_TOOLBAR_HEIGHT <= y < self.main_height - SCRUB_HEIGHT):
            return None
        frame_height, frame_width = self.frame.shape[:2]
        return (int((x - self.main_left) * frame_width / self.main_width),
                int(y * frame_height / self.main_height))

    def panel_index_at(self, x: int, y: int) -> int | None:
        if x >= self.sidebar_width or y >= STAGE_COUNT * self.panel_height:
            return None
        index = y // self.panel_height
        return int(index) if 0 <= index < STAGE_COUNT else None

    def button_at(self, x: int, y: int) -> str | None:
        for action, _text, left, top, right, bottom in self.button_boxes():
            if left <= x <= right and top <= y <= bottom:
                return action
        return None

    # ---- frame access -------------------------------------------------
    def seek(self, index: int) -> bool:
        cv2 = self.cv2
        index = max(0, min(self.total_frames - 1, index))
        self.capture.set(cv2.CAP_PROP_POS_FRAMES, index)
        ok, frame = self.capture.read()
        if not ok:
            return False
        self.frame, self.frame_index = frame, index
        self.sync_picks_for_frame()
        return True

    def advance(self) -> bool:
        ok, frame = self.capture.read()
        if not ok:
            return self.seek(0)
        self.frame = frame
        self.frame_index += 1
        self.sync_picks_for_frame()
        return True

    # ---- rendering ----------------------------------------------------
    def make_panel(self, image, is_selected: bool):
        cv2 = self.cv2

        if image.ndim == 2:
            image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
        tile = cv2.resize(image, (self.panel_width, self.panel_height))
        cv2.rectangle(tile, (0, 0), (self.panel_width - 1, 19), (0, 0, 0), -1)
        cv2.rectangle(tile, (0, 0), (self.panel_width - 1, self.panel_height - 1),
                      SELECTED_COLOR if is_selected else (70, 70, 70), 3 if is_selected else 1)
        return tile

    def draw_controls(self, canvas) -> None:
        cv2 = self.cv2

        cv2.rectangle(canvas, (self.controls_left, 0), (self.width, self.height), (34, 34, 34), -1)
        cv2.line(canvas, (self.controls_left, 0), (self.controls_left, self.height), (70, 70, 70), 1)

        for key, label, left, top, right, bottom in self.tab_boxes():
            active = key == self.panel_tab
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          (48, 78, 48) if active else (40, 40, 40), -1)
            cv2.line(canvas, (left, bottom), (right, bottom),
                     SELECTED_COLOR if active else (70, 70, 70), 2)
            self.text.add(label, (left + 12, top + 6), 12,
                          (255, 255, 255) if active else (150, 150, 150))

        if self.panel_tab == "dataset":
            self.draw_dataset_panel(canvas)
            return

        for name, y in self.group_rows:
            cv2.line(canvas, (self.controls_left + SLIDER_MARGIN, y + 20),
                     (self.width - SLIDER_MARGIN, y + 20), (68, 68, 68), 1)
            self.text.add(name, (self.controls_left + SLIDER_MARGIN, y + 2), 12, (150, 200, 255))

        for index, slider in enumerate(self.sliders):
            left, right, track_y = self.slider_track(index)
            highlighted = index in (self.hover, self.dragging)
            label_color = (235, 235, 235) if highlighted else (175, 175, 175)

            if slider.is_check:
                half = CHECK_BOX // 2
                box = (left, track_y - half, left + CHECK_BOX, track_y + half)
                checked = bool(slider.value)
                cv2.rectangle(canvas, box[:2], box[2:],
                              (45, 95, 45) if checked else (48, 48, 48), -1)
                cv2.rectangle(canvas, box[:2], box[2:],
                              SELECTED_COLOR if checked else (130, 130, 130), 1)
                if checked:
                    cv2.line(canvas, (box[0] + 3, track_y),
                             (box[0] + 5, track_y + half - 2), SELECTED_COLOR, 2)
                    cv2.line(canvas, (box[0] + 5, track_y + half - 2),
                             (box[2] - 2, box[1] + 2), SELECTED_COLOR, 2)
                self.text.add(slider.label, (left + CHECK_BOX + 8, track_y - 8), 11, label_color)
                continue

            if slider.key == "blur_type":
                shown = BLUR_TYPES[slider.value]
            elif slider.key in ("gauss_sigma10", "dp10", "ransac_reach10", "ransac_tol10"):
                shown = f"{slider.value / 10:.1f}"
            else:
                shown = str(slider.value)

            self.text.add(slider.label, (left, track_y - 16), 11, label_color)
            value_width = self.text.measure(shown, 11)
            self.text.add(shown, (right - value_width, track_y - 16), 11, SELECTED_COLOR)

            cv2.line(canvas, (left, track_y), (right, track_y), (85, 85, 85), 3)
            span = max(1, slider.maximum - slider.minimum)
            handle_x = int(left + (slider.value - slider.minimum) / span * (right - left))
            cv2.line(canvas, (left, track_y), (handle_x, track_y), (90, 170, 100), 3)
            cv2.circle(canvas, (handle_x, track_y), 5, (120, 220, 130), -1)
            cv2.circle(canvas, (handle_x, track_y), 5, (30, 30, 30), 1)

        self.draw_help_box(canvas)

    def tab_boxes(self):
        width = (CONTROLS_WIDTH - 12) // 2
        return [
            ("params", "파라미터", self.controls_left + 6, 4,
             self.controls_left + 6 + width, 30),
            ("dataset", "학습 데이터", self.controls_left + 6 + width, 4,
             self.controls_left + 6 + width * 2, 30),
        ]

    def export_box(self):
        return (self.controls_left + SLIDER_MARGIN, DATASET_EXPORT_TOP,
                self.width - SLIDER_MARGIN, DATASET_EXPORT_TOP + 32)

    def format_box(self):
        return (self.controls_left + SLIDER_MARGIN, DATASET_FORMAT_TOP,
                self.width - SLIDER_MARGIN, DATASET_FORMAT_TOP + 26)

    def format_button_at(self, x: int, y: int) -> bool:
        if self.panel_tab != "dataset":
            return False
        left, top, right, bottom = self.format_box()
        return left <= x <= right and top <= y <= bottom

    def export_button_at(self, x: int, y: int) -> bool:
        if self.panel_tab != "dataset":
            return False
        left, top, right, bottom = self.export_box()
        return left <= x <= right and top <= y <= bottom

    def dataset_row_at(self, x: int, y: int) -> int | None:
        if self.panel_tab != "dataset" or x < self.controls_left:
            return None
        for index, top, bottom in self.dataset_rows:
            if top <= y <= bottom:
                return index
        return None

    def tab_at(self, x: int, y: int) -> str | None:
        for key, _label, left, top, right, bottom in self.tab_boxes():
            if left <= x <= right and top <= y <= bottom:
                return key
        return None

    def draw_dataset_panel(self, canvas) -> None:
        cv2 = self.cv2

        left = self.controls_left + SLIDER_MARGIN
        right = self.width - SLIDER_MARGIN
        frames_total = sum(len(f) for f in self.datasets.values())
        boxes = sum(len(p) for f in self.datasets.values() for p in f.values())

        y = DATASET_STATS_TOP
        for line, color in [
            (f"모은 프레임  {frames_total}장 (현재 {len(self.dataset)})", (235, 235, 235)),
            (f"공 라벨      {boxes}개", (235, 235, 235)),
            (f"현재 선택    {len(self.picked)}개", SELECTED_COLOR),
            (f"라벨링 모드  {'켜짐' if self.labeling else '꺼짐'}",
             SELECTED_COLOR if self.labeling else (150, 150, 150)),
        ]:
            self.text.add(line, (left, y), 12, color)
            y += DATASET_STATS_PITCH

        fl, ft, fr, fb = self.format_box()
        cv2.rectangle(canvas, (fl, ft), (fr, fb), (44, 44, 60), -1)
        cv2.rectangle(canvas, (fl, ft), (fr, fb), (120, 120, 190), 1)
        self.text.add(f"형식:  {'마스크 (segment)' if self.export_seg else '박스 (detect)'}",
                      (fl + 12, ft + 5), 12, (215, 215, 255))

        bl, bt, br, bb = self.export_box()
        enabled = bool(self.datasets and any(self.datasets.values()))
        cv2.rectangle(canvas, (bl, bt), (br, bb),
                      (45, 95, 45) if enabled else (48, 48, 48), -1)
        cv2.rectangle(canvas, (bl, bt), (br, bb),
                      SELECTED_COLOR if enabled else (100, 100, 100), 1)
        total = sum(len(frames) for frames in self.datasets.values())
        caption = f"내보내기  ({total}장)" if enabled else "내보내기  (비어 있음)"
        self.text.add(caption, (bl + 12, bt + 8), 12,
                      (255, 255, 255) if enabled else (130, 130, 130))
        self.text.add(f"-> {DATASET_DIR}/", (left, bb + 6), 10, (140, 140, 140))

        y = DATASET_ROWS_TOP - 28
        cv2.line(canvas, (left, y), (right, y), (68, 68, 68), 1)
        self.text.add("프레임별 (클릭하면 이동)", (left, y + 6), 11, (150, 200, 255))
        y = DATASET_ROWS_TOP

        rows = sorted(self.dataset.items())
        room = (self.height - HELP_BOX_HEIGHT - y - 22) // 20
        self.dataset_rows = []
        for index, picks in (rows[-room:] if room > 0 else []):
            current = index == self.frame_index
            hovered = index == self.hover_frame
            if current or hovered:
                cv2.rectangle(canvas, (left - 6, y - 3), (right, y + 15),
                              (48, 78, 48) if current else (44, 44, 44), -1)
            self.text.add(f"{'▶' if current else '·'} f{index}   공 {len(picks)}개",
                          (left, y), 11,
                          SELECTED_COLOR if current else (215, 215, 215) if hovered else (185, 185, 185))
            self.dataset_rows.append((index, y - 3, y + 15))
            y += 20
        if room > 0 and len(rows) > room:
            self.text.add(f"... 위로 {len(rows) - room}개 더", (left, y), 11, (120, 120, 120))

        box_top = self.height - HELP_BOX_HEIGHT
        cv2.rectangle(canvas, (self.controls_left + 6, box_top),
                      (self.width - 6, self.height - 8), (20, 20, 20), -1)
        cv2.rectangle(canvas, (self.controls_left + 6, box_top),
                      (self.width - 6, self.height - 8), (70, 70, 70), 1)
        for offset, line in enumerate([
            "1. 학습 데이터 선택 켜기",
            "2. 원 클릭 = 선택/해제",
            "   빈 곳 드래그 = 직접 추가",
            "3. 목록에 자동으로 쌓입니다",
        ]):
            self.text.add(line, (left, box_top + 8 + offset * 18), 11, (170, 170, 170))

    def draw_help_box(self, canvas) -> None:
        cv2 = self.cv2

        box_top = self.height - HELP_BOX_HEIGHT
        left = self.controls_left + SLIDER_MARGIN
        right = self.width - SLIDER_MARGIN
        cv2.rectangle(canvas, (self.controls_left + 6, box_top),
                      (self.width - 6, self.height - 8), (20, 20, 20), -1)
        cv2.rectangle(canvas, (self.controls_left + 6, box_top),
                      (self.width - 6, self.height - 8), (70, 70, 70), 1)

        index = self.hover if self.hover is not None else self.dragging
        if index is None:
            self.text.add("슬라이더에 마우스를 올리면", (left, box_top + 10), 11, (120, 120, 120))
            self.text.add("설명이 여기에 나옵니다", (left, box_top + 28), 11, (120, 120, 120))
            return

        slider = self.sliders[index]
        self.text.add(slider.label, (left, box_top + 8), 12, SELECTED_COLOR)
        for offset, line in enumerate(
            self.text.wrap(SLIDER_HELP.get(slider.key, ""), right - left, 11, limit=4)
        ):
            self.text.add(line, (left, box_top + 28 + offset * 16), 11, (185, 185, 185))

    def render(self):
        cv2 = self.cv2
        import numpy as np

        key = (self.frame_index, tuple(s.value for s in self.sliders), tuple(self.roi_points))
        if key == self._stage_key and self._stage_cache is not None:
            stages, circles, refined = self._stage_cache
        else:
            stages, circles, refined = build_stages(self.frame, self.params, self.roi_points)
            self._stage_cache, self._stage_key = (stages, circles, refined), key
        self.circle_count = len(refined) if refined else len(circles)
        show_hough = bool(self.params.raw("show_hough"))
        show_ransac = bool(self.params.raw("show_ransac"))

        people = []
        if self.params.raw("skeleton_on"):
            try:
                people = self.pose_keypoints()
            except Exception as exc:                      # 모델이 없어도 나머지는 계속
                self.params.by_key["skeleton_on"].value = 0
                self.status = "포즈 모델 로딩 실패"
                self.status_ticks = 120
                print(f"YOLO 포즈 실행 실패: {exc}")
        skeleton_conf = self.params.raw("skeleton_conf") / 100

        self.last_refined = refined

        def overlay(image):
            drawn = with_circles(image, circles, refined, show_hough, show_ransac,
                                 self.roi_points, self.roi_drawing)
            if len(people):
                drawn = draw_skeleton(drawn, people, skeleton_conf)
            for cx, cy, r in self.picked:
                half = int(r * 1.15)
                cv2.rectangle(drawn, (cx - half, cy - half), (cx + half, cy + half),
                              PICKED_COLOR, 2)
                cv2.putText(drawn, "ball", (cx - half, cy - half - 6),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, PICKED_COLOR, 1)
            if self.box_start is not None and self.box_now is not None:
                cv2.rectangle(drawn, self.box_start, self.box_now, MANUAL_BOX_COLOR, 2)
            return drawn

        canvas = np.full((self.height, self.width, 3), PANEL_BG, np.uint8)

        for index, (title, image) in enumerate(stages):
            shown = overlay(image) if index == STAGE_COUNT - 1 else image
            top = index * self.panel_height
            canvas[top : top + self.panel_height, : self.panel_width] = \
                self.make_panel(shown, index == self.selected)
            self.text.add(title, (5, top + 2), 11,
                          SELECTED_COLOR if index == self.selected else (245, 245, 245))

        for action, text, left, top, right, bottom in self.button_boxes():
            active = action == "play_pause" and not self.paused
            if action == "quit":
                fill, border = (40, 40, 95), (110, 110, 210)
            elif action == "save":
                fill, border = ((45, 95, 45), (120, 200, 120)) if self.status_ticks \
                    else ((58, 58, 58), (140, 140, 140))
            elif active:
                fill, border = (55, 120, 65), (120, 220, 130)
            else:
                fill, border = (58, 58, 58), (140, 140, 140)
            cv2.rectangle(canvas, (left, top), (right, bottom), fill, -1)
            cv2.rectangle(canvas, (left, top), (right, bottom), border, 1)
            self.text.add(text, (left + 10, top + 8), 12, (240, 240, 240))

        info_top = self.button_boxes()[-1][5] + 18
        for offset, line in enumerate([
            f"프레임 {self.frame_index}/{self.total_frames - 1}",
            f"허프 {len(circles)}개",
            f"RANSAC {len(refined)}개" if self.params.raw("ransac_on") else "RANSAC 꺼짐",
            (f"인라이어 중앙 {int(np.median([c[4] for c in refined]) * 100)}%"
             if refined else f"최소 {self.params.raw('min_inlier_pct')}%"),
            (f"ROI {len(self.roi_points)}점" if self.roi_points else "ROI 없음")
            + (f" · 사람 {len(people)}" if len(people) else ""),
            "패널 클릭 또는 1-5",
            "Space  ,  .  S  Q/Esc",
        ]):
            self.text.add(line, (9, info_top + offset * 17), 11, (170, 170, 170))

        if self.status_ticks:
            self.status_ticks -= 1
        self.text.add(self.status, (9, info_top + 6 * 17 + 6), 11,
                      SELECTED_COLOR if self.status_ticks else (120, 120, 120))

        title, image = stages[self.selected]
        main = cv2.resize(overlay(image), (self.main_width, self.main_height))
        canvas[: self.main_height, self.main_left : self.controls_left] = main

        cv2.rectangle(canvas, (self.main_left, 0), (self.controls_left, MAIN_TOOLBAR_HEIGHT),
                      (18, 18, 18), -1)
        for action, text, left, top, right, bottom in self.main_tool_boxes():
            on = action == "label_mode" and self.labeling
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          (45, 95, 45) if on else (52, 52, 52), -1)
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          SELECTED_COLOR if on else (135, 135, 135), 1)
            self.text.add(text, (left + 11, top + 6), 12,
                          (255, 255, 255) if on else (225, 225, 225))

        summary = (f"{self.video_path.name}   ·  {title}   ·  "
                   f"고름 {len(self.picked)}  ·  담김 {len(self.dataset)}장")
        self.text.add(summary, (self.main_tool_boxes()[-1][4] + 18, 12), 13, SELECTED_COLOR)

        left, right, track_y = self.scrub_track()
        cv2.rectangle(canvas, (self.main_left, self.main_height - SCRUB_HEIGHT),
                      (self.controls_left, self.main_height), (18, 18, 18), -1)
        cv2.line(canvas, (left, track_y), (right, track_y), (85, 85, 85), 4)
        ratio = self.frame_index / max(1, self.total_frames - 1)
        handle_x = int(left + ratio * (right - left))
        cv2.line(canvas, (left, track_y), (handle_x, track_y), (90, 170, 100), 4)
        for index in self.dataset:                      # 담아둔 프레임을 눈금으로 표시
            mark = int(left + index / max(1, self.total_frames - 1) * (right - left))
            cv2.line(canvas, (mark, track_y - 9), (mark, track_y - 4), PICKED_COLOR, 2)
        cv2.circle(canvas, (handle_x, track_y), 8, (120, 220, 130), -1)
        cv2.circle(canvas, (handle_x, track_y), 8, (25, 25, 25), 1)
        self.text.add(f"{self.frame_index} / {self.total_frames - 1}",
                      (right - 90, track_y - 20), 11, (200, 200, 200))

        for index, path, left, top, right, bottom in self.menu_boxes():
            current = path == self.video_path
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          (45, 85, 45) if current else (46, 46, 46), -1)
            cv2.rectangle(canvas, (left, top), (right, bottom), (120, 120, 120), 1)
            picks = len(self.datasets.get(path.stem, {}))
            note = f"   [{picks}장 담김]" if picks else ""
            self.text.add(f"{'▶' if current else '·'} {path.name}{note}",
                          (left + 12, top + 8), 12,
                          SELECTED_COLOR if current else (230, 230, 230))

        self.draw_controls(canvas)
        return self.text.flush(canvas)

    # ---- input --------------------------------------------------------
    def on_mouse(self, event: int, x: int, y: int, _flags: int, _param) -> None:
        cv2 = self.cv2

        if event == cv2.EVENT_LBUTTONDOWN and not self.menu_open and self.scrub_at(x, y):
            self.scrubbing = True
            self.paused = True
            self.seek_from_x(x)
            return

        if event == cv2.EVENT_LBUTTONDOWN and self.labeling and not self.menu_open:
            point = self.main_to_frame(x, y)
            if point is not None:
                self.box_start = self.box_now = point
                return

        if event == cv2.EVENT_LBUTTONDOWN and self.panel_tab != "params":
            return

        if event == cv2.EVENT_LBUTTONDOWN:
            index = self.slider_at(x, y)
            if index is None:
                return
            if self.sliders[index].is_check:
                self.sliders[index].value = 0 if self.sliders[index].value else 1
                return
            self.dragging = index
            self.set_slider_from_x(index, x)
            return

        if event == cv2.EVENT_MOUSEMOVE:
            if self.box_start is not None:
                point = self.main_to_frame(x, y)
                if point is not None:
                    self.box_now = point
                return
            if self.scrubbing:
                self.seek_from_x(x)
                return
            if self.dragging is not None:
                self.set_slider_from_x(self.dragging, x)
            elif self.panel_tab == "params":
                self.hover = self.slider_at(x, y)
            else:
                self.hover_frame = self.dataset_row_at(x, y)
            return

        if event != cv2.EVENT_LBUTTONUP:
            return

        if self.box_start is not None:
            start, self.box_start = self.box_start, None
            end = self.main_to_frame(x, y) or self.box_now
            self.box_now = None
            if end is not None:
                # 거의 안 움직였으면 클릭으로 본다 (원 선택/해제)
                if max(abs(end[0] - start[0]), abs(end[1] - start[1])) < 6:
                    self.pick_at(start)
                else:
                    self.add_manual_box(start, end)
            return

        if self.scrubbing:
            self.seek_from_x(x)
            self.scrubbing = False
            return

        if self.dragging is not None:
            self.set_slider_from_x(self.dragging, x)
            self.dragging = None
            return

        tab = self.tab_at(x, y)
        if tab is not None:
            self.panel_tab = tab
            self.hover = None
            self.hover_frame = None
            return

        if self.format_button_at(x, y):
            self.export_seg = not self.export_seg
            self.status = "마스크 형식" if self.export_seg else "박스 형식"
            self.status_ticks = 90
            return

        if self.export_button_at(x, y):
            self.export_dataset()
            return

        target = self.dataset_row_at(x, y)
        if target is not None:
            self.paused = True
            self.seek(target)                 # 저장해둔 선택도 함께 복원된다
            self.status = f"f{target} 로 이동"
            self.status_ticks = 60
            return

        if self.menu_open:
            chosen = self.menu_at(x, y)
            if chosen is not None:
                self.open_video(chosen)
            else:
                self.menu_open = False
            return

        tool = self.main_tool_at(x, y)
        if tool == "open":
            self.videos = find_video_files(Path.cwd())
            self.menu_open = True
            self.paused = True
            return
        if tool == "label_mode":
            self.labeling = not self.labeling
            self.paused = True if self.labeling else self.paused
            self.status = f"라벨링 {'켜짐' if self.labeling else '꺼짐'}"
            self.status_ticks = 60
            return
        if tool == "clear_pick":
            self.picked = []
            self.autocommit()
            self.status = "이 프레임 선택 해제"
            self.status_ticks = 60
            return



        if self.roi_drawing:
            point = self.main_to_frame(x, y)
            if point is not None:
                self.roi_points.append(point)
                if len(self.roi_points) >= ROI_POINT_COUNT:
                    self.roi_drawing = False
                    self.status = "ROI 지정 완료"
                    self.status_ticks = 90
                return

        index = self.panel_index_at(x, y)
        if index is not None:
            self.selected = index
            return

        action = self.button_at(x, y)
        if action == "play_pause":
            self.paused = not self.paused
        elif action == "prev":
            self.paused = True
            self.seek(self.frame_index - 1)
        elif action == "next":
            self.paused = True
            self.seek(self.frame_index + 1)
        elif action == "roi":
            self.roi_drawing = True
            self.roi_points = []
            self.paused = True
            self.status = "ROI 4점을 클릭"
            self.status_ticks = 0
        elif action == "save":
            self.save_now()
        elif action == "quit":
            self.should_quit = True

    def save_now(self) -> None:
        save_settings(self.sliders, self.selected, self.roi_points)
        self.status = "설정을 저장했습니다"
        self.status_ticks = 90

    def handle_key(self, key: int) -> bool:
        normalized = key & 0xFF if key >= 0 else key
        if normalized == 27 and self.menu_open:
            self.menu_open = False
            return True
        if normalized in (ord("q"), 27):
            return False
        if normalized == ord(" "):
            self.paused = not self.paused
        elif normalized in (ord(","), 81):
            self.paused = True
            self.seek(self.frame_index - 1)
        elif normalized in (ord("."), 83):
            self.paused = True
            self.seek(self.frame_index + 1)
        elif normalized == ord("s"):
            self.save_now()
        elif normalized == ord("x"):
            self.roi_points = []
            self.roi_drawing = False
            self.status = "ROI 를 지웠습니다"
            self.status_ticks = 90
        elif ord("1") <= normalized <= ord("5"):
            self.selected = normalized - ord("1")
        return True

    # ---- loops --------------------------------------------------------
    def run(self) -> None:
        cv2 = self.cv2
        cv2.namedWindow(self.window, cv2.WINDOW_AUTOSIZE)
        cv2.setMouseCallback(self.window, self.on_mouse)
        if not self.seek(0):
            raise RuntimeError("첫 프레임을 읽지 못했습니다.")
        print(__doc__)

        try:
            while True:
                if not self.paused and not self.advance():
                    break
                cv2.imshow(self.window, self.render())
                key = cv2.waitKey(20 if self.paused else self.delay_ms)
                if key >= 0 and not self.handle_key(key):
                    break
                if self.should_quit:
                    break
        finally:
            save_settings(self.sliders, self.selected, self.roi_points)
            self.capture.release()
            cv2.destroyAllWindows()

    def export(self, out_path: Path, step: int = 10) -> None:
        cv2 = self.cv2

        self.seek(0)
        sample = self.render()
        height, width = sample.shape[:2]
        writer = cv2.VideoWriter(str(out_path), cv2.VideoWriter_fourcc(*"mp4v"), 30, (width, height))
        written = 0
        for index in range(0, self.total_frames, step):
            if not self.seek(index):
                break
            writer.write(self.render())
            written += 1
            if written % 25 == 0:
                print(f"  {written} 프레임 기록 (frame {index})")
        writer.release()
        self.capture.release()
        print(f"저장 완료: {out_path}  ({written} 프레임, {width}x{height})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--width", type=int, default=1600, help="창 전체 가로 크기")
    parser.add_argument("--video", default=None, help="영상 파일 (생략하면 선택창)")
    parser.add_argument("--stage", type=int, default=None, help="본문에 크게 볼 단계 1-5")
    parser.add_argument("--export", type=Path, default=None, help="합성 화면을 mp4 로 저장")
    parser.add_argument("--step", type=int, default=10, help="export 시 프레임 간격")
    args = parser.parse_args()

    if args.video:
        selected = Path(args.video)
    else:
        videos = find_video_files(Path.cwd())
        if not videos:
            print("현재 폴더에서 영상 파일을 찾지 못했습니다.")
            return 1
        selected = videos[0]        # 팝업 없이 첫 영상으로 시작, 상단 '영상 열기' 로 전환

    viewer = HoughViewer(selected, width=args.width)
    if args.stage is not None:
        viewer.selected = max(0, min(STAGE_COUNT - 1, args.stage - 1))
    elif SETTINGS_PATH.exists():
        try:
            stored = json.loads(SETTINGS_PATH.read_text()).get("_selected_stage")
            if isinstance(stored, int):
                viewer.selected = max(0, min(STAGE_COUNT - 1, stored))
        except (json.JSONDecodeError, OSError):
            pass

    if args.export:
        viewer.export(args.export, step=args.step)
    else:
        viewer.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
