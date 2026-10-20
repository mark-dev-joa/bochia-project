# Boccia Hub 2단계: 허브 화면과 `Boccia Hub.exe` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 이 PC에서 `Boccia Hub.exe`를 열면 붙은 라벨러 목록과 작업자별 활동·진행을 시안 그대로의 화면에서 실시간으로 본다.

**Architecture:** 허브(`hub/hub.py`)가 `hub/ui/`의 HTML·JS·CSS를 내주고, 화면은 2초마다 제어 API를 부른다. `Boccia Hub.exe`(C++·WebView2)는 허브가 꺼져 있으면 `pythonw`로 켜고 자기 창에 그 화면을 띄운다. 진행 숫자는 라벨러가 라벨 파일에서 센다. 확인·보류할 때 프레임에 `reviewed_at`·`reviewed_by`를 적고, 그 값으로 센 숫자를 상태에 실어 보낸다.

**Tech Stack:** Python 3.12 표준 라이브러리, HTML/CSS/바닐라 JS, C++20 + WebView2 SDK 1.0.4258.31(정적 로더), WinHTTP, CMake/MSVC(`build-labeler`), headless Edge(화면 점검·캡처).

**Spec:** `design/hub/DESIGN.md`("화면", "구현 순서" 2). 1단계는 `design/hub/PLAN-1.md`와 리뷰 `design/hub/REVIEW-1.md`. 2026-10-20 사용자 결정은 두 가지다: 창은 exe 자체 창(WebView2), 확인 수는 라벨 파일에 기록. 시안은 `design/trainer/workers-light.html`, 캡처는 `design/trainer/shots/workers-{list,activity,progress}.png`.

## Global Constraints

- 화면 문구는 라벨·숫자·버튼만 쓰고 설명 문장은 넣지 않는다. 밝은 바탕, 모서리 5px, 아바타 없음. 색·글꼴 토큰은 시안의 `:root`를 그대로 쓴다.
- 시안과 다른 곳은 다음뿐이다.
  - 맨 위 이름: "Boccia Trainer" 대신 "Boccia Hub"
  - 탭: `작업자 · 설정`. 트래커·버전 탭은 5·6단계에서 만든다.
  - 상세의 원격 탭(3단계)과 진행 탭의 "가져온 것"·가져오기(4단계)는 없다.
- 허브는 Python 표준 라이브러리만 쓴다. Python 3.10 이상.
- 화면(`/`, `/ui/…`)과 제어 API(`/api/…`)는 127.0.0.1·::1과 `--control-from` 주소에만 답한다. 앱 API는 1단계 그대로다.
- 점검 중 화면에 창을 띄우지 않는다.
  - 라벨러는 `--smoke-hidden`, `Boccia Hub.exe`는 `--check`(ShowWindow 없음)로 돌린다.
  - 화면 캡처와 DOM 확인은 headless Edge로 한다.
  - 보이는 창에 입력을 보내지 않는다.
- 점검은 사용자의 실제 허브를 건드리지 않는다. 포트 8790, `D:\Boccia Hub`, 실제 시작프로그램 폴더 대신 빈 포트, 스크래치 저장소, `BOCCIA_STARTUP_DIR`을 쓴다.
- 사용자 작업 프로젝트(`bin/release/labeler/data/labels/project.json`)는 열지 않는다. QA는 스크래치 `step3/yolo-project`의 복사본으로 한다.
- 빌드는 `build-labeler`에서 필요한 대상만 한다. `--clean-first`·Rebuild는 쓰지 않는다.
- 커밋은 작업마다 하고 푸시는 하지 않는다.
  - 미커밋 백로그가 섞인 파일(`CMakeLists.txt` 등)은 인덱스 판본에 이 작업의 변경만 더해 스테이징한다(스크래치 `stage.py`). 인덱스 판본에 고칠 자리가 없으면 파일을 통째로 커밋하고, 원장에 `Ruling:`을 남긴다(1단계 리뷰 I1과 같은 사정).
  - 줄바꿈·BOM은 파일에 있던 그대로 둔다.
- WebView2 SDK
  - 출처: nuget.org `Microsoft.Web.WebView2` 1.0.4258.31
  - 파일: `microsoft.web.webview2.1.0.4258.31.nupkg`, 9,801,922 바이트
  - SHA-512(base64): `HlGVwvyP/IWiXAU8K57Vmg59khY3DWMOxMnH4AHusFRy0/vDAjKItXUIVNXiXXaq4CLfSZyNUic1wOIlnPLsPQ==`
  - `third_party/webview2/`에 풀고 git에서는 뺀다. 정적 로더라 함께 배포할 DLL이 없다.
- 활동 문장은 아래 표의 문구를 그대로 쓴다. 허브는 앞말(첫 ` · ` 앞, 또는 문장 전체)로 종류를 가른다. 규칙은 `text == 앞말` 또는 `text.startswith(앞말 + " ")`이다.

| 앞말 | 종류 | 예 |
| --- | --- | --- |
| 장면 확인 · 확인(옛 문구) · 보류 · 공 추가 · 공 수정 · 공 삭제 · 되돌리기 · 이전 적용 · 전파 → | `action` | `장면 확인 · 촬영 03 · 프레임 0142 · 공 6개`, `공 수정 · 모노 A · 공 2 · 클릭 분할`, `되돌리기 · 모노 A`, `이전 적용 켬` |
| 오류 · 자동 초안 오류 · 초안 실패 · 임포트 복사 실패 · GPU 못 씀 | `error` | `오류 · 내보내기 · 디스크 공간 부족` (작업 실패는 모두 `오류 · <작업 이름> · <이유>`) |
| 알림 | `notice`(활동 목록에서 숨김) | `알림 · 확인했습니다` |
| 그 밖 | `job` | `초안 다시 만들기 · 498장`, `초안 다시 만들기 중단`, `빠진 공 찾기 · 시험 · 공 12개 · 30장`, `빠진 공 찾기 · 공 12개 · 30장`, `원형 검출 · 찾기 · 공 9개 · 30장`, `원형 검출 · 더하기 · 공 9개 · 30장`, `가려진 공 표시 · 시험 · 공 5개 · 4장`, `가려진 공 표시 · 공 5개 · 4장`, `둘레 다듬기 · 공 40개`, `색 채우기 · 공 40개`, `내보내기 · YOLO · COCO 검출 · 1,180장 · C:\…\exports\…`, `종료`, `시작 · …`, `프로젝트 열기 · …` |

