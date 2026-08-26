import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from video_player import (
    Ball,
    ControlButton,
    HomographySelector,
    PlaybackState,
    Point,
    ROI,
    RoiSelector,
    apply_homography,
    apply_otsu_threshold,
    apply_spatial_bandpass,
    build_panorama_display,
    build_video_menu_items,
    build_control_buttons,
    calculate_homography_size,
    clamp_bandpass_cutoffs,
    detect_ball_in_roi,
    draw_contours_on_frame,
    draw_yolo_skeletons,
    find_video_files,
    find_control_button_at,
    find_contours_in_binary,
    find_local_pose_weight,
    find_video_menu_item_at,
    format_roi_size_label,
    handle_key,
    map_panorama_image_point,
    normalize_roi,
    order_homography_points,
    parse_video_selection,
    run_player,
)


class VideoPlayerTests(unittest.TestCase):
    def test_find_video_files_returns_supported_videos_sorted_by_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "b.mov").touch()
            (root / "notes.txt").touch()
            (root / "a.mp4").touch()
            (root / "clip.MKV").touch()

            videos = find_video_files(root)

        self.assertEqual([path.name for path in videos], ["a.mp4", "b.mov", "clip.MKV"])

    def test_parse_video_selection_returns_zero_based_index_for_valid_choice(self):
        videos = [Path("one.mp4"), Path("two.mp4")]

        selected = parse_video_selection("2", videos)

        self.assertEqual(selected, 1)

    def test_parse_video_selection_rejects_out_of_range_and_non_numeric_input(self):
        videos = [Path("one.mp4")]

        with self.assertRaises(ValueError):
            parse_video_selection("2", videos)

        with self.assertRaises(ValueError):
            parse_video_selection("abc", videos)

    def test_handle_key_toggles_pause_stops_and_quits(self):
        playing = PlaybackState(paused=False, stopped=False, should_quit=False)

        paused = handle_key(ord(" "), playing)
        stopped = handle_key(ord("s"), playing)
        quit_by_q = handle_key(ord("q"), playing)
        quit_by_escape = handle_key(27, playing)

        self.assertEqual(paused, PlaybackState(paused=True, stopped=False, should_quit=False))
        self.assertEqual(stopped, PlaybackState(paused=True, stopped=True, should_quit=False))
        self.assertEqual(quit_by_q, PlaybackState(paused=False, stopped=False, should_quit=True))
        self.assertEqual(quit_by_escape, PlaybackState(paused=False, stopped=False, should_quit=True))

    def test_run_player_uses_selector_result_for_playback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = root / "first.mp4"
            second = root / "second.mp4"
            first.touch()
            second.touch()
            played = []

            def select_video(videos):
                self.assertEqual(videos, [first, second])
                return second

            result = run_player(root, select_video=select_video, play=played.append)

        self.assertEqual(result, 0)
        self.assertEqual(played, [second])

    def test_run_player_does_not_play_when_selection_is_cancelled(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "first.mp4").touch()
            played = []

            result = run_player(root, select_video=lambda videos: None, play=played.append)

        self.assertEqual(result, 0)
        self.assertEqual(played, [])

    def test_normalize_roi_clamps_reversed_drag_to_frame_bounds(self):
        roi = normalize_roi((120, 80), (-10, 20), frame_width=100, frame_height=60)

        self.assertEqual(roi, ROI(x=0, y=20, width=100, height=40))

    def test_normalize_roi_ignores_tiny_regions(self):
        roi = normalize_roi((10, 10), (12, 12), frame_width=100, frame_height=60)

        self.assertIsNone(roi)

    def test_roi_selector_tracks_preview_and_final_roi(self):
        selector = RoiSelector(min_size=5)

        selector.begin(10, 20)
        selector.update(40, 50)
        preview = selector.preview(frame_width=100, frame_height=100)
        final = selector.finish(frame_width=100, frame_height=100)

        self.assertEqual(preview, ROI(x=10, y=20, width=30, height=30))
        self.assertEqual(final, ROI(x=10, y=20, width=30, height=30))
        self.assertEqual(selector.roi, final)
        self.assertFalse(selector.is_drawing)

    def test_roi_selector_clear_removes_roi(self):
        selector = RoiSelector(min_size=5)
        selector.begin(10, 20)
        selector.finish(frame_width=100, frame_height=100)

        selector.clear()

        self.assertIsNone(selector.roi)

    def test_find_video_menu_item_at_returns_clicked_video_index(self):
        items = build_video_menu_items([Path("a.mp4"), Path("b.mp4")], width=640)

        clicked = find_video_menu_item_at(items, x=100, y=items[1].top + 10)

        self.assertEqual(clicked, 1)

    def test_find_video_menu_item_at_ignores_clicks_outside_items(self):
        items = build_video_menu_items([Path("a.mp4")], width=640)

        clicked = find_video_menu_item_at(items, x=10, y=10)

        self.assertIsNone(clicked)

    def test_detect_ball_in_roi_returns_frame_coordinates(self):
        frame = np.zeros((180, 220, 3), dtype=np.uint8)
        cv2.circle(frame, (130, 90), 28, (230, 230, 230), -1)
        roi = ROI(x=80, y=40, width=100, height=100)

        ball = detect_ball_in_roi(frame, roi)

        self.assertIsNotNone(ball)
        assert ball is not None
        self.assertLessEqual(abs(ball.x - 130), 4)
        self.assertLessEqual(abs(ball.y - 90), 4)
        self.assertLessEqual(abs(ball.radius - 28), 5)

    def test_detect_ball_in_roi_returns_none_when_roi_has_no_circle(self):
        frame = np.zeros((120, 120, 3), dtype=np.uint8)
        roi = ROI(x=10, y=10, width=80, height=80)

        ball = detect_ball_in_roi(frame, roi)

        self.assertIsNone(ball)

    def test_order_homography_points_returns_clockwise_rectangle_order(self):
        points = [Point(100, 80), Point(10, 10), Point(110, 20), Point(20, 90)]

        ordered = order_homography_points(points)

        self.assertEqual(
            ordered,
            [Point(10, 10), Point(110, 20), Point(100, 80), Point(20, 90)],
        )

    def test_calculate_homography_size_uses_opposite_edge_lengths(self):
        ordered = [Point(10, 10), Point(110, 10), Point(130, 70), Point(20, 70)]

        width, height = calculate_homography_size(ordered)

        self.assertEqual(width, 110)
        self.assertEqual(height, 63)

    def test_homography_selector_collects_four_points_then_resets(self):
        selector = HomographySelector()

        self.assertTrue(selector.add_point(10, 10))
        self.assertTrue(selector.add_point(20, 10))
        self.assertTrue(selector.add_point(20, 20))
        self.assertTrue(selector.add_point(10, 20))
        self.assertFalse(selector.add_point(30, 30))
        self.assertTrue(selector.is_complete)

        selector.clear()

        self.assertEqual(selector.points, [])
        self.assertFalse(selector.is_complete)

    def test_apply_homography_warps_four_point_region_to_rectangle(self):
        frame = np.zeros((100, 120, 3), dtype=np.uint8)
        cv2.rectangle(frame, (20, 20), (80, 60), (255, 255, 255), -1)
        points = [Point(20, 20), Point(80, 20), Point(80, 60), Point(20, 60)]

        warped = apply_homography(frame, points)

        self.assertEqual(warped.shape[:2], (40, 60))
        self.assertGreater(int(warped.mean()), 240)

    def test_find_control_button_at_returns_clicked_action(self):
        buttons = [ControlButton("play", "Play", 10, 20, 70, 50)]

        action = find_control_button_at(buttons, x=30, y=30)

        self.assertEqual(action, "play")

    def test_find_control_button_at_ignores_clicks_outside_buttons(self):
        buttons = [ControlButton("play", "Play", 10, 20, 70, 50)]

        action = find_control_button_at(buttons, x=80, y=30)

        self.assertIsNone(action)

    def test_build_control_buttons_places_buttons_above_frame_bottom(self):
        buttons = build_control_buttons(frame_width=640, frame_height=480)

        self.assertEqual(buttons[0].action, "play_pause")
        self.assertTrue(all(button.top >= 420 for button in buttons))
        self.assertTrue(all(button.bottom <= 480 for button in buttons))

    def test_clamp_bandpass_cutoffs_keeps_low_below_high(self):
        low, high = clamp_bandpass_cutoffs(low=120, high=80, max_radius=200)

        self.assertEqual((low, high), (79, 80))

    def test_apply_spatial_bandpass_preserves_shape_and_uint8_type(self):
        frame = np.zeros((80, 100, 3), dtype=np.uint8)
        cv2.circle(frame, (50, 40), 12, (255, 255, 255), -1)

        filtered = apply_spatial_bandpass(frame, low_cutoff=1, high_cutoff=30)

        self.assertEqual(filtered.shape, frame.shape)
        self.assertEqual(filtered.dtype, np.uint8)

    def test_build_panorama_display_places_original_and_processed_side_by_side(self):
        original = np.zeros((40, 60, 3), dtype=np.uint8)
        processed = np.full((40, 60, 3), 255, dtype=np.uint8)
        otsu = np.full((40, 60, 3), 127, dtype=np.uint8)
        skeleton = np.full((40, 60, 3), 64, dtype=np.uint8)

        panorama, buttons = build_panorama_display(
            original,
            processed,
            otsu,
            skeleton,
            active_tool="none",
            is_paused=False,
            bandpass_enabled=False,
        )

        self.assertEqual(panorama.shape, (94, 240, 3))
        self.assertEqual(int(panorama[39, 50].mean()), 0)
        self.assertEqual(int(panorama[39, 110].mean()), 255)
        self.assertEqual(int(panorama[39, 170].mean()), 127)
        self.assertEqual(int(panorama[39, 230].mean()), 64)
        self.assertTrue(all(button.top >= 40 for button in buttons))

    def test_map_panorama_image_point_accepts_only_original_panel(self):
        self.assertEqual(
            map_panorama_image_point(x=30, y=20, frame_width=60, frame_height=40),
            Point(30, 20),
        )
        self.assertIsNone(
            map_panorama_image_point(x=80, y=20, frame_width=60, frame_height=40)
        )
        self.assertIsNone(
            map_panorama_image_point(x=30, y=60, frame_width=60, frame_height=40)
        )

    def test_apply_otsu_threshold_returns_binary_bgr_image(self):
        frame = np.zeros((60, 80, 3), dtype=np.uint8)
        frame[:, 40:] = 220

        otsu = apply_otsu_threshold(frame)

        self.assertEqual(otsu.shape, frame.shape)
        self.assertEqual(otsu.dtype, np.uint8)
        self.assertEqual(set(np.unique(otsu).tolist()), {0, 255})

    def test_find_contours_in_binary_filters_small_noise(self):
        binary = np.zeros((80, 100, 3), dtype=np.uint8)
        cv2.rectangle(binary, (10, 10), (50, 50), (255, 255, 255), -1)
        cv2.rectangle(binary, (90, 70), (91, 71), (255, 255, 255), -1)

        contours = find_contours_in_binary(binary, min_area=100)

        self.assertEqual(len(contours), 1)
        self.assertGreater(cv2.contourArea(contours[0]), 100)

    def test_draw_contours_on_frame_draws_green_outline(self):
        binary = np.zeros((80, 100, 3), dtype=np.uint8)
        cv2.rectangle(binary, (10, 10), (50, 50), (255, 255, 255), -1)
        contours = find_contours_in_binary(binary, min_area=100)

        drawn = draw_contours_on_frame(binary, contours)

        self.assertTrue(np.any(np.all(drawn == [0, 255, 0], axis=2)))

    def test_format_roi_size_label_shows_pixel_dimensions(self):
        label = format_roi_size_label(ROI(x=10, y=20, width=351, height=316))

        self.assertEqual(label, "351 x 316 px")

    def test_draw_yolo_skeletons_draws_keypoints_and_edges(self):
        frame = np.zeros((120, 160, 3), dtype=np.uint8)
        keypoints = np.zeros((1, 17, 3), dtype=float)
        keypoints[0, 5] = [40, 50, 0.9]
        keypoints[0, 6] = [90, 50, 0.9]

        drawn = draw_yolo_skeletons(frame, keypoints, confidence_threshold=0.5)

        self.assertTrue(np.any(np.all(drawn == [255, 0, 255], axis=2)))
        self.assertTrue(np.any(np.all(drawn == [0, 255, 255], axis=2)))

    def test_find_local_pose_weight_prefers_pose_named_weight(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "plain.pt").touch()
            pose = root / "yolov8n-pose.pt"
            pose.touch()

            found = find_local_pose_weight([root])

        self.assertEqual(found, pose)


if __name__ == "__main__":
    unittest.main()
