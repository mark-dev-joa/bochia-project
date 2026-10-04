# 라벨러 v2 화면 교체 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Codex가 구현한 네이티브 라벨러의 화면과 조작을 [DESIGN.md](DESIGN.md)대로 바꾼다. 엔진의 저장·분할·내보내기 규칙은 유지한다.

**Architecture:** Dear ImGui 화면(`src/native_labeler_app.cpp`)이 localhost의 Python 엔진(`ball_labeler.py`, `labeler_native.py`)과 통신하는 지금 구조를 그대로 둔다. 배치·확대·상태 집계 같은 계산은 ImGui와 분리한 `native_labeler_view` 모듈로 빼서 테스트하고, 그리기 코드는 시안 캡처와 비교해 확인한다. 엔진에는 썸네일, 촬영별 통계, 박스 프롬프트, 내보내기 기록만 추가한다.

**Tech Stack:** C++20, Dear ImGui 1.92, GLFW, OpenGL 3.3, OpenCV 4.8, nlohmann/json, WinHTTP, Python 3.12 unittest, CMake와 Visual Studio

---

## 범위

이 계획은 **1차: 화면 교체**입니다. 데이터 준비 화면은 지금 엔진이 할 수 있는 것(촬영 표, 자동 초안 시작·중단)만 새 디자인으로 옮깁니다.

다음 항목은 **2차 계획**으로 따로 씁니다. 엔진 구조를 바꾸는 독립된 작업이기 때문입니다.

- 촬영 ID를 내용 기반으로 바꾸고 원본 서명을 촬영별로 저장
- 기존 프로젝트 변환, 원본 위치 다시 지정
- 촬영 폴더 추가 창, 새 촬영의 공 후보 찾기

## 반드시 지킬 것

- **사용자의 작업 프로젝트를 건드리지 않는다.** 개발과 캡처는 Task 0에서 만드는 복사본과 포트 8792만 쓴다. 포트 8766, 8767, 8768, 8770은 다른 서버가 쓰고 있다.
- **원본 라벨 해시를 지킨다.** `output/ball-labeler/project.json`의 SHA-256은 `12485efb0f227acc78c108753f1f2a510b5ae8efdda4d77a170ba64a1f56bc83`이어야 한다. 각 Task가 끝날 때 확인한다.
- **Codex와 같은 파일을 동시에 고치지 않는다.** 시작 전에 사용자에게 Codex가 `src/native_labeler_*`, `ball_labeler.py`, `labeler_native.py`를 수정 중이 아닌지 확인한다.
- **빌드는 `build-labeler` 폴더에서 한다.** Codex가 같은 저장소에서 `build`를 쓰고 있어서 겹치지 않게 한다. 처음 한 번 `cmake -S . -B build-labeler -DFETCHCONTENT_SOURCE_DIR_ZSTD=D:/works/bochia/build/_deps/zstd-src`로 구성한다.
- **커밋은 사용자가 승인한 뒤에만 한다.** 네이티브 라벨러 파일은 아직 git에 올라가 있지 않다. 승인 전에는 각 Task의 커밋 단계를 건너뛰고 바꾼 파일 목록만 남긴다.
- 그리기 Task는 수치와 순서를 명세하고, 헷갈리기 쉬운 부분만 코드를 싣는다. 완료 판단은 캡처를 `shots/`의 시안과 나란히 놓고 한다. 완료를 말하기 전에 @superpowers:verification-before-completion 을 따른다.

## 공통 명령

모든 명령은 `D:\works\bochia`에서 PowerShell로 실행한다.

```powershell
# 빌드
cmake --build build-labeler --config Release --target boccia_labeler native_labeler_tests -j 4

# C++ 테스트. 기대 출력: "PASS: RLE, original-pixel edge corrections, ..." 로 끝남
.\build-labeler\Release\native_labeler_tests.exe

# Python 테스트. 기대 출력: 마지막 줄 "OK"
python -m unittest discover -s tests -p "test*labeler*.py"

# 원본 라벨 해시
(Get-FileHash output\ball-labeler\project.json -Algorithm SHA256).Hash.ToLower()

# 캡처. <view>는 Task 15 이후 prepare, overview, focus, split, export
.\build-labeler\Release\boccia_labeler.exe --smoke-hidden --no-start-server --port 8792 `
  --project D:\works\bochia\output\native-labeler\v2-qa-project.json `
  --export-root D:\works\bochia\output\native-labeler\v2-qa-exports `
  --view <view> --recording 5 --frame 22 --width 1920 --height 1080 `
  --capture D:\works\bochia\output\native-labeler\v2-<view>.png
```

---

### Task 0: 기준 확인과 테스트용 엔진

**Files:**
- Create: `output/native-labeler/v2-qa-project.json`, `output/native-labeler/v2-qa-project-selection.json` (복사본, git 추적 대상 아님)

**Step 1: 기준 빌드와 테스트가 통과하는지 확인**

공통 명령의 빌드, C++ 테스트, Python 테스트를 실행한다.
Expected: 빌드 성공, C++ 테스트 PASS, Python 테스트 29개 OK.

**Step 2: 원본 라벨 해시 기록**

공통 명령의 해시 명령을 실행한다.
Expected: `12485efb0f227acc78c108753f1f2a510b5ae8efdda4d77a170ba64a1f56bc83`

**Step 3: 테스트용 프로젝트 복사**

```powershell
Copy-Item output\native-labeler\project.json output\native-labeler\v2-qa-project.json
Copy-Item output\native-labeler\project-selection.json output\native-labeler\v2-qa-project-selection.json
```

**Step 4: 테스트용 엔진을 8792에서 실행**

별도 터미널에서 실행하고 켜 둔다.

```powershell
python -u ball_labeler.py --no-browser --port 8792 --source data\imports `
  --project output\native-labeler\v2-qa-project.json --export-root output\native-labeler\v2-qa-exports
```

Expected: `Boccia labeler: http://127.0.0.1:8792/` 출력.

**Step 5: 지금 화면을 기준 캡처로 남김**

공통 명령의 캡처를 `--view overview`, `--capture ...\v2-before-overview.png`로 실행한다.
Expected: `PASS: native UI loaded real scene; view rendered without GL errors`

**Step 6: 기준 커밋 (사용자 승인 뒤에만)**

```powershell
git add src/native_labeler_*.cpp src/native_labeler_*.hpp tests/native_labeler_tests.cpp tests/test_native_labeler.py `
  labeler_native.py ball_labeler.py labeler_assist.py labeler_auto.py labeler_selection.py NATIVE_LABELER.md CMakeLists.txt design/labeler-v2
git commit -m "Add native labeler baseline and v2 design"
```

`CMakeLists.txt`와 `ball_labeler.py`에는 다른 작업의 변경이 섞여 있을 수 있다. 커밋 전에 `git diff --stat`으로 범위를 사용자에게 보여 준다.

---

### Task 1: 배치·확대·조각 영역 계산

**Files:**
- Create: `src/native_labeler_view.hpp`, `src/native_labeler_view.cpp`
- Modify: `CMakeLists.txt:136` (`native_labeler_model` 라이브러리에 소스 추가)
- Test: `tests/native_labeler_tests.cpp`

**Step 1: 실패하는 테스트 작성**

`tests/native_labeler_tests.cpp` 위쪽 include에 `#include "native_labeler_view.hpp"`를 추가하고, `check` 정의 아래에 넣는다.

```cpp
bool about(double a, double b, double tolerance = .01) { return std::abs(a - b) <= tolerance; }
void viewTests() {
    auto equal = tileSpans(-1, 1252);
    check(about(equal[0].width, 410.667) && about(equal[1].x, 420.667) && about(equal[2].x + equal[2].width, 1252), "equal tiles wrong");
    auto first = tileSpans(0, 1252), middle = tileSpans(1, 1252), last = tileSpans(2, 1252);
    check(about(first[0].width, 864) && about(first[1].x, 874) && about(first[1].width, 184) && about(first[2].x, 1068), "focused first tile wrong");
    check(about(middle[0].width, 184) && about(middle[1].x, 194) && about(middle[1].width, 864) && about(middle[2].x, 1068), "focus must keep camera order");
    check(about(last[2].x, 388) && about(last[2].width, 864), "focused last tile wrong");
    auto narrow = tileSpans(0, 700);
    check(about(narrow[1].width, 170) && about(narrow[0].width, 340), "narrow window must shrink side tiles");
    auto fit = fitView(1440, 1080, 864, 858);
    check(about(fit.scale, .6) && about(fit.x, 0) && about(fit.y, 105), "fit view wrong");
    cv::Rect2d ball(1174, 296, 38, 34);
    auto zoom = ballView(ball, 1440, 1080, 864, 858);
    check(about(zoom.scale, 858 * .34 / 38) && about(1193 * zoom.scale + zoom.x, 432, .05) && about(313 * zoom.scale + zoom.y, 429, .05), "selected ball must be centered at a third of the height");
    check(about(ballView({100, 100, 900, 900}, 1440, 1080, 864, 858).scale, .6), "large ball must not zoom below fit");
    check(about(ballView({100, 100, 4, 4}, 1440, 1080, 864, 858).scale, 14), "tiny ball must cap at 14x");
    auto crop = cropRegion({816, 465, 28, 28}, 1440, 1080);
    check(about(crop.width, 58.8) && about(crop.x, 800.6) && about(crop.y, 449.6), "crop is not centered");
    auto corner = cropRegion({2, 3, 20, 20}, 1440, 1080);
    check(about(corner.x, 0) && about(corner.y, 0) && about(corner.width, 42), "crop must shift inside the image");
}
```

`main`의 `try{` 바로 뒤에 `viewTests();`를 추가한다. 파일 위쪽에 `#include <cmath>`도 추가한다.

**Step 2: 실패 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4`
Expected: FAIL. `native_labeler_view.hpp`를 찾을 수 없다는 컴파일 오류.

**Step 3: 최소 구현**

`src/native_labeler_view.hpp`:

```cpp
#pragma once
#include "native_labeler_model.hpp"
#include <array>
#include <map>
#include <vector>

namespace boccia::labeler {
struct Span { float x = 0, width = 0; };
struct View { float scale = 1, x = 0, y = 0; };
// focus < 0이면 같은 폭 세 칸. 아니면 초점 타일이 남는 폭을 모두 갖는다. C, A, B 순서는 유지한다.
std::array<Span, 3> tileSpans(int focus, float width, float gap = 10, float narrow = 184);
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 선택한 공이 보기 높이의 약 34%를 차지한다. 화면 맞춤보다 작아지지 않고 14배를 넘지 않는다.
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 공 확대 조각의 정사각형 영역. 이미지 가장자리에서는 줄이지 않고 안쪽으로 민다.
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor = 2.1);
}
```

`src/native_labeler_view.cpp`:

```cpp
#include "native_labeler_view.hpp"
#include <algorithm>

namespace boccia::labeler {
std::array<Span, 3> tileSpans(int focus, float width, float gap, float narrow) {
    std::array<Span, 3> spans;
    if (focus < 0 || focus > 2) {
        const float each = (width - 2 * gap) / 3;
        for (int i = 0; i < 3; ++i) spans[i] = {i * (each + gap), each};
        return spans;
    }
    narrow = std::min(narrow, (width - 2 * gap) / 4);
    const float wide = width - 2 * gap - 2 * narrow;
    float x = 0;
    for (int i = 0; i < 3; ++i) { spans[i] = {x, i == focus ? wide : narrow}; x += spans[i].width + gap; }
    return spans;
}
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float scale = std::min(viewWidth / imageWidth, viewHeight / imageHeight);
    return {scale, (viewWidth - imageWidth * scale) / 2, (viewHeight - imageHeight * scale) / 2};
}
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float fit = fitView(imageWidth, imageHeight, viewWidth, viewHeight).scale;
    const float scale = std::clamp(float(viewHeight * .34 / std::max(box.width, box.height)), fit, 14.f);
    return {scale, float(viewWidth / 2 - (box.x + box.width / 2) * scale), float(viewHeight / 2 - (box.y + box.height / 2) * scale)};
}
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor) {
    const double side = std::min({std::max(box.width, box.height) * factor, double(imageWidth), double(imageHeight)});
    const double x = std::clamp(box.x + box.width / 2 - side / 2, 0., imageWidth - side);
    const double y = std::clamp(box.y + box.height / 2 - side / 2, 0., imageHeight - side);
    return {x, y, side, side};
}
}
```

`CMakeLists.txt`의 `add_library(native_labeler_model src/native_labeler_model.cpp)`를 다음으로 바꾼다.

```cmake
add_library(native_labeler_model src/native_labeler_model.cpp src/native_labeler_view.cpp)
```

**Step 4: 통과 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4; .\build-labeler\Release\native_labeler_tests.exe`
Expected: PASS