## Review Focus

1. **허브가 이미 켜진 채 `Boccia Hub.exe`를 누르거나, exe를 두 번 누름:** 허브를 하나 더 켜지 않고 열린 창만 앞으로 가져온다. Task 5 `ensure_hub_twice_starts_one`, 그리고 단일 실행 뮤텍스.
2. **Python이 없거나 Microsoft Store 별칭(`…\WindowsApps\python*.exe`)만 있는 PC:** 멈추지 않고 `Python 3.10 이상 필요`를 띄운다. Task 5 `python_on_path_skips_store_alias`.
3. **8790 포트를 허브가 아닌 프로그램이 씀:** 거기에 붙지 않고 `8790 포트를 다른 프로그램이 씀`을 띄운다. Task 5 `ping_tells_other_from_hub`.
4. **허브가 꺼져 있던 날의 확인:** 날짜별 확인이 라벨 파일 기준으로 맞아야 한다.
   - 기록이 없는 옛 확인은 0으로 센다.
   - 다른 작업자의 확인은 빼고, 다시 확인한 프레임은 한 번만 센다.
   - 테스트: Task 3 `hubProgressTests`.
5. **활동이 수만 줄:** 화면이 2초마다 불러도 허브가 느려지지 않는다. Task 1 `test_events_fast_with_many`.

---

### Task 1: 허브: 화면 제공, 활동 분류·캐시, 끄기

**Files:**
- Modify: `hub/hub.py`, `hub/cli.py`
- Test: `hub/tests/test_hub.py`, `hub/tests/test_cli.py`

**Interfaces:**
- Consumes: 1단계의 `Hub`, `Handler`, `Hub.control_allowed`.
- Produces:
  - `classify(text: str) -> str`: `"action" | "error" | "notice" | "job"`. 규칙은 Global Constraints 표를 따른다.
  - `Hub(storage, say=None, control_from=(), ui=None)`: `ui`의 기본값은 `Path(__file__).with_name("ui")`. `Hub.stopped`는 `threading.Event`로, 끄면 켜진다. `shutdown()`은 두 번 불려도 된다.
  - `GET /api/workers/<id>/events` 항목에 `"kind"`가 붙는다.
  - `GET /api/workers` 항목에 두 필드가 붙는다.
    - `"registered"`: 마지막 등록 시각
    - `"last_action"`: `{"time","text","kind"}` 또는 `null`. `notice`가 아닌 마지막 활동이다.
  - `GET /api/status`에 `"address"`가 붙는다. 서버를 켤 때 `tailscale status --json`(5초 제한)의 `Self.DNSName`에서 끝 점을 빼고 `:<port>`를 붙인 값이다. 모르면 `""`.
  - `POST /api/shutdown` → `200 {"ok": true}`를 보낸 뒤 허브가 꺼진다. `main()`은 `stopped`를 기다리다 0으로 끝난다.
  - 정적 파일. 둘 다 `control_allowed`가 아니면 403, 없으면 404를 준다. 이름은 `[A-Za-z0-9._-]+`만 받는다.
    - `GET /`는 `<ui>/index.html`을 준다.
    - `GET /ui/<name>`은 `<ui>/<name>`을 준다.
    - `GET /ui/fonts/<name>`은 `<ui>/fonts/<name>`을 주고, 없으면 `<hub.py의 상위>/../assets/fonts/<name>`을 준다.
    - Content-Type: `.html` `text/html; charset=utf-8`, `.js` `text/javascript; charset=utf-8`, `.css` `text/css; charset=utf-8`, `.ttf` `font/ttf`, `.svg` `image/svg+xml`, `.png` `image/png`. 모두 `Cache-Control: no-store`.
  - 활동 캐시
    - 작업자별 최근 20,000줄을 메모리에 둔다. 처음 부를 때 파일에서 읽고, 그 뒤엔 `add_events`가 덧붙인다.
    - `events()`와 중복 거름은 캐시만 본다(리뷰 M14).
  - `Handler.timeout = 120`(리뷰 M16).
  - 허브의 한 줄 기록(`say`)은 `<storage>/hub.log`에도 붙여 쓴다. `sys.stdout`이 없으면(pythonw) 파일에만 쓴다.
  - CLI `stop`은 `POST /api/shutdown`을 보내고 `허브 끔`을 찍는다.

- [ ] **Step 1: 테스트 쓰기** (`hub/tests/test_hub.py`, 1단계 `HubTest` 안. `from hub.hub import Hub, classify`)

```python
def test_classify_kinds(self):
    cases = {"장면 확인 · 촬영 03 · 프레임 0142 · 공 6개": "action", "확인 · 촬영 01 · 프레임 0003": "action", "이전 적용 켬": "action",
             "전파 → · 반복 4장 · 초안 7장": "action", "공 수정 · 모노 A · 공 2 · 클릭 분할": "action", "되돌리기 · 모노 A": "action",
             "오류 · 내보내기 · 디스크 공간 부족": "error", "초안 실패 · 촬영 04 · x": "error", "임포트 복사 실패 · x": "error",
             "알림 · 원본과 저장 라벨 표시 중": "notice", "초안 다시 만들기 · 498장": "job", "시작 · D:\\x · CUDA 없음": "job", "확인한 라벨": "job"}
    self.assertEqual({text: classify(text) for text in cases}, cases)

def test_events_carry_kind_and_worker_list_has_last_action(self):
    token = self.register()
    self.call("POST", "/events", {"events": [{"time": "2026-10-20 09:00:00.000", "text": "장면 확인 · 촬영 01 · 프레임 0001 · 공 2개"},
                                             {"time": "2026-10-20 09:00:01.000", "text": "알림 · 확인했습니다"}]}, token)
    self.assertEqual([e["kind"] for e in self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]], ["action", "notice"])
    worker = self.call("GET", "/api/workers")[1][0]
    self.assertEqual(worker["last_action"]["text"], "장면 확인 · 촬영 01 · 프레임 0001 · 공 2개")
    self.assertTrue(worker["registered"])

def test_events_fast_with_many(self):
    token = self.register()
    for chunk in range(20):
        self.call("POST", "/events", {"events": [{"time": f"2026-10-20 09:{chunk:02d}:{i // 1000:02d}.{i % 1000:03d}", "text": f"줄 {chunk}-{i}"} for i in range(1000)]}, token)
    last = self.call("GET", "/api/workers/홍길동@joonyeon/events?limit=1")[1][0]["received"]
    started = time.monotonic()
    for _ in range(20):
        self.assertEqual(self.call("GET", f"/api/workers/홍길동@joonyeon/events?since={last}&limit=0")[1], [])
    self.assertLess(time.monotonic() - started, 1.0)

def test_cache_reloads_after_restart(self):
    # 줄 둘 → 허브 다시 켬 → 목록에 kind와 함께 둘, 같은 줄은 다시 거부(stored 0)
def test_static_ui_only_for_allowed(self):
    # Hub(ui=<temp>)에 index.html("<title>Boccia Hub</title>")과 app.js("let x=1;")를 두고 띄움:
    # GET / → 200, Content-Type text/html, 본문에 "Boccia Hub"; /ui/app.js → 200 text/javascript; /ui/none.js → 404
    # /ui/..%5chub.py → 400 또는 404; hub.control_allowed = lambda a: False 뒤 GET / → 403
def test_fonts_fall_back_to_assets(self):
    # GET /ui/fonts/NotoSansKR.ttf → 200, font/ttf, 길이가 assets/fonts/NotoSansKR.ttf와 같음
def test_shutdown_stops_the_hub(self):
    # POST /api/shutdown(JSON {}) → 200 {"ok": true}; 3초 안에 hub.stopped.is_set(); 그 뒤 /ping은 연결 실패
def test_hub_log_written(self):
    # register 뒤 <storage>/hub.log에 "홍길동@joonyeon 접속"
def test_status_has_address(self):
    # /api/status에 "address" 키(문자열)
```

