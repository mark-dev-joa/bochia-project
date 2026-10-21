# 라벨러 배포: 런처 하나로 받기 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 작업자가 빈 폴더에 `BocciaLauncher.exe` 하나만 두고 켜면, 런처가 허브에서 라벨러 · 모델 · CUDA를 받아 깔고 띄운다. 다음부터는 바뀐 파일만 받는다.

**Architecture:** 이 PC의 배포 스크립트가 끝에 `hub/release.py`로 배포 폴더를 허브 저장소에 한 버전으로 등록한다. 파일 내용은 SHA-256 이름으로 한 벌만 둔다. 허브는 작업자 토큰으로 `/releases/labeler`(목록)와 `/releases/files/<sha256>`(파일)를 내준다. 런처는 `src/labeler_update.*`로 목록과 로컬 파일을 비교해 받고 검증하고, 백업을 둔 채 바꾼다. 새 라벨러가 10초 안에 죽으면 되돌린다.

**Tech Stack:** Python 3.12 표준 라이브러리(허브), C++20 + WinHTTP + BCrypt + Win32 기본 컨트롤 + nlohmann/json 헤더, CMake/MSVC(`build-labeler`), PowerShell 5.1.

**Spec:** `design/hub/LAUNCHER.md`(2026-10-21 승인). 바탕은 `design/hub/DESIGN.md`의 "새 버전 패치"와 "보안".

## Global Constraints