**Step 5: Commit**

```powershell
git add src/native_labeler_view.hpp src/native_labeler_view.cpp tests/native_labeler_tests.cpp CMakeLists.txt
git commit -m "Add labeler view math for tiles, zoom and crops"
```

---

### Task 2: 상태 집계와 공 순회 순서

**Files:**
- Modify: `src/native_labeler_view.hpp`, `src/native_labeler_view.cpp`
- Test: `tests/native_labeler_tests.cpp`

**Step 1: 실패하는 테스트 작성**

`viewTests()` 끝에 추가한다.

```cpp
    check(memberState(Json::parse(R"({"selected":false,"status":"reviewed"})")) == MemberState::Duplicate, "unselected frame is a duplicate");
    check(memberState(Json::parse(R"({"status":"reviewed"})")) == MemberState::Done && memberState(Json::parse(R"({"status":"hold"})")) == MemberState::Hold
        && memberState(Json::parse(R"({"status":"editing"})")) == MemberState::Editing && memberState(Json::object()) == MemberState::Todo, "member state mapping wrong");
    auto scene = Json::parse(R"({"members":[{"count":2},{"count":2},{"count":3,"selected":false}]})");
    check(!countsDiffer(scene), "duplicates must not trigger a count warning");
    scene["members"][1]["count"] = 1;
    check(countsDiffer(scene), "different ball counts must warn");
    std::vector<Json> annotations = {Json::parse(R"({"boxes":[{"bbox":[50,0,5,5]},{"bbox":[10,0,5,5]}]})"), Json::parse(R"({"boxes":[{"bbox":[5,0,5,5]}]})")};
    auto order = ballOrder(annotations);
    check(order.size() == 3 && order[0].camera == 0 && order[0].index == 1 && order[1].index == 0 && order[2].camera == 1, "ball order must follow camera then x");
    auto begin = nextBall(order, {}, 1), wrapped = nextBall(order, order[2], 1), back = nextBall(order, order[0], -1);
    check(begin.camera == 0 && begin.index == 1 && wrapped.camera == 0 && wrapped.index == 1 && back.camera == 1, "ball cycling wrong");
    check(nextBall({}, {}, 1).camera == -1, "empty scene has no ball to select");
```

**Step 2: 실패 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4`
Expected: FAIL. `memberState` 등이 선언되지 않았다는 컴파일 오류.

**Step 3: 최소 구현**

`native_labeler_view.hpp`의 `cropRegion` 선언 아래에 추가한다.

```cpp
enum class MemberState { Todo, Editing, Done, Hold, Duplicate };
struct BallRef { int camera = -1, index = -1; };
MemberState memberState(const Json& member);
// 검수 대상인 카메라끼리 공 개수가 다르면 true.
bool countsDiffer(const Json& scene);
// 카메라 순서, 그 안에서는 박스의 왼쪽 좌표 순서.
std::vector<BallRef> ballOrder(const std::vector<Json>& annotations);
BallRef nextBall(const std::vector<BallRef>& order, BallRef current, int step);
```

`native_labeler_view.cpp`에 추가한다. 파일 위쪽에 `#include <numeric>`을 넣는다.

```cpp
MemberState memberState(const Json& member) {
    if (!member.value("selected", true)) return MemberState::Duplicate;
    const auto status = member.value("status", "draft");
    return status == "reviewed" ? MemberState::Done : status == "hold" ? MemberState::Hold : status == "editing" ? MemberState::Editing : MemberState::Todo;
}
bool countsDiffer(const Json& scene) {
    int first = -1;
    for (const auto& member : scene.at("members")) {
        if (!member.value("selected", true)) continue;
        const int count = member.value("count", 0);
        if (first < 0) first = count; else if (count != first) return true;
    }
    return false;
}
std::vector<BallRef> ballOrder(const std::vector<Json>& annotations) {
    std::vector<BallRef> order;
    for (int camera = 0; camera < int(annotations.size()); ++camera) {
        const auto& boxes = annotations[camera].at("boxes");
        std::vector<int> indexes(boxes.size());
        std::iota(indexes.begin(), indexes.end(), 0);
        std::stable_sort(indexes.begin(), indexes.end(), [&](int a, int b) { return detectionBox(boxes[a]).x < detectionBox(boxes[b]).x; });
        for (int index : indexes) order.push_back({camera, index});
    }
    return order;
}
BallRef nextBall(const std::vector<BallRef>& order, BallRef current, int step) {
    if (order.empty()) return {};
    const auto found = std::find_if(order.begin(), order.end(), [&](BallRef ball) { return ball.camera == current.camera && ball.index == current.index; });
    const int size = int(order.size()), at = found == order.end() ? (step > 0 ? -1 : 0) : int(found - order.begin());
    return order[((at + step) % size + size) % size];
}
```

**Step 4: 통과 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4; .\build-labeler\Release\native_labeler_tests.exe`
Expected: PASS

**Step 5: Commit**

```powershell
git add src/native_labeler_view.hpp src/native_labeler_view.cpp tests/native_labeler_tests.cpp
git commit -m "Add member state, count warning and ball cycling order"
```

---

### Task 3: 엔진에 썸네일, 촬영별 통계, 박스 프롬프트, 내보내기 기록 추가

**Files:**
- Modify: `labeler_native.py` (import, `native_catalog`, 새 함수 3개)
- Modify: `ball_labeler.py:549-551` (GET 경로 2개), `ball_labeler.py:322` (`assist.segment` 호출)
- Test: `tests/test_native_labeler.py`

**Step 1: 실패하는 테스트 작성**

`tests/test_native_labeler.py`의 import에 `import cv2`를 추가하고 `NativeWorkflowTests` 안에 넣는다.

```python
    def test_thumbnail_is_a_small_cached_jpeg(self):
        from labeler_native import thumbnail
        first = thumbnail(self.project, self.ids[0], "camera-0", 0, 32)
        image = cv2.imdecode(np.frombuffer(first, np.uint8), cv2.IMREAD_COLOR)
        self.assertEqual(image.shape[:2], (24, 32))
        self.assertIs(thumbnail(self.project, self.ids[0], "camera-0", 0, 32), first)
        with self.assertRaises(ValueError):
            thumbnail(self.project, self.ids[0], "camera-0", 0, 4000)

    def test_catalog_reports_attention_and_recording_statistics(self):
        self.save(boxes=[{"bbox": [2, 1, 3, 4]}, {"bbox": [0, 0, 2, 2]}])
        native = native_catalog(self.project)
        stats = native["stats"][self.ids[0]]
        self.assertEqual((stats["number"], stats["images"], stats["balls"], stats["empty"], stats["small"], stats["reviewed"]), (1, 6, 2, 5, 2, 1))
        self.assertIsNone(stats["day"])
        self.assertEqual(stats["frames"], [3, 3])
        self.assertTrue(all("attention" in member for scene in native["scenes"] for member in scene["members"]))

    def test_segment_passes_the_dragged_box_as_seed(self):
        calls = []

        class Assist:
            def segment(self, image, key, points, radius=14, seed_bbox=None):
                calls.append(seed_bbox)
                return {"bbox": [1, 1, 2, 2]}

        payload = {"recording": self.ids[0], "camera": "camera-0", "index": 0,
                   "points": [{"x": 3, "y": 2, "label": 1}], "box": [1, 1, 4, 3]}
        self.project.assistance(Assist(), "segment", payload)
        self.project.assistance(Assist(), "segment", {**payload, "box": None})
        self.assertEqual(calls, [[1, 1, 4, 3], None])

    def test_export_history_lists_only_completed_exports(self):
        from labeler_native import export_history
        self.assertEqual(export_history(self.project), [])
        self.review_all()
        self.confirm()
        native_action(self.project, "export", {"crops": False})
        deadline = time.monotonic() + 5
        while self.project.job["status"] == "running" and time.monotonic() < deadline:
            time.sleep(.01)
        broken = self.project.export_root / "20990101-000000-broken"
        broken.mkdir(parents=True)
        (broken / "manifest.json").write_text("{}")
        (broken / "INCOMPLETE.txt").write_text("x")
        history = export_history(self.project)
        self.assertEqual(len(history), 1)
        self.assertEqual(history[0]["fingerprint"], self.project.state["native_split"]["fingerprint"])
        self.assertEqual(sum(item["images"] for item in history[0]["counts"].values()), 18)
```

기존 `test_native_http_catalog_and_review_use_same_project`의 `try:` 블록 끝에 썸네일 경로 확인을 추가한다.

```python
            with urlopen(base + f"/api/native/thumb?recording={self.ids[0]}&camera=camera-0&index=0&width=32") as response:
                self.assertEqual(response.headers["Content-Type"], "image/jpeg")
                self.assertGreater(len(response.read()), 100)
            with urlopen(base + "/api/native/exports") as response:
                self.assertEqual(json.load(response), {"exports": []})
```

**Step 2: 실패 확인**

Run: `python -m unittest discover -s tests -p "test_native_labeler.py"`
Expected: FAIL. `ImportError: cannot import name 'thumbnail'`, `KeyError: 'stats'` 등 5건.

**Step 3: 최소 구현**

`labeler_native.py`의 import를 바꾼다.

```python
import bisect
import copy
import hashlib
import json
from datetime import datetime

import cv2
```

`native_catalog`의 `member.update(...)`에 `attention`을 넣고 반환값에 `stats`를 넣는다.

```python
                member.update(selected=entry.get("selected", True), reviewed=entry.get("reviewed", False),
                              status=entry.get("review_status", "draft"), count=entry.get("count", 0),
                              attention=entry.get("needs_attention", False))
```

```python
                "split_confirmed": split_valid(project), "groups": project.state.get("native_split", {}).get("groups", {}),
                "stats": recording_stats(project)}
```

`native_catalog` 위에 함수 3개를 추가한다.

```python
def recording_stats(project):
    """Per-recording counts of the review targets for the data and split tables."""
    stats = {}
    for number, (recording_id, recording) in enumerate(project.catalog.recordings.items(), 1):
        stamp = recording["name"].rsplit("-", 1)[-1]
        started = datetime.fromtimestamp(int(stamp) / 1000) if stamp.isdigit() and len(stamp) == 13 else None
        times = next(iter(recording["cameras"].values()))["times"]
        stats[recording_id] = {"number": number, "name": recording["name"],
                               "day": started.strftime("%Y-%m-%d") if started else None,
                               "started": started.strftime("%m-%d %H:%M:%S") if started else None,
                               "seconds": round((times[-1] - times[0]) / 1e9, 1) if started else None,
                               "frames": [len(camera["frames"]) for camera in recording["cameras"].values()],
                               "images": 0, "balls": 0, "empty": 0, "small": 0, "reviewed": 0}
    for member in targets(project):
        item = stats[member["key"].split("/")[0]]
        frame = project.state["frames"].get(member["key"], {})
        boxes = frame.get("boxes", [])
        item["images"] += 1
        item["balls"] += len(boxes)
        item["empty"] += not boxes
        item["small"] += sum(max(box.get("detection_bbox", box["bbox"])[2:]) < 32 for box in boxes)
        item["reviewed"] += bool(frame.get("reviewed"))
    return stats


def thumbnail(project, recording, camera, index, width=128):
    """Small JPEG of one source frame for the scene list. Never written to disk."""
    width = int(width)
    if not 16 <= width <= 512:
        raise ValueError("썸네일 폭이 올바르지 않습니다.")
    cache = project.__dict__.setdefault("thumbnails", {})
    key = (recording, camera, int(index), width)
    if key not in cache:
        image = project.catalog.read(recording, camera, int(index))
        height = max(1, round(image.shape[0] * width / image.shape[1]))
        ok, encoded = cv2.imencode(".jpg", cv2.resize(image, (width, height), interpolation=cv2.INTER_AREA),
                                   [cv2.IMWRITE_JPEG_QUALITY, 85])
        if not ok:
            raise ValueError("썸네일을 만들지 못했습니다.")
        cache[key] = encoded.tobytes()
    return cache[key]


