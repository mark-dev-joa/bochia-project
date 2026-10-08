# 라벨러 엔진 C++ 통합 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** [DESIGN.md](DESIGN.md)대로 Python 엔진을 C++ 라이브러리 `labeler_engine`으로 옮기고 `boccia_labeler.exe`가 Python 없이 돌게 한다. 파일 형식과 화면이 받는 JSON 모양은 그대로다.

**Architecture:** Python 함수를 하나씩 같은 이름의 C++ 함수로 옮긴다. 각 Task는 먼저 해당 Python 테스트를 C++로 옮겨 실패를 보고, 구현해서 통과시키고, 실데이터 대조가 있으면 돌린 뒤 커밋한다. 화면은 마지막 Task에서 `Client`를 `Engine`으로 바꾸고, 그때까지 지금 앱은 그대로 돈다.

**Tech Stack:** C++20(MSVC), OpenCV 4.11, zstd 1.5.7(FetchContent), nlohmann/json, ONNX Runtime 1.30.0 C++ API, Windows CNG(SHA-256), CMake. 검증에 Python 3.12(기존 테스트와 대조 스크립트).

---

## 반드시 지킬 것

- **사용자의 작업 프로젝트를 건드리지 않는다.** `bin/release/labeler/data/labels/project.json`과 포트 8767의 엔진은 읽지도 않는다. 실데이터 대조는 `output/native-labeler/v2-qa-project.json`(QA 사본)과 그 `-selection.json`만 쓴다.
- **원본 라벨 해시를 지킨다.** `output/ball-labeler/project.json`의 SHA-256은 `12485efb0f227acc78c108753f1f2a510b5ae8efdda4d77a170ba64a1f56bc83`이어야 한다.
- **빌드는 `build-labeler`에서.** 새 타깃을 넣은 뒤 한 번 `cmake -S . -B build-labeler`로 다시 구성한다.
- **Python 엔진과 테스트는 지우지 않는다.** 웹 라벨러가 쓴다. 이 계획이 끝나면 네이티브 앱만 Python을 안 쓴다.
- **커밋에 `bin/`, 실제 촬영, Codex의 다른 변경(48개 항목)을 넣지 않는다.** 새 파일은 이름을 지정해 `git add`한다.
- 한국어 문자열이 있는 파일은 Write 도구로 쓴다(heredoc은 cp949로 깨진다).

## 공통 명령

```powershell
# 다시 구성(새 타깃을 넣은 뒤 한 번)
cmake -S . -B build-labeler
# 빌드
cmake --build build-labeler --config Release --target labeler_engine_tests -j 8
# 엔진 테스트. 모든 줄이 "PASS: …"로 끝나야 한다
build-labeler\Release\labeler_engine_tests.exe
# Python 테스트(대조용, 바뀌지 않아야 한다): 64개 OK
python -m unittest tests.test_ball_labeler tests.test_native_labeler tests.test_labeler_selection
# 원본 라벨 해시
(Get-FileHash output/ball-labeler/project.json).Hash
```

테스트 파일은 `tests/labeler_engine_tests.cpp` 하나다. `tests/native_labeler_tests.cpp`처럼 `check(조건, 메시지)`를 쓰고, 검사 묶음마다 함수를 두며, 묶음이 끝날 때 `PASS: <묶음 이름>`을 출력한다. 픽스처 `struct Fixture`는 Python `LabelerTests.setUp`(tests/test_ball_labeler.py:23-50)을 그대로 옮긴다: 임시 폴더에 `capture-a/b/c`, 각 `camera-0/1`, 8×6 Mono8 픽셀 0..47, 델타(stride 1) 뒤 zstd 압축, 프레임 3장, `host_ns` = 1000 + 100·index (+5 for camera-1).

---

## Task 0: 빌드 뼈대와 유틸리티

**Files:** Create `src/labeler_util.hpp`, `src/labeler_util.cpp`, `tests/labeler_engine_tests.cpp`. Modify `CMakeLists.txt`.