- 화면 문구는 라벨 · 숫자 · 버튼만 쓴다. 문구는 이것뿐이다: `이름`, `초대 코드`, `허브 주소`, `시작`, `받는 중 · 1.2 / 2.6 GB`(전체가 1 GiB 미만이면 둘 다 MB), `허브에 연결할 수 없습니다`, `다시`, `빈 폴더에 넣어 주세요`. 창 제목과 바로가기 이름은 `Boccia Labeler`.
- 허브 주소 기본값은 `LabelerSettings`의 `mark-hong-pc.tail68c059.ts.net:8790` 그대로. 허브 저장소 기본값은 `D:\Boccia Hub`.
- 런처는 아무것도 받기 전의 PC에서도 떠야 한다. 정적 CRT(`MultiThreaded`)로 빌드하고 시스템 DLL만 쓴다: winhttp, bcrypt, user32, gdi32, comctl32, shell32, ole32, uuid. OpenCV · ONNX Runtime · `labeler_engine` · `hub_client`는 링크하지 않는다.
- 허브는 Python 표준 라이브러리만 쓴다.
- 받은 파일은 SHA-256이 목록과 같을 때만 제자리에 둔다. 목록 경로는 `/`로 나눈 상대 경로이고, `..` · 절대 경로 · 드라이브 문자 · `\`가 든 경로는 목록째 거부한다.
- 런처가 옮기거나 지우는 것은 지난 목록이나 새 목록에 있는 경로뿐이다. `labeler/data/`와 `labeler/logs/`는 등록에서 빠지고 런처도 건드리지 않는다.
- 새 버전의 첫 실행이 10초 안에 0이 아닌 종료 코드로 끝나면 되돌리고 그 버전을 bad로 적는다. 10초 안이라도 0이면 작업자가 닫은 것이다.
- 점검은 사용자의 실제 것을 건드리지 않는다. 설정은 자식 프로세스의 `LOCALAPPDATA`를 스크래치로 바꾸고, 허브는 빈 포트와 스크래치 저장소, 바로가기는 `--shortcut-folder`를 쓴다. 실제 `D:\Boccia Hub`는 Task 6의 마지막 배포에서만 쓴다.
- 보이는 창에 입력을 보내지 않는다. 런처 창 확인은 화면 밖에 만든 자기 창을 `PrintWindow`로 찍는다(`--capture`).
- 사용자 작업 프로젝트(`bin/release/labeler/data/labels/project.json`)는 열지 않는다. 라벨러 QA는 스크래치 `step3/yolo-project`의 복사본으로 한다.
- 빌드는 `build-labeler`에서 필요한 대상만 한다. `--clean-first` · Rebuild는 쓰지 않는다.
- 커밋은 작업마다 하고 푸시는 하지 않는다. 미커밋 백로그가 섞인 `CMakeLists.txt`는 스크래치 `stage.py`로 인덱스 판본에 이 작업의 블록만 더해 스테이징한다. 줄바꿈과 BOM은 그대로 둔다.
- 판정: 런처 소스 `src/labeler_launcher_main.cpp`, 아이콘 `assets/icons/labeler_icon.rc` · `assets/icons/boccia-labeler.ico`, `CMakeLists.txt`의 런처 블록은 커밋된 적 없는 백로그다. 다른 백로그 파일에 기대지 않으므로 Task 5가 이 기능과 함께 커밋한다.
- 판정: 런처 자신도 다른 파일처럼 `update\backup\`으로 옮기고 새 것을 놓는다. 돌고 있는 exe도 같은 드라이브 안에서는 옮길 수 있다. 설계의 `.old` 이름은 쓰지 않고, Task 6에서 `LAUNCHER.md`를 고친다.

## Review Focus

1. 수백 MB짜리 cuDNN을 받는 중에 허브가 꺼진다. 아무것도 바뀌지 않고, 다음 실행에서 다시 받는다. Task 4 `interrupted_download_changes_nothing`.
2. 작업자가 런처를 두 번 빨리 누른다. 두 번째는 같은 폴더를 건드리지 않는다. Task 3 `second_sync_on_the_same_root_is_busy`.
3. 초대 코드가 틀리거나 바뀌었다(401). 연결 실패 창이 아니라 설정 창이 다시 뜬다. Task 4 `wrong_token_is_unauthorized`, Task 5 `wrong_token_exits_4`.
4. 설치 경로에 한글이 있다(`보치아 라벨러`). 설치와 실행이 다 된다. Task 4 `korean_root_installs`.
5. 디스크에 쓸 수 없다(`.part`를 못 만듦). 아무것도 바뀌지 않는다. Task 3 `unwritable_part_changes_nothing`.

---

### Task 1: 허브 버전 등록 (`hub/release.py`, `boccia-hub release`)

**Files:**
- Create: `hub/release.py`, `hub/tests/test_release.py`
- Modify: `hub/cli.py`(명령 `release`), `tools/collect_hub.ps1`(복사할 이름에 `release.py`), `hub/README.md`(명령 한 단락)

**Interfaces:**
- Produces:
  - `release_files(root: Path, app: str) -> list[tuple[str, Path]]`: (목록 경로, 원본) 이름순. labeler는 `labeler/` 아래 파일에서 `labeler/data/` · `labeler/logs/` 아래, 이름이 `*-previous.exe`인 것, `labeler/BocciaLauncher.exe`를 뺀 것과 `models/` · `cuda/` 아래 전부, 그리고 `labeler/BocciaLauncher.exe`를 경로 `BocciaLauncher.exe`로 넣는다. 모르는 app은 `ValueError`.
  - `version_id(version: str) -> str`: `[^0-9A-Za-z]+`를 `-`로 바꾸고 양끝 `-`를 뗀다.
  - `register(storage: Path, app: str, root: Path, version: str | None = None) -> dict`: `{"manifest", "added_bytes", "hashed"}`를 돌려준다. `version`이 없으면 `root/labeler/boccia_labeler.exe --version`의 출력을 strip해서 쓴다.
  - `current(storage: Path, app: str) -> dict | None`, `blob(storage: Path, sha256: str) -> Path | None`(64자 소문자 16진수가 아니거나 없으면 None).
  - 저장소 모양
    - `releases/files/<sha256>`: 블롭. 임시 이름으로 복사한 뒤 이름을 바꾼다.
    - `releases/<app>/<id>.json`: 목록 `{"app","id","version","registered","files":[{"path","size","sha256"}]}`.
    - `releases/<app>/index.json`: `{"current": id, "versions": [{"id","version","registered","files","bytes"}]}`. 같은 id는 바꿔 끼운다.
    - `releases/hash-cache.json`: 원본 절대 경로마다 `size` · `mtime_ns` · `sha256`. 둘이 같으면 다시 재지 않는다.
    - JSON은 임시 파일에 쓴 뒤 바꿔 끼운다.
  - CLI `boccia-hub release <app> <root> [--storage D:\Boccia Hub] [--version TEXT]`: 허브 서버 없이 저장소에 바로 쓴다. 출력은 한 줄 `labeler <version> · 파일 <n>개 · <x.x> GB · 새로 <y.y> MB`, 종료 코드 0. 실패하면 사유 한 줄, 종료 코드 1.

- [ ] **Step 1: 실패하는 테스트를 쓴다.** 임시 폴더에 작은 가짜 배포 트리(`labeler/boccia_labeler.exe`, `labeler/a.dll`, `labeler/data/x`, `labeler/logs/y`, `labeler/boccia_labeler-previous.exe`, `labeler/BocciaLauncher.exe`, `models/boccia/m.onnx`, `cuda/c.dll`)를 만든다.

```python
def test_register_leaves_out_worker_files(self):
    paths = [f["path"] for f in register(self.storage, "labeler", self.root, "v1")["manifest"]["files"]]
    for left_out in ("labeler/data/x", "labeler/logs/y", "labeler/boccia_labeler-previous.exe", "labeler/BocciaLauncher.exe"):
        self.assertNotIn(left_out, paths)
    for kept in ("BocciaLauncher.exe", "labeler/a.dll", "models/boccia/m.onnx", "cuda/c.dll"):
        self.assertIn(kept, paths)