def export_history(project):
    """Completed export folders, newest first."""
    entries = []
    if not project.export_root.is_dir():
        return entries
    for folder in sorted(project.export_root.iterdir(), reverse=True):
        manifest = folder / "manifest.json"
        if not manifest.is_file() or (folder / "INCOMPLETE.txt").exists():
            continue
        try:
            value = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        entries.append({"name": folder.name, "path": str(folder), "created_at": value.get("created_at"),
                        "counts": value.get("counts", {}),
                        "fingerprint": (value.get("native_split") or {}).get("fingerprint")})
    return entries
```

`ball_labeler.py`의 `do_GET`에서 `/api/native/catalog` 분기 바로 아래에 추가한다.

```python
            elif parsed.path == "/api/native/thumb":
                from labeler_native import thumbnail
                self.respond(200, thumbnail(project, query["recording"][0], query["camera"][0], int(query["index"][0]),
                                            int(query.get("width", [128])[0])), "image/jpeg")
            elif parsed.path == "/api/native/exports":
                from labeler_native import export_history
                self.json_response({"exports": export_history(project)})
```

`ball_labeler.py`의 `assistance`에서 `assist.segment` 호출에 박스를 넘긴다.

```python
                box = assist.segment(image, self.catalog.signature + "/" + frame["key"], payload["points"],
                                     payload.get("radius", 14), payload.get("box"))
```

**Step 4: 통과 확인**

Run: `python -m unittest discover -s tests -p "test*labeler*.py"`
Expected: 33개 OK (기존 29개와 새 4개).

**Step 5: 테스트용 엔진 다시 시작**

Task 0 Step 4의 엔진을 `Ctrl+C`로 내리고 같은 명령으로 다시 띄운다. 실행 중인 엔진은 코드 변경을 읽지 않는다.

Run: `curl.exe -s -o NUL -w "%{http_code} %{content_type}`n" "http://127.0.0.1:8792/api/native/thumb?recording=3746f2726fcc456a&camera=camera-0&index=22"`
Expected: `200 image/jpeg`

**Step 6: Commit**

```powershell
git add labeler_native.py ball_labeler.py tests/test_native_labeler.py
git commit -m "Add thumbnails, recording stats, box prompt and export history to the native engine"
```

---

### Task 4: 테마와 그리기 도우미

**Files:**
- Create: `src/native_labeler_theme.hpp`, `src/native_labeler_theme.cpp`
- Modify: `src/native_labeler_main.cpp:36-38` (글꼴과 스타일 설정을 테마 호출로 교체)
- Modify: `CMakeLists.txt:139` (`boccia_labeler` 소스에 추가)

**Step 1: 테마 작성**

`src/native_labeler_theme.hpp`:

```cpp
#pragma once
#include "native_labeler_view.hpp"
#include <imgui.h>

namespace boccia::labeler::theme {
constexpr ImU32 rgb(unsigned hex, unsigned alpha = 255) { return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, alpha); }
inline constexpr ImU32 bg0 = rgb(0x0d0f12), bg1 = rgb(0x15181c), bg2 = rgb(0x1c2025), bg3 = rgb(0x252a31), bg4 = rgb(0x2f353d),
    line = rgb(0x2a3038), line2 = rgb(0x3b434d), text = rgb(0xe8ebef), text2 = rgb(0xa5aeba), text3 = rgb(0x727c88),
    accent = rgb(0x5b9dff), accentSoft = rgb(0x5b9dff, 38), ok = rgb(0x3dd68c), okSoft = rgb(0x3dd68c, 36), okFill = rgb(0x1f9d63),
    hold = rgb(0xf5b544), holdSoft = rgb(0xf5b544, 36), warn = rgb(0xff8a4c), mask = rgb(0x2ee6d0), selected = rgb(0xffd84a),
    proposal = rgb(0xff5ad9), train = rgb(0x5b9dff), val = rgb(0xb48cff), test = rgb(0xff7eb6), ink = rgb(0x08111f);
inline constexpr float base = 14, sub = 13, caption = 12, title = 16, heading = 21, number = 23;
extern ImFont* mono;
void loadFonts();
void apply();
ImU32 stateColor(MemberState state);
const char* stateLabel(MemberState state);
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius = 5);
bool chip(const char* id, MemberState state);
void keycap(const char* key);
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors = nullptr, float height = 28);
bool toggle(const char* id, bool* value);
bool primaryButton(const char* label, ImVec2 size);
void check(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void warning(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
}
```

`src/native_labeler_theme.cpp`:

```cpp
#include "native_labeler_theme.hpp"
#include <filesystem>

namespace boccia::labeler::theme {
ImFont* mono = nullptr;
void loadFonts() {
    auto& io = ImGui::GetIO();
    const char* fonts = "C:/Windows/Fonts/";
    auto path = [&](const char* name) { return std::string(fonts) + name; };
    ImFontConfig merge; merge.MergeMode = true;
    if (std::filesystem::exists(path("segoeui.ttf"))) io.Fonts->AddFontFromFileTTF(path("segoeui.ttf").c_str());
    if (std::filesystem::exists(path("malgun.ttf"))) io.Fonts->AddFontFromFileTTF(path("malgun.ttf").c_str(), 0, io.Fonts->Fonts.empty() ? nullptr : &merge);
    const auto code = std::filesystem::exists(path("CascadiaMono.ttf")) ? path("CascadiaMono.ttf") : path("consola.ttf");
    if (std::filesystem::exists(code)) mono = io.Fonts->AddFontFromFileTTF(code.c_str());
    ImGui::GetStyle().FontSizeBase = base;
}
void apply() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {0, 0}; style.FramePadding = {9, 6}; style.ItemSpacing = {8, 8}; style.FrameRounding = 6; style.ChildRounding = 8;
    style.PopupRounding = 10; style.WindowBorderSize = 0; style.ChildBorderSize = 1; style.ScrollbarSize = 10; style.ScrollbarRounding = 5;
    auto set = [&](ImGuiCol slot, ImU32 color) { style.Colors[slot] = ImGui::ColorConvertU32ToFloat4(color); };
    set(ImGuiCol_WindowBg, bg1); set(ImGuiCol_ChildBg, bg1); set(ImGuiCol_PopupBg, bg1); set(ImGuiCol_Border, line); set(ImGuiCol_Text, text);
    set(ImGuiCol_TextDisabled, text3); set(ImGuiCol_Button, bg2); set(ImGuiCol_ButtonHovered, bg4); set(ImGuiCol_ButtonActive, bg3);
    set(ImGuiCol_FrameBg, bg2); set(ImGuiCol_FrameBgHovered, bg3); set(ImGuiCol_FrameBgActive, bg3); set(ImGuiCol_Header, accentSoft);
    set(ImGuiCol_HeaderHovered, bg2); set(ImGuiCol_HeaderActive, accentSoft); set(ImGuiCol_CheckMark, accent); set(ImGuiCol_SliderGrab, text2);
    set(ImGuiCol_SliderGrabActive, text); set(ImGuiCol_Separator, line); set(ImGuiCol_ScrollbarBg, rgb(0, 0)); set(ImGuiCol_ScrollbarGrab, bg4);
    set(ImGuiCol_TableHeaderBg, bg2); set(ImGuiCol_TableBorderLight, line); set(ImGuiCol_TableBorderStrong, line); set(ImGuiCol_ModalWindowDimBg, rgb(0x050608, 158));
    set(ImGuiCol_PlotHistogram, ok);
}
ImU32 stateColor(MemberState state) {
    return state == MemberState::Done ? ok : state == MemberState::Hold ? hold : state == MemberState::Editing ? accent : state == MemberState::Duplicate ? line2 : text3;
}
const char* stateLabel(MemberState state) {
    return state == MemberState::Done ? "확인됨" : state == MemberState::Hold ? "보류" : state == MemberState::Editing ? "수정 중" : state == MemberState::Duplicate ? "반복" : "미검수";
}
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius) {
    const ImU32 color = stateColor(state);
    if (state == MemberState::Duplicate) draw->AddLine({center.x - radius, center.y}, {center.x + radius, center.y}, color, 2);
    else if (state == MemberState::Done || state == MemberState::Hold) draw->AddCircleFilled(center, radius, color);
    else { if (state == MemberState::Editing) draw->AddCircleFilled(center, radius, accentSoft); draw->AddCircle(center, radius - .75f, color, 0, 1.5f); }
}
bool chip(const char* id, MemberState state) {
    const char* label = stateLabel(state);
    const bool filled = state == MemberState::Done || state == MemberState::Hold || state == MemberState::Editing;
    ImGui::PushFont(nullptr, caption);
    const ImVec2 size{ImGui::CalcTextSize(label).x + (state == MemberState::Done ? 30 : 18), 22}, pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    auto draw = ImGui::GetWindowDrawList();
    const ImU32 soft = state == MemberState::Done ? okSoft : state == MemberState::Hold ? holdSoft : accentSoft;
    if (filled) draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, soft, 11); else draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, line2, 11);
    float x = pos.x + 9;
    if (state == MemberState::Done) { check(draw, {x + 4, pos.y + 11}, 10, ok); x += 12; }
    draw->AddText({x, pos.y + 3}, filled ? stateColor(state) : text2, label);
    ImGui::PopFont();
    return clicked;
}
void keycap(const char* key) {
    ImGui::PushFont(mono, 11);
    const ImVec2 size{std::max(18.f, ImGui::CalcTextSize(key).x + 10), 18}, pos = ImGui::GetCursorScreenPos();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, bg3, 4); draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, line2, 4);
    draw->AddText({pos.x + (size.x - ImGui::CalcTextSize(key).x) / 2, pos.y + 2}, text2, key);
    ImGui::PopFont(); ImGui::Dummy(size);
}
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors, float height) {
    ImGui::PushID(id); ImGui::PushFont(nullptr, caption);
    bool changed = false; auto draw = ImGui::GetWindowDrawList(); const ImVec2 start = ImGui::GetCursorScreenPos(); float x = start.x;
    for (int i = 0; i < count; ++i) {
        const float width = ImGui::CalcTextSize(labels[i]).x + 30;
        ImGui::SetCursorScreenPos({x, start.y}); ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", {width, height}) && *value != i) { *value = i; changed = true; }
        const bool on = *value == i, hovered = ImGui::IsItemHovered(); ImGui::PopID();
        if (on) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, colors ? colors[i] : bg4, 6, i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        else if (hovered) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, bg3);
        if (i) draw->AddLine({x, start.y}, {x, start.y + height}, line2);
        draw->AddText({x + 15, start.y + (height - ImGui::GetFontSize()) / 2}, on ? (colors ? ink : text) : text2, labels[i]);
        x += width;
    }
    draw->AddRect(start, {x, start.y + height}, line2, 6);
    ImGui::SetCursorScreenPos(start); ImGui::Dummy({x - start.x, height});
    ImGui::PopFont(); ImGui::PopID();
    return changed;
}
bool toggle(const char* id, bool* value) {
    const ImVec2 pos = ImGui::GetCursorScreenPos(), size{30, 16};
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (clicked) *value = !*value;
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, *value ? accent : bg4, 8);
    draw->AddCircleFilled({pos.x + (*value ? 22.f : 8.f), pos.y + 8}, 6, *value ? rgb(0xffffff) : text2);
    return clicked;
}
bool primaryButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, okFill); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0x23ad6e)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, okFill);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xffffff));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}
void check(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float s = size / 2;
    const ImVec2 points[] = {{center.x - s, center.y}, {center.x - s * .25f, center.y + s * .7f}, {center.x + s, center.y - s * .7f}};
    draw->AddPolyline(points, 3, color, ImDrawFlags_None, 2);
}
void warning(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float s = size / 2;
    draw->AddTriangle({center.x, center.y - s}, {center.x + s, center.y + s * .85f}, {center.x - s, center.y + s * .85f}, color, 1.5f);
    draw->AddLine({center.x, center.y - s * .25f}, {center.x, center.y + s * .3f}, color, 1.5f);
    draw->AddCircleFilled({center.x, center.y + s * .58f}, 1, color);
}
}
```

**Step 2: main에서 테마 사용**

`src/native_labeler_main.cpp`에 `#include "native_labeler_theme.hpp"`를 추가한다. `io.IniFilename=nullptr;` 다음의 글꼴 줄(`if(std::filesystem::exists("C:/Windows/Fonts/malgun.ttf"))...`)과 `ImGui::StyleColorsDark();`부터 `style.Colors[ImGuiCol_Text]=...;`까지 두 줄을 지우고 다음으로 바꾼다.