1. CMake에 추가: `add_library(labeler_engine STATIC src/labeler_util.cpp)`(이후 Task마다 파일 추가), include `src`, OpenCV, nlohmann; link `${OpenCV_LIBS} libzstd_static bcrypt`; `/utf-8 /W4`. `add_executable(labeler_engine_tests tests/labeler_engine_tests.cpp)` + `add_test(NAME labeler_engine COMMAND labeler_engine_tests)` + opencv DLL 복사. ONNX Runtime 임포트 타깃은 Task 6에서.
2. `labeler_util.hpp`:
   ```cpp
   std::string sha256Hex(std::string_view bytes);                 // CNG BCryptHash, SHA-256
   class Sha256 { public: void update(std::string_view); std::string hex(); };   // 서명처럼 여러 번 더할 때
   std::string randomHex(int bytes = 16);                        // uuid4().hex 대용
   std::string utcIso();                                         // 2026-10-08T05:11:22.123456+00:00
   std::string localStamp();                                     // 20261008-141122
   void atomicJson(const std::filesystem::path&, const Json&, bool indent);   // indent: frames < 128
   bool pathLess(const std::filesystem::path&, const std::filesystem::path&); // Python Path 정렬: 소문자 조각 비교
   ```
3. 테스트: `sha256Hex("abc") == "ba7816bf…"`, `Sha256` 두 번 update가 이어붙인 것과 같음, `atomicJson` 뒤 임시 파일 없음·끝에 `\n`, `pathLess("a/b","a-b/c")`가 true(문자열 비교와 다름).
4. 빌드·테스트 통과 후 커밋: `Start the C++ labeler engine with hashing and atomic JSON`.

## Task 1: 촬영 카탈로그와 원본 해독

**Files:** Create `src/labeler_catalog.hpp/.cpp`. Port `ball_labeler.py:decode_raw`(57-99), `Catalog`(102-190).

```cpp
struct FrameInfo { int width=0,height=0,paddingX=0,codec=0,filterStride=0,status=0; uint32_t pixelFormat=0; uint64_t size=0,storedSize=0,offset=0,hostNs=0,id=0; bool hasHostNs=false,hasId=false; };
struct CameraStream { std::string id,name,serial,kind; std::vector<FrameInfo> frames; std::vector<int64_t> times; std::filesystem::path rawPath; };
struct Recording { std::string id,name,relativePath; bool complete=false; std::vector<CameraStream> cameras; /* Python dict 순서 = camera-N 정렬 */ };
class Catalog {
public:
    explicit Catalog(const std::filesystem::path& source);
    std::filesystem::path source; std::string signature;
    std::vector<Recording> recordings;                 // Python dict 순서(정렬된 session.json 순)
    const Recording& recording(const std::string&) const; const CameraStream& camera(const std::string& rec, const std::string& cam) const;
    const FrameInfo& frameInfo(const std::string&, const std::string&, int) const;
    cv::Mat read(const std::string&, const std::string&, int) const;
    int nearest(const std::string&, const std::string&, int64_t hostNs) const;
    Json metadata() const;
};
cv::Mat decodeRaw(const std::filesystem::path& raw, const FrameInfo& info);
```

- 서명 계산 순서, `kind`("모노"/"컬러"), `times`(host_ns 없으면 index)는 Python과 같게. `session.json`은 BOM 허용(`utf-8-sig`).
- 해독: `codec` 0/1, `filter_stride` 0..8, `pixel_format` 0x01080001 Mono8→BGR 3채널, 0x02180014 RGB8→BGR, 0x02180015 BGR8, 0x01080009 BayerRG8→`cv::COLOR_BayerRGGB2BGR`, 그 외는 "지원하지 않는 픽셀 형식". 오류 문구 그대로.
- 테스트(Python 이름 그대로): `test_lossless_compressed_mono_and_nearest_camera`, `test_bayer_channel_order_padding_and_truncation`(tests/test_ball_labeler.py:65-90의 픽셀값을 그대로 옮김), 촬영 없음 오류.
- 실데이터 대조(스크래치 스크립트, 결과는 커밋 메시지에): `data/imports`로 `Catalog`를 만들어 서명이 QA 사본의 `source_signature`(`f5c3ffc05d21…`)와 같은지, 촬영 ID 9개가 같은지, 모노 3장·Bayer 3장의 해독 결과 sha256이 Python `decode_raw`와 같은지. 대조용 작은 실행 파일 `labeler_engine_tests.exe --signature <source> <frame…>` 모드를 둔다.
- 커밋: `Read recordings and decode raw frames in the C++ engine`.