def test_same_content_is_stored_once(self):     # a.dll과 내용이 같은 cuda/c.dll → releases/files의 블롭 수가 경로 수보다 1 적다
def test_register_sets_current_and_keeps_versions(self):   # v1, v2 → current == version_id("v2"), versions 2개, <id>.json 둘 다 남음
def test_same_version_again_replaces_it(self):  # v1 두 번 → versions 1개
def test_unchanged_files_are_not_hashed_again(self):   # 같은 트리를 v1, v2로 → 두 번째 결과 hashed == 0, added_bytes == 0
def test_version_id(self):
    self.assertEqual(version_id("2026-10-21 10:12 · 2039bd5"), "2026-10-21-10-12-2039bd5")
def test_cli_release(self):   # python hub/cli.py release labeler <root> --storage <tmp> --version v1 → returncode 0, stdout에 "파일", current 있음
```

- [ ] **Step 2: 돌려서 실패를 본다.** `python -m unittest hub.tests.test_release` → `ModuleNotFoundError: hub.release`.
- [ ] **Step 3: `hub/release.py`와 CLI `release`를 만든다.** `cli.py`는 `hub.py`를 불러오는 방식(`importlib.util.spec_from_file_location`, 자기 옆 파일) 그대로 `release.py`를 부른다. 해시는 1 MiB씩 읽어 `hashlib.sha256`.
- [ ] **Step 4: 통과를 본다.** `python -m unittest hub.tests.test_release hub.tests.test_hub hub.tests.test_cli hub.tests.test_ui` → 전부 OK.
- [ ] **Step 5: `tools/collect_hub.ps1`이 `release.py`도 복사하게 하고, `hub/README.md`에 명령을 적는다.**
- [ ] **Step 6: 커밋한다.** `git add hub/release.py hub/tests/test_release.py hub/cli.py tools/collect_hub.ps1 hub/README.md`

### Task 2: 허브가 내주기 (`/releases/<app>`, `/releases/files/<sha256>`)

**Files:**
- Modify: `hub/hub.py`, `hub/tests/test_release.py`(서버 테스트 클래스, `HubTest`처럼 `Hub(...).serve(["127.0.0.1"], 0)`)

**Interfaces:**
- Consumes: `release.current`, `release.blob`
- Produces:
  - `Hub.token_owner(hash_: str) -> str`: 토큰의 작업자 이름. 모르는 토큰이면 `HubError(401, "unknown token")`.
  - 앱 API의 GET 두 경로. `resolve` 앞에서 답하므로 `/register`가 필요 없다. 목록은 `_json`, 파일은 새 `Handler._stream(path: Path)`로 보낸다. `_stream`은 `application/octet-stream`, `Content-Length`, 1 MiB씩 쓴다. 없으면 404 `"no release"` · `"no such file"`.
  - `hub.py`는 `try: from . import release` / `except ImportError: import release`로 부른다. 패키지로도, 스크립트로도 돈다.

- [ ] **Step 1: 실패하는 테스트를 쓴다.**

```python
def test_release_needs_a_token(self):   # Authorization 없음 → 401, "Bearer nope" → 401
def test_no_register_needed(self):      # invite만 하고 /register 없이 GET /releases/labeler → 200, ["id"] == version_id("v1")
def test_no_release_is_404(self):       # 빈 저장소 → 404
def test_release_file_bytes(self):      # GET /releases/files/<sha> → sha256(body) == sha, Content-Length == size
def test_unknown_file_is_404(self):     # "0"*64 → 404, "abc" → 404
def test_big_file_streams(self):        # 24 MiB 파일을 등록하고 받는다 → sha256 일치
```

- [ ] **Step 2: 실패를 본다.** `python -m unittest hub.tests.test_release` → 새 테스트가 404 · 403으로 실패.
- [ ] **Step 3: `token_owner`, 두 경로, `_stream`을 만든다.**
- [ ] **Step 4: 허브 테스트 전체가 통과하는지 본다.** Task 1의 Step 4 명령.
- [ ] **Step 5: 커밋한다.** `git add hub/hub.py hub/tests/test_release.py`

### Task 3: 갱신 엔진, 로컬 (`src/labeler_update.*`)

**Files:**
- Create: `src/labeler_update.hpp`, `src/labeler_update.cpp`, `tests/labeler_update_tests.cpp`
- Modify: `CMakeLists.txt`. `labeler_update` STATIC과 `labeler_update_tests`를 만든다. 둘 다 `MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"`, `/utf-8 /W4`, `NOMINMAX WIN32_LEAN_AND_MEAN`, include `src`와 `${DEPENDENCY_ROOT}/nlohmann/include`, 링크 `winhttp bcrypt`. `add_test(NAME labeler_update COMMAND labeler_update_tests)`.