```cpp
    theme::loadFonts();theme::apply();
```

`glClearColor(.04f,.06f,.09f,1)`를 `glClearColor(.051f,.059f,.071f,1)`로 바꾼다.

`CMakeLists.txt`의 `add_executable(boccia_labeler ...)`에 `src/native_labeler_theme.cpp`를 추가한다.

**Step 3: 빌드와 캡처**

Run: 공통 명령의 빌드, 그다음 `--view overview` 캡처를 `v2-task4.png`로.
Expected: 빌드 성공, `PASS`. 캡처에서 글꼴이 Segoe UI와 맑은 고딕으로 바뀌고 바탕이 `#15181c` 계열인지 Read 도구로 이미지를 열어 확인한다. 한글이 네모로 나오면 글꼴 병합이 실패한 것이다.

**Step 4: Commit**

```powershell
git add src/native_labeler_theme.hpp src/native_labeler_theme.cpp src/native_labeler_main.cpp CMakeLists.txt
git commit -m "Add labeler v2 theme, fonts and drawing helpers"
```

---

### Task 5: 위쪽 단계 막대와 상태 막대

**Files:**
- Modify: `src/native_labeler_app.cpp:357-381` (`draw()`)
- Modify: `src/native_labeler_app.hpp` (새 멤버 선언)

**명세**

| 영역 | 수치 | 내용 |
| --- | --- | --- |
| 위쪽 막대 | 높이 48, 아래 선 `line` | 왼쪽 14부터 앱 이름(굵게 15), 세로 선, 프로젝트 이름(`text2`) |
| 단계 표시 | 가운데 정렬, 단계 높이 30, 좌우 여백 12, 사이 선 18 | 번호 원 지름 18. 지난 단계는 `okSoft` 원과 체크, 현재 단계는 `accentSoft` 바탕과 `accent` 원 |
| 진행 | 오른쪽 | "확인한 이미지" + 고정폭 `done / target` + 120×4 막대(`ok`) + 저장 상태 |
| 상태 막대 | 높이 28, 위 선 `line` | 왼쪽에 `message_`(처리 중이면 `hold` 색과 `busyLabel_`), 오른쪽에 원본 경로 |

**Step 1: `draw()` 교체**

- 기존 `ImGui::TextColored(green,"Boccia Lab")`부터 페이지 `Selectable` 반복문과 `Separator`까지를 `topBar()` 호출로 바꾼다.
- 마지막 `ImGui::Separator();ImGui::TextColored(busy()?amber:muted, ...)`를 `statusBar()` 호출로 바꾼다.
- 본문 높이는 `ImGui::GetContentRegionAvail().y - 28`.
- 키 처리 블록(370-378행)은 Task 10에서 `keys()`로 옮긴다. 이 Task에서는 그대로 둔다.

단계 전환은 기존 규칙을 그대로 쓴다.

```cpp
void Application::openPage(Page page) {
    if (!guard()) return;
    if (!frames_.empty() && current().dirty) save([this, page] { page_ = page; }); else page_ = page;
}
```

숫자는 천 단위 구분을 넣는다. 파일 위쪽 익명 네임스페이스에 추가한다.

```cpp
std::string grouped(int value) { auto text = std::to_string(value); for (int at = int(text.size()) - 3; at > 0; at -= 3) text.insert(at, ","); return text; }
```

**Step 2: 빌드와 캡처**

Run: 빌드 후 `--view overview` 캡처를 `v2-task5.png`로.
Expected: `shots/review-overview.png`의 위쪽 막대와 같은 구성. 단계 4개가 가운데에 있고 "검수"가 강조된다.

**Step 3: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp
git commit -m "Replace labeler page tabs with step bar and status bar"
```

---

### Task 6: 장면 목록

**Files:**
- Modify: `src/native_labeler_app.cpp:195-212` (`queue()`), `:74-96` (`updateCatalog`, `updateSummary`)
- Modify: `src/native_labeler_app.hpp`, `src/native_labeler_client.hpp`, `src/native_labeler_client.cpp`

**명세**

| 요소 | 수치 |
| --- | --- |
| 목록 폭 | 300. 창 폭이 1500 미만이면 232 |
| 머리 | 제목 "장면 목록"과 오른쪽 "대표 장면 N개", 필터 4칸(전체, 미검수, 보류, 확인됨) 높이 24, "반복 장면 숨김" 체크 |
| 촬영 머리 행 | 높이 30, 바탕 `bg2`, "촬영 05"와 장면 수, 오른쪽에 `C A B` 글자 |
| 장면 행 | 높이 54. 현재 행은 `accentSoft` 바탕과 왼쪽 3픽셀 `accent` 막대 |
| 썸네일 | 60×45, 왼쪽 12 |
| 글자 | x 82. 첫 줄 "프레임 0022"와 초(`text3`, 11), 둘째 줄 "공 3 · 3 · 3"(`text2`, 12). 경고가 있으면 둘째 줄 앞에 `warn` 삼각형 |
| 상태 점 | 오른쪽 12에서 지름 10, 간격 4로 세 개. 그 카메라가 장면에 없으면 그리지 않음 |
| 바닥 | 높이 44. "이전 A", "다음 D", "다음 미검수 N" |

**Step 1: 행 데이터를 미리 계산**

`native_labeler_app.hpp`에 추가한다.

```cpp
    struct SceneRow { int scene = -1, recording = 0, index = 0, cameras = 0; float seconds = 0; bool header = false, warn = false;
        std::array<MemberState, 3> state{}; std::array<int, 3> count{}; std::string thumb; };
    std::vector<SceneRow> rows_;
    bool rowsDirty_ = true;
    void rebuildRows();
    int rowOf(int scene) const;
    void stepRow(int step);
```

`rebuildRows()`는 `catalog_["scenes"]`를 한 번 돌면서 필터와 "반복 장면 숨김"을 적용한다. 촬영 번호가 바뀌면 `header = true`인 행을 먼저 넣는다. 규칙은 다음과 같다.

- 숨김이 켜져 있고 검수 대상 카메라가 하나도 없으면 건너뛴다.
- 필터: 미검수는 `Todo` 또는 `Editing`이 하나라도 있는 장면, 보류는 `Hold`가 하나라도 있는 장면, 확인됨은 검수 대상이 모두 `Done`인 장면.
- `warn`은 `countsDiffer(scene)`이거나 `attention`이 true인 카메라가 있을 때.
- `seconds`는 같은 촬영 첫 장면의 `host_ns`와의 차이.
- `thumb`은 첫 번째 카메라의 `key`.

`updateCatalog`, `updateSummary`, 필터 변경, 숨김 변경에서 `rowsDirty_ = true`로 표시하고, `queue()` 시작에서 `if (rowsDirty_) rebuildRows();`를 호출한다. JSON을 매 프레임 1,488번 찾지 않기 위해서다.

**Step 2: 썸네일**

`Client`에 추가한다.

```cpp
    cv::Mat thumbnail(const std::string& key) const;   // key = "recording/camera/index"
```

```cpp
cv::Mat Client::thumbnail(const std::string& key) const {
    const auto first = key.find('/'), second = key.find('/', first + 1);
    auto bytes = request("GET", "/api/native/thumb?recording=" + key.substr(0, first) + "&camera=" + key.substr(first + 1, second - first - 1) + "&index=" + key.substr(second + 1), "");
    return cv::imdecode(bytes, cv::IMREAD_COLOR);
}
```

`Application`에 썸네일 캐시를 둔다. `task_`와 별도의 future를 써서 장면 이동을 막지 않는다.

```cpp
    std::unordered_map<std::string, GLuint> thumbs_;
    std::vector<std::string> thumbWanted_;
    std::future<std::vector<std::pair<std::string, cv::Mat>>> thumbTask_;
    GLuint thumb(const std::string& key);   // 없으면 0을 돌려주고 요청 목록에 넣는다
```

`tick()`에서 `thumbTask_`가 끝났으면 텍스처로 올리고, 비어 있고 `thumbWanted_`가 있으면 최대 12개를 묶어 새로 요청한다. 실패한 썸네일은 건너뛴다. 소멸자에서 `thumbTask_.wait()`와 텍스처 삭제를 한다.

**Step 3: `queue()` 교체**

행은 `ImGuiListClipper`로 보이는 것만 그린다. 머리 행과 장면 행의 높이가 다르므로 `clipper.Begin(int(rows_.size()), 54)` 대신 누적 y를 직접 계산한다.

```cpp
    float y = 0; const float top = ImGui::GetScrollY(), bottom = top + ImGui::GetWindowHeight();
    for (int i = 0; i < int(rows_.size()); ++i) {
        const float height = rows_[i].header ? 30.f : 54.f;
        if (y + height >= top && y <= bottom) { ImGui::SetCursorPosY(y); rows_[i].header ? headerRow(rows_[i]) : sceneRow(rows_[i], i); }
        y += height;
    }
    ImGui::SetCursorPosY(y); ImGui::Dummy({1, 1});
```

`scrollToCurrent_`가 켜져 있으면 현재 행의 y로 `ImGui::SetScrollY(y - 창 높이 / 2)`를 준다.

`stepRow(step)`은 `rows_`에서 현재 장면의 앞뒤로 `header`가 아닌 행을 찾아 `navigate(row.scene)`을 부른다. 바닥의 이전·다음 버튼과 Task 10의 `A`, `D`가 이것을 쓴다.

**Step 4: 빌드와 캡처**

Run: 빌드 후 `--view overview` 캡처를 `v2-task6.png`로.
Expected: `shots/review-overview.png`의 왼쪽 목록과 같은 구성. 프레임 22 행이 강조되고, 썸네일이 보이고(첫 캡처에서는 비어 있을 수 있으므로 `settled` 대기 횟수를 3에서 40으로 늘린다), 촬영 05의 프레임 14~19 행에 짧은 줄(반복)이 섞여 있다.

**Step 5: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp src/native_labeler_client.cpp src/native_labeler_client.hpp src/native_labeler_main.cpp
git commit -m "Redesign scene list with thumbnails, state dots and filters"
```

---

### Task 7: 석 장 보기

**Files:**
- Modify: `src/native_labeler_app.cpp:217-224` (`repaint`), `:231-269` (`imageCanvas`), `:270-287` (`overview`)
- Modify: `src/native_labeler_app.hpp` (`FrameView`에 윤곽 캐시)

**명세**

| 요소 | 수치 |
| --- | --- |
| 가운데 영역 | 여백 좌우 12, 위아래 10. 장면 줄 40, 타일 영역, 안내 줄 34. 사이 간격 8 |
| 장면 줄 | "촬영 05 › 프레임 0022"(굵게 16), 카메라 수와 공 수와 수신 시각 차(`text3`, 12). 오른쪽에 마스크 채움(M), 박스(X), 표시 밝기 슬라이더 |
| 타일 | `tileSpans(-1, 폭)`. 바탕 `bg1`, 테두리 `line`, 모서리 8 |
| 타일 머리 | 높이 34. 키 표시(1, 2, 3), 카메라 이름(굵게), "공 3개"(`text3`), 오른쪽에 상태 칩 |
| 전체 영상 | 폭 = 타일 폭, 높이 = 폭 × 0.75. `fitView`로 맞춤 |
| 공 번호 | 공 위에 지름 16 알약. 바탕 `bg0` 82%, 글자 `mask`(선택한 공은 `selected`) |
| 공 조각 | 여백 10, 두 칸, 간격 10. 정사각형과 높이 22 설명 줄("1", "28×28", 오른쪽에 출처 꼬리표 SAM, 추적, 수동) |
| 빈 칸 | 공이 홀수 개면 마지막 칸에 점선 테두리의 "빠진 공 추가" |
| 안내 줄 | 키 표시와 설명. 클릭, Tab, E, Q, A D |

**Step 1: 윤곽을 한 번만 계산**

지금은 편집 화면에서 매 프레임 `decodeMask`와 `findContours`를 한다. `FrameView`에 캐시를 두고 `repaint()`에서 채운다.

```cpp
    std::vector<std::vector<std::vector<cv::Point>>> contours;   // 공마다 윤곽 목록
```

`repaint()`에서 공을 돌 때 `cv::findContours(mask.clone(), found, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE)`의 결과를 `f.contours[i]`에 넣는다. 마스크가 없는 공은 빈 목록이다.