`hub/tests/test_cli.py`에는 `test_stop`을 넣는다. `run_cli("stop")`의 결과가 `(0, "허브 끔\n")`이고, 3초 안에 `hub.stopped.is_set()`이어야 한다.

- [ ] **Step 2: 실패 확인.** `python -m unittest hub.tests.test_hub hub.tests.test_cli` → `ImportError: cannot import name 'classify'`.
- [ ] **Step 3: 구현.** Interfaces대로 한다. 캐시는 `dict[id, list[record]]`로, 길이가 20,000을 넘으면 앞을 자른다. `since`가 있으면 `received` 순서이므로 `bisect`로 자리를 찾는다. 정적 파일의 이름 검사는 URL 디코드 뒤에 한다.
- [ ] **Step 4: 통과 확인.** 같은 명령 → 전부 OK.
- [ ] **Step 5: 커밋.** `git add hub/hub.py hub/cli.py hub/tests/test_hub.py hub/tests/test_cli.py && git commit -m "Hub serves its screen, classifies and caches activity, stops on request"`

### Task 2: 라벨 파일에 확인 시각·작업자 기록

**Files:**
- Modify: `src/labeler_native.cpp`(`review` 동작), `src/labeler_store.cpp`(`Project::summary`), `src/native_labeler_app.cpp`(`review()`의 payload)
- Test: `tests/labeler_engine_tests.cpp`

**Interfaces:**
- Produces:
  - 엔진 동작 `review`가 선택 키 `"worker"`(문자열)를 받는다.
  - 이 동작이 쓰는 프레임마다 두 값을 적는다. `reviewed`와 `hold` 둘 다다.
    - `"reviewed_at"`: 로컬 시각 `"YYYY-MM-DD HH:MM:SS"`
    - `"reviewed_by"`: 작업자 이름. 이름이 비면 키를 넣지 않는다.
  - `Project::summary()["frames"][key]`에도 저장돼 있으면 `reviewed_at`·`reviewed_by`가 실린다.
  - 앱의 `review()` payload에 `{"worker", settings_.worker}`가 붙는다.

- [ ] **Step 1: 테스트.** 위치는 `tests/labeler_engine_tests.cpp`의 "edits and the confirmation must land in one write" 블록 안, `check(... "the light catalog leaves the batch counts out")` 다음이다. `#include <regex>`와 `#include <ctime>`를 더한다.

```cpp
{   // 2026-10-20 hub stage 2: a confirmation stores its local time and worker; the summary carries them
    Project stamped(catalog, fixture.temp.path / "stamp-project.json", fixture.temp.path / "stamp-exports");   // its own label file: scenes 0 and 1 are untouched in it
    const Json scene2 = sceneKeysOf(sceneIndex(catalog)[0]);
    const std::time_t before = std::time(nullptr);
    nativeAction(stamped, "review", {{"keys", scene2}, {"status", "reviewed"}, {"worker", "홍길동"}, {"expected_revision", stamped.state["revision"]}});
    const Json& frame = stamped.state["frames"][scene2[0].get<std::string>()];
    std::tm parsed{}; std::istringstream(frame.value("reviewed_at", "")) >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S"); parsed.tm_isdst = -1;
    check(std::regex_match(frame.value("reviewed_at", ""), std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")) && std::abs(std::mktime(&parsed) - before) <= 5,
          "a confirmation must store the local time");
    check(frame.value("reviewed_by", "") == "홍길동" && stamped.summary()["frames"][scene2[0].get<std::string>()].value("reviewed_by", "") == "홍길동",
          "the worker must be stored and summarized");
    const Json scene3 = sceneKeysOf(sceneIndex(catalog)[1]);
    nativeAction(stamped, "review", {{"keys", scene3}, {"status", "hold"}, {"expected_revision", stamped.state["revision"]}});
    check(stamped.state["frames"][scene3[0].get<std::string>()].contains("reviewed_at") && !stamped.state["frames"][scene3[0].get<std::string>()].contains("reviewed_by"),
          "a hold is stamped too, and no worker means no reviewed_by");
}
```

- [ ] **Step 2: 실패 확인.** `cmake --build build-labeler --config Release --target labeler_engine_tests`, 이어서 `build-labeler\Release\labeler_engine_tests.exe` → `a confirmation must store the local time`.
- [ ] **Step 3: 구현.** 엔진 `review`의 `mutate` 안에서 `annotation["reviewed"]=…` 옆에 두 키를 적는다. 시각은 `localtime_s` + `strftime("%Y-%m-%d %H:%M:%S")`로 만든다. `Project::summary()`는 저장된 프레임에 있을 때만 두 키를 복사한다. 앱의 `review()`에는 `{"worker",settings_.worker}`를 더한다.
- [ ] **Step 4: 통과 확인.** `labeler_engine_tests.exe` → 모든 묶음 `PASS:`, `ctest -C Release -R "labeler_engine|native_labeler_data_contracts"` 통과.
- [ ] **Step 5: 커밋.** `git add src/labeler_native.cpp src/labeler_store.cpp src/native_labeler_app.cpp tests/labeler_engine_tests.cpp && git commit -m "Store when and by whom each frame was confirmed"`. 엔진 파일 셋도 미커밋 백로그가 있으면 인덱스 판본 + 이 변경만 스테이징한다(`git diff --cached`로 확인).