**Interfaces:**
- Produces (namespace `boccia::update`):

```cpp
struct ReleaseFile { std::string path; std::uint64_t size = 0; std::string sha256; };
struct Manifest { std::string app, id, version; std::vector<ReleaseFile> files; };
Manifest parseManifest(const nlohmann::json& json);            // 경로나 해시가 틀리면 std::runtime_error
std::string fileSha256(const std::filesystem::path& file);      // 소문자 16진수
std::string sha256Hex(std::string_view bytes);
using Fetch = std::function<void(const ReleaseFile& file, const std::filesystem::path& part,
                                 const std::function<void(std::uint64_t bytes)>& received)>;   // part를 다 쓰거나 throw
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
enum class Outcome { UpToDate, Updated, Busy, BadVersion, Failed };
struct SyncResult { Outcome outcome = Outcome::Failed; std::string error; std::uint64_t downloaded = 0; bool firstInstall = false; };
bool installable(const std::filesystem::path& root, const std::filesystem::path& launcher);
SyncResult sync(const std::filesystem::path& root, const Manifest& manifest, const Fetch& fetch, const Progress& progress);
bool pendingCheck(const std::filesystem::path& root);        // 마지막 sync가 바꿨고 첫 실행을 아직 확인하지 않음
void confirmStart(const std::filesystem::path& root);
std::string revertLastUpdate(const std::filesystem::path& root);   // 되돌린 버전의 version 문자열, 되돌릴 것이 없으면 ""
std::string installedVersion(const std::filesystem::path& root);
```

- 규칙
  - `installable`: `update/state.json`이 있거나, 폴더에 런처와 숨김 · 시스템 파일 말고 아무것도 없을 때 참.
  - `sync`는 이름 있는 뮤텍스 `Local\BocciaLauncher-<sha256Hex(소문자 root 경로)의 앞 16자>`를 잡는다. 이미 잡혀 있으면 `Busy`이고 아무것도 건드리지 않는다.
  - `state.bad`에 목록 id가 있으면 `BadVersion`이고 아무것도 건드리지 않는다.
  - 처음 설치는 받기 전에 빈 `update/state.json`부터 쓴다. 끊겨도 다음 실행이 이어서 받는다.
  - 비교는 `state.files`의 size · 마지막 수정 시각이 같으면 저장된 해시를 쓰고, 다르면 다시 잰다.
  - 받기는 `update/<sha256>.part`. `sync`가 SHA-256을 직접 재고, 틀리면 part를 지우고 `Failed`.
  - 다 받은 뒤에 `update/backup/`을 비운다. 바뀌는 대상을 `update/backup/<path>`로, part를 대상으로 옮긴다. 새 목록에 없는 지난 경로도 backup으로 옮긴다. 하나라도 실패하면 옮긴 것을 거꾸로 되돌리고 `Failed`.
  - 끝나면 state에 `version` · `id` · `files`, `last`(from · to 버전과 id, replaced · added · removed, 지난 files 항목), `check: true`를 쓴다.
  - `revertLastUpdate`는 `last`로 되돌리고 to id를 `bad`에 넣고 `check`를 끈다.

- [ ] **Step 1: 실패하는 테스트를 쓴다.** `Fetch`는 원본 폴더에서 복사하는 로컬 함수다. 원본 트리는 Task 1과 같은 모양이다. `check(cond, message)`는 실패하면 throw하고, `main`이 잡아 `FAIL: …`을 찍고 1을 돌려준다.