마스크 채움 색도 바꾼다. 오버레이는 RGBA 순서다.

```cpp
    // 모든 공은 청록 20%, 선택한 공은 노랑 5%. 제안은 채우지 않고 윤곽만 그린다.
    out[x] = active ? cv::Vec4b(255, 216, 74, 13) : cv::Vec4b(46, 230, 208, 51);
```

**Step 2: 그리기와 입력을 분리**

`imageCanvas`를 둘로 나눈다. 석 장 보기와 작은 타일, 조각은 입력이 없는 `paintImage`만 쓴다.

```cpp
    struct Paint { bool fill = true, boxes = false, numbers = false, contours = true; int selected = -1; };
    // 원본 영역 region을 화면 사각형 [a, b]에 그린다. 영상, 마스크 채움, 윤곽, 박스 순서.
    void paintImage(FrameView& frame, ImVec2 a, ImVec2 b, cv::Rect2d region, Paint paint);
```

핵심은 텍스처의 일부만 그리는 것이다. 조각마다 텍스처를 따로 만들지 않는다.

```cpp
void Application::paintImage(FrameView& f, ImVec2 a, ImVec2 b, cv::Rect2d region, Paint paint) {
    if (f.repaint) repaint(f);
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 uv0{float(region.x / f.image.cols), float(region.y / f.image.rows)}, uv1{float((region.x + region.width) / f.image.cols), float((region.y + region.height) / f.image.rows)};
    const float scale = (b.x - a.x) / float(region.width);
    auto at = [&](double x, double y) { return ImVec2{a.x + float(x - region.x) * scale, a.y + float(y - region.y) * scale}; };
    draw->PushClipRect(a, b, true);
    draw->AddImage(ImTextureRef(ImTextureID(f.texture)), a, b, uv0, uv1);
    if (paint.fill && masks_) draw->AddImage(ImTextureRef(ImTextureID(f.overlay)), a, b, uv0, uv1);
    const auto& boxes = f.annotation["boxes"];
    for (int i = 0; i < int(boxes.size()); ++i) {
        const bool active = i == paint.selected; const ImU32 color = active ? theme::selected : theme::mask;
        if (paint.contours && i < int(f.contours.size())) for (const auto& contour : f.contours[i]) {
            std::vector<ImVec2> points; points.reserve(contour.size());
            for (const auto& p : contour) points.push_back(at(p.x + .5, p.y + .5));
            if (points.size() > 2) draw->AddPolyline(points.data(), int(points.size()), color, ImDrawFlags_Closed, active ? 2.f : 1.5f);
        }
        const auto r = detectionBox(boxes[i]);
        if (paint.boxes || boxes[i].contains("detection_bbox")) {
            draw->AddRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), IM_COL32(0, 0, 0, 150), 0, 0, 3);
            draw->AddRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), IM_COL32(255, 255, 255, 255), 0, 0, 1);
        }
    }
    draw->PopClipRect();
}
```

조각은 `paintImage(frame, a, b, cropRegion(detectionBox(box), cols, rows), {.fill = false, .boxes = boxes_, .selected = ...})`로 그린다.

**Step 3: 타일과 조각 클릭**

- 전체 영상 클릭: 공 위면 `select(camera, hit)`, 아니면 `focus(camera)`. 둘 다 Task 8에서 만든다. 이 Task에서는 기존 `showEditor(camera)`를 부른다.
- 조각 클릭: `select(camera, ball)`.
- "빠진 공 추가" 클릭: 그 카메라를 열고 `tool_ = Tool::Ai`, 선택 해제.
- 상태 칩 클릭: `review(false, false, slot)`. 이미 확인됨이면 아무것도 하지 않는다.
- 전체 영상 위에 마우스를 올리면 돋보기를 그린다. 커서 주변 원본 45×45를 180×180으로, `ImGui::GetForegroundDrawList()`에 커서 오른쪽 아래 16 떨어진 곳에 그린다.

기존 카드의 "보류", "확인 완료" 버튼과 아래쪽 "장면 보류", "현재 장면 확인 완료", "다음 장면" 버튼은 지운다. 오른쪽 패널(Task 9)이 대신한다.

**Step 4: 빌드와 캡처**

Run: 빌드 후 `--view overview` 캡처를 `v2-task7.png`로.
Expected: `shots/review-overview.png`와 같은 구성. 카메라마다 조각 3개와 "빠진 공 추가" 칸이 있고, 조각에는 윤곽선만 있다.

**Step 5: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp
git commit -m "Redesign three-camera view with per-ball crops"
```

---

### Task 8: 제자리 확대

**Files:**
- Modify: `src/native_labeler_app.cpp:226-230` (`fit`), `:231-269` (`imageCanvas`), `:288-298` (`editor`), `:110-115` (`showEditor`, `switchCamera`)
- Modify: `src/native_labeler_app.hpp`

**명세**

| 요소 | 수치 |
| --- | --- |
| 배치 | `tileSpans(camera_, 폭)`. 초점 타일의 테두리는 `accent` |
| 작은 타일 | 머리(키 표시, 이름, 오른쪽에 상태 점), 영상 폭 × 0.75, 조각 두 칸(여백 6, 간격 6, 설명 줄 18). 클릭하면 그 카메라로 전환 |
| 초점 타일 머리 | 키 표시, 이름, "프레임 22 · 공 3개 · 수신 시각 +0.00ms", 상태 칩 |
| 도구 막대 | 보기 영역 왼쪽 위 (10, 10). 버튼 36×36, 간격 2, 바탕 `bg1` 92%, 테두리 `line2`. 선택, AI 분할, 브러시, 폴리곤, 박스, 구분선, 되돌리기, 구분선, 화면 맞춤, 선택한 공으로. 켜진 도구는 `accent` 바탕. 오른쪽 아래에 키 글자 |
| 도구 설명 줄 | (66, 10), 높이 36. 도구별 한 줄 설명 |
| 확대율 | 오른쪽 위. "748%"와 "원본 1픽셀 = 화면 7.5픽셀" |
| 미니맵 | 왼쪽 아래 (10, 10), 188×141. 보이는 영역을 `selected` 테두리로 표시 |
| 픽셀 격자 | 확대율 5배 이상일 때 원본 픽셀 경계마다 흰색 9% 선 |
| 선택한 공 | 윤곽 `selected` 2픽셀, 검출 박스 흰 실선과 검은 외곽선, 경계 가운데에 9×9 흰 손잡이 4개, 박스 위에 "39 × 35 px" 꼬리표. 보정값이 있으면 마스크 외곽을 `mask` 점선으로 |
| 미확정 제안 | 윤곽 `proposal` 2픽셀. 포함 점은 초록 원에 +, 제외 점은 빨강 원에 −. 박스 아래 22 지점에 "제안 미확정", "반영 Enter", "취소 Esc" |

**Step 1: 자동 확대**

`fit()`을 바꾼다. `scale == 0`은 "자동"을 뜻하고, 선택한 공이 있으면 그 공으로, 없으면 화면 맞춤이다.

```cpp
void Application::fit(FrameView& f, ImVec2 size, bool toBall) {
    const auto& boxes = f.annotation["boxes"];
    const View view = toBall && selected_ >= 0 && selected_ < int(boxes.size())
        ? ballView(detectionBox(boxes[selected_]), f.image.cols, f.image.rows, size.x, size.y)
        : fitView(f.image.cols, f.image.rows, size.x, size.y);
    f.scale = view.scale; f.offsetX = view.x; f.offsetY = view.y;
}
```

`loadScene`의 완료 처리에서 `selected_`를 0이 아니라 -1로 둔다. 장면을 열자마자 공 하나가 선택돼 있으면 석 장 보기에서 혼란스럽다.

**Step 2: `select`와 `focus`**

```cpp
void Application::focus(int camera) {            // 그 카메라를 크게, 선택 없이 화면 맞춤
    if (frames_.empty() || camera >= int(frames_.size()) || !guard()) return;
    if (current().dirty) { save([this, camera] { focus(camera); }); return; }
    camera_ = camera; editor_ = true; selected_ = -1; current().scale = 0; current().repaint = true; cancel();
}
void Application::select(int camera, int ball) { // 그 카메라를 크게, 공을 선택하고 그 공으로 확대
    if (frames_.empty() || camera >= int(frames_.size()) || !guard()) return;
    if (current().dirty && camera != camera_) { save([this, camera, ball] { select(camera, ball); }); return; }
    if (camera != camera_) { current().repaint = true; cancel(); }
    camera_ = camera; editor_ = true; selected_ = ball; current().scale = 0; current().repaint = true;
}
```

`showEditor`와 `switchCamera`는 `focus`를 부르도록 바꾸거나 지운다. `main.cpp`의 캡처 코드가 `showEditor(0)`을 부르므로 이름은 남기고 내용만 `focus(camera)`로 둔다.

**Step 3: `editor()` 교체**

- "← 3개 보기" 버튼, 카메라 버튼 줄, 도구 `Selectable` 줄, "새 공 추가 / 선택 공 보정" 라디오, 체크박스 줄, 아래쪽 버튼 줄을 모두 지운다.
- 초점 타일과 작은 타일 두 개를 `tileSpans(camera_, 폭)`의 자리에 그린다.
- 보기 영역의 `InvisibleButton` 앞에 `ImGui::SetNextItemAllowOverlap()`을 부른다. 도구 막대와 제안 줄의 버튼이 그 위에서 클릭을 받아야 한다.
- 도구 막대와 제안 줄은 `ImGui::SetCursorScreenPos`로 자리를 잡고 보통 버튼으로 그린다.
- `replaceSelected_` 멤버와 그것을 읽는 코드를 지운다. 규칙은 "AI 분할의 첫 클릭이 기존 공 위면 그 공을 고치고, 아니면 새 공"이다. `segment()`의 `if(replacement_<0&&replaceSelected_)replacement_=selected_;`를 지운다.

**Step 4: 빌드와 캡처**

Task 15의 `--view focus`와 `--select`가 아직 없으므로 `--view editor`로 캡처한다.

Run: 빌드 후 `--view editor` 캡처를 `v2-task8.png`로.
Expected: `shots/review-focus.png`와 같은 구성. 컬러 타일이 크고 모노 A, B가 오른쪽에 작게 있다. 도구 막대, 확대율, 미니맵이 보인다.

**Step 5: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp
git commit -m "Replace separate editor with in-place focus tile"
```

---

### Task 9: 오른쪽 패널

**Files:**
- Modify: `src/native_labeler_app.cpp:299-317` (`properties()`를 `sidePanel()`로), `:357-381` (`draw()`에서 패널 배치)
- Modify: `src/native_labeler_app.hpp`

**명세**

패널은 검수 단계 내내 보인다. 폭 344(창 폭 1500 미만이면 296), 왼쪽 선 `line`, 구역 여백 12×14.

| 구역 | 내용 |
| --- | --- |
| 이 장면의 공 | 카메라별 묶음 머리(상태 점, 이름). 공 행 높이 30: 번호 원, "공 1", 크기(고정폭), "수정됨"(`accent`), "가림"(`hold`), 오른쪽에 출처 꼬리표와 삭제. 선택한 행은 `selected` 10% 바탕과 45% 테두리. 클릭하면 `select(camera, ball)` |
| 선택한 공 | 제목 "컬러 C · 공 3"과 출처. 일부 가림 토글(O). 색상 콤보(컬러 카메라만). 박스 보정 |
| 박스 보정 | 가운데 104×78 박스에 "39 × 35". 위·아래·왼쪽·오른쪽에 [−][보정값][+] (칸 26×26, 값 폭 30). 보정값이 0이 아니면 `selected` 색. 아래에 X, Y, W, H 네 칸. "마스크에 맞춤" 버튼. 방향키 안내 두 줄 |
| 장면 요약 | 선택한 공이 없을 때. 카메라별 "공 3개 · 미검수". 공 개수가 다르면 `warn` 색 안내 |
| 확인 | 맨 아래 고정. "세 장 확인하고 다음 장면 E"(높이 44, `okFill`), "보류 Q"와 "다음 미검수 N"(높이 32), 안내 한 줄 |

**Step 1: 보정을 한 함수로**

버튼과 방향키(Task 10)가 같은 함수를 쓴다.