### Task 3: 라벨러: 진행 숫자, GPU 이름, 활동 문장

**Files:**
- Modify: `src/native_labeler_model.hpp/.cpp`, `src/hub_client.hpp/.cpp`(+`dxgi` 링크, `CMakeLists.txt`), `src/native_labeler_hub.cpp`, `src/native_labeler_app.cpp`
- Test: `tests/native_labeler_tests.cpp`, `tests/hub_client_tests.cpp`

**Interfaces:**
- Consumes: Task 2의 `reviewed_at`·`reviewed_by`(catalog `summary.frames`).
- Produces:

```cpp
// native_labeler_model.hpp
Json hubProgress(const Json& catalog, const std::string& worker, std::time_t now);
// {"target","reviewed","hold","propagated","recordings":[{"name","reviewed","target"}],
//  "days":[{"date":"MM-DD","count"}] (7개, 오래된 것부터, 마지막이 오늘),"today","last10"}
Json hubStatusJson(const std::string& page, const Json& scene, const Json& job, bool applyPrevious, bool gpu,
                   const std::string& error, const Json& progress = nullptr);   // progress가 객체면 status["progress"]
// hub_client.hpp
std::string gpuName();   // DXGI: 첫 NVIDIA 어댑터 이름, 없으면 첫 하드웨어 어댑터 이름, 없으면 ""
```

- `hubProgress` 규칙
  - `target`·`reviewed`: `catalog["target_images"]`, `catalog["reviewed_target_images"]`(위 막대의 검수 숫자와 같음)
  - `recordings`: 장면 순서대로 `recording_number`마다 하나
    - 이름은 `촬영 NN`. 장면 `source_type`이 `"images"`면 `이미지 자료 NN`.
    - `target`은 `selected`(기본 true)인 멤버 수, `reviewed`는 그중 `reviewed`인 멤버 수.
  - `hold`: `summary.frames` 중 `review_status=="hold"`인 수
  - `propagated`: `summary.frames` 중 `propagated`인 수
  - `days`·`today`·`last10`: `summary.frames` 중 `review_status=="reviewed"`이고 `reviewed_by==worker`인 프레임만 센다. `reviewed_at`은 로컬 시각으로 읽는다.
    - `days`: 오늘까지 로컬 날짜 7일
    - `today`: 마지막 날의 수
    - `last10`: `now-600 ≤ t ≤ now`
- 앱
  - `hubTick()`에서 진행을 다시 셈한다. `catalog_["summary"]["revision"]`이 바뀌었거나 10초가 지났을 때다.
  - 셈한 값은 `hubStatusJson(..., progress)`로 보낸다.
  - 등록할 때 `device`에 `"name": hub::gpuName()`이 붙는다.
- 활동 문장(Global Constraints 표)
  - `확인 · ` → `장면 확인 · `
  - `공 수정`에도 방법(`클릭 분할|브러시|폴리곤|박스`)을 붙인다.
  - `undo()`에서 `되돌리기 · <카메라>`를 적는다.
  - `~Application` 맨 앞에서 `종료`를 적는다.
  - `pollDone_`의 작업 끝마다 표의 문장을 적는다: 초안 다시 만들기, 빠진 공 찾기(시험/적용, `method=="circles"`면 원형 검출 찾기/더하기), 가려진 공 표시, 둘레 다듬기, 색 채우기, 내보내기.
    - 내보내기의 형식은 화면에서 고른 `YOLO · COCO 검출 · COCO 마스크 · 크롭`, 장수는 `exportStatus_["total"]`, 폴더는 `exportStatus_["path"]`다.
    - 실패하면 `오류 · <작업 이름> · <error>`를 적는다.

- [ ] **Step 1: 테스트** (`tests/native_labeler_tests.cpp`에 `hubProgressTests()`를 만들고 `main`에서 `settingsTests()` 뒤에 부른다)

```cpp
void hubProgressTests() {
    std::tm noon{}; noon.tm_year = 2026 - 1900; noon.tm_mon = 9; noon.tm_mday = 20; noon.tm_hour = 15; noon.tm_isdst = -1;
    const std::time_t now = std::mktime(&noon);   // 2026-10-20 15:00:00 local
    auto frame = [](const char* status, const char* by, const char* at, bool propagated = false) {
        Json made{{"review_status", status}, {"reviewed", std::string(status) == "reviewed"}, {"propagated", propagated}};
        if (*by) made["reviewed_by"] = by; if (*at) made["reviewed_at"] = at; return made; };
    const Json catalog{{"target_images", 7}, {"reviewed_target_images", 4},
        {"scenes", Json::array({
            {{"recording_number", 1}, {"members", Json::array({{{"key", "a"}, {"selected", true}, {"reviewed", true}}, {{"key", "b"}, {"selected", true}, {"reviewed", true}}})}},
            {{"recording_number", 2}, {"members", Json::array({{{"key", "c"}, {"selected", true}, {"reviewed", true}}, {{"key", "x"}, {"selected", false}, {"reviewed", false}}})}},
            {{"recording_number", 3}, {"source_type", "images"}, {"members", Json::array({{{"key", "p"}, {"reviewed", false}}})}}})},
        {"summary", {{"frames", {
            {"a", frame("reviewed", "홍길동", "2026-10-20 14:55:00")}, {"b", frame("reviewed", "홍길동", "2026-10-20 09:00:00")},
            {"c", frame("reviewed", "홍길동", "2026-10-17 10:00:00")}, {"d", frame("reviewed", "김민수", "2026-10-20 14:58:00")},
            {"e", frame("reviewed", "", "")}, {"f", frame("hold", "홍길동", "2026-10-20 14:59:00")},
            {"g", frame("draft", "", "", true)}, {"h", frame("reviewed", "홍길동", "2026-10-12 10:00:00")}}}}}};
    const Json progress = hubProgress(catalog, "홍길동", now);
    check(progress["target"] == 7 && progress["reviewed"] == 4 && progress["hold"] == 1 && progress["propagated"] == 1, "progress totals");
    check(progress["recordings"] == Json::array({{{"name", "촬영 01"}, {"reviewed", 2}, {"target", 2}}, {{"name", "촬영 02"}, {"reviewed", 1}, {"target", 1}},
                                                 {{"name", "이미지 자료 03"}, {"reviewed", 0}, {"target", 1}}}), "progress per recording");
    Json dates = Json::array(), counts = Json::array();
    for (const auto& day : progress["days"]) { dates.push_back(day["date"]); counts.push_back(day["count"]); }
    check(dates == Json::array({"10-14", "10-15", "10-16", "10-17", "10-18", "10-19", "10-20"}) && counts == Json::array({0, 0, 0, 1, 0, 0, 2}),
          "seven days of this worker's confirmations, older and other workers' left out");
    check(progress["today"] == 2 && progress["last10"] == 1, "today and the last ten minutes");
    check(hubStatusJson("review", nullptr, nullptr, false, true, "", progress)["progress"]["today"] == 2, "the status carries the progress");
}
```