## Task 2: 마스크·검증 도우미

**Files:** Create `src/labeler_assist.hpp/.cpp`(이 Task는 SAM 없이 도우미만). Port `labeler_assist.py:ball_color`(23-45), `mask_contours`(66-68), `overlap`(71-75), `bounded_number`, `polygon_proposal`(246-263); `ball_labeler.py:validate_boxes`(193-231).

- `decodeMask/encodeMask`는 `native_labeler_model.cpp`의 것을 쓴다. Python `decode_mask`의 추가 검사(빈 마스크, 길이 초과)를 `validateBoxes`에서 같은 문구로 던진다.
- `Json validateBoxes(const Json& boxes, int width, int height)`: bbox 4개 유한수, 범위, 0.5 이상, `occluded` bool, `detection_bbox`, `color` 다섯 값, `source` 허용 목록, `segmentation`이 있으면 `boundingRect == bbox`(허용 오차 .01). 좌표는 `round(x, 4)` 실수로 저장.
- 테스트: `test_manual_polygon_mask_and_rejected_coordinates`, `test_invalid_coordinates…`의 좌표 부분, `test_ball_color_from_the_colour_camera`(tests/test_native_labeler.py:133-147의 합성 이미지), overlap 값.
- 커밋: `Port the mask, colour and box validation helpers`.

## Task 3: 라벨 저장소와 대표 프레임 사이드카

**Files:** Create `src/labeler_store.hpp/.cpp`. Port `ball_labeler.py:Project`(234-402 중 export 제외), `selected_reviewed`(409-412), `labeler_selection.py:SelectionFile`(31-74).

```cpp
class Project {
public:
    Project(const Catalog&, std::filesystem::path project, std::filesystem::path exports);
    const Catalog& catalog; std::filesystem::path path, exportRoot;
    mutable std::recursive_mutex lock; Json state; Json job{{"status","idle"}}; SelectionFile selection;
    Json summary() const; int commitAutomatic(const std::vector<std::pair<std::string,Json>>&);
    Json mutate(int expectedRevision, const std::function<void(Json&)>&);   // Conflict
    Json frame(const std::string&, const std::string&, int) const;         // contours 포함
    Json saveFrame(const Json& payload); Json saveSplits(const Json& payload);
};
class SelectionFile { public: Json read(); Json view(const Json& frames); };   // mtime 캐시, 검증 문구 그대로
std::vector<std::pair<std::string, Json>> selectedReviewed(const Json& snapshot);
```

- 프로젝트 열기: `version`, `source_signature` 검사 문구 그대로. 새 프로젝트 초기 상태도 같게.
- `summary()`의 `review_status` 기본값 규칙(`reviewed`→reviewed, `human_touched`→editing, 아니면 draft)을 그대로.
- 테스트: `test_save_resume_and_conflict_do_not_overwrite`, `test_invalid_coordinates_and_missing_split_rejected`, `test_mask_persistence_export_and_bbox_agree`(내보내기 부분은 Task 5로), `test_background_writes_do_not_conflict_or_overwrite_human_edits`, `test_source_change_rejected…`, selection의 `test_sidecar_human_override_reload_and_invalid_reference`.
- 실데이터 대조: QA 사본을 열어 `summary()`의 `reviewed`, `export_frames`, `selection.selected`(3,866), `excluded`(1,303)가 Python `Project.summary()`와 같은지.
- 커밋: `Open, save and summarise label projects from C++`.