```cpp
void Application::nudge(int edge, bool expand) {   // edge: 0 위, 1 아래, 2 왼쪽, 3 오른쪽
    if (!editor_ || selected_ < 0 || pending() || busy()) return;
    auto& frame = current(); auto& box = frame.annotation["boxes"][selected_]; auto trial = box;
    if (!nudgeEdge(trial, edge, expand, frame.image.cols, frame.image.rows)) { message_ = expand ? "이미지 밖으로 넓힐 수 없습니다." : "박스는 1픽셀보다 작아질 수 없습니다."; return; }
    checkpoint(); box = trial; boxes_ = true; changed();
}
```

보정값은 `detectionBox(box)`와 `bbox`의 차이로 계산한다. 위 = `bbox.y - detection.y`, 왼쪽 = `bbox.x - detection.x`, 아래 = `(detection.y + detection.height) - (bbox.y + bbox.height)`, 오른쪽도 같은 방식이다.

**Step 2: `draw()`의 검수 배치**

```cpp
        if (page_ == Page::Review) {
            const float width = ImGui::GetContentRegionAvail().x, list = width < 1500 ? 232.f : 300.f, side = width < 1500 ? 296.f : 344.f;
            ImGui::BeginChild("QueuePane", {list, 0}); queue(); ImGui::EndChild(); ImGui::SameLine(0, 0);
            ImGui::BeginChild("ReviewPane", {width - list - side, 0}); if (editor_) editor(); else overview(); ImGui::EndChild(); ImGui::SameLine(0, 0);
            ImGui::BeginChild("SidePane", {side, 0}); sidePanel(); ImGui::EndChild();
        }
```

가운데 영역의 바탕은 `bg0`이다. `ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg0)`으로 감싼다.

**Step 3: 빌드와 캡처**

Run: 빌드 후 `--view overview`와 `--view editor` 캡처를 `v2-task9-overview.png`, `v2-task9-editor.png`로.
Expected: 두 캡처 모두 오른쪽에 공 목록과 확인 버튼이 있다. `shots/review-overview.png`, `shots/review-focus.png`의 오른쪽 패널과 같은 구성.

**Step 4: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp
git commit -m "Add persistent side panel with ball list, box nudge pad and confirm actions"
```

---

### Task 10: 조작

**Files:**
- Modify: `src/native_labeler_app.cpp:156-171` (`review`, `nextUnreviewed`), `:249-267` (보기 영역 입력), `:370-378` (키 처리를 `keys()`로)
- Modify: `src/native_labeler_app.hpp`
- Test: `tests/native_labeler_tests.cpp` (HTTP 구간에 박스 프롬프트 확인 추가)

**Step 1: 확인 범위**

```cpp
    enum class Scope { Scene, Camera };
    void review(Scope scope, bool hold, int member = -1);
```

```cpp
void Application::review(Scope scope, bool hold, int member) {
    if (!guard() || frames_.empty()) return;
    if (current().dirty) { save([this, scope, hold, member] { review(scope, hold, member); }); return; }
    Json keys = Json::array();
    if (scope == Scope::Camera) keys.push_back(frames_.at(member >= 0 ? member : camera_).annotation.at("key"));
    else for (auto& frame : frames_) if (frame.member.value("selected", true) && (!hold || !frame.member.value("reviewed", false))) keys.push_back(frame.annotation.at("key"));
    if (keys.empty()) return;
    const bool advance = scope == Scope::Scene || hold;
    const auto client = client_; Json payload = {{"keys", keys}, {"status", hold ? "hold" : "reviewed"}, {"expected_revision", catalog_["summary"]["revision"]}};
    launch(hold ? "보류 상태 저장" : "확인 저장", [client, payload] { return JobResult{client.post("/api/native/review", payload), {}}; },
        [this, advance, hold, keys](JobResult result) {
            updateSummary(result.data.at("summary"));
            for (auto& f : frames_) if (std::find(keys.begin(), keys.end(), f.annotation["key"]) != keys.end()) { f.annotation["reviewed"] = !hold; f.annotation["review_status"] = hold ? "hold" : "reviewed"; }
            message_ = hold ? "보류로 표시했습니다" : "확인했습니다";
            if (advance) nextUnreviewed();
        });
}
```

`nextUnreviewed()`에서 "같은 장면의 다음 카메라" 분기를 지우고, 보류한 이미지는 건너뛴다.

```cpp
void Application::nextUnreviewed() {
    const int count = int(catalog_["scenes"].size());
    for (int offset = 1; offset <= count; ++offset) {
        const int index = (scene_ + offset) % count;
        for (auto& member : catalog_["scenes"][index]["members"])
            if (member.value("selected", true) && !member.value("reviewed", false) && member.value("status", "") != "hold") { loadScene(index); return; }
    }
    editor_ = false; message_ = "검수 대상을 모두 확인했습니다. 분할 단계로 넘어가세요.";
}
```

**Step 2: 키 처리**

`draw()`의 키 블록을 지우고 `keys()`를 부른다.

```cpp
void Application::keys() {
    auto& io = ImGui::GetIO();
    if (page_ != Page::Review || io.WantTextInput || frames_.empty()) return;
    auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
    if (pressed(ImGuiKey_Escape)) {
        if (pending()) cancel();
        else if (editor_ && selected_ >= 0) { selected_ = -1; current().scale = 0; current().repaint = true; }
        else if (editor_ && guard()) { if (current().dirty) save([this] { editor_ = false; }); else editor_ = false; }
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) apply();
    if (io.KeyCtrl && pressed(ImGuiKey_Z)) undo();
    if (io.KeyCtrl && pressed(ImGuiKey_S)) save();
    if (io.KeyCtrl || io.KeyAlt) return;
    if (pressed(ImGuiKey_E)) review(io.KeyShift && editor_ ? Scope::Camera : Scope::Scene, false);
    if (pressed(ImGuiKey_Q)) review(editor_ ? Scope::Camera : Scope::Scene, true);
    if (pressed(ImGuiKey_N) && guard()) nextUnreviewed();
    if (pressed(ImGuiKey_A)) stepRow(-1);
    if (pressed(ImGuiKey_D)) stepRow(1);
    if (pressed(ImGuiKey_1)) focus(0);
    if (pressed(ImGuiKey_2)) focus(1);
    if (pressed(ImGuiKey_3)) focus(2);
    if (pressed(ImGuiKey_Tab)) cycle(io.KeyShift ? -1 : 1);
    static const ImGuiKey arrows[] = {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow};
    for (int edge = 0; edge < 4; ++edge) if (ImGui::IsKeyPressed(arrows[edge], true)) nudge(edge, !io.KeyShift);
    if (pressed(ImGuiKey_M)) { masks_ = !masks_; }
    if (pressed(ImGuiKey_X)) boxes_ = !boxes_;
    if (!editor_ || pending() || busy()) return;
    if (pressed(ImGuiKey_V)) tool_ = Tool::Select;
    if (pressed(ImGuiKey_S)) tool_ = Tool::Ai;
    if (pressed(ImGuiKey_B)) tool_ = Tool::Brush;
    if (pressed(ImGuiKey_P)) tool_ = Tool::Polygon;
    if (pressed(ImGuiKey_R)) tool_ = Tool::Box;
    if (pressed(ImGuiKey_F)) { selected_ = -1; current().scale = 0; }
    if (pressed(ImGuiKey_Z)) current().scale = 0;
    if (pressed(ImGuiKey_LeftBracket)) brushSize_ = std::max(2.f, brushSize_ - 1);
    if (pressed(ImGuiKey_RightBracket)) brushSize_ = std::min(30.f, brushSize_ + 1);
    auto& objects = current().annotation["boxes"];
    if (selected_ < 0 || selected_ >= int(objects.size())) return;
    if (pressed(ImGuiKey_O)) { checkpoint(); objects[selected_]["occluded"] = !objects[selected_].value("occluded", false); changed(); }
    if (pressed(ImGuiKey_Delete)) { checkpoint(); objects.erase(objects.begin() + selected_); selected_ = -1; current().scale = 0; changed(); }
}
void Application::cycle(int step) {
    if (!guard()) return;
    std::vector<Json> annotations; for (auto& frame : frames_) annotations.push_back(frame.annotation);
    const auto next = nextBall(ballOrder(annotations), editor_ ? BallRef{camera_, selected_} : BallRef{}, step);
    if (next.camera >= 0) select(next.camera, next.index);
}
```

`main.cpp`에서 `io.ConfigFlags`에 키보드 내비게이션을 켜지 않는다. 켜면 Tab이 위젯 이동에 쓰인다.

**Step 3: 마우스 규칙**

보기 영역 입력(지금 252-266행)을 다음 규칙으로 다시 쓴다. 왼쪽 버튼은 추가, 오른쪽 버튼 또는 `Shift`+왼쪽은 제거다.

| 도구 | 왼쪽 | 오른쪽 또는 Shift+왼쪽 |
| --- | --- | --- |
| 선택 | 공 위에서 누르면 선택하고 그 공으로 확대. 빈 곳에서 끌면 화면 이동 | 없음 |
| AI 분할 | 클릭은 포함 점. 4픽셀 넘게 끌면 박스 지정 | 제외 점 |
| 브러시 | 칠하기 | 지우기 |
| 폴리곤 | 꼭짓점 추가 | 마지막 꼭짓점 취소 |
| 박스 | 끌어서 새 박스 | 없음 |

- 휠 확대, 가운데 버튼 끌기, `Space`+끌기는 지금처럼 모든 도구에서 동작한다.
- 화면 이동 상태를 나타내는 `bool panning_`을 추가한다. 누를 때 켜고 왼쪽 버튼을 놓을 때 끈다.
- AI 분할의 박스 지정은 놓을 때 처리한다. 화면 거리 4픽셀 이하면 `segment(point, false)`, 넘으면 `segmentBox`.

```cpp
void Application::segmentBox(cv::Rect2d box) {   // 끌어서 지정한 박스의 가운데를 포함 점으로 함께 보낸다
    if (busy() || !points_.empty() || box.width < 2 || box.height < 2) return;
    const cv::Point2d center{box.x + box.width / 2, box.y + box.height / 2};
    replacement_ = hit(current(), center); if (replacement_ >= 0) selected_ = replacement_;
    points_.push_back({{"x", center.x}, {"y", center.y}, {"label", 1}});
    auto payload = annotationPayload(current().annotation, catalog_["summary"]["revision"].get<int>());
    payload["points"] = points_; payload["radius"] = std::max(box.width, box.height) / 2; payload["box"] = {box.x, box.y, box.width, box.height};
    const auto key = current().annotation.at("key").get<std::string>(); const auto client = client_; const auto token = generation_;
    launch("EfficientSAM으로 공 윤곽 분리", [client, payload] { return JobResult{client.post("/api/assist/segment", payload), {}}; },
        [this, key, token](JobResult result) { if (token != generation_ || current().annotation.value("key", "") != key) return; proposal_ = result.data.at("proposals").at(0); current().repaint = true; message_ = "제안 미확정 · Enter 반영, Esc 취소"; });
}
```

- 박스 도구는 항상 새 공을 만든다. 선택한 공의 검출 박스를 바꾸는 분기(`proposal_["detection_bbox"]=...`)를 지운다. 박스 보정은 방향키와 패널 버튼이 한다.
- 브러시는 왼쪽이나 오른쪽 버튼을 누를 때 시작하고 둘 중 하나가 눌려 있는 동안 이어진다. 지우기 여부는 `ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.KeyShift`다. `erase_` 멤버와 "지우개" 체크박스를 지운다.

**Step 4: 박스 프롬프트 통합 확인**

`tests/native_labeler_tests.cpp`의 HTTP 구간에서 `proposed` 확인 바로 아래에 추가한다.

```cpp
        Json boxed=annotationPayload(original,saved["summary"]["revision"]);boxed["points"]=prompt["points"];boxed["radius"]=18;boxed["box"]={rectangle.x-2,rectangle.y-2,rectangle.width+4,rectangle.height+4};
        auto boxProposal=client.post("/api/assist/segment",boxed);check(boxProposal["proposals"][0].contains("segmentation"),"box prompted SAM inference failed");