```cpp
void first_install_places_every_file();        // 빈 root+런처 → 모든 파일 바이트 일치, firstInstall, installedVersion == "v1"
void second_sync_downloads_nothing();          // 다시 sync → UpToDate, downloaded == 0
void only_the_changed_file_comes();            // v2에서 a.dll만 바뀜 → downloaded == a.dll 크기, 다른 대상의 수정 시각 그대로
void dropped_file_goes_to_backup();            // v2에 models/boccia/m.onnx 없음 → root에서 사라지고 update/backup에 있음
void bad_hash_changes_nothing();               // Fetch가 다른 바이트를 씀 → Failed, root와 state 그대로, .part 없음
void locked_target_rolls_back();               // 대상 하나를 FILE_SHARE_DELETE 없이 열어 둠 → Failed, 먼저 옮긴 파일들 원래대로
void data_and_logs_stay();                     // labeler/data/x, labeler/logs/y가 두 번의 갱신 뒤에도 그대로, backup에 없음
void not_empty_folder_is_not_installable();    // note.txt만 있음 → false, 런처와 숨김 desktop.ini만 있음 → true
void second_sync_on_the_same_root_is_busy();   // 한 sync의 Fetch 안에서 다른 스레드로 같은 root를 sync → 그 결과 Busy(같은 스레드는 뮤텍스를 다시 잡으므로)
void unwritable_part_changes_nothing();        // update/<sha>.part를 폴더로 미리 만듦 → Failed, root 그대로
void revert_restores_the_files_before();       // v1→v2(바뀜·더함·뺌) → revertLastUpdate == "v2", 바이트 v1, 더한 것 없음, 뺀 것 돌아옴, installedVersion == "v1", 다시 sync(v2) → BadVersion
```

