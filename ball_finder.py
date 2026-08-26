"""보치아 공 검출 GUI - 영상을 재생하면서 실시간으로 공을 찾는다.

조작
    Space   재생/일시정지          C  코트 지정 시작 (4점 클릭)
    D       검출 On/Off            R  사각 ROI 드래그 모드
    P       발목 veto On/Off       X  코트/ROI 지우기
    S       처음으로               F  한 프레임만 검출
    Q/Esc   종료

검출은 기본적으로 모든 프레임에서 돈다 (step=1). 검출이 약 8ms 라 120fps
원본보다 빠르지만, 사람이 보려면 slow ms 트랙바로 재생을 늦추면 된다.

    step      1이면 모든 프레임 (풀 프레임). 올리면 프레임을 건너뛴다.
    slow ms   프레임마다 추가로 기다리는 시간. 올릴수록 느린 재생.

코트를 지정하지 않으면 화면 전체에서 찾는다 (배경 오검출이 많아진다).
설정은 ball_finder_config.json 에 영상별로 저장된다.
"""
from __future__ import annotations

import json
from dataclasses import asdict, replace
from pathlib import Path

from ball_detector import DetectorConfig, StageCounts, detect_balls, draw_balls, find_ankles
from video_player import DEFAULT_POSE_WEIGHT, find_local_pose_weight, find_video_files, select_video_with_ui

CONFIG_PATH = Path("ball_finder_config.json")
MAX_DISPLAY_WIDTH = 1280
TOOLBAR_HEIGHT = 96
BUTTONS = [
    ("play_pause", "Play/Pause"),
    ("court", "Set Court"),
    ("roi", "Rect ROI"),
    ("detect", "Detect"),
    ("pose", "Pose Veto"),
    ("once", "Detect 1"),
    ("clear", "Clear"),
    ("save", "Save Cfg"),
    ("quit", "Quit"),
]


def load_config(video_name: str) -> tuple[list[tuple[int, int]] | None, DetectorConfig]:
    if not CONFIG_PATH.exists():
        return None, DetectorConfig()
    try:
        stored = json.loads(CONFIG_PATH.read_text()).get(video_name, {})
    except (json.JSONDecodeError, OSError):
        return None, DetectorConfig()

    polygon = stored.get("court")
    polygon = [tuple(point) for point in polygon] if polygon else None
    known = {field: value for field, value in stored.get("config", {}).items()
             if field in DetectorConfig.__dataclass_fields__}
    return polygon, DetectorConfig(**known)


def save_config(video_name: str, polygon, config: DetectorConfig) -> None:
    payload = {}
    if CONFIG_PATH.exists():
        try:
            payload = json.loads(CONFIG_PATH.read_text())
        except (json.JSONDecodeError, OSError):
            payload = {}
    payload[video_name] = {
        "court": [list(point) for point in polygon] if polygon else None,
        "config": asdict(config),
    }
    CONFIG_PATH.write_text(json.dumps(payload, indent=2))
    print(f"설정 저장: {CONFIG_PATH}")