`tests/hub_client_tests.cpp`에는 `gpuName()`이 던지지 않는지 확인하고, 그 이름을 `GPU <이름>`으로 찍는 한 줄을 더한다.

- [ ] **Step 2: 실패 확인.** `native_labeler_tests` 빌드 실패(`hubProgress` 없음).
- [ ] **Step 3: 구현.** Interfaces대로 한다. 날짜 계산은 `now`의 로컬 날짜에서 하루씩 빼서 만든다(`mktime` 정규화). `reviewed_at`은 `std::get_time`으로 읽는다.
- [ ] **Step 4: 확인**
  - `cmake --build build-labeler --config Release --target boccia_labeler native_labeler_tests hub_client_tests`
  - `native_labeler_tests.exe` → `PASS:`, `hub_client_tests.exe` → `PASS:`
  - 숨긴 실행 셋, 모두 QA 복사본(`robocopy <step3/yolo-project> <scratch>\t3-qa /E /R:0`)에서 `--log`를 붙여 돌린다. 로그는 `build-labeler\Release\logs\labeler-<날짜>.log`에서 본다.
    1. `--workspace <복사본> --smoke-hidden --view overview --prompt --log` → `장면 확인 · 촬영 01 · 프레임 0000 · 공`
    2. `--workspace <복사본> --smoke-hidden --batch-drawer redraft:0 --log --timeout 300` → `초안 다시 만들기 · `
    3. `--workspace <복사본> --smoke-hidden --view exports --export-state run --log --timeout 300` → `내보내기 · `
  - 셋 모두 마지막 줄이 `종료`다.
- [ ] **Step 5: 커밋.** `git add src/native_labeler_model.hpp src/native_labeler_model.cpp src/hub_client.hpp src/hub_client.cpp src/native_labeler_hub.cpp src/native_labeler_app.cpp tests/native_labeler_tests.cpp tests/hub_client_tests.cpp`. 이어서 `CMakeLists.txt`는 `stage.py`로 `dxgi` 줄만 스테이징한다. 그 뒤 `git commit -m "Labeler reports its progress, GPU name and the rest of its activity to the hub"`.

### Task 4: 화면 (`hub/ui/`)

**Files:**
- Create: `hub/ui/index.html`, `hub/ui/app.js`, `hub/ui/style.css`, `hub/tests/test_ui.py`

**Interfaces:**
- Consumes
  - Task 1 API: `/api/status`, `/api/workers`(`status.progress`, `status.text`, `status.job`, `status.apply_previous`, `status.device`, `device.name`, `summary.project`, `version`, `registered`, `last_action`, `connected`, `last_seen`), `/api/workers/<id>/events`(`kind`), `/api/invites`, `/api/shutdown`
  - Task 3의 `progress` 모양
- Produces
  - 경로: `#/workers`(기본), `#/workers/<id>/activity`, `#/workers/<id>/progress`, `#/settings`
  - `document.title = "Boccia Hub"`
  - WebView2 메시지(Task 5)
    - 페이지가 보내는 것: 설정 화면을 열 때 `{"type":"autostart?"}`, 토글할 때 `{"type":"autostart","on":bool}`, 끈 뒤 `{"type":"close"}`
    - 페이지가 받는 것: `{"type":"autostart","on":bool}`
    - `window.chrome?.webview`가 없으면(일반 브라우저) 자동 시작 줄을 숨긴다.
- 화면(시안 그대로, Global Constraints의 차이만)
  - **위 막대**
    - 로고 + `Boccia Hub`, 탭 `작업자 · 설정`
    - 오른쪽: 점 + `허브` + `address`(비면 `location.host`)
    - 허브가 답하지 않으면 회색 점 + `꺼짐`
  - **작업자 목록**
    - 제목 `작업자`, 오른쪽에 `초대` 버튼
    - 네 칸
      - 붙은 라벨러 `N대`, 아래 `떨어짐 M`
      - 오늘 확인 `Σtoday 장`
      - 지금 속도 `Σlast10 장 / 10분`
      - 전체 진행 `⌊Σreviewed/Σtarget·100⌋ %`, 아래 `a / b장`
    - 표의 열
      - 작업자: 점, 이름, `PC · GPU|CPU · MM-DD HH:MM`(버전 앞부분)
      - 프로젝트
      - 진행: 막대와 `reviewed / target`, 오른쪽에 `last10장 / 10분`
      - 지금: `status.text`에서 화면 이름 뒤 부분. 아래 줄에 `job.text`, 또는 `← 이전 적용 켬`
      - 최근: `last_action`의 앞말과 `N초 전·N분 전`. 떨어졌으면 `떨어짐 HH:MM`
      - 끝에 `›`
    - 떨어진 줄은 회색이고, 줄을 누르면 상세 활동으로 간다.
  - **상세**
    - 위: `← 작업자 › 이름 ●`
    - 요약 카드: PC · 라벨러(버전 `MM-DD HH:MM`) · 실행 장치(`GPU · <name>` 또는 `CPU`) · 프로젝트 · 진행 · 지금 · 붙음(`HH:MM부터`)
    - 탭: `활동 · 진행`
  - **활동 탭**
    - 거름: `전체 · 작업 · 동작 · 오류`. `notice`는 늘 뺀다. 오른쪽에 `오늘 N장 · 오류 M`.
    - 한 줄: 시각 `HH:MM:SS`(mono) · 앞말별 아이콘(시안의 11개 모양) · 문장. 첫 ` · ` 뒤 꼬리는 시안처럼 흐리게, `error` 줄은 빨강.
    - 최신이 위. 처음에 `limit=500`으로 받고, 그 뒤엔 `since`로 이어 붙인다.
  - **진행 탭**
    - 촬영별 확인: 이름 · `reviewed / target` · 막대(다 끝나면 초록)
    - 날짜별 확인: 7일 막대, 아래 `MM-DD 끝 두 자리`와 수, 오른쪽 위 `최근 7일 · Σ장`
    - 요약: 확인 `r / t · %`, 보류, 전파됨, 속도 `last10장 / 10분`
  - **설정**
    - 허브 주소 · 저장소 · 켠 시각
    - `로그인할 때 켜기`(창 안에서만, 토글)
    - `허브 끄기`(빨간 테두리 버튼). 누르면 `/api/shutdown`을 부르고, 창 안이면 `close`를 보낸다.
  - **초대 창**: `이름` 칸, `만들기` → 코드(mono)와 `복사`
  - 2초마다 새로 고친다. 403이면 화면 가운데에 `허용되지 않은 기기` 한 줄만 보인다.