```

Run: `cmake --build build-labeler --config Release --target boccia_labeler native_labeler_tests -j 4; .\build-labeler\Release\native_labeler_tests.exe 8792`
Expected: `PASS: native HTTP image, mask-preserving save/reload and actual EfficientSAM inference`와 `PASS: RLE, ...`

이 테스트는 8792의 테스트용 프로젝트에 저장했다가 되돌린다. 8767로 실행하지 않는다.

**Step 5: 손으로 확인**

숨김 캡처로는 입력을 확인할 수 없다. 앱을 테스트용 프로젝트로 띄워서 직접 확인한다.

```powershell
.\build-labeler\Release\boccia_labeler.exe --no-start-server --port 8792 --project D:\works\bochia\output\native-labeler\v2-qa-project.json --export-root D:\works\bochia\output\native-labeler\v2-qa-exports
```

| 조작 | 기대 결과 |
| --- | --- |
| `Tab` 9번 | 컬러 1, 2, 3 → 모노 A 1, 2, 3 → 모노 B 1, 2, 3 순서로 초점과 확대가 옮겨 간다 |
| 공 선택 후 `↑` | 박스 위 경계가 1픽셀 올라가고 패널의 위 보정값이 +1, 꼬리표의 높이가 1 늘어난다 |
| `Shift`+`↑` | 원래대로 돌아오고 보정값이 0이 된다 |
| `S` 후 공 안 왼쪽 클릭, 바깥 오른쪽 클릭, `Enter` | 제안이 나오고 반영된다. 상태가 수정 중으로 바뀐다 |
| `S` 후 빈 곳의 공 둘레를 끌기, `Enter` | 박스로 지정한 공의 윤곽이 새 공으로 추가된다 |
| `E` | 세 장이 확인됨이 되고 다음 미검수 장면이 열린다. 목록의 점 3개가 초록이 된다 |
| 확인한 장면으로 돌아가 `↑` | 그 카메라의 점만 파랑 테두리로 바뀐다 |
| `Q` | 보류로 표시되고 다음 장면이 열린다. `N`은 보류한 장면을 건너뛴다 |
| `Esc` 세 번 | 제안 취소, 선택 해제, 석 장 보기 순서로 돌아간다 |

**Step 6: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp tests/native_labeler_tests.cpp
git commit -m "Apply v2 key map, left-add right-remove mouse rule and scene-level confirm"
```

---

### Task 11: 분할 화면

**Files:**
- Modify: `src/native_labeler_view.hpp`, `src/native_labeler_view.cpp` (합계와 점검)
- Modify: `src/native_labeler_app.cpp:333-349` (`splitPage()`)
- Test: `tests/native_labeler_tests.cpp`

**Step 1: 실패하는 테스트 작성**

`viewTests()` 끝에 추가한다.

```cpp
    auto stats = Json::parse(R"({"a":{"number":1,"day":"2026-10-02","images":420,"balls":563,"empty":0},
                                 "b":{"number":2,"day":"2026-10-02","images":492,"balls":935,"empty":39},
                                 "c":{"number":3,"day":"2026-10-02","images":629,"balls":1322,"empty":0}})");
    auto splits = Json::parse(R"({"a":"test","b":"train","c":"val"})"), groups = Json::parse(R"({"a":"a","b":"b","c":"c"})");
    auto totals = splitTotals(stats, splits);
    check(totals["train"].images == 492 && totals["train"].empty == 39 && totals["test"].recordings == 1 && totals["val"].balls == 1322, "split totals wrong");
    auto checks = splitChecks(stats, splits, groups);
    check(checks.size() == 5 && checks[0].ok && checks[1].ok && !checks[2].ok && !checks[3].ok && !checks[4].ok, "split checks wrong");
    check(checks[2].detail.find("02") != std::string::npos, "empty-image check must name the recordings that have them");
    groups["b"] = "a";
    check(!splitChecks(stats, splits, groups)[0].ok, "a group across two splits must fail");
    splits["a"] = "unassigned";
    check(!splitChecks(stats, splits, groups)[1].ok, "a split without images must fail");
```

**Step 2: 실패 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4`
Expected: FAIL. `splitTotals`가 선언되지 않았다는 컴파일 오류.

**Step 3: 최소 구현**

`native_labeler_view.hpp`에 추가한다.

```cpp
struct SplitTotal { int recordings = 0, images = 0, balls = 0, empty = 0; };
struct SplitCheck { bool ok = false; std::string text, detail; };
std::map<std::string, SplitTotal> splitTotals(const Json& stats, const Json& splits);
// 순서: 묶음이 두 쪽에 걸치는지, 세 쪽에 이미지가 있는지, 공 없는 이미지, 촬영 1개뿐인 쪽, 같은 날.
std::vector<SplitCheck> splitChecks(const Json& stats, const Json& splits, const Json& groups);
```

`native_labeler_view.cpp`에 추가한다. 위쪽에 `#include <set>`과 `#include <cstdio>`를 넣는다.

```cpp
std::map<std::string, SplitTotal> splitTotals(const Json& stats, const Json& splits) {
    std::map<std::string, SplitTotal> totals{{"train", {}}, {"val", {}}, {"test", {}}};
    for (const auto& [recording, item] : stats.items()) {
        const auto found = totals.find(splits.value(recording, "unassigned"));
        if (found == totals.end()) continue;
        auto& total = found->second;
        ++total.recordings; total.images += item.value("images", 0); total.balls += item.value("balls", 0); total.empty += item.value("empty", 0);
    }
    return totals;
}
std::vector<SplitCheck> splitChecks(const Json& stats, const Json& splits, const Json& groups) {
    static const std::pair<const char*, const char*> names[] = {{"train", "학습"}, {"val", "검증"}, {"test", "평가"}};
    auto totals = splitTotals(stats, splits);
    auto join = [](const std::vector<std::string>& parts) { std::string text; for (const auto& part : parts) text += (text.empty() ? "" : ", ") + part; return text; };
    std::vector<SplitCheck> checks;
    std::map<std::string, std::string> owner; bool crossing = false;
    for (const auto& [recording, group] : groups.items()) {
        const auto split = splits.value(recording, "unassigned");
        const auto [found, added] = owner.emplace(group.get<std::string>(), split);
        crossing |= !added && found->second != split;
    }
    checks.push_back({!crossing, crossing ? "같은 묶음의 촬영이 서로 다른 쪽에 들어 있습니다" : "같은 촬영과 같은 묶음이 두 쪽에 걸치지 않습니다", ""});
    std::vector<std::string> missing, noEmpty, single;
    for (const auto& [key, label] : names) {
        const auto& total = totals[key];
        if (!total.images) { missing.push_back(label); continue; }
        if (!total.empty) noEmpty.push_back(label);
        if (total.recordings < 2) single.push_back(label);
    }
    checks.push_back({missing.empty(), missing.empty() ? "학습·검증·평가에 모두 이미지가 있습니다" : join(missing) + "에 이미지가 없습니다", ""});
    std::vector<std::string> withEmpty; std::set<std::string> days;
    for (const auto& [recording, item] : stats.items()) {
        if (item.value("empty", 0)) { char number[8]; std::snprintf(number, sizeof number, "%02d", item.value("number", 0)); withEmpty.push_back(number); }
        if (item.contains("day") && item["day"].is_string()) days.insert(item["day"].get<std::string>());
    }
    std::sort(withEmpty.begin(), withEmpty.end());
    checks.push_back({noEmpty.empty(), noEmpty.empty() ? "세 쪽 모두 공 없는 이미지가 들어 있습니다" : join(noEmpty) + "에 공 없는 이미지가 없습니다",
                      noEmpty.empty() ? "" : "공 없는 이미지가 있는 촬영: " + join(withEmpty)});
    checks.push_back({single.empty(), single.empty() ? "모든 쪽에 촬영이 2개 이상입니다" : join(single) + "의 촬영이 1개뿐입니다",
                      single.empty() ? "" : "장면 다양성이 낮아 점수가 그 촬영에 좌우됩니다"});
    checks.push_back({days.size() != 1, days.size() == 1 ? "모든 촬영이 같은 날입니다" : "여러 날의 촬영이 섞여 있습니다",
                      days.size() == 1 ? "다른 날 촬영이 들어오면 평가에 넣는 것을 권합니다" : ""});
    return checks;
}
```

**Step 4: 통과 확인**

Run: `cmake --build build-labeler --config Release --target native_labeler_tests -j 4; .\build-labeler\Release\native_labeler_tests.exe`
Expected: PASS

**Step 5: `splitPage()` 교체**

단계 화면(준비, 분할, 내보내기)의 공통 배치는 왼쪽 본문과 오른쪽 폭 372의 요약 열이다. 바깥 여백은 위아래 20, 좌우 28, 열 사이 22다. 이 배치를 `stagePage(본문 함수, 요약 함수)`로 한 번만 만든다.

| 요소 | 내용 |
| --- | --- |
| 제목 | "분할"(21), 설명 한 줄. 오른쪽에 "선택한 촬영을 같은 배치로 묶기", "배분안 다시 제안" |
| 알림 | 모두 확인했으면 `okSoft` 바탕에 "검수 대상 3,936장을 모두 확인했습니다". 아니면 `accentSoft` 바탕에 "확인한 이미지 0 / 3,936장 · 확정은 모두 확인한 뒤에 할 수 있습니다" |
| 비율 막대 | 높이 30. 학습, 검증, 평가 순서로 `train`, `val`, `test` 색. 4% 넘는 칸에 "학습 78.0%". 80%와 90% 위치에 흰 세로선. 아래에 쪽별 "촬영 7개 · 3,072장 · 공 4,857개 · 공 없는 이미지 213장" |
| 촬영 표 | 열: 번호, 촬영 시각과 길이, 확인한 이미지, 공, 공 없는 이미지, 작은 공과 비율, 묶음, 배정. 행 높이 45. 배정은 `theme::segmented`에 `train`, `val`, `test` 색 |
| 점검 | `splitChecks` 결과. 통과는 `ok` 체크, 아니면 `warn` 삼각형과 설명 |
| 확정 | "분할 확정"(높이 44). 모두 확인하지 않았으면 누를 때 남은 장수를 `message_`로 알려 주고 아무것도 하지 않는다 |

- 수치는 `catalog_["stats"]`에서 읽는다. `updateCatalog`에서 받는다.
- 묶음은 지금의 `groups_`(촬영 ID → 묶음 ID)를 그대로 쓴다. 행 왼쪽 체크박스로 고른 촬영에 "같은 배치로 묶기"를 누르면 첫 번째 촬영의 ID를 묶음 ID로 준다. 묶인 촬영에는 "묶음 A, B..." 꼬리표와 "풀기"를 표시한다. 꼬리표 글자는 묶음 ID가 처음 나온 순서다.
- 배정을 바꾸면 같은 묶음의 촬영이 함께 바뀐다. 지금 코드의 `for(auto& [recording,value]:groups_.items())if(value==groups_[id])splits_[recording]=...`를 그대로 쓴다.
- "배분안 다시 제안"과 "분할 확정"은 지금의 `/api/native/split-preview`, `confirmSplit()`을 그대로 부른다.

**Step 6: 빌드와 캡처**

Run: 빌드 후 `--view split` 캡처를 `v2-task11.png`로.
Expected: `shots/split.png`와 같은 구성. 테스트용 프로젝트는 확인한 이미지가 0장이므로 알림이 파랑이고 배정은 모두 미지정이다. 비율 막대는 비어 있다.

**Step 7: Commit**

```powershell
git add src/native_labeler_view.hpp src/native_labeler_view.cpp src/native_labeler_app.cpp src/native_labeler_app.hpp tests/native_labeler_tests.cpp
git commit -m "Redesign split page with ratio bar, per-recording stats and checks"
```

---

### Task 12: 내보내기 화면

**Files:**
- Modify: `src/native_labeler_app.cpp:350-356` (`exportPage()`)
- Modify: `src/native_labeler_app.hpp` (`Json exports_`)

**명세**

| 요소 | 내용 |
| --- | --- |
| 제목 | "내보내기"와 설명 한 줄 |
| 카드 4개 | 데이터 버전(확정 지문의 앞 8자, 확정 전이면 "확정 전"), 학습, 검증, 평가의 이미지 수와 촬영 수와 공 수 |
| 형식 표 | 행 높이 58. YOLO 검출 라벨, COCO 검출, COCO 마스크는 항상 만들므로 체크된 채 바꿀 수 없게 표시. 공 조각 이미지만 `crops_` 체크박스. 열: 이름과 박스 종류, 파일 경로(고정폭), 대상 모델 |
| 항상 포함 | 원본 이미지와 `manifest.json` 두 줄 |
| 저장 위치 | `config_.exports` 경로 |
| 실행 | "내보내기"(높이 44). 진행 중이면 진행 막대. 끝나면 완료 폴더 경로 |
| 학습 시작 | YOLO26 명령과 "복사". `ImGui::SetClipboardText`로 전체 경로를 넣는다. 최근 내보내기가 없으면 "먼저 내보내세요" |
| 내보내기 기록 | `/api/native/exports` 결과. 폴더 이름, 시각, 쪽별 장수. "폴더 열기"는 `ShellExecuteW(nullptr, L"open", 경로, ...)` |
| 비교 원칙 | 안내 두 줄 |