def build_buttons(width: int, top: int):
    margin, gap, height = 10, 8, 40
    available = width - margin * 2 - gap * (len(BUTTONS) - 1)
    button_width = max(70, available // len(BUTTONS))

    buttons, left = [], margin
    for action, text in BUTTONS:
        right = min(width - margin, left + button_width)
        buttons.append((action, text, left, top, right, top + height))
        left = right + gap
    return buttons


def button_at(buttons, x: int, y: int) -> str | None:
    for action, _text, left, top, right, bottom in buttons:
        if left <= x <= right and top <= y <= bottom:
            return action
    return None


class BallFinder:
    def __init__(self, video_path: Path) -> None:
        import cv2

        self.cv2 = cv2
        self.video_path = video_path
        self.capture = cv2.VideoCapture(str(video_path))
        if not self.capture.isOpened():
            raise RuntimeError(f"영상을 열 수 없습니다: {video_path}")

        self.frame_width = int(self.capture.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.frame_height = int(self.capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fps = self.capture.get(cv2.CAP_PROP_FPS) or 30
        self.base_delay = max(1, round(1000 / fps))

        self.scale = min(1.0, MAX_DISPLAY_WIDTH / self.frame_width)
        self.display_width = int(self.frame_width * self.scale)
        self.display_height = int(self.frame_height * self.scale)

        self.court, self.config = load_config(video_path.name)
        self.frame = None
        self.paused = False
        self.detect_enabled = True
        self.pose_enabled = False
        self.pose_model = None
        self.tool = "none"
        self.pending_points: list[tuple[int, int]] = []
        self.drag_start = None
        self.drag_now = None
        self.balls = []
        self.counts = StageCounts()
        self.frame_index = 0
        self.step = 1          # 1 = 모든 프레임 검출 (풀 프레임)
        self.slow_ms = 0       # 재생을 늦추는 추가 지연
        self.detect_ms = 0.0
        self.window = f"Ball Finder - {video_path.name}"
        self.status = "C로 코트를 지정하면 오검출이 크게 줄어듭니다."

    # ---- geometry -----------------------------------------------------
    def to_frame_point(self, x: int, y: int) -> tuple[int, int] | None:
        if y >= self.display_height:
            return None
        return int(x / self.scale), int(y / self.scale)

    # ---- detection ----------------------------------------------------
    def load_pose_model(self):
        if self.pose_model is not None:
            return self.pose_model
        from ultralytics import YOLO

        weight = find_local_pose_weight()
        weight = str(weight) if weight else DEFAULT_POSE_WEIGHT
        print(f"pose weight 로딩: {weight}")
        self.pose_model = YOLO(weight)
        return self.pose_model

    def run_detection(self) -> None:
        import time

        if self.frame is None:
            return
        started = time.perf_counter()
        ankles = ()
        if self.pose_enabled:
            try:
                ankles = find_ankles(self.frame, self.load_pose_model())
            except Exception as exc:  # 모델이 없거나 로딩 실패해도 검출은 계속
                self.pose_enabled = False
                self.status = f"pose 사용 불가: {exc}"
        self.balls, self.counts = detect_balls(
            self.frame,
            court_polygon=self.court,
            ankles=ankles,
            config=self.config,
        )
        self.detect_ms = (time.perf_counter() - started) * 1000

    # ---- trackbars ----------------------------------------------------
    def create_trackbars(self) -> None:
        cv2 = self.cv2
        cv2.createTrackbar("r far", self.window, self.config.radius_far, 60, lambda _v: None)
        cv2.createTrackbar("r near", self.window, self.config.radius_near, 120, lambda _v: None)
        cv2.createTrackbar("tol %", self.window, int(self.config.radius_tolerance * 100), 90, lambda _v: None)
        cv2.createTrackbar("min shade", self.window, int(self.config.min_shade), 60, lambda _v: None)
        cv2.createTrackbar("step", self.window, self.step, 10, lambda _v: None)
        cv2.createTrackbar("slow ms", self.window, self.slow_ms, 300, lambda _v: None)

    def read_trackbars(self) -> None:
        cv2 = self.cv2
        far = max(3, cv2.getTrackbarPos("r far", self.window))
        near = max(far + 1, cv2.getTrackbarPos("r near", self.window))
        self.config = replace(
            self.config,
            radius_far=far,
            radius_near=near,
            radius_tolerance=max(0.05, cv2.getTrackbarPos("tol %", self.window) / 100),
            min_shade=float(cv2.getTrackbarPos("min shade", self.window)),
        )
        self.step = max(1, cv2.getTrackbarPos("step", self.window))
        self.slow_ms = max(0, cv2.getTrackbarPos("slow ms", self.window))

    # ---- input --------------------------------------------------------
    def on_mouse(self, event: int, x: int, y: int, _flags: int, _param) -> None:
        cv2 = self.cv2

        if event == cv2.EVENT_LBUTTONUP:
            action = button_at(build_buttons(self.display_width, self.display_height + 8), x, y)
            if action is not None:
                self.handle_action(action)
                return

        point = self.to_frame_point(x, y)
        if point is None:
            return

        if self.tool == "court" and event == cv2.EVENT_LBUTTONUP:
            self.pending_points.append(point)
            self.status = f"코트 점 {len(self.pending_points)}/4"
            if len(self.pending_points) == 4:
                self.court = list(self.pending_points)
                self.pending_points = []
                self.tool = "none"
                self.status = "코트 지정 완료"
                self.run_detection()
            return

        if self.tool == "roi":
            if event == cv2.EVENT_LBUTTONDOWN:
                self.drag_start = point
                self.drag_now = point
            elif event == cv2.EVENT_MOUSEMOVE and self.drag_start:
                self.drag_now = point
            elif event == cv2.EVENT_LBUTTONUP and self.drag_start:
                (x0, y0), (x1, y1) = self.drag_start, point
                left, right = sorted((x0, x1))
                top, bottom = sorted((y0, y1))
                if right - left > 20 and bottom - top > 20:
                    self.court = [(left, top), (right, top), (right, bottom), (left, bottom)]
                    self.status = f"ROI {right - left}x{bottom - top}"
                    self.run_detection()
                self.drag_start = self.drag_now = None
                self.tool = "none"

    def handle_action(self, action: str) -> bool:
        if action == "play_pause":
            self.paused = not self.paused
        elif action == "court":
            self.paused = True
            self.tool = "court"
            self.pending_points = []
            self.status = "코트 4점을 클릭하세요 (먼 쪽 → 가까운 쪽)"
        elif action == "roi":
            self.paused = True
            self.tool = "roi"
            self.status = "사각형을 드래그하세요"
        elif action == "detect":
            self.detect_enabled = not self.detect_enabled
            self.status = f"검출 {'ON' if self.detect_enabled else 'OFF'}"
        elif action == "pose":
            self.pose_enabled = not self.pose_enabled
            self.status = f"발목 veto {'ON' if self.pose_enabled else 'OFF'}"
            if self.paused:
                self.run_detection()
        elif action == "once":
            self.paused = True
            self.run_detection()
            self.status = f"단계별 후보: {self.counts.as_label()}"
        elif action == "clear":
            self.court = None
            self.pending_points = []
            self.balls = []
            self.tool = "none"
            self.status = "코트/ROI를 지웠습니다"
        elif action == "save":
            save_config(self.video_path.name, self.court, self.config)
            self.status = "설정을 저장했습니다"
        elif action == "quit":
            return False
        return True

    def handle_key(self, key: int) -> bool:
        mapping = {
            ord(" "): "play_pause",
            ord("c"): "court",
            ord("r"): "roi",
            ord("d"): "detect",
            ord("p"): "pose",
            ord("f"): "once",
            ord("x"): "clear",
        }
        normalized = key & 0xFF if key >= 0 else key
        if normalized in (ord("q"), 27):
            return False
        if normalized == ord("s"):
            self.capture.set(self.cv2.CAP_PROP_POS_FRAMES, 0)
            self.frame_index = 0
            self.paused = True
            return True
        if normalized in mapping:
            return self.handle_action(mapping[normalized])
        return True

    # ---- rendering ----------------------------------------------------
    def render(self):
        cv2, np = self.cv2, __import__("numpy")

        overlay = draw_balls(self.frame, self.balls, self.court, thickness=2)
        for index, point in enumerate(self.pending_points, start=1):
            cv2.circle(overlay, point, 8, (255, 0, 255), -1)
            cv2.putText(overlay, str(index), (point[0] + 10, point[1] - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 0, 255), 2)
        if self.drag_start and self.drag_now:
            cv2.rectangle(overlay, self.drag_start, self.drag_now, (255, 200, 0), 2)

        display = cv2.resize(overlay, (self.display_width, self.display_height))
        canvas = np.full((self.display_height + TOOLBAR_HEIGHT, self.display_width, 3), 30, np.uint8)
        canvas[: self.display_height] = display

        headline = (
            f"frame {self.frame_index}  |  balls {len(self.balls)}  "
            f"|  stages {self.counts.as_label()}  |  detect {'ON' if self.detect_enabled else 'OFF'}"
            f" {self.detect_ms:.0f}ms  |  pose {'ON' if self.pose_enabled else 'OFF'}"
            f"  |  step {self.step}{' (full)' if self.step == 1 else ''}  |  +{self.slow_ms}ms"
        )
        cv2.putText(canvas, headline, (12, self.display_height + 62),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 255, 200), 1)
        cv2.putText(canvas, self.status, (12, self.display_height + 84),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (170, 170, 170), 1)

        for action, text, left, top, right, bottom in build_buttons(self.display_width, self.display_height + 8):
            active = (
                (action == "detect" and self.detect_enabled)
                or (action == "pose" and self.pose_enabled)
                or (action == "play_pause" and self.paused)
                or action == self.tool
            )
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          (55, 120, 65) if active else (55, 55, 55), -1)
            cv2.rectangle(canvas, (left, top), (right, bottom),
                          (120, 220, 130) if active else (140, 140, 140), 1)
            cv2.putText(canvas, text, (left + 6, top + 26),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.42, (240, 240, 240), 1)
        return canvas

    # ---- main loop ----------------------------------------------------
    def run(self) -> None:
        cv2 = self.cv2
        cv2.namedWindow(self.window, cv2.WINDOW_AUTOSIZE)
        cv2.setMouseCallback(self.window, self.on_mouse)
        self.create_trackbars()

        ok, self.frame = self.capture.read()
        if not ok:
            raise RuntimeError("첫 프레임을 읽지 못했습니다.")
        print(__doc__)

        try:
            while True:
                self.read_trackbars()

                if not self.paused:
                    for _ in range(self.step):
                        ok, frame = self.capture.read()
                        if not ok:
                            self.capture.set(cv2.CAP_PROP_POS_FRAMES, 0)
                            self.frame_index = 0
                            ok, frame = self.capture.read()
                            if not ok:
                                return
                        self.frame_index += 1
                    self.frame = frame

                    if self.detect_enabled:
                        self.run_detection()

                cv2.imshow(self.window, self.render())
                key = cv2.waitKey(20 if self.paused else max(1, self.slow_ms))
                if key >= 0 and not self.handle_key(key):
                    return
        finally:
            self.capture.release()
            cv2.destroyAllWindows()


def main() -> int:
    videos = find_video_files(Path.cwd())
    if not videos:
        print("현재 폴더에서 영상 파일을 찾지 못했습니다.")
        return 1

    selected = select_video_with_ui(videos)
    if selected is None:
        print("영상 선택이 취소되었습니다.")
        return 0

    BallFinder(selected).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