- [ ] **Step 1: 테스트** (`hub/tests/test_ui.py`)
  - 실제 `hub/ui`로 `Hub`를 띄우고 HTTP로 다음을 채운다.
    - `홍길동@joonyeon` 등록: `device {"gpu": true, "name": "NVIDIA GeForce RTX 4090 Laptop GPU"}`, `summary {"project": "경기장 테스트"}`
    - 상태: `{"page":"review","text":"검수 · 촬영 03 · 프레임 0142","progress":{"target":3869,"reviewed":1214,"hold":12,"propagated":1302,"today":412,"last10":12,"recordings":[{"name":"촬영 01","reviewed":612,"target":612}],"days":[{"date":"10-20","count":412}]}}`
    - 활동 셋: `장면 확인 · 촬영 03 · 프레임 0142 · 공 6개`, `오류 · 내보내기 · 디스크 공간 부족`, `알림 · 확인했습니다`
  - DOM은 headless Edge로 받는다. Edge가 없으면 `skipTest`.

```python
def dump(self, route):
    return subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--no-first-run", f"--user-data-dir={self.profile}",
                           "--virtual-time-budget=8000", "--dump-dom", f"http://127.0.0.1:{self.port}/{route}"],
                          capture_output=True, timeout=90).stdout.decode("utf-8")
def test_list_shows_workers_from_the_hub(self):
    dom = self.dump("#/workers")
    for text in ("Boccia Hub", "홍길동", "joonyeon", "경기장 테스트", "1,214 / 3,869", "412", "12장 / 10분", "촬영 03 · 프레임 0142"):
        self.assertIn(text, dom)
def test_activity_lists_events_and_hides_notices(self):
    dom = self.dump("#/workers/%ED%99%8D%EA%B8%B8%EB%8F%99@joonyeon/activity")
    self.assertIn("장면 확인", dom); self.assertIn("디스크 공간 부족", dom); self.assertNotIn("확인했습니다", dom)
def test_progress_shows_recordings_and_days(self):
    dom = self.dump("#/workers/%ED%99%8D%EA%B8%B8%EB%8F%99@joonyeon/progress")
    for text in ("촬영 01", "612 / 612", "412", "1,302"): self.assertIn(text, dom)
def test_settings_has_shutdown_and_storage(self):
    dom = self.dump("#/settings"); self.assertIn("허브 끄기", dom); self.assertIn(str(self.temp), dom)
```

`EDGE`는 `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`이고, 없으면 `C:\Program Files\…`를 본다.

- [ ] **Step 2: 실패 확인.** `python -m unittest hub.tests.test_ui -v` → GET `/`가 404라 `AssertionError`.
- [ ] **Step 3: 구현.** 시안의 CSS 토큰과 마크업을 옮긴다. `#app`의 고정 1440×850 대신 창 전체를 쓰고, 최소 폭은 1100px로 한다. 글꼴은 `url("fonts/NotoSansKR.ttf")`.
- [ ] **Step 4: 통과 확인.** `python -m unittest hub.tests.test_hub hub.tests.test_cli hub.tests.test_ui` → 전부 OK. headless Edge `--screenshot --window-size=1440,900`으로 세 경로를 찍어 시안 캡처와 나란히 본다(스크래치).
- [ ] **Step 5: 커밋.** `git add hub/ui hub/tests/test_ui.py && git commit -m "Hub screen: workers list, activity and progress tabs, settings, invite"`

### Task 5: `Boccia Hub.exe`

**Files:**
- Create: `tools/fetch-webview2.ps1`, `tools/make-ico.py`, `assets/icons/boccia-hub.svg`, `assets/icons/boccia-hub.png`, `assets/icons/boccia-hub.ico`, `assets/icons/hub_icon.rc`, `src/hub_launch.hpp`, `src/hub_launch.cpp`, `src/hub_app_main.cpp`, `tests/hub_launch_tests.cpp`
- Modify: `CMakeLists.txt`, `.gitignore`(`/third_party/`)

**Interfaces:**

```cpp
namespace boccia::hubapp {
namespace fs = std::filesystem;
fs::path pythonOnPath(const std::wstring& path);   // PATH 문자열에서 pythonw.exe; "\WindowsApps\"가 든 폴더는 건너뜀; 없으면 빈 경로
fs::path findPython(const fs::path& programFolder);   // <program>\python\pythonw.exe → PEP 514(HKCU, HKLM Software\Python\PythonCore\3.x ≥ 3.10, 새 버전부터,
                                                      //  WindowedExecutablePath 또는 InstallPath\pythonw.exe) → pythonOnPath(%PATH%); 없으면 빈 경로
fs::path hubScript(const fs::path& programFolder, const fs::path& repository);   // <program>\hub\hub.py → <repository>\hub\hub.py → 빈 경로
enum class Ping { Hub, Other, None };
Ping ping(int port);   // 127.0.0.1:<port>/ping, 2초: {"hub":"boccia"}면 Hub, 다른 답이면 Other, 답이 없으면 None
// 허브가 답하면 "" (필요하면 켜고 10초까지 기다림); 아니면 화면에 띄울 문구:
// "Python 3.10 이상 필요", "<port> 포트를 다른 프로그램이 씀", "허브를 켜지 못함"
std::string ensureHub(const fs::path& python, const fs::path& script, int port, const fs::path& storage, bool* started = nullptr);
fs::path startupFolder();   // BOCCIA_STARTUP_DIR, 없으면 FOLDERID_Startup
bool autostartOn();         // <startup>\Boccia Hub.lnk가 있음
void setAutostart(bool on, const fs::path& exe);   // 바로가기: 대상 exe, 인수 "--background", 작업 폴더 = exe 폴더
}
```