- 분할이 확정되지 않았으면 "내보내기"를 누를 때 "분할을 먼저 확정하세요"를 `message_`로 알려 주고 분할 화면으로 보낸다.
- 내보내기 기록은 화면에 들어올 때와 내보내기가 끝났을 때 다시 읽는다.
- YOLO26 명령 문자열:

```cpp
std::string command = "yolo detect train data=\"" + path + "\\data.yaml\" model=yolo26s.pt imgsz=1440 batch=-1";
```

**Step 1: 구현**

위 명세대로 `exportPage()`를 교체한다. `main.cpp`의 `--view`에 `export`를 추가한다(`application.showExport()`).

**Step 2: 빌드와 캡처**

Run: 빌드 후 `--view export` 캡처를 `v2-task12.png`로.
Expected: `shots/export.png`와 같은 구성. 테스트용 프로젝트는 확정 전이므로 데이터 버전 카드가 "확정 전"이고 기록이 비어 있다.

**Step 3: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp src/native_labeler_main.cpp
git commit -m "Redesign export page with format table, history and training command"
```

---

### Task 13: 데이터 준비 화면

**Files:**
- Modify: `src/native_labeler_app.cpp:318-328` (`preparePage()`)

**명세**

지금 엔진이 할 수 있는 것만 옮긴다. "촬영 폴더 추가"와 새 촬영 알림은 2차 계획에서 넣는다. 동작하지 않는 버튼은 그리지 않는다.

| 요소 | 내용 |
| --- | --- |
| 제목 | "데이터 준비"와 설명 한 줄 |
| 카드 4개 | 촬영 수와 카메라 수, 원본 이미지 수, 자동 초안(진행 중이면 진행 막대), 검수 대상과 장면 수와 보관한 반복 장면 수 |
| 촬영 표 | 열: 번호, 촬영 시각과 길이, 프레임 수 × 카메라 수, 자동 초안(공 후보 수, 후보 없는 이미지 수는 `warn`), 반복 장면 필터(검수 대상과 보관), 검수 진행 막대, 분할 배정. 수치는 `catalog_["stats"]`와 `catalog_["recordings"]` |
| 다음 단계 | "검수 시작"(높이 44). 검수 단계로 이동 |
| 자동 초안 | 상태와 진행 막대, "시작 / 이어하기", "중단". 지금의 `/api/auto-start`, `/api/auto-stop` |
| 카메라 | 이름과 종류. 시리얼은 `catalog_["recordings"][0]["cameras"]`에 없으므로 표시하지 않는다 |
| 원본 보호 | 안내 두 줄과 입력 경로, 프로젝트 경로 |

- 지금 화면의 "컬러 보기 밝기" 슬라이더는 검수 화면의 장면 줄로 옮겼다(Task 7). 여기서는 지운다.
- "후보 없는 이미지" 수는 `stats`의 `empty`다. 전체 원본 기준이 아니라 검수 대상 기준이라는 점을 열 제목에 "검수 대상 기준"으로 적는다.

**Step 1: 구현**

위 명세대로 `preparePage()`를 교체한다. `main.cpp`의 `--view`에 `prepare`를 추가한다.

**Step 2: 빌드와 캡처**

Run: 빌드 후 `--view prepare` 캡처를 `v2-task13.png`로.
Expected: `shots/prep-main.png`에서 알림과 새 촬영 묶음, "촬영 폴더 추가" 버튼을 뺀 구성. 촬영 08 행에 "후보 없는 이미지 165장"이 `warn` 색으로 나온다.

**Step 3: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_main.cpp
git commit -m "Redesign data preparation page with recording table"
```

---

### Task 14: 다음 장면 미리 읽기

**Files:**
- Modify: `src/native_labeler_app.cpp:98-106` (`loadScene`), `:175-193` (`tick`)
- Modify: `src/native_labeler_app.hpp`

2026-10-04 측정으로 엔진에서 이미지 1장을 받는 데 약 0.1초, 장면 하나에 약 0.3초다. `E`를 누를 때마다 이만큼 기다리지 않도록 한다.

**Step 1: 장면 안의 카메라 3대를 동시에 받기**

`loadScene`의 작업 함수에서 카메라마다 `std::async`를 쓴다. `Client`는 요청마다 WinHTTP 세션을 새로 여는 const 객체라 여러 스레드에서 함께 써도 된다.

```cpp
JobResult fetchScene(const Client& client, const Json& scene) {
    const auto recording = scene.at("recording").get<std::string>();
    std::vector<std::future<FrameView>> pending;
    for (const auto& member : scene.at("members")) pending.push_back(std::async(std::launch::async, [client, member, recording] {
        FrameView frame; frame.member = member;
        frame.annotation = client.get("/api/frame" + Client::frameQuery(member, recording));
        frame.annotation["review_status"] = member.value("status", "draft");
        frame.image = client.image(member, recording);
        return frame;
    }));
    JobResult result; for (auto& frame : pending) result.frames.push_back(frame.get());
    return result;
}
```

**Step 2: 다음 미검수 장면을 미리 받기**

```cpp
    std::future<JobResult> ahead_;
    int aheadScene_ = -1, aheadAuto_ = -1;
    int nextUnreviewedIndex() const;   // nextUnreviewed()의 탐색 부분을 분리. 없으면 -1
```

- `tick()`에서 `ready() && !busy() && !ahead_.valid()`이고 `aheadScene_ != nextUnreviewedIndex()`면 그 장면을 `ahead_`로 받기 시작한다. `aheadAuto_`에 `catalog_["summary"]["auto_revision"]`를 적어 둔다.
- `loadScene(index)`에서 `index == aheadScene_`이고 `ahead_`가 끝났고 `auto_revision`이 그대로면, 서버에 요청하지 않고 그 결과를 쓴다. 그 사이 `updateSummary`로 바뀐 상태를 반영하기 위해 각 프레임의 `member`는 `catalog_["scenes"][index]["members"]`에서 다시 읽는다.
- 조건이 맞지 않으면 미리 받은 결과를 버리고 지금처럼 받는다.
- 소멸자에서 `ahead_.wait()`를 부른다.

미리 받는 것은 다른 장면의 프레임이므로 현재 장면의 저장과 충돌하지 않는다. 저장 요청은 프레임의 `revision`이 아니라 `catalog_["summary"]["revision"]`을 쓴다.

**Step 3: 손으로 확인**

Task 10 Step 5의 명령으로 앱을 띄우고 `E`를 연속으로 5번 누른다.
Expected: 두 번째부터는 다음 장면이 기다림 없이 열린다. 상태 막대에 "카메라 이미지 불러오기"가 보이지 않는다.

확인이 끝나면 테스트용 프로젝트를 Task 0 Step 3의 명령으로 다시 복사해 되돌린다(엔진을 먼저 내린다).

**Step 4: Commit**

```powershell
git add src/native_labeler_app.cpp src/native_labeler_app.hpp
git commit -m "Fetch cameras in parallel and prefetch the next unreviewed scene"
```

---

### Task 15: 캡처 옵션과 시안 비교

**Files:**
- Modify: `src/native_labeler_main.cpp:17-25` (옵션), `:47-51` (캡처 단계)
- Modify: `src/native_labeler_app.hpp` (캡처용 공개 함수)
- Modify: `NATIVE_LABELER.md` (화면과 단축키 설명 갱신)

**Step 1: 캡처 옵션**

- `--view`: `prepare`, `overview`, `focus`, `split`, `export`. 기존 `editor`는 `focus`와 같게 둔다.
- `--select <카메라>:<공>`: `focus`에서 그 공을 선택한다. 예: `--select 0:2`.
- 썸네일이 도착할 시간을 주기 위해 캡처 전 대기를 `++settled>=3`에서 `++settled>=40`으로 바꾸고, `application.busy()`뿐 아니라 썸네일 요청이 남아 있는지도 본다.

`Application`에 추가한다.

```cpp
    void showPage(int page);              // 0 준비, 1 검수, 2 분할, 3 내보내기
    void selectForCapture(int camera, int ball) { select(camera, ball); }
    bool settled() const;                 // 진행 중인 요청과 남은 썸네일이 없으면 true
```

**Step 2: 다섯 화면을 두 크기로 캡처**

```powershell
$base = "--smoke-hidden --no-start-server --port 8792 --project D:\works\bochia\output\native-labeler\v2-qa-project.json --export-root D:\works\bochia\output\native-labeler\v2-qa-exports --recording 5 --frame 22"
foreach ($size in @(@(1920,1080), @(1280,720))) {
  foreach ($view in "prepare","overview","focus","split","export") {
    $extra = if ($view -eq "focus") { "--select 0:2" } else { "" }
    $out = "D:\works\bochia\output\native-labeler\v2-$view-$($size[0]).png"
    Start-Process -Wait -NoNewWindow .\build-labeler\Release\boccia_labeler.exe -ArgumentList "$base --view $view $extra --width $($size[0]) --height $($size[1]) --capture $out"
  }
}
```

Expected: 파일 10개. 각 실행이 `PASS`를 출력한다.

**Step 3: 시안과 비교**

Read 도구로 캡처와 시안을 차례로 연다.

| 캡처 | 시안 | 확인할 것 |
| --- | --- | --- |
| `v2-prepare-1920.png` | `shots/prep-main.png` | 카드 4개, 촬영 9행, 촬영 08의 경고 |
| `v2-overview-1920.png` | `shots/review-overview.png` | 목록의 상태 점, 타일 3개, 카메라마다 조각 3개와 빈 칸 |
| `v2-focus-1920.png` | `shots/review-focus.png` | 큰 타일과 작은 타일 2개, 도구 막대, 미니맵, 박스 보정 패널 |
| `v2-split-1920.png` | `shots/split.png` | 비율 막대, 촬영 표, 점검 |
| `v2-export-1920.png` | `shots/export.png` | 카드, 형식 표, 오른쪽 열 |
| `v2-*-1280.png` | 없음 | 글자나 버튼이 잘리지 않고 모든 조작에 닿을 수 있음 |

시안은 검수가 진행된 예시 상태이고 캡처는 확인한 이미지가 0장인 실제 상태다. 구성과 수치(폭, 높이, 색)를 비교하고 상태 값의 차이는 무시한다. 어긋난 곳은 고친 뒤 다시 캡처한다. 원인을 모르겠으면 @superpowers:systematic-debugging 을 따른다.

**Step 4: 전체 검증**

Run: 공통 명령의 빌드, C++ 테스트, Python 테스트, 해시.
Expected: 빌드 성공, C++ PASS, Python 33개 OK, 해시 `12485efb0f227acc78c108753f1f2a510b5ae8efdda4d77a170ba64a1f56bc83`.

**Step 5: 문서 갱신**

`NATIVE_LABELER.md`의 "구현된 작업 흐름", "편집 단축키"를 새 화면과 키에 맞게 고친다. [DESIGN.md](DESIGN.md) 맨 위의 "아직 구현되지 않았습니다"를 "1차 화면 교체까지 구현했습니다. 촬영 추가는 2차 계획입니다"로 바꾼다.

**Step 6: Commit**

```powershell
git add src/native_labeler_main.cpp src/native_labeler_app.hpp src/native_labeler_app.cpp NATIVE_LABELER.md design/labeler-v2/DESIGN.md
git commit -m "Add v2 capture views and update native labeler documentation"
```

---

## 끝난 뒤

- 테스트용 엔진(8792)을 내리고 `output/native-labeler/v2-qa-*` 파일은 남겨 둔다. git 추적 대상이 아니다.
- 사용자의 작업 프로젝트(`output/native-labeler/project.json`, 포트 8767)는 새 앱으로 그대로 열린다. 엔진을 다시 띄워야 Task 3의 변경이 적용된다.
- 2차 계획(촬영 추가)을 쓴다.