- [ ] **Step 2: 실패를 본다.** `cmake --build build-labeler --config Release --target labeler_update_tests` → `labeler_update.hpp`가 없어 빌드 실패.
- [ ] **Step 3: `src/labeler_update.cpp`를 만든다.** 해시는 BCrypt SHA-256, 읽기는 1 MiB 단위. 파일 이동은 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`이다.
- [ ] **Step 4: 통과를 본다.** 빌드 후 `build-labeler/Release/labeler_update_tests.exe` → `PASS: …` 한 줄, 종료 코드 0.
- [ ] **Step 5: 커밋한다.** 새 파일은 `git add`, `CMakeLists.txt`는 `stage.py`로 두 대상 블록만 더한다. 자리는 인덱스 판본의 `add_test(NAME hub_client COMMAND hub_client_tests)` 뒤.

### Task 4: 갱신 엔진, 네트워크

**Files:**
- Create: `tests/hub_process.hpp`(`tests/hub_client_tests.cpp`의 `HubProcess`를 옮긴 것, Win32만 씀)
- Modify: `src/labeler_update.hpp`, `src/labeler_update.cpp`, `tests/labeler_update_tests.cpp`, `tests/hub_client_tests.cpp`(헤더를 씀, 동작 그대로)

**Interfaces:**
- Consumes: Task 3의 `Manifest`, `Fetch`, `parseManifest`
- Produces:

```cpp
struct HubAddress { std::string address, token; };    // "host:port", 초대 코드
enum class Answer { Ok, Unreachable, Unauthorized, NoRelease };
Answer fetchManifest(const HubAddress& hub, const std::string& app, Manifest& out);   // GET /releases/<app>, 연결 5초
Fetch hubFetch(const HubAddress& hub);   // GET /releases/files/<sha256>을 part로 흘려 받음. 200이 아니거나 끊기면 throw
```

- 401은 `Unauthorized`, 404는 `NoRelease`, 연결 실패와 시간 초과는 `Unreachable`이다. 받는 읽기 하나의 시간 제한은 30초.
- 테스트의 허브: 스크래치 저장소의 `tokens.json`에 `{sha256Hex(토큰): "점검"}`을 써 두고 `HubProcess`로 띄운다. 등록은 `python <repo>/hub/cli.py release labeler <트리> --storage <저장소> --version vN`을 자식 프로세스로 돌린다.

- [ ] **Step 1: 실패하는 테스트를 쓴다.**

```cpp
void installs_from_the_hub();                 // fetchManifest == Ok → sync(hubFetch) → Updated, 바이트 일치, installedVersion == "v1"
void wrong_token_is_unauthorized();           // 토큰 "nope" → Unauthorized
void no_release_is_no_release();              // 등록 전 → NoRelease
void hub_off_is_unreachable();                // 허브를 끔 → 6초 안에 Unreachable
void interrupted_download_changes_nothing();  // 64 MiB 파일, Progress가 8 MiB에서 허브를 끔 → Failed, root 그대로; 허브를 다시 켜고 sync → Updated
void korean_root_installs();                  // root 이름 "보치아 라벨러" → Updated
```

- [ ] **Step 2: 실패를 본다.** 빌드 → `fetchManifest`가 없어 실패.
- [ ] **Step 3: WinHTTP로 두 함수를 만들고 `HubProcess`를 헤더로 옮긴다.**
- [ ] **Step 4: 통과를 본다.** `labeler_update_tests.exe` → 0, `hub_client_tests.exe` → 0. 끝난 뒤 `hub-client-test`나 스크래치 저장소를 쓰는 python 프로세스가 남지 않는다.
- [ ] **Step 5: 커밋한다.** `git add tests/hub_process.hpp tests/hub_client_tests.cpp src/labeler_update.hpp src/labeler_update.cpp tests/labeler_update_tests.cpp`

### Task 5: 런처

**Files:**
- Create: `tests/fake_labeler.cpp`
- Modify:
  - `src/labeler_launcher_main.cpp`: 전부 다시 쓴다.
  - `src/labeler_settings.cpp`: `labeler_util.hpp` 대신 같은 폴더 임시 파일과 `MoveFileExW`로 원자적으로 쓴다. 런처가 이 파일을 함께 컴파일한다.
  - `CMakeLists.txt`
    - 런처: `src/labeler_settings.cpp`를 더하고 `labeler_update`, comctl32 · ole32 · uuid · gdi32를 링크한다.
    - `fake_labeler` 콘솔 대상을 만든다.
    - `labeler_update_tests`에 `BOCCIA_LAUNCHER_EXE="$<TARGET_FILE:boccia_labeler_launcher>"`, `BOCCIA_FAKE_LABELER_EXE="$<TARGET_FILE:fake_labeler>"`, `BOCCIA_TEST_ROOT`를 정의하고 `add_dependencies`를 단다.
  - `tests/labeler_update_tests.cpp`
- Commit(새로 추적): `src/labeler_launcher_main.cpp`, `assets/icons/labeler_icon.rc`, `assets/icons/boccia-labeler.ico`

**Interfaces:**
- Consumes: Task 3 · 4의 API, `loadLabelerSettings` · `saveLabelerSettings`
- Produces:
  - 명령줄 `BocciaLauncher.exe [--no-window] [--sync-only] [--shortcut-folder <dir>] [--capture <png> --view settings|progress|unreachable]`
  - 종료 코드: 0 됨, 3 허브 없음이고 깔린 것 없음, 4 초대 코드 틀림, 5 빈 폴더 아님, 6 다른 런처가 갱신 중, 7 실패이고 깔린 것 없음
  - `fake_labeler.exe`: `--version`이면 옆의 `fake-version.txt`를 찍는다(없으면 `fake`). 아니면 옆의 `ran.txt`에 한 줄 더하고, `fake-exit.txt`의 `<코드> <밀리초>`만큼 기다린 뒤 그 코드로 끝난다(없으면 `0 0`).

- 순서. 테스트가 이 순서를 못 박는다.
  1. 옆에 `boccia_labeler.exe`가 있으면 지금처럼 띄우고 기다린다. 설정과 허브는 보지 않는다.
  2. root는 런처 폴더다. `installable`이 아니면 `빈 폴더에 넣어 주세요`를 띄운다(`--no-window`면 5).
  3. 설정에 이름이나 토큰이 없으면 설정 창(이름 · 초대 코드 · 허브 주소 · 시작)을 띄우고 저장한다(`--no-window`면 4).
  4. 이 root의 `labeler\boccia_labeler.exe`가 이미 돌고 있으면 받지 않고 띄운다.
  5. `fetchManifest`
     - `Unauthorized`: 설정 창을 칸을 채운 채 다시 띄운다(`--no-window`면 4).
     - `Unreachable`, `NoRelease`: 깔려 있으면 띄우고, 아니면 `허브에 연결할 수 없습니다` + `다시`(`--no-window`면 3).
  6. `sync`. 받을 바이트가 있을 때만 진행 창을 띄운다.
     - `Busy`면 6.
     - `Failed`, `BadVersion`: 깔려 있으면 띄우고, 아니면 7.
  7. 처음 설치가 끝나면 `Boccia Labeler.lnk`를 바탕 화면(또는 `--shortcut-folder`)에 만든다. 대상은 root의 런처다.
  8. `labeler\boccia_labeler.exe`를 `labeler\`에서 띄운다(`--sync-only`면 여기서 0으로 끝).
     - `pendingCheck`면 10초까지 지켜본다. 0이 아닌 코드로 끝나면 `revertLastUpdate`를 하고, `labeler\logs\labeler-YYYY-MM-DD.log`에 `YYYY-MM-DD HH:MM:SS.mmm  업데이트 되돌림 · <version>`을 더한 뒤 한 번 더 띄운다.
     - 그 밖에는 `confirmStart`를 한다.
     - 그다음은 지금처럼 라벨러가 끝날 때까지 기다린다.

- [ ] **Step 1: 실패하는 테스트를 쓴다.** 빌드된 런처를 임시 root에 `BocciaLauncher.exe`로 복사하고 `--no-window`로 띄운다. 자식 환경의 `LOCALAPPDATA`는 스크래치이고 거기 설정 JSON을 미리 쓴다. 등록하는 트리의 `labeler/boccia_labeler.exe`는 `fake_labeler.exe`다.

```cpp
void launcher_installs_and_starts();         // → 0, 파일 일치, labeler\ran.txt 있음
void launcher_dev_mode_only_starts();        // 런처 옆에 fake boccia_labeler.exe, 설정 · 허브 없음 → 0, ran.txt, update\ 없음
void launcher_refuses_a_full_folder();       // root에 note.txt → 5, labeler\ 없음
void wrong_token_exits_4();                  // 설정 토큰 "nope" → 4
void crash_within_ten_seconds_reverts();     // v1 "0 0", v2 "3 500" → 로그에 "업데이트 되돌림 · v2", installedVersion == "v1", 다음 실행 뒤에도 "v1"
void quick_close_is_kept();                  // v2 "0 200" → installedVersion == "v2"
void first_install_makes_the_shortcut();     // --shortcut-folder <tmp> → Boccia Labeler.lnk의 대상이 root\BocciaLauncher.exe
void launcher_updates_itself();              // v2의 BocciaLauncher.exe = 빌드된 런처 + 끝에 1바이트 → 실행 뒤 root의 런처 == v2 바이트, 옛 것은 update\backup
void running_labeler_is_not_updated();       // root의 fake 라벨러가 "0 5000"으로 도는 중에 v2 등록 → 런처 → 0, installedVersion == "v1"
```

- [ ] **Step 2: 실패를 본다.** 빌드 → `fake_labeler` 대상이 없어 실패하거나, 지금 런처가 받지 않아 테스트가 실패한다.
- [ ] **Step 3: 런처, 설정 쓰기, fake_labeler를 만든다.** 창은 Win32 기본 컨트롤(EDIT, BUTTON, `PROGRESS_CLASS`)로 만들고 글꼴은 `SPI_GETNONCLIENTMETRICS`의 메시지 글꼴을 쓴다. `--capture`는 창을 화면 밖에 만들어 `PrintWindow`로 PNG를 쓴다(WIC). 예시 값은 `받는 중 · 1.2 / 2.6 GB`.
- [ ] **Step 4: 통과를 본다.** `labeler_update_tests.exe` → 0. `native_labeler_tests.exe` → 0(설정 저장 검사). 세 창을 캡처해서 문구와 칸을 눈으로 확인한다.
- [ ] **Step 5: 커밋한다.** `git add src/labeler_launcher_main.cpp src/labeler_settings.cpp tests/fake_labeler.cpp tests/labeler_update_tests.cpp assets/icons/labeler_icon.rc assets/icons/boccia-labeler.ico`. `CMakeLists.txt`는 `stage.py`로 런처 블록과 `fake_labeler` 블록, 테스트 정의만 더한다.

### Task 6: 배포 스크립트, 문서, 처음부터 끝까지

**Files:**
- Modify:
  - `tools/collect_release.ps1`: `param([string]$HubStorage = 'D:\Boccia Hub')`를 둔다. 끝에서 `python hub\cli.py release labeler <bin\release> --storage $HubStorage`를 부른다. Python이 없거나 실패하면 `Write-Warning`만 하고 배포는 끝난 것으로 둔다.
  - `hub/README.md`: 두 경로를 적는다.
  - `NATIVE_LABELER.md`: "작업자 PC에 설치" 단락. 빈 폴더, 런처, 이름 · 초대 코드, 업데이트, 되돌림.
  - `design/hub/DESIGN.md`: "새 버전 패치"에 라벨러는 `LAUNCHER.md`로 갔다고 한 줄.
  - `design/hub/LAUNCHER.md`: 런처 자신은 backup으로 옮긴다(판정).

- [ ] **Step 1: 배포 스크립트를 고친다.**
- [ ] **Step 2: 스크래치에서 처음부터 끝까지 돌린다.**
  - `powershell -NoProfile -File tools\collect_release.ps1 -HubStorage <scratch>\hub-e2e` → 마지막 줄이 `labeler … · 파일 …개 · 2.6 GB · 새로 …`.
  - 그 저장소로 허브를 포트 8798에 띄우고 `tokens.json`에 점검용 토큰을 넣는다. 설정은 스크래치 `LOCALAPPDATA`.
  - 빌드된 런처를 `<scratch>\설치 점검\BocciaLauncher.exe`로 복사하고 `--no-window --sync-only --shortcut-folder <scratch>\desk`로 돌린다 → 0. `labeler\boccia_labeler.exe`, `models\boccia\yolo26n-20261012-1280x960-u8.onnx`, `cuda\cudnn64_9.dll`이 있다.
  - 깔린 라벨러를 숨긴 점검으로 띄운다: `<설치 점검>\labeler\boccia_labeler.exe --workspace <step3 복사본> --smoke-hidden --view overview --timeout 120` → PASS. 그 폴더 로그의 시작 줄에 `CUDA <설치 점검>\cuda`가 있다.
  - 런처를 다시 돌린다 → 5초 안에 끝나고 깔린 파일의 수정 시각이 그대로다.
  - 스크래치 설치와 저장소를 지우고, 점검 허브를 끈다.
- [ ] **Step 3: 실제로 배포한다.** `powershell -NoProfile -File tools\collect_release.ps1` → `D:\Boccia Hub\releases\labeler\index.json`의 `current`가 방금 버전이다. 허브 파일도 `tools\collect_hub.ps1`로 배포한다.
- [ ] **Step 4: 문서를 고친다.**
- [ ] **Step 5: 커밋한다.** `git add tools/collect_release.ps1 hub/README.md NATIVE_LABELER.md design/hub/DESIGN.md design/hub/LAUNCHER.md`
- [ ] **Step 6: 사용자에게 알린다.** 허브 서버를 한 번 다시 켜야 `/releases`가 열린다. 작업자에게 줄 파일은 `bin\release\labeler\BocciaLauncher.exe` 하나다. 같은 파일이 라벨러 옆에서는 지금처럼 띄우기만 하고, 빈 폴더에서는 받아서 깐다.

## 자기 검토

- 설계 문서의 각 항목을 덮는 작업
  - 런처 하나로 받기, 처음 설정 창, 받기, 띄우기: Task 3 · 4 · 5
  - 바뀐 파일만 받기: Task 3 `only_the_changed_file_comes`
  - 허브가 꺼져 있으면 깔린 것 띄우기: Task 4 · 5
  - 배포마다 등록하고 최근 것 보내기: Task 1 · 6
  - 작업자 파일을 건드리지 않음: Task 1 · 3
  - 빈 폴더 규칙: Task 3 · 5
  - 개발용 런처는 받지 않음: Task 5
  - 이미 떠 있으면 받지 않음: Task 5 `running_labeler_is_not_updated`
  - 바로가기: Task 5
  - 허브 주소 기본값: Task 5(설정 기본값)
  - 토큰만으로 받기: Task 2
  - 해시 검증과 실패 시 되돌림: Task 3
  - 진행 창, 연결 실패 창: Task 5
  - 런처 자신 갱신: Task 5
  - 10초 되돌림, bad, 로그 한 줄: Task 3 · 5
- 되돌리기 화면, 트래커, 라벨러가 도는 중의 갱신은 설계대로 이 계획에 없다.
- 이름 확인: `fetchManifest` · `hubFetch` · `sync` · `revertLastUpdate` · `installedVersion` · `pendingCheck` · `confirmStart`는 Task 3 · 4에서 정의하고 Task 5에서 그대로 쓴다. 허브 쪽 `register` · `current` · `blob` · `version_id`는 Task 1에서 정의하고 Task 2 · 테스트에서 쓴다.