- 허브 켜기
  - 명령: `"<python>" "<script>" serve --port <port> --storage "<storage>" --bind auto`
  - `CreateProcessW` 플래그: `CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB`. 거절되면 breakaway 없이 다시 시도한다.
  - 표준 입출력은 NUL로 보낸다. 허브가 자기 `hub.log`를 쓴다.
- exe(`src/hub_app_main.cpp`, 출력 이름 `Boccia Hub`)
  - 옵션: `--port <n>`(기본 8790), `--storage <dir>`(기본 `D:\Boccia Hub`)
  - 인수 없음 → 창
    - 단일 실행: 뮤텍스 `Local\BocciaHubWindow`. 이미 있으면 그 창(클래스 `BocciaHubWindow`)을 앞으로 가져오고 끝낸다.
    - `ensureHub`가 실패하면 그 문구를 `MessageBoxW`(제목 `Boccia Hub`)로 띄우고 끝낸다.
    - 창: 1440×900 가운데, 최소 1100×700, 제목 `Boccia Hub`, 아이콘 101
    - WebView2: 사용자 데이터 `%LOCALAPPDATA%\Boccia\hub-webview`, `Navigate(http://127.0.0.1:<port>/)`, 창 크기에 맞춤
    - 웹 메시지: `autostart?` → `{"type":"autostart","on":…}` 응답, `autostart` → `setAutostart`, `close` → 창 닫기
    - 환경 생성이 실패하면 기본 브라우저로 같은 주소를 열고 끝낸다.
    - 처음 실행할 때 `%LOCALAPPDATA%\Boccia\hub-settings.json`에 `"autostart"`가 없으면 `setAutostart(true)` 하고 `{"autostart":true}`를 적는다(설계 결정 3). 끄면 `false`로 남는다.
    - 창을 닫아도 허브는 끄지 않는다.
  - `--background` → `ensureHub`만 하고 끝낸다(로그인 시작용).
  - `--check` → 숨긴 창을 만든다(ShowWindow 없음, 시작프로그램을 건드리지 않음, 사용자 데이터는 임시 폴더).
    - `ensureHub` → WebView2 → `NavigationCompleted` 성공 → `ExecuteScript("document.title")` == `"\"Boccia Hub\""`
    - 통과하면 `PASS: hub ready, WebView2 loaded the screen`을 찍는다. 이 실행이 켠 허브면 `/api/shutdown` 뒤 0으로 끝낸다. 30초 넘으면 1로 끝낸다.
- 개발 빌드는 `BOCCIA_HUB_ROOT`(저장소 경로) 정의로 `hub\hub.py`를 찾는다.
- 아이콘
  - `boccia-hub.svg`는 다른 앱 아이콘과 같은 틀이다: 256 캔버스, 바깥 `rx 52 #172321`, 안쪽 `rx 24`.
  - 무늬: 가운데 큰 원 하나에서 선 셋이 작은 원 셋(빨강 `#e75161` · 파랑 `#5489ef` · 흰 `#f2f6f3`)으로 간다.
  - headless Edge로 16·24·32·48·64·128·256px PNG를 만들고, `tools/make-ico.py`가 PNG를 담은 ICO로 묶는다(stdlib `struct`).
- CMake
  - `hub_launch`(STATIC, `hub_client`·`shell32`·`ole32`·`advapi32` 링크)와 `hub_launch_tests` + `add_test(NAME hub_launch …)`
  - `third_party/webview2/include/WebView2.h`가 있을 때만 `boccia_hub`(WIN32)를 만든다: include, `lib/x64/WebView2LoaderStatic.lib`, `version user32 shell32 ole32 oleaut32 shlwapi` 링크, `/utf-8 /W4`
  - SDK가 없으면 `message(STATUS "WebView2 SDK: powershell -File tools/fetch-webview2.ps1")`

- [ ] **Step 1: SDK 받기**
  - `powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-webview2.ps1`
  - 스크립트가 하는 일: 고정 URL에서 nupkg를 받아 크기와 SHA-512를 확인하고, `build/native/include/*.h`와 `build/native/x64/WebView2LoaderStatic.lib`를 새 빈 폴더 `third_party/webview2/`에 푼다.
  - → `WebView2 SDK 1.0.4258.31 → third_party\webview2`. 해시가 다르면 아무것도 풀지 않고 exit 1.
- [ ] **Step 2: 테스트** (`tests/hub_launch_tests.cpp`. 형식은 `hub_client_tests`와 같다: `check`, `waitUntil`, 빈 포트, 스크래치)
  - `find_python_finds_this_pc`: 결과가 비지 않고, 파일 이름이 `pythonw.exe`이고, 경로에 `WindowsApps`가 없다.
  - `python_on_path_skips_store_alias`: `<t>\WindowsApps\pythonw.exe`와 `<t>\bin\pythonw.exe`(빈 파일)를 만든다.
    - `pythonOnPath(L"<t>\\WindowsApps")`는 빈 경로다.
    - `pythonOnPath(L"<t>\\WindowsApps;<t>\\bin")`는 `<t>\bin\pythonw.exe`다.
  - `hub_script_found`: `hubScript(<t>, BOCCIA_TEST_ROOT) == <repo>\hub\hub.py`
  - `ping_none_on_free_port`, `ping_tells_other_from_hub`:
    - 빈 포트에서는 `Ping::None`이다.
    - `python -m http.server <port> --bind 127.0.0.1`을 띄우면 `Ping::Other`, 그 서버를 끝낸다.
  - `ensure_hub_twice_starts_one`:
    - 첫 `ensureHub(findPython(…), hubScript(…), port, scratch, &started)`는 `""`이고 `started==true`, `ping==Hub`다.
    - 두 번째는 `""`이고 `started==false`다.
    - 끝에 `/api/shutdown`을 보낸다.
  - `autostart_toggle`:
    - `BOCCIA_STARTUP_DIR=<t>`로 둔다.
    - `setAutostart(true, exe)`를 하면 `<t>\Boccia Hub.lnk`가 생기고 `autostartOn()`이며, `IShellLinkW::GetArguments`가 `--background`다.
    - `setAutostart(false, exe)`를 하면 파일이 없다.
