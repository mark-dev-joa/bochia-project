from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
from typing import Callable, Sequence


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


@dataclass(frozen=True)
class PlaybackState:
    paused: bool
    stopped: bool
    should_quit: bool


@dataclass(frozen=True)
class Point:
    x: int
    y: int


@dataclass(frozen=True)
class ROI:
    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class Ball:
    x: int
    y: int
    radius: int


@dataclass(frozen=True)
class VideoMenuItem:
    index: int
    left: int
    top: int
    right: int
    bottom: int


@dataclass(frozen=True)
class ControlButton:
    action: str
    label: str
    left: int
    top: int
    right: int
    bottom: int


class RoiSelector:
    def __init__(self, min_size: int = 8) -> None:
        self.min_size = min_size
        self.roi: ROI | None = None
        self.start: tuple[int, int] | None = None
        self.current: tuple[int, int] | None = None
        self.is_drawing = False

    def begin(self, x: int, y: int) -> None:
        self.start = (x, y)
        self.current = (x, y)
        self.is_drawing = True

    def update(self, x: int, y: int) -> None:
        if not self.is_drawing:
            return
        self.current = (x, y)

    def finish(self, frame_width: int, frame_height: int) -> ROI | None:
        if self.start is None or self.current is None:
            self.is_drawing = False
            return self.roi

        roi = normalize_roi(
            self.start,
            self.current,
            frame_width=frame_width,
            frame_height=frame_height,
            min_size=self.min_size,
        )
        self.is_drawing = False
        self.start = None
        self.current = None

        if roi is not None:
            self.roi = roi

        return self.roi

    def preview(self, frame_width: int, frame_height: int) -> ROI | None:
        if self.start is None or self.current is None:
            return self.roi

        return normalize_roi(
            self.start,
            self.current,
            frame_width=frame_width,
            frame_height=frame_height,
            min_size=self.min_size,
        )

    def clear(self) -> None:
        self.roi = None
        self.start = None
        self.current = None
        self.is_drawing = False


class HomographySelector:
    def __init__(self) -> None:
        self.points: list[Point] = []
        self.is_selecting = False

    @property
    def is_complete(self) -> bool:
        return len(self.points) == 4

    def start(self) -> None:
        self.points = []
        self.is_selecting = True

    def add_point(self, x: int, y: int) -> bool:
        if self.is_complete:
            return False
        self.points.append(Point(x=x, y=y))
        if self.is_complete:
            self.is_selecting = False
        return True

    def clear(self) -> None:
        self.points = []
        self.is_selecting = False


def normalize_roi(
    start: tuple[int, int],
    end: tuple[int, int],
    frame_width: int,
    frame_height: int,
    min_size: int = 8,
) -> ROI | None:
    start_x, start_y = start
    end_x, end_y = end

    left = max(0, min(start_x, end_x))
    top = max(0, min(start_y, end_y))
    right = min(frame_width, max(start_x, end_x))
    bottom = min(frame_height, max(start_y, end_y))

    width = right - left
    height = bottom - top
    if width < min_size or height < min_size:
        return None

    return ROI(x=left, y=top, width=width, height=height)