## Task 4: 장면 색인, 검수, 분할, 넓히기, 색상 채우기

**Files:** Create `src/labeler_native.hpp/.cpp`. Port `labeler_native.py` 전체(368줄).

- `sceneIndex(catalog)`, `targets(project)`, `fingerprint(project, state, groups)`, `splitValid`, `recordingStats`, `thumbnail`(cv::Mat 캐시), `exportHistory`, `colorable`, `colorFillStatus`, `startColorFill`(jthread), `growable`, `nativeCatalog`(api_version 5), `groupPlan`, `nativeAction(project, action, payload)`(review, split-preview, split-confirm, fill-colors, grow-masks; export는 Task 5).
- 지문 직렬화는 `snapshot.dump(-1, ' ', true)`(ASCII 이스케이프)로 하고 Python과 비교한다.
- 테스트: `test_scene_index_covers_each_frame_once_and_retains_unmatched`, `test_review_all_then_confirm_and_label_edit_invalidates_confirmation`, `test_data_version_covers_the_assignment_and_the_groups`, `test_batch_groups_survive_a_label_fix_and_a_restart`, `test_review_refuses_labels_the_screen_has_not_shown`, `test_colour_fill_runs_in_the_background_and_keeps_given_colours`, `test_grow_masks_widens_draft_masks_and_leaves_human_work`, `test_groups_never_cross_splits_and_insufficient_groups_rejected`, `test_atomic_scene_review_hold_and_mono_color_guard`, `test_thumbnail_is_a_small_cached_jpeg`(JPEG 대신 크기·캐시), `test_catalog_reports_attention_and_recording_statistics`.
- 실데이터 대조: QA 사본의 `nativeCatalog()`에서 `scenes`(id, members의 key/index/host_ns/delta_ms)와 `stats`를 Python `native_catalog`와 JSON으로 비교해 같음. 지문은 QA 사본에 임시 분할을 확정해 비교(사본만).
- 커밋: `Index scenes, review, split and fill colours in the C++ engine`.

## Task 5: 내보내기

**Files:** Create `src/labeler_export.hpp/.cpp`. Port `ball_labeler.py:export_dataset`(419-493), `Project.start_export/_export_job/_progress`(380-402), `labeler_native.py` export action, `export_history`.

- 폴더 이름 `localStamp() + "-" + randomHex(4)`, `INCOMPLETE.txt` 문구, COCO 두 벌(`instances_<split>.json`, `instances_detection_<split>.json`), YOLO 줄 형식 `%.8f`, crops 파일명 `<stem>_<annotation_id:06>.png`, `data.yaml`, `manifest.json` 키 그대로(`version` 1, `frame_selection`에서 `frames` 제외, `native_split`).
- 테스트: `test_detection_override_keeps_mask_and_both_coco_coordinate_contracts`, `test_confirmed_native_export_contains_all_reviewed_images_and_fixed_groups`, `test_export_group_splits_review_filter_and_coordinate_agreement`, `test_failed_export_is_marked_incomplete`, `test_export_history_lists_only_completed_exports`, selection의 `test_export_subset_masks_and_explicit_human_inclusion`, `test_project_export_uses_filter_and_preserves_existing_review`.
- 실데이터 대조: QA 사본(임시 분할 확정)으로 C++와 Python이 각각 내보낸 폴더의 `counts`, 파일 수, `instances_train.json`의 annotation 수·bbox 합이 같음. 내보낸 폴더는 스크래치에 두고 지운다.
- 커밋: `Export reviewed images from the C++ engine`.

## Task 6: EfficientSAM, 원 후보, 추적

**Files:** Modify `src/labeler_assist.hpp/.cpp`, `CMakeLists.txt`(ONNX Runtime). Port `labeler_assist.py:BallAssist`(90-243).