- [ ] **Step 3: 실패 확인.** `cmake --build build-labeler --config Release --target hub_launch_tests` → 빌드 실패(`hub_launch.hpp` 없음).
- [ ] **Step 4: 구현.** Interfaces대로 한다. 아이콘은 위 방법으로 만든다.
- [ ] **Step 5: 통과 확인**
  - `hub_launch_tests.exe` → `PASS: …`
  - `cmake --build build-labeler --config Release --target boccia_hub`
  - `"build-labeler\Release\Boccia Hub.exe" --check --port <빈 포트> --storage <스크래치>` → `PASS: hub ready, WebView2 loaded the screen`, exit 0. 스크래치 `hub.log`에 시작 줄이 있고, 실제 시작프로그램 폴더에 `Boccia Hub.lnk`가 생기지 않았다.
- [ ] **Step 6: 커밋.** `git add tools/fetch-webview2.ps1 tools/make-ico.py assets/icons/boccia-hub.* assets/icons/hub_icon.rc src/hub_launch.hpp src/hub_launch.cpp src/hub_app_main.cpp tests/hub_launch_tests.cpp .gitignore`. 이어서 `CMakeLists.txt`는 `stage.py`로 이 작업의 줄만 스테이징한다. 그 뒤 `git commit -m "Boccia Hub.exe: starts the hub and shows its screen in its own window"`.

### Task 6: 배포 폴더, 처음부터 끝까지 점검, 문서

**Files:**
- Create: `tools/collect_hub.ps1`, `tools/hub-screen-check.ps1`(UTF-8 BOM)
- Modify: `hub/README.md`, `NATIVE_LABELER.md`(허브 절), `design/hub/DESIGN.md`("화면" 절 경로 `hub/ui/`, 창 `Boccia Hub.exe`(WebView2), 확인 수 출처)

**Interfaces:**
- Consumes: Task 1~5 전부.
- Produces
  - `tools/collect_hub.ps1`이 `bin/release/hub/`를 채운다(커밋은 사용자가 요청할 때만).
    - `Boccia Hub.exe`
    - `hub/__init__.py`, `hub/hub.py`, `hub/cli.py`
    - `hub/ui/{index.html,app.js,style.css}`, `hub/ui/fonts/NotoSansKR.ttf`, `licenses/NotoSansKR-OFL.txt`
    - `boccia-hub.cmd`(`python "%~dp0hub\cli.py" %*`)
  - `tools/hub-screen-check.ps1 -Project <QA 프로젝트 폴더>`
    1. 빈 포트·스크래치 저장소로 허브를 켜고 `가`·`나`를 초대한다.
    2. 프로젝트를 둘 복사한다(`robocopy /E /R:0`).
    3. 숨긴 라벨러 둘을 띄운다: `--workspace <복사본> --smoke-hidden --view overview --prompt --hub <주소> --worker 가|나 --token <코드> --hub-wait 15 --timeout 120`
    4. 둘 다 아래가 될 때까지 기다린다.
       - `last_action.text`가 `장면 확인 · `로 시작한다.
       - `status.progress.today ≥ 1`이다.
    5. headless Edge `--screenshot --window-size=1440,900 --virtual-time-budget=8000`으로 `#/workers`, `#/workers/가@<PC>/activity`, `#/workers/가@<PC>/progress`를 찍어 `design/hub/shots/hub-{list,activity,progress}.png`로 둔다.
    6. `Boccia Hub.exe --check --port <같은 포트>`를 돌린다(이미 켜진 허브를 쓰므로 끄지 않음).
    7. 허브를 끄고 `PASS: two labelers confirmed, screens captured, Boccia Hub.exe loaded the screen`을 찍는다.

- [ ] **Step 1: 두 스크립트 작성.** 실패 조건마다 `throw`.
- [ ] **Step 2: 실행**
  - `powershell -NoProfile -ExecutionPolicy Bypass -File tools/hub-screen-check.ps1 -Project <스크래치>\step3\yolo-project` → `PASS: …`
  - 캡처 셋을 시안 셋과 나란히 사용자에게 보인다(SendUserFile render).
- [ ] **Step 3: 배포 확인**
  - `powershell -NoProfile -ExecutionPolicy Bypass -File tools/collect_hub.ps1`
  - `"bin\release\hub\Boccia Hub.exe" --check --port <빈 포트> --storage <스크래치>` → `PASS`. 배포 폴더의 `hub\hub.py`를 쓴다.
- [ ] **Step 4: 문서**
  - `hub/README.md`: 창(`Boccia Hub.exe`), 화면 경로, 설정·끄기·로그인 시작, `stop`
  - `NATIVE_LABELER.md`: 진행 숫자, `reviewed_at`·`reviewed_by`, 활동 문장 표
  - `DESIGN.md`: "화면" 절
- [ ] **Step 5: 커밋.** `git add tools/collect_hub.ps1 tools/hub-screen-check.ps1 hub/README.md NATIVE_LABELER.md design/hub/DESIGN.md design/hub/shots/hub-*.png && git commit -m "Hub stage 2: release folder, end-to-end screen check and docs"`

---

## 자기 검토

- **범위**: "구현 순서 2"의 항목(작업자 목록, 상세의 활동·진행 탭, 창)은 Task 1·4·5가 한다. 숫자 출처(사용자 결정 1)는 Task 2·3이 한다. 원격 탭(3), 가져오기(4), 버전(5), 트래커(6)는 뺐다. 1단계 리뷰 중 화면에 걸리는 M14·M16은 Task 1에 넣었다.
- **이름 일치**: `classify`, `last_action`, `registered`, `address`, `/api/shutdown`, `hubProgress`, `hubStatusJson(..., progress)`, `gpuName`, `reviewed_at`·`reviewed_by`, `ensureHub`, `setAutostart`는 Task 1~6에서 같은 이름이다. 화면의 메시지 `autostart?`·`autostart`·`close`도 Task 4·5에서 같다.
- **Review Focus**:
  - 1 → Task 5 `ensure_hub_twice_starts_one` + 뮤텍스
  - 2 → Task 5 `python_on_path_skips_store_alias`
  - 3 → Task 5 `ping_tells_other_from_hub`
  - 4 → Task 3 `hubProgressTests`
  - 5 → Task 1 `test_events_fast_with_many`
