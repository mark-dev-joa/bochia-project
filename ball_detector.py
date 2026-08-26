"""보치아 공 검출 - 고전 영상처리 파이프라인 (학습 불필요).

단계
    1. 코트 마스크    공은 바닥에만 있으므로 배경을 통째로 제거
    2. Hough 후보     원 후보 생성 (판별은 하지 않음)
    3. 원근 게이트    화면 y좌표에 맞는 크기인지 검사
    4. 구 음영 검증   위에서 조명받는 구는 위쪽 절반이 밝다
    5. 발목 veto      사람 발 위 후보는 기각 (기성 pose 모델 사용)
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence

ANKLE_KEYPOINTS = (15, 16)


@dataclass(frozen=True)
class BallDetection:
    x: int
    y: int
    radius: int
    shade: float
    edge: float


@dataclass(frozen=True)
class DetectorConfig:
    """원근 보정은 코트 위/아래 끝에서의 공 반지름으로 지정한다."""

    radius_far: int = 9
    radius_near: int = 37
    radius_tolerance: float = 0.45
    min_shade: float = 15.0
    min_edge: float = 1.5
    hough_param1: int = 180
    hough_param2: int = 14
    hough_min_dist: int = 18


@dataclass(frozen=True)
class StageCounts:
    raw: int = 0
    in_court: int = 0
    size_ok: int = 0
    shade_ok: int = 0
    final: int = 0

    def as_label(self) -> str:
        return f"{self.raw}>{self.in_court}>{self.size_ok}>{self.shade_ok}>{self.final}"


def build_court_mask(shape, polygon: Sequence[tuple[int, int]] | None):
    import cv2
    import numpy as np

    height, width = shape[:2]
    mask = np.zeros((height, width), np.uint8)
    if polygon is None or len(polygon) < 3:
        mask[:] = 255
        return mask
    cv2.fillPoly(mask, [np.asarray(polygon, np.int32)], 255)
    return mask


def polygon_row_span(polygon: Sequence[tuple[int, int]] | None, height: int) -> tuple[int, int]:
    if polygon is None or len(polygon) < 3:
        return 0, max(1, height - 1)
    rows = [int(point[1]) for point in polygon]
    top, bottom = min(rows), max(rows)
    return top, max(top + 1, bottom)


def expected_radius_at(y: int, top: int, bottom: int, config: DetectorConfig) -> float:
    """코트 위/아래 끝 반지름 사이를 선형 보간한다."""
    ratio = (y - top) / max(1, bottom - top)
    ratio = min(1.5, max(-0.5, ratio))
    return config.radius_far + (config.radius_near - config.radius_far) * ratio


def radius_bounds_at(y, top, bottom, config: DetectorConfig) -> tuple[float, float]:
    expected = max(2.0, expected_radius_at(y, top, bottom, config))
    return expected * (1 - config.radius_tolerance), expected * (1 + config.radius_tolerance)


def sphere_features(
    gray_float,
    magnitude,
    x: int,
    y: int,
    r: int,
    magnitude_mean: float | None = None,
) -> tuple[float, float]:
    """위에서 조명받는 구는 위쪽 절반이 아래쪽보다 밝다.

    후보 주변 패치에서만 계산한다. 전체 프레임 마스크를 후보마다 만들면
    후보 하나당 3ms 넘게 걸린다.
    """
    import numpy as np

    height, width = gray_float.shape
    if magnitude_mean is None:
        magnitude_mean = float(magnitude.mean())

    reach = int(np.ceil(r * 1.15)) + 1
    left, top_row = max(0, x - reach), max(0, y - reach)
    right, bottom_row = min(width, x + reach + 1), min(height, y + reach + 1)
    if right - left < 2 or bottom_row - top_row < 2:
        return -1e9, 0.0

    gray_patch = gray_float[top_row:bottom_row, left:right]
    magnitude_patch = magnitude[top_row:bottom_row, left:right]

    y_indices, x_indices = np.ogrid[top_row:bottom_row, left:right]
    squared_distance = (x_indices - x) ** 2 + (y_indices - y) ** 2

    inside = squared_distance <= (r * 0.75) ** 2
    upper = inside & (y_indices < y)
    lower = inside & (y_indices > y)
    if not upper.any() or not lower.any():
        return -1e9, 0.0

    shade = float(gray_patch[upper].mean() - gray_patch[lower].mean())
    ring = (squared_distance <= (r * 1.15) ** 2) & (squared_distance >= (r * 0.85) ** 2)
    edge = float(magnitude_patch[ring].mean() / (magnitude_mean + 1e-6)) if ring.any() else 0.0
    return shade, edge


def find_ankles(frame, pose_model, confidence_threshold: float = 0.3) -> list[tuple[float, float]]:
    if pose_model is None:
        return []

    results = pose_model.predict(frame, verbose=False)
    if not results:
        return []
    keypoints = results[0].keypoints
    if keypoints is None or keypoints.data is None:
        return []

    people = keypoints.data.cpu().numpy()
    return [
        (float(person[index][0]), float(person[index][1]))
        for person in people
        for index in ANKLE_KEYPOINTS
        if len(person) > index and person[index][2] >= confidence_threshold
    ]


def detect_balls(
    frame,
    court_polygon: Sequence[tuple[int, int]] | None = None,
    ankles: Sequence[tuple[float, float]] = (),
    config: DetectorConfig | None = None,
) -> tuple[list[BallDetection], StageCounts]:
    import cv2
    import numpy as np

    config = config or DetectorConfig()
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame.ndim == 3 else frame.copy()
    height, width = gray.shape

    top, bottom = polygon_row_span(court_polygon, height)
    search_min = max(3, int(config.radius_far * (1 - config.radius_tolerance)))
    search_max = max(search_min + 2, int(config.radius_near * (1 + config.radius_tolerance)))

    circles = cv2.HoughCircles(
        cv2.medianBlur(gray, 5),
        cv2.HOUGH_GRADIENT,
        dp=1.0,
        minDist=config.hough_min_dist,
        param1=config.hough_param1,
        param2=config.hough_param2,
        minRadius=search_min,
        maxRadius=search_max,
    )
    if circles is None:
        return [], StageCounts()

    candidates = np.rint(circles[0]).astype(int)
    court = build_court_mask(gray.shape, court_polygon)

    in_court = [
        (int(x), int(y), int(r))
        for x, y, r in candidates
        if 0 <= x < width and 0 <= y < height and court[y, x]
    ]

    size_ok = []
    for x, y, r in in_court:
        low, high = radius_bounds_at(y, top, bottom, config)
        if low <= r <= high:
            size_ok.append((x, y, r))

    gray_float = gray.astype(np.float32)
    magnitude = cv2.magnitude(
        cv2.Sobel(gray_float, cv2.CV_32F, 1, 0, ksize=3),
        cv2.Sobel(gray_float, cv2.CV_32F, 0, 1, ksize=3),
    )

    magnitude_mean = float(magnitude.mean())
    shade_ok = []
    for x, y, r in size_ok:
        shade, edge = sphere_features(gray_float, magnitude, x, y, r, magnitude_mean)
        if shade >= config.min_shade and edge >= config.min_edge:
            shade_ok.append(BallDetection(x, y, r, shade, edge))

    if ankles:
        final = [
            ball
            for ball in shade_ok
            if min(np.hypot(ball.x - ax, ball.y - ay) for ax, ay in ankles)
            >= max(60.0, ball.radius * 3.5)
        ]
    else:
        final = list(shade_ok)

    counts = StageCounts(
        raw=len(candidates),
        in_court=len(in_court),
        size_ok=len(size_ok),
        shade_ok=len(shade_ok),
        final=len(final),
    )
    return sorted(final, key=lambda ball: -ball.shade), counts


def draw_balls(frame, balls: Sequence[BallDetection], polygon=None, thickness: int = 2):
    import cv2
    import numpy as np

    display = frame.copy()
    if polygon is not None and len(polygon) >= 2:
        cv2.polylines(
            display,
            [np.asarray(polygon, np.int32)],
            len(polygon) >= 3,
            (255, 200, 0),
            thickness,
        )
    for index, ball in enumerate(balls, start=1):
        cv2.circle(display, (ball.x, ball.y), ball.radius, (0, 220, 0), thickness)
        cv2.circle(display, (ball.x, ball.y), 2, (0, 0, 255), -1)
        cv2.putText(
            display,
            str(index),
            (ball.x + ball.radius + 4, ball.y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            (0, 220, 0),
            2,
        )
    return display