- CMake: `set(ONNXRUNTIME_ROOT "${DEPENDENCY_ROOT}/onnxruntime" CACHE PATH …)`, `add_library(onnxruntime SHARED IMPORTED)` with `IMPORTED_IMPLIB lib/onnxruntime.lib`, `IMPORTED_LOCATION lib/onnxruntime.dll`, include `include`. `labeler_engine`에 링크, 테스트와 앱 빌드 뒤 DLL 복사.
- `BallAssist(modelDir)`: `status()`(ready, model, device, license, model_files, runtime=true), `segment(image, key, points, radius, seedBox)`, `static circles(image, min, max, existing)`, `static propagate(previous, image, boxes, existing, search)`. 세션은 `Ort::Env` 하나(static), `intra 4 / inter 1`, 입력 이름 `batched_images`; 디코더 `image_embeddings`, `batched_point_coords`[1,1,N,2], `batched_point_labels`[1,1,N], `orig_im_size`[2] int64; 출력 `output_masks`[1,1,3,H,W], `iou_predictions`[1,1,3].
- 테스트: `test_circle_candidates_and_moving_box_matching`, `test_tracking_mask_translation_and_scale_matches_bbox`(labeler_auto의 `transfer_mask`는 Task 8에서; 여기서는 propagate), `test_prompt_crop_rgb_conversion_cache_and_negative_click`과 `test_segment_passes_the_dragged_box_as_seed`(모델 파일이 있을 때만; 없으면 `SKIP: …` 출력), `test_assist_proposals_do_not_mutate_or_cross_cameras`.
- 실데이터 대조: QA 사본 촬영 5번 프레임 0의 공 10개 중심을 클릭해 Python `BallAssist.segment`와 마스크 IoU ≥ 0.99, `quality` 차이 < 1e-3.
- 커밋: `Run EfficientSAM, circle candidates and tracking from C++`.

## Task 7: Engine 파사드와 화면 전환

**Files:** Create `src/labeler_engine.hpp/.cpp`. Modify `src/native_labeler_app.hpp/.cpp`, `src/native_labeler_review.cpp`, `src/native_labeler_pages.cpp`, `src/native_labeler_main.cpp`, `tests/native_labeler_tests.cpp`, `CMakeLists.txt`. Delete `src/native_labeler_client.hpp/.cpp`.

- `Engine`은 DESIGN의 인터페이스대로. `assist(method, payload)`는 `Project.assistance`(ball_labeler.py:329-363)를 옮긴다.
- 앱: `connect()`가 작업 스레드에서 `std::make_unique<Engine>`를 만들어 콜백에서 `engine_`에 넣는다. 호출 19곳 교체(목록: connect, refreshStage, fetchScene, save, growMasks, fillColors, segment, segmentBox, polygonProposal, review, tick 폴링, 썸네일, auto-start/stop, proposeSplit, confirmSplit, startExport). `--port`, `--no-start-server` 제거. 모델 폴더: `<exe>/models/efficientsam` 없으면 `<root>/data/models/efficientsam`.
- `native_labeler_tests`: `argc > 1`이면 `argv[1]`을 원본 폴더로 받아 인프로세스 `Engine`으로 같은 검사(HTTP 대신). 구성 `BOCCIA_LABELER_PYTHON`, `winhttp` 제거.
- 검증: `build-labeler\Release\boccia_labeler.exe --project output/native-labeler/v2-qa-project.json --export-root <스크래치>/exports --smoke-hidden --view overview --capture output/native-labeler/v3-overview-1920.png`, `--self-test`, `--stage-test`(QA 사본에 임시로 분할 확정). 캡처를 `v2-overview-1920.png`와 나란히 본다. 장면 읽기 시간을 메시지에 찍어 0.16초보다 빠른지 확인.
- 커밋: `Run the labeler without the Python engine`.

## Task 8: 대표 프레임 필터 만들기

**Files:** Create `src/labeler_selection.hpp/.cpp`. Port `labeler_selection.py:RULES`, `match_boxes`(77-98), `features`(101-108), `ball_patch`(111-116), `compare_frames`(119-156), `build_selection`(159-215).