def detect_ball_in_roi(frame, roi: ROI) -> Ball | None:
    import cv2
    import numpy as np

    frame_height, frame_width = frame.shape[:2]
    safe_roi = normalize_roi(
        (roi.x, roi.y),
        (roi.x + roi.width, roi.y + roi.height),
        frame_width=frame_width,
        frame_height=frame_height,
    )
    if safe_roi is None:
        return None

    crop = frame[
        safe_roi.y : safe_roi.y + safe_roi.height,
        safe_roi.x : safe_roi.x + safe_roi.width,
    ]
    if crop.size == 0:
        return None

    if len(crop.shape) == 3:
        gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
    else:
        gray = crop.copy()

    gray = cv2.equalizeHist(gray)
    gray = cv2.medianBlur(gray, 5)

    min_dimension = min(safe_roi.width, safe_roi.height)
    min_radius = max(4, min_dimension // 40)
    max_radius = max(min_radius + 2, min(180, min_dimension // 3))
    min_distance = max(12, min_dimension // 4)

    circles = cv2.HoughCircles(
        gray,
        cv2.HOUGH_GRADIENT,
        dp=1.2,
        minDist=min_distance,
        param1=100,
        param2=18,
        minRadius=min_radius,
        maxRadius=max_radius,
    )
    if circles is None:
        return None

    rounded_circles = np.rint(circles[0]).astype(int)
    best_circle = rounded_circles[0]
    return Ball(
        x=safe_roi.x + int(best_circle[0]),
        y=safe_roi.y + int(best_circle[1]),
        radius=int(best_circle[2]),
    )


def distance_between(first: Point, second: Point) -> float:
    return ((first.x - second.x) ** 2 + (first.y - second.y) ** 2) ** 0.5


def order_homography_points(points: Sequence[Point]) -> list[Point]:
    if len(points) != 4:
        raise ValueError("호모그래피에는 4점이 필요합니다.")

    sorted_by_y = sorted(points, key=lambda point: (point.y, point.x))
    top_points = sorted(sorted_by_y[:2], key=lambda point: point.x)
    bottom_points = sorted(sorted_by_y[2:], key=lambda point: point.x)

    top_left, top_right = top_points
    bottom_left, bottom_right = bottom_points
    return [top_left, top_right, bottom_right, bottom_left]


def calculate_homography_size(ordered_points: Sequence[Point]) -> tuple[int, int]:
    if len(ordered_points) != 4:
        raise ValueError("호모그래피에는 4점이 필요합니다.")

    top_left, top_right, bottom_right, bottom_left = ordered_points
    width = round(
        max(
            distance_between(top_left, top_right),
            distance_between(bottom_left, bottom_right),
        )
    )
    height = round(
        max(
            distance_between(top_left, bottom_left),
            distance_between(top_right, bottom_right),
        )
    )
    return max(1, width), max(1, height)


def apply_homography(frame, points: Sequence[Point]):
    import cv2
    import numpy as np

    ordered_points = order_homography_points(points)
    width, height = calculate_homography_size(ordered_points)
    source = np.array(
        [[point.x, point.y] for point in ordered_points],
        dtype=np.float32,
    )
    destination = np.array(
        [
            [0, 0],
            [width - 1, 0],
            [width - 1, height - 1],
            [0, height - 1],
        ],
        dtype=np.float32,
    )
    matrix = cv2.getPerspectiveTransform(source, destination)
    return cv2.warpPerspective(frame, matrix, (width, height))


def build_control_buttons(frame_width: int, frame_height: int) -> list[ControlButton]:
    labels = [
        ("play_pause", "Play/Pause"),
        ("stop", "Stop"),
        ("roi", "ROI"),
        ("measure", "Measure"),
        ("find_ball", "Find"),
        ("homography", "4-Point"),
        ("bandpass", "Band"),
        ("skeleton", "Skeleton"),
        ("clear", "Clear"),
        ("quit", "Quit"),
    ]
    margin = 10
    gap = 8
    button_height = 44
    top = max(0, frame_height - button_height - 10)
    available_width = frame_width - margin * 2 - gap * (len(labels) - 1)
    button_width = max(64, available_width // len(labels))

    buttons = []
    left = margin
    for action, label in labels:
        right = min(frame_width - margin, left + button_width)
        buttons.append(
            ControlButton(
                action=action,
                label=label,
                left=left,
                top=top,
                right=right,
                bottom=top + button_height,
            )
        )
        left = right + gap
    return buttons


def find_control_button_at(
    buttons: Sequence[ControlButton],
    x: int,
    y: int,
) -> str | None:
    for button in buttons:
        if button.left <= x <= button.right and button.top <= y <= button.bottom:
            return button.action
    return None


def draw_control_buttons(
    frame,
    buttons: Sequence[ControlButton],
    active_tool: str,
    is_paused: bool,
    bandpass_enabled: bool,
) -> None:
    import cv2

    for button in buttons:
        is_active = (
            button.action == active_tool
            or (button.action == "play_pause" and is_paused)
            or (button.action == "bandpass" and bandpass_enabled)
        )
        fill_color = (55, 120, 65) if is_active else (55, 55, 55)
        border_color = (120, 220, 130) if is_active else (150, 150, 150)
        cv2.rectangle(
            frame,
            (button.left, button.top),
            (button.right, button.bottom),
            fill_color,
            -1,
        )
        cv2.rectangle(
            frame,
            (button.left, button.top),
            (button.right, button.bottom),
            border_color,
            1,
        )
        cv2.putText(
            frame,
            button.label,
            (button.left + 8, button.top + 28),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.45,
            (245, 245, 245),
            1,
        )


def format_roi_size_label(roi: ROI) -> str:
    return f"{roi.width} x {roi.height} px"


def map_panorama_image_point(
    x: int,
    y: int,
    frame_width: int,
    frame_height: int,
) -> Point | None:
    if 0 <= x < frame_width and 0 <= y < frame_height:
        return Point(x=x, y=y)
    return None


def draw_panel_label(frame, label: str, position: tuple[int, int]) -> None:
    import cv2

    x, y = position
    cv2.rectangle(frame, (x - 8, y - 24), (x + 178, y + 10), (0, 0, 0), -1)
    cv2.putText(
        frame,
        label,
        (x, y),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 255),
        2,
    )


def build_panorama_display(
    original_frame,
    processed_frame,
    otsu_frame,
    skeleton_frame,
    active_tool: str,
    is_paused: bool,
    bandpass_enabled: bool,
):
    import cv2
    import numpy as np

    frame_height, frame_width = original_frame.shape[:2]
    if processed_frame.shape[:2] != (frame_height, frame_width):
        processed_frame = cv2.resize(processed_frame, (frame_width, frame_height))
    if otsu_frame.shape[:2] != (frame_height, frame_width):
        otsu_frame = cv2.resize(otsu_frame, (frame_width, frame_height))
    if skeleton_frame.shape[:2] != (frame_height, frame_width):
        skeleton_frame = cv2.resize(skeleton_frame, (frame_width, frame_height))

    left_panel = original_frame.copy()
    right_panel = processed_frame.copy()
    otsu_panel = otsu_frame.copy()
    skeleton_panel = skeleton_frame.copy()
    draw_panel_label(left_panel, "Original", (12, 28))
    draw_panel_label(right_panel, "Processed", (12, 28))
    draw_panel_label(otsu_panel, "Otsu + Contours", (12, 28))
    draw_panel_label(skeleton_panel, "YOLO Skeleton", (12, 28))

    image_row = cv2.hconcat([left_panel, right_panel, otsu_panel, skeleton_panel])
    toolbar_height = 54
    toolbar = np.full((toolbar_height, frame_width * 4, 3), 34, dtype=np.uint8)
    panorama = cv2.vconcat([image_row, toolbar])
    buttons = build_control_buttons(frame_width * 4, frame_height + toolbar_height)
    draw_control_buttons(
        panorama,
        buttons,
        active_tool=active_tool,
        is_paused=is_paused,
        bandpass_enabled=bandpass_enabled,
    )
    return panorama, buttons


def clamp_bandpass_cutoffs(low: int, high: int, max_radius: int) -> tuple[int, int]:
    max_radius = max(2, max_radius)
    high = min(max(1, high), max_radius)
    low = min(max(0, low), high - 1)
    return low, high


def apply_spatial_bandpass(frame, low_cutoff: int, high_cutoff: int):
    import cv2
    import numpy as np

    if len(frame.shape) == 3:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = frame.copy()

    height, width = gray.shape[:2]
    max_radius = min(width, height) // 2
    low_cutoff, high_cutoff = clamp_bandpass_cutoffs(
        low=low_cutoff,
        high=high_cutoff,
        max_radius=max_radius,
    )

    y_indices, x_indices = np.ogrid[:height, :width]
    center_y = height // 2
    center_x = width // 2
    radius = np.sqrt((x_indices - center_x) ** 2 + (y_indices - center_y) ** 2)
    mask = (radius >= low_cutoff) & (radius <= high_cutoff)

    spectrum = np.fft.fftshift(np.fft.fft2(gray.astype(np.float32)))
    filtered = spectrum * mask
    inverse = np.fft.ifft2(np.fft.ifftshift(filtered))
    magnitude = np.abs(inverse)
    normalized = cv2.normalize(magnitude, None, 0, 255, cv2.NORM_MINMAX)
    filtered_gray = normalized.astype(np.uint8)

    if len(frame.shape) == 3:
        return cv2.cvtColor(filtered_gray, cv2.COLOR_GRAY2BGR)
    return filtered_gray


def apply_otsu_threshold(frame):
    import cv2

    if len(frame.shape) == 3:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = frame.copy()

    blurred = cv2.GaussianBlur(gray, (5, 5), 0)
    _threshold, binary = cv2.threshold(
        blurred,
        0,
        255,
        cv2.THRESH_BINARY | cv2.THRESH_OTSU,
    )
    return cv2.cvtColor(binary, cv2.COLOR_GRAY2BGR)


def find_contours_in_binary(binary_frame, min_area: float = 40):
    import cv2

    if len(binary_frame.shape) == 3:
        gray = cv2.cvtColor(binary_frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = binary_frame.copy()

    contours, _hierarchy = cv2.findContours(
        gray,
        cv2.RETR_EXTERNAL,
        cv2.CHAIN_APPROX_SIMPLE,
    )
    return [
        contour
        for contour in contours
        if cv2.contourArea(contour) >= min_area
    ]


def draw_contours_on_frame(frame, contours):
    import cv2

    display_frame = frame.copy()
    cv2.drawContours(display_frame, contours, -1, (0, 255, 0), 2)
    return display_frame


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


def draw_yolo_skeletons(frame, keypoints, confidence_threshold: float = 0.25):
    import cv2
    import numpy as np

    display_frame = frame.copy()
    keypoints_array = np.asarray(keypoints)
    if keypoints_array.size == 0:
        return display_frame

    for person in keypoints_array:
        for start_index, end_index in YOLO_POSE_EDGES:
            if start_index >= len(person) or end_index >= len(person):
                continue

            start = person[start_index]
            end = person[end_index]
            start_confidence = start[2] if len(start) > 2 else 1.0
            end_confidence = end[2] if len(end) > 2 else 1.0
            if start_confidence < confidence_threshold or end_confidence < confidence_threshold:
                continue

            start_point = (int(round(start[0])), int(round(start[1])))
            end_point = (int(round(end[0])), int(round(end[1])))
            cv2.line(display_frame, start_point, end_point, (255, 0, 255), 2)

        for keypoint in person:
            confidence = keypoint[2] if len(keypoint) > 2 else 1.0
            if confidence < confidence_threshold:
                continue
            center = (int(round(keypoint[0])), int(round(keypoint[1])))
            cv2.circle(display_frame, center, 4, (0, 255, 255), -1)

    return display_frame


def run_yolo_pose(frame, model):
    import cv2
    import numpy as np

    results = model.predict(frame, verbose=False)
    if not results:
        return frame.copy(), np.empty((0, 17, 3), dtype=float)

    result = results[0]
    keypoints = result.keypoints
    if keypoints is None or keypoints.data is None:
        return frame.copy(), np.empty((0, 17, 3), dtype=float)

    keypoints_array = keypoints.data.cpu().numpy()
    skeleton_frame = draw_yolo_skeletons(frame, keypoints_array)
    if len(skeleton_frame.shape) == 3 and skeleton_frame.shape[2] == 3:
        return skeleton_frame, keypoints_array
    return cv2.cvtColor(skeleton_frame, cv2.COLOR_GRAY2BGR), keypoints_array


def find_video_files(directory: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in directory.iterdir()
            if path.is_file() and path.suffix.lower() in SUPPORTED_VIDEO_EXTENSIONS
        ),
        key=lambda path: path.name.lower(),
    )


def build_video_menu_items(videos: Sequence[Path], width: int) -> list[VideoMenuItem]:
    margin = 36
    top = 86
    item_height = 56
    gap = 12

    return [
        VideoMenuItem(
            index=index,
            left=margin,
            top=top + index * (item_height + gap),
            right=width - margin,
            bottom=top + index * (item_height + gap) + item_height,
        )
        for index, _video in enumerate(videos)
    ]


def find_video_menu_item_at(
    items: Sequence[VideoMenuItem],
    x: int,
    y: int,
) -> int | None:
    for item in items:
        if item.left <= x <= item.right and item.top <= y <= item.bottom:
            return item.index
    return None


def parse_video_selection(raw_choice: str, videos: Sequence[Path]) -> int:
    try:
        selected = int(raw_choice.strip())
    except ValueError as exc:
        raise ValueError("숫자로 선택해 주세요.") from exc

    if selected < 1 or selected > len(videos):
        raise ValueError(f"1부터 {len(videos)} 사이의 번호를 선택해 주세요.")

    return selected - 1


def handle_key(key: int, state: PlaybackState) -> PlaybackState:
    normalized_key = key & 0xFF if key >= 0 else key

    if normalized_key in (ord("q"), 27):
        return PlaybackState(
            paused=state.paused,
            stopped=state.stopped,
            should_quit=True,
        )

    if normalized_key == ord(" "):
        return PlaybackState(
            paused=not state.paused,
            stopped=False,
            should_quit=False,
        )

    if normalized_key == ord("s"):
        return PlaybackState(paused=True, stopped=True, should_quit=False)

    return PlaybackState(
        paused=state.paused,
        stopped=False,
        should_quit=state.should_quit,
    )


def prompt_for_video(videos: Sequence[Path]) -> Path:
    print("영상 목록:")
    for index, video in enumerate(videos, start=1):
        print(f"{index}. {video.name}")

    while True:
        raw_choice = input("재생할 영상 번호를 선택하세요: ")
        try:
            selected_index = parse_video_selection(raw_choice, videos)
        except ValueError as exc:
            print(exc)
            continue

        return videos[selected_index]


def draw_video_menu(canvas, videos: Sequence[Path], items: Sequence[VideoMenuItem]) -> None:
    import cv2

    canvas[:] = (36, 36, 36)
    cv2.putText(
        canvas,
        "Select video",
        (36, 48),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.0,
        (235, 235, 235),
        2,
    )
    cv2.putText(
        canvas,
        "Click a video, or press 1-9. Esc/q cancels.",
        (36, 74),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (185, 185, 185),
        1,
    )

    for item in items:
        cv2.rectangle(
            canvas,
            (item.left, item.top),
            (item.right, item.bottom),
            (82, 82, 82),
            -1,
        )
        cv2.rectangle(
            canvas,
            (item.left, item.top),
            (item.right, item.bottom),
            (140, 140, 140),
            1,
        )
        label = f"{item.index + 1}. {videos[item.index].name}"
        cv2.putText(
            canvas,
            label,
            (item.left + 18, item.top + 36),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.72,
            (245, 245, 245),
            2,
        )


def select_video_with_ui(videos: Sequence[Path]) -> Path | None:
    import cv2
    import numpy as np

    window_name = "Video Selection"
    width = 720
    height = max(240, 112 + len(videos) * 68)
    items = build_video_menu_items(videos, width)
    selected_index: dict[str, int | None] = {"value": None}

    def on_mouse(event: int, x: int, y: int, _flags: int, _param) -> None:
        if event != cv2.EVENT_LBUTTONUP:
            return
        selected_index["value"] = find_video_menu_item_at(items, x=x, y=y)

    cv2.namedWindow(window_name, cv2.WINDOW_AUTOSIZE)
    cv2.setMouseCallback(window_name, on_mouse)

    try:
        while selected_index["value"] is None:
            canvas = np.zeros((height, width, 3), dtype=np.uint8)
            draw_video_menu(canvas, videos, items)
            cv2.imshow(window_name, canvas)

            key = cv2.waitKey(30)
            normalized_key = key & 0xFF if key >= 0 else key
            if normalized_key in (ord("q"), 27):
                return None
            if ord("1") <= normalized_key <= ord("9"):
                index = normalized_key - ord("1")
                if index < len(videos):
                    selected_index["value"] = index
    finally:
        cv2.destroyWindow(window_name)

    return videos[selected_index["value"]]


def get_frame_delay_ms(capture) -> int:
    fps = capture.get(5)
    if fps <= 0:
        fps = 30
    return max(1, round(1000 / fps))


def play_video(video_path: Path) -> None:
    import cv2
    import numpy as np

    capture = cv2.VideoCapture(str(video_path))
    if not capture.isOpened():
        raise RuntimeError(f"영상을 열 수 없습니다: {video_path}")

    window_name = f"Video Player - {video_path.name}"
    delay_ms = get_frame_delay_ms(capture)
    state = PlaybackState(paused=False, stopped=False, should_quit=False)
    current_frame = None
    roi_selector = RoiSelector()
    measure_selector = RoiSelector()
    homography_selector = HomographySelector()
    detected_ball: Ball | None = None
    skeleton_frame = None
    skeleton_keypoint_count = 0
    pose_model = None
    active_tool = "none"
    bandpass_enabled = False
    controls_window_name = "Controls"
    homography_window_name = "Homography View"

    def get_bandpass_cutoffs(frame_width: int, frame_height: int) -> tuple[int, int]:
        max_radius = min(frame_width, frame_height) // 2
        low = cv2.getTrackbarPos("Low cutoff", controls_window_name)
        high = cv2.getTrackbarPos("High cutoff", controls_window_name)
        return clamp_bandpass_cutoffs(low=low, high=high, max_radius=max_radius)

    def get_analysis_frame():
        if current_frame is None:
            return None
        if not bandpass_enabled:
            return current_frame
        frame_height, frame_width = current_frame.shape[:2]
        low, high = get_bandpass_cutoffs(frame_width=frame_width, frame_height=frame_height)
        return apply_spatial_bandpass(current_frame, low_cutoff=low, high_cutoff=high)

    def find_ball_from_current_roi() -> None:
        nonlocal detected_ball

        analysis_frame = get_analysis_frame()
        if analysis_frame is None:
            print("현재 프레임이 없습니다.")
            return
        if roi_selector.roi is None:
            print("ROI를 먼저 선택하세요.")
            return

        detected_ball = detect_ball_in_roi(analysis_frame, roi_selector.roi)
        if detected_ball is None:
            print("ROI 안에서 공을 찾지 못했습니다.")
            return

        print(
            "공 찾음: "
            f"x={detected_ball.x}, y={detected_ball.y}, radius={detected_ball.radius}"
        )

    def apply_current_homography() -> None:
        analysis_frame = get_analysis_frame()
        if analysis_frame is None:
            print("현재 프레임이 없습니다.")
            return
        if not homography_selector.is_complete:
            print("호모그래피 4점을 먼저 선택하세요.")
            return

        warped = apply_homography(analysis_frame, homography_selector.points)
        cv2.imshow(homography_window_name, warped)
        print(
            "호모그래피 적용됨: "
            f"points={homography_selector.points}, size={warped.shape[1]}x{warped.shape[0]}"
        )

    def load_pose_model():
        nonlocal pose_model

        if pose_model is not None:
            return pose_model

        from ultralytics import YOLO

        local_weight = find_local_pose_weight()
        weight = str(local_weight) if local_weight is not None else DEFAULT_POSE_WEIGHT
        print(f"YOLO pose weight 로딩: {weight}")
        pose_model = YOLO(weight)
        return pose_model

    def update_skeleton_output() -> None:
        nonlocal skeleton_frame, skeleton_keypoint_count

        analysis_frame = get_analysis_frame()
        if analysis_frame is None:
            print("현재 프레임이 없습니다.")
            return

        try:
            model = load_pose_model()
            skeleton_frame, keypoints = run_yolo_pose(analysis_frame, model)
        except Exception as exc:
            skeleton_frame = analysis_frame.copy()
            skeleton_keypoint_count = 0
            print(f"YOLO pose 실행 실패: {exc}")
            return

        skeleton_keypoint_count = int(len(keypoints))
        print(f"YOLO skeleton 출력: persons={skeleton_keypoint_count}")

    def handle_button_action(action: str) -> None:
        nonlocal active_tool, bandpass_enabled, detected_ball, skeleton_frame, skeleton_keypoint_count, state

        if action == "play_pause":
            state = handle_key(ord(" "), state)
            if not state.paused:
                active_tool = "none"
                detected_ball = None
                skeleton_frame = None
                skeleton_keypoint_count = 0
            return

        if action == "stop":
            state = PlaybackState(paused=True, stopped=True, should_quit=False)
            active_tool = "none"
            detected_ball = None
            skeleton_frame = None
            skeleton_keypoint_count = 0
            return

        if action == "roi":
            state = PlaybackState(paused=True, stopped=False, should_quit=False)
            active_tool = "roi"
            print("ROI 모드: 공 주변을 드래그하세요.")
            return

        if action == "measure":
            state = PlaybackState(paused=True, stopped=False, should_quit=False)
            active_tool = "measure"
            print("측정 모드: 크기를 잴 사각형을 드래그하세요.")
            return

        if action == "find_ball":
            find_ball_from_current_roi()
            return

        if action == "homography":
            state = PlaybackState(paused=True, stopped=False, should_quit=False)
            active_tool = "homography"
            homography_selector.start()
            print("호모그래피 모드: 펼칠 영역의 4개 꼭짓점을 클릭하세요.")
            return

        if action == "bandpass":
            bandpass_enabled = not bandpass_enabled
            detected_ball = None
            skeleton_frame = None
            skeleton_keypoint_count = 0
            print(f"밴드패스 {'켜짐' if bandpass_enabled else '꺼짐'}")
            return

        if action == "skeleton":
            state = PlaybackState(paused=True, stopped=False, should_quit=False)
            update_skeleton_output()
            return

        if action == "clear":
            active_tool = "none"
            roi_selector.clear()
            measure_selector.clear()
            homography_selector.clear()
            detected_ball = None
            skeleton_frame = None
            skeleton_keypoint_count = 0
            try:
                cv2.destroyWindow(homography_window_name)
            except cv2.error:
                pass
            print("ROI, 공 표시, 호모그래피 점을 지웠습니다.")
            return

        if action == "quit":
            state = PlaybackState(
                paused=state.paused,
                stopped=state.stopped,
                should_quit=True,
            )

    def on_mouse(event: int, x: int, y: int, _flags: int, _param) -> None:
        nonlocal active_tool, detected_ball

        if current_frame is not None and event == cv2.EVENT_LBUTTONUP:
            frame_height, frame_width = current_frame.shape[:2]
            action = find_control_button_at(
                build_control_buttons(frame_width * 4, frame_height + 54),
                x=x,
                y=y,
            )
            if action is not None:
                handle_button_action(action)
                return

        if current_frame is None or not state.paused:
            return

        frame_height, frame_width = current_frame.shape[:2]
        image_point = map_panorama_image_point(
            x=x,
            y=y,
            frame_width=frame_width,
            frame_height=frame_height,
        )
        if image_point is None:
            return

        if active_tool == "homography":
            if event == cv2.EVENT_LBUTTONUP:
                if homography_selector.add_point(image_point.x, image_point.y):
                    print(
                        "호모그래피 점 "
                        f"{len(homography_selector.points)}/4: "
                        f"x={image_point.x}, y={image_point.y}"
                    )
                if homography_selector.is_complete:
                    active_tool = "none"
                    apply_current_homography()
            return

        if active_tool not in ("roi", "measure"):
            return

        selector = roi_selector if active_tool == "roi" else measure_selector
        if event == cv2.EVENT_LBUTTONDOWN:
            selector.begin(image_point.x, image_point.y)
            if active_tool == "roi":
                detected_ball = None
        elif event == cv2.EVENT_MOUSEMOVE:
            selector.update(image_point.x, image_point.y)
        elif event == cv2.EVENT_LBUTTONUP:
            selected_roi = selector.finish(frame_width=frame_width, frame_height=frame_height)
            if selected_roi is not None and active_tool == "roi":
                print(
                    "ROI 선택됨: "
                    f"x={selected_roi.x}, y={selected_roi.y}, "
                    f"width={selected_roi.width}, height={selected_roi.height}"
                )
                find_ball_from_current_roi()
            elif selected_roi is not None:
                print(f"측정됨: {format_roi_size_label(selected_roi)}")

    cv2.namedWindow(window_name)
    cv2.setMouseCallback(window_name, on_mouse)
    cv2.namedWindow(controls_window_name, cv2.WINDOW_AUTOSIZE)
    cv2.imshow(controls_window_name, np.zeros((1, 420, 3), dtype=np.uint8))
    cv2.createTrackbar("Low cutoff", controls_window_name, 5, 300, lambda _value: None)
    cv2.createTrackbar("High cutoff", controls_window_name, 80, 300, lambda _value: None)

    print(
        "조작: 화면 아래 버튼 사용. ROI는 ROI 버튼 후 드래그, "
        "4점 보정은 4-Point 버튼 후 4개 꼭짓점 클릭, 밴드패스는 Band 버튼과 Controls 트랙바 사용."
    )

    try:
        while not state.should_quit:
            if state.stopped:
                capture.set(cv2.CAP_PROP_POS_FRAMES, 0)
                success, current_frame = capture.read()
                if not success:
                    break
                state = PlaybackState(paused=True, stopped=False, should_quit=False)

            if not state.paused:
                success, frame = capture.read()
                if not success:
                    capture.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    state = PlaybackState(paused=True, stopped=False, should_quit=False)
                    continue
                current_frame = frame

            if current_frame is not None:
                analysis_frame = get_analysis_frame()
                processed_frame = (
                    analysis_frame.copy()
                    if analysis_frame is not None
                    else current_frame.copy()
                )
                otsu_frame = apply_otsu_threshold(processed_frame)
                contours = find_contours_in_binary(otsu_frame)
                otsu_frame = draw_contours_on_frame(otsu_frame, contours)
                skeleton_display = (
                    skeleton_frame.copy()
                    if skeleton_frame is not None
                    else processed_frame.copy()
                )
                original_frame = current_frame.copy()
                frame_height, frame_width = original_frame.shape[:2]
                active_roi = roi_selector.preview(
                    frame_width=frame_width,
                    frame_height=frame_height,
                )
                if active_roi is None:
                    active_roi = roi_selector.roi
                if active_roi is not None:
                    top_left = (active_roi.x, active_roi.y)
                    bottom_right = (
                        active_roi.x + active_roi.width,
                        active_roi.y + active_roi.height,
                    )
                    cv2.rectangle(original_frame, top_left, bottom_right, (0, 255, 0), 2)
                    cv2.rectangle(processed_frame, top_left, bottom_right, (0, 255, 0), 2)
                    cv2.putText(
                        original_frame,
                        "ROI",
                        (active_roi.x, max(20, active_roi.y - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        (0, 255, 0),
                        2,
                    )
                    cv2.putText(
                        processed_frame,
                        "ROI",
                        (active_roi.x, max(20, active_roi.y - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        (0, 255, 0),
                        2,
                    )
                active_measure = measure_selector.preview(
                    frame_width=frame_width,
                    frame_height=frame_height,
                )
                if active_measure is None:
                    active_measure = measure_selector.roi
                if active_measure is not None:
                    top_left = (active_measure.x, active_measure.y)
                    bottom_right = (
                        active_measure.x + active_measure.width,
                        active_measure.y + active_measure.height,
                    )
                    label = format_roi_size_label(active_measure)
                    for target_frame in (original_frame, processed_frame, otsu_frame, skeleton_display):
                        cv2.rectangle(target_frame, top_left, bottom_right, (255, 180, 0), 2)
                        cv2.putText(
                            target_frame,
                            label,
                            (active_measure.x, max(22, active_measure.y - 8)),
                            cv2.FONT_HERSHEY_SIMPLEX,
                            0.7,
                            (255, 180, 0),
                            2,
                        )
                for index, point in enumerate(homography_selector.points, start=1):
                    for target_frame in (original_frame, processed_frame, otsu_frame, skeleton_display):
                        cv2.circle(target_frame, (point.x, point.y), 6, (255, 0, 255), -1)
                        cv2.putText(
                            target_frame,
                            str(index),
                            (point.x + 8, point.y - 8),
                            cv2.FONT_HERSHEY_SIMPLEX,
                            0.65,
                            (255, 0, 255),
                            2,
                        )
                if len(homography_selector.points) >= 2:
                    for first, second in zip(
                        homography_selector.points,
                        homography_selector.points[1:],
                    ):
                        for target_frame in (original_frame, processed_frame, otsu_frame, skeleton_display):
                            cv2.line(
                                target_frame,
                                (first.x, first.y),
                                (second.x, second.y),
                                (255, 0, 255),
                                2,
                            )
                    if homography_selector.is_complete:
                        first = homography_selector.points[0]
                        last = homography_selector.points[-1]
                        for target_frame in (original_frame, processed_frame, otsu_frame, skeleton_display):
                            cv2.line(
                                target_frame,
                                (last.x, last.y),
                                (first.x, first.y),
                                (255, 0, 255),
                                2,
                            )
                if detected_ball is not None:
                    cv2.circle(
                        processed_frame,
                        (detected_ball.x, detected_ball.y),
                        detected_ball.radius,
                        (0, 0, 255),
                        3,
                    )
                    cv2.circle(
                        processed_frame,
                        (detected_ball.x, detected_ball.y),
                        3,
                        (255, 255, 255),
                        -1,
                    )
                    cv2.putText(
                        processed_frame,
                        "BALL",
                        (
                            detected_ball.x + detected_ball.radius + 8,
                            max(20, detected_ball.y),
                        ),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        (0, 0, 255),
                        2,
                    )
                if bandpass_enabled:
                    low, high = get_bandpass_cutoffs(
                        frame_width=frame_width,
                        frame_height=frame_height,
                    )
                    cv2.putText(
                        processed_frame,
                        f"Bandpass {low}-{high}",
                        (12, 28),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.75,
                        (0, 255, 255),
                        2,
                    )
                if skeleton_frame is not None:
                    cv2.putText(
                        skeleton_display,
                        f"Persons {skeleton_keypoint_count}",
                        (12, 58),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.75,
                        (0, 255, 255),
                        2,
                    )
                display_frame, _buttons = build_panorama_display(
                    original_frame,
                    processed_frame,
                    otsu_frame,
                    skeleton_display,
                    active_tool=active_tool,
                    is_paused=state.paused,
                    bandpass_enabled=bandpass_enabled,
                )
                cv2.imshow(window_name, display_frame)

            key = cv2.waitKey(20 if state.paused else delay_ms)
            normalized_key = key & 0xFF if key >= 0 else key
            if normalized_key == ord("c"):
                roi_selector.clear()
                detected_ball = None
                print("ROI를 지웠습니다.")
                continue
            if normalized_key == ord("f"):
                find_ball_from_current_roi()
                continue

            previous_state = state
            state = handle_key(key, state)
            if previous_state.paused and not state.paused:
                detected_ball = None
    finally:
        capture.release()
        cv2.destroyAllWindows()


def run_player(
    directory: Path,
    select_video: Callable[[Sequence[Path]], Path | None],
    play: Callable[[Path], None],
) -> int:
    videos = find_video_files(directory)
    if not videos:
        print("현재 폴더에서 영상 파일을 찾지 못했습니다.")
        return 1

    selected_video = select_video(videos)
    if selected_video is None:
        print("영상 선택이 취소되었습니다.")
        return 0

    play(selected_video)
    return 0


def main() -> int:
    return run_player(Path.cwd(), select_video=select_video_with_ui, play=play_video)


if __name__ == "__main__":
    raise SystemExit(main())