- `Json buildSelection(const Catalog&, const Json& snapshot, progress, rules)`; 결과 JSON 키 그대로(`algorithm` "within-camera-geometry-mask-appearance-v2").
- `boccia_labeler --build-selection`(창 없이, 사이드카 쓰고 종료)로 `tools/filter_labeler_frames.py`와 같은 일을 한다.
- 테스트: `test_static_selection_representatives_and_camera_isolation`, `test_movement_occlusion_light_texture_and_unknown_ball_are_preserved`, `test_box_jitter_on_a_still_ball_is_a_duplicate_but_a_moved_ball_is_not`, `test_mask_shape_change_is_kept_even_with_identical_pixels`.
- 실데이터 대조: QA 사본으로 만든 사이드카와 기존 `v2-qa-project-selection.json`의 `selected` 수(3,866)와 프레임별 결정 비교. 다른 프레임이 있으면 수와 이유를 기록한다(OpenCV 4.11/5.0 차이 가능).
- 커밋: `Pick representative frames in C++`.

## Task 9: 자동 초안

**Files:** Create `src/labeler_auto.hpp/.cpp`. Port `labeler_auto.py` 전체(274줄).

- `SeedCache(catalog, root)`: `output/ball-detection-2026-10-02/full/detections.jsonl`, `output/mono-color-2026-10-03/final-sessions/*/matches.jsonl`(root 기준) 인증 규칙 그대로; 없으면 `circle_candidates`.
- `AutoLabelJob(project, root, modelDir)`: 상태 JSON 키 그대로, 상태 파일 `<프로젝트>-auto-status.json`, 작업자 수 `min(3, max(1, cpu/4))`, 32장 단위 `commitAutomatic`, 컬러 카메라 색상, `needs_attention` 규칙.
- 테스트: `test_complete_auto_job_resume_and_mask_tracking`, `test_tracking_mask_translation_and_scale_matches_bbox`(transfer_mask).
- 실데이터 대조(선택, 모델 있을 때): QA 사본을 복사해 촬영 1개의 `camera-1` 프레임 라벨을 지우고 자동 초안을 돌려 Python 결과와 공 수·마스크 IoU 중앙값을 비교.
- 커밋: `Draft labels automatically from C++`.

## Task 10: 배포, 문서, 정리

**Files:** Modify `tools/collect_release.ps1`, `NATIVE_LABELER.md`, `design/labeler-v2/DESIGN.md`("1차 구현에서 달라진 점" 표에 엔진 통합 한 줄), `BALL_LABELER.md`(네이티브 앱 설명 한 줄), `.gitignore` 필요 시, 메모리 `labeler-qa-setup.md`(포트·엔진 절차 삭제).

- collect: `onnxruntime.dll`, `models/efficientsam/*`(onnx 둘, LICENSE, provenance.json) 복사. `onnxruntime_providers_shared.dll` 없이 앱의 분할이 도는지 확인한 뒤 뺀다.
- NATIVE_LABELER.md: 엔진 절(수명, 로그, API 버전) 삭제, "빌드와 개발 실행"에서 Python 요구 삭제, 소스 표 교체, 테스트 수 갱신, 제한 사항 갱신.
- 전체 검증: Python 테스트 64개 OK(바뀌지 않음), `labeler_engine_tests`, `native_labeler_tests`, 앱 스모크 3종, 원본 라벨 해시, `git status`에 `bin/` 없음.
- 커밋: `Ship the labeler with its engine built in`.

## 완료 뒤 사용자에게 알릴 것

- 새 빌드는 포트 8767의 Python 엔진이 필요 없다. 아직 떠 있으면 내려도 된다.
- `bin/release/labeler`에 `onnxruntime.dll`과 `models/` 폴더가 생긴다.
- 처음 열 때 `project.json`은 키 순서만 바뀌어 다시 저장된다(내용 동일).
