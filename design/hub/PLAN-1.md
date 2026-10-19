# Boccia Hub 1단계 구현 계획: 허브 최소 · CLI · 공용 클라이언트 · 라벨러 연결 · 작업자 이름 · 토큰

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 라벨러가 켜지면 이 PC의 허브에 붙어 등록·상태·이벤트를 보내고 명령(알림·로그·ping)을 받아 가며, 허브 CLI로 그것을 보고 명령을 넣을 수 있게 한다. 화면은 없다(2단계).

**Architecture:** 허브는 Python 표준 라이브러리만 쓰는 HTTP 서버 하나(`hub/hub.py`)로, 앱 쪽 API(토큰 필요)와 제어 API(`/api/…`, localhost·Tailscale)를 한 포트에서 받고 `D:\Boccia Hub\`에 저장한다. 앱 쪽은 C++ 라이브러리 `hub_client`(WinHTTP, 스레드 둘: 보내기·명령 대기)가 라벨러·트래커 공용이고, 라벨러는 로그 줄을 이벤트로 흘려보내며 설정 파일에 작업자 이름·허브 주소·토큰을 둔다. 라벨러가 떨어져 있던 동안의 이벤트는 로그 파일에서 이어 보낸다.

**Tech Stack:** Python 3.12 (`http.server.ThreadingHTTPServer`, `json`, `unittest`), C++20 / WinHTTP / nlohmann::json (기존 `Json`), CMake(Visual Studio 17 2022, `build-labeler`), ImGui 대화상자(`theme::beginDialog` 등).

**Spec:** `design/hub/DESIGN.md` — 결정 1·3·4·5·6·12, "앱이 붙는 방식", "보내는 내용(라벨러)", "명령"(로그·알림), "보안"(제어면 분리·토큰), "앱 구조", "구현 순서 1".

## Global Constraints

- 허브는 Python **표준 라이브러리만**(설치 없음). 포트 **8790**. 기본 저장소 `D:\Boccia Hub\`. 바인드는 `127.0.0.1`과 Tailscale IPv4(없으면 127.0.0.1만), `--bind all`로 전부.
- 앱 쪽 API는 `Authorization: Bearer <토큰>` 없이는 `401`. `/ping`만 예외. 제어 API `/api/…`는 토큰 없음(localhost·Tailscale에서만 닿음).
- 토큰은 **32바이트 난수**의 base64url(43자). `invite <이름>`으로 만든다.
- 작업자 식별자 = `<이름>@<PC>`. 표시는 "이름 · PC".
- 라벨러 설정 파일 `%LOCALAPPDATA%\Boccia\labeler-settings.json`에 `"hub": {"address": "mark-hong-pc.tail68c059.ts.net:8790", "worker": "", "token": ""}`. 기존 `"device"` 키는 그대로.
- 앱은 허브가 없어도 **절대 멈추지 않는다**: 모든 HTTP 호출은 연결 3초·응답 5초(명령 대기만 30초) 제한, 화면 스레드에서는 호출하지 않는다.
- 노크 10초마다, 이벤트는 1초마다 묶어 보냄, 명령 대기 `wait=25`.
- 이어 보내기는 로그 파일에서 **마지막 2,000줄**까지만.
- 허브는 (시각, 문장)이 같은 이벤트를 한 번만 저장한다(이어 보내기와 실시간 전송이 겹치는 줄).
- 문구는 라벨·숫자·버튼만(설명 문장 없음). 코드 주석과 문서는 기존 파일의 밀도를 따른다. 파일 인코딩·줄바꿈은 기존대로(main·projects CRLF, 나머지 LF).
- 빌드는 `build-labeler`에서만. `--clean-first`·삭제 금지. 숨긴 점검은 반드시 `--smoke-hidden`과 함께.
- 커밋은 작업마다 하되 **푸시는 하지 않는다**(사용자가 정함).

## Review Focus

1. 토큰이 틀린 라벨러: 허브가 401을 주면 라벨러는 10초마다 다시 시도하지 않고 멈추고 설정 창에서 고치기 전까지 "초대 코드 확인"만 보인다 → Task 4·5 테스트.
2. 허브가 꺼진 채 라벨러를 몇 시간 쓰다 허브를 켬: 로그 줄이 수천 개여도 마지막 2,000줄만, 중복 없이 시간순으로 들어간다 → Task 1·4 테스트.
3. 두 PC의 시계가 다름: 허브는 라벨러가 적은 시각과 자기가 받은 시각을 둘 다 저장하고, 목록은 받은 시각으로 정렬한다 → Task 1 테스트.
4. 같은 이름의 작업자가 두 PC에서 켬: 식별자가 PC로 갈려 둘 다 보인다 → Task 1 테스트.
5. 라벨러가 명령을 처리하다 죽음: 결과가 안 온 명령은 허브에 "보냄"으로 남고 다음 접속 때 다시 주지 않는다(한 번만) → Task 1 테스트.

---

## 파일 구조

| 파일 | 책임 |
| --- | --- |
| `hub/hub.py` (새) | 서버: 저장소, 토큰, 등록·상태·이벤트·명령 큐·파일 업로드, 제어 API, `serve` 진입 |
| `hub/cli.py` (새), `boccia-hub.cmd` (새, 저장소 루트) | CLI: `serve status workers events send invite logs` |
| `hub/tests/test_hub.py` (새) | 서버 테스트(스레드로 띄움, `urllib`) |
| `hub/tests/test_cli.py` (새) | CLI 출력 테스트 |
| `cmake/build_info.cmake` (새), `CMakeLists.txt` | 빌드 시각·git 해시 헤더 `build_info.hpp` 생성, 대상 `hub_client`, `hub_client_tests` |
| `src/hub_client.hpp/.cpp` (새) | 공용 클라이언트: WinHTTP, 스레드, 보관함, 이어 보내기, 명령 |
| `tests/hub_client_tests.cpp` (새) | 실제 `hub.py`를 자식 프로세스로 띄워 클라이언트를 검사 |
| `src/labeler_log.hpp/.cpp` | 로그 줄 구독(`setLogSink`) |
| `src/labeler_settings.hpp/.cpp` (새) | `labeler-settings.json` 읽기·쓰기(`device`, `hub`) — `native_labeler_projects.cpp`의 `deviceFile/loadDevice/saveDevice`가 이리로 옮겨 감 |
| `src/native_labeler_app.hpp/.cpp`, `native_labeler_projects.cpp`, `native_labeler_main.cpp` | 작업자 창, 클라이언트 시작·상태·등록, 이벤트 문장, 명령 처리, 상태 막대 표시, `--version`, 숨긴 캡처 옵션 |
| `NATIVE_LABELER.md`, `hub/README.md` (새) | 사용법 |

---

### Task 1: 허브 서버 핵심 (`hub/hub.py`)

**Files:**
- Create: `hub/hub.py`, `hub/tests/test_hub.py`, `hub/tests/__init__.py`

**Interfaces:**
- Produces (Python): `class Hub(storage: Path)`, `Hub.invite(worker: str) -> str`(토큰), `Hub.serve(bind: list[str], port: int) -> http.server.ThreadingHTTPServer`(스레드로 돌림; `server.server_port`), 요청 처리는 `Handler(BaseHTTPRequestHandler)`.
- Produces (HTTP, 앱 쪽, 토큰 필요):
  - `GET /ping` → `200 {"hub":"boccia","time":"<ISO>"}` (토큰 불필요)
  - `POST /register` `{"app":"labeler","worker":"홍길동","pc":"joonyeon","account":"markhong","version":"…","device":{…},"summary":{…}}` → `200 {"ok":true,"id":"홍길동@joonyeon","since":"<이 작업자의 마지막 이벤트 시각 또는 "">"}`. 토큰의 이름과 `worker`가 다르면 `403`.
  - `POST /status` `{…}` → `204`. 저장소 `status\<id>.json`에 덮어씀(`received` 필드 추가).
  - `POST /events` `{"events":[{"time":"2026-10-19 14:31:07.112","text":"…"}]}` → `200 {"stored":n}`. `events\<id>\<날짜>.jsonl`에 `{"time","received","text"}`. (time,text) 중복은 버림.
  - `GET /commands?wait=25` → `200 {"commands":[{"id":"c-…","name":"notify","value":"…"}]}`. 없으면 최대 `wait`초 기다렸다 `{"commands":[]}`. 준 명령은 "sent"로 표시해 다시 주지 않는다.
  - `POST /commands/<id>/result` `{"ok":true,"text":"…"}` → `204`.
  - `POST /files/<kind>/<name>` (kind `log|screen`, 본문 그대로, 최대 50 MB) → `200 {"path":"…"}`. `<kind>\<id>\<name>`에 저장. 이름은 `[A-Za-z0-9._-]+`만.
- Produces (HTTP, 제어 `/api/…`, 토큰 없음):
  - `GET /api/status` → `{"workers":n,"connected":m,"storage":"…","started":"…"}`
  - `GET /api/workers` → `[{"id","worker","pc","app","version","connected":bool,"last_seen":"…","status":{…}}]` (`last_seen` 15초 안이면 connected)
  - `GET /api/workers/<id>/events?since=<ISO>&limit=200` → `[{"time","received","text"}]` 받은 시각 순
  - `POST /api/workers/<id>/commands` `{"name":"notify","value":"…"}` → `{"id":"c-…"}`
  - `GET /api/workers/<id>/commands` → `[{"id","name","value","state":"queued|sent|done|failed","result":"…"}]`
  - `POST /api/invites` `{"worker":"홍길동"}` → `{"token":"…"}`. 같은 이름을 다시 부르면 새 토큰이 옛 것을 대신한다.
  - `GET /api/workers/<id>/files/<kind>` → 그 작업자의 파일 목록 `[{"name","size","time"}]`
- 저장소: `workers.json`(`{id: {worker, pc, app, version, token_hash, last_seen, since}}`), `tokens.json`(`{sha256(token): worker}`), `status\`, `events\`, `log\`, `screen\`, `commands.jsonl`(모든 명령과 결과).

- [ ] **Step 1: 테스트 쓰기 (`hub/tests/test_hub.py`)** — `unittest`, `setUp`에서 `tempfile.mkdtemp()` 저장소로 `Hub`를 만들고 `serve(["127.0.0.1"], 0)`을 스레드로 띄움, `urllib.request`로 호출하는 도우미 `call(method, path, body=None, token=None) -> (status, json|None)`.

```python
def test_ping_needs_no_token(self):
    status, body = self.call("GET", "/ping")
    self.assertEqual(status, 200); self.assertEqual(body["hub"], "boccia")

def test_register_requires_token_and_matching_name(self):
    self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"})[0], 401)
    token = self.hub.invite("홍길동")
    self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "김민수", "pc": "joonyeon"}, token)[0], 403)
    status, body = self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon", "version": "1"}, token)
    self.assertEqual((status, body["id"], body["since"]), (200, "홍길동@joonyeon", ""))

def test_same_name_two_pcs_are_two_workers(self):
    token = self.hub.invite("홍길동")
    for pc in ("joonyeon", "lab-pc-02"): self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": pc}, token)
    self.assertEqual(sorted(w["id"] for w in self.call("GET", "/api/workers")[1]), ["홍길동@joonyeon", "홍길동@lab-pc-02"])

def test_events_dedupe_and_keep_received_time(self):
    token = self.register()
    one = {"time": "2026-10-19 14:31:07.112", "text": "확인 · 촬영 03 · 프레임 0142"}
    self.assertEqual(self.call("POST", "/events", {"events": [one, one]}, token)[1]["stored"], 1)
    self.assertEqual(self.call("POST", "/events", {"events": [one]}, token)[1]["stored"], 0)
    events = self.call("GET", "/api/workers/홍길동@joonyeon/events")[1]
    self.assertEqual(len(events), 1); self.assertIn("received", events[0])
    self.assertEqual(self.call("POST", "/register", {"app": "labeler", "worker": "홍길동", "pc": "joonyeon"}, token)[1]["since"], one["time"])

def test_command_longpoll_is_delivered_once(self):
    token = self.register()
    self.assertEqual(self.call("GET", "/commands?wait=0", token=token)[1]["commands"], [])
    cid = self.call("POST", "/api/workers/홍길동@joonyeon/commands", {"name": "notify", "value": "쉬세요"})[1]["id"]
    got = self.call("GET", "/commands?wait=5", token=token)[1]["commands"]
    self.assertEqual([(c["id"], c["name"], c["value"]) for c in got], [(cid, "notify", "쉬세요")])
    self.assertEqual(self.call("GET", "/commands?wait=0", token=token)[1]["commands"], [])   # once only, even without a result
    self.assertEqual(self.call("POST", f"/commands/{cid}/result", {"ok": True, "text": "띄움"}, token)[0], 204)
    self.assertEqual(self.call("GET", "/api/workers/홍길동@joonyeon/commands")[1][0]["state"], "done")

def test_longpoll_wakes_when_a_command_arrives(self):   # 2초 뒤에 넣고 wait=10이 즉시 돌아오는지(경과 < 5초)

def test_file_upload_and_listing(self):
    token = self.register()
    status, body = self.call_raw("POST", "/files/log/labeler-2026-10-19.log", b"line\n", token)
    self.assertEqual(status, 200)
    self.assertEqual(self.call("GET", "/api/workers/홍길동@joonyeon/files/log")[1][0]["name"], "labeler-2026-10-19.log")
    self.assertEqual(self.call_raw("POST", "/files/log/..%5c..%5cevil", b"x", token)[0], 400)

def test_status_and_connected_flag(self):   # status 저장, workers의 connected가 last_seen 기준
def test_invite_again_replaces_token(self):  # 옛 토큰으로 register → 401
```

- [ ] **Step 2: 실패 확인** — `python -m unittest hub.tests.test_hub -v` → `ModuleNotFoundError: hub.hub`.
- [ ] **Step 3: `hub/hub.py` 구현** — `Hub` 클래스(저장소 폴더 만들기, `workers.json`·`tokens.json` 읽기/쓰기 원자적 교체, `threading.Lock`), 명령 큐 `dict[id, list[Command]]`와 `threading.Condition`으로 long-poll, `Handler`는 경로를 `/api/`(제어)와 나머지(앱, `Authorization` 확인 → 작업자)로 가르고 JSON 본문은 `Content-Length` 50 MB 상한. 시각은 `datetime.now().isoformat(timespec="milliseconds")`. `serve(bind, port)`는 바인드마다 `ThreadingHTTPServer`를 만들어 데몬 스레드로 돌리고 첫 서버를 돌려준다. `python hub/hub.py serve --port 8790 --storage "D:\Boccia Hub" [--bind auto|all|127.0.0.1] [--print-port]`: `auto`는 `127.0.0.1` + `tailscale ip -4`가 주는 주소(명령이 없거나 실패하면 생략). `--print-port`는 첫 줄에 `port <n>`을 찍는다(테스트용).
- [ ] **Step 4: 테스트 통과 확인** — 같은 명령 → 전부 `ok`.
- [ ] **Step 5: 커밋** — `git add hub/hub.py hub/tests && git commit -m "Hub server: registration, status, events, commands and files with per-worker tokens"`

### Task 2: CLI (`hub/cli.py`, `boccia-hub.cmd`)

**Files:**
- Create: `hub/cli.py`, `boccia-hub.cmd`, `hub/tests/test_cli.py`

**Interfaces:**
- Consumes: Task 1 제어 API.
- Produces: `python hub/cli.py [--hub 127.0.0.1:8790] <명령>`; `boccia-hub.cmd %*` → `python "%~dp0hub\cli.py" %*`.
  - `serve …` → `hub.py serve`와 같음(그대로 넘김)
  - `status` → `허브 127.0.0.1:8790 · 붙음 2 · 전체 3 · 저장소 D:\Boccia Hub`
  - `workers` → 표: `이름  PC  앱  버전  상태  마지막  지금` (connected는 `붙음`, 아니면 `떨어짐 14:02`)
  - `events <id|이름> [--follow] [--limit 50]` → `14:31:07  확인 · 촬영 03 · 프레임 0142` 한 줄씩; `--follow`는 2초마다 새 줄
  - `send <id|이름> <명령> [값]` → `c-… 보냄` 그리고 10초 안에 결과가 오면 `완료 · <text>`
  - `invite <이름>` → 토큰 한 줄(그대로 라벨러 설정에 붙여 넣음)
  - `logs <id|이름>` → 가장 최근 로그 파일을 그대로 출력(제어 API에 `GET /api/workers/<id>/files/log/<name>` 추가: Task 1에 한 엔드포인트 더)
  - 이름만 주면 그 이름의 작업자가 하나일 때 그것, 둘 이상이면 목록을 보이고 끝(exit 2)
- 출력은 UTF-8(`sys.stdout.reconfigure(encoding="utf-8")`).

- [ ] **Step 1: 테스트 쓰기 (`hub/tests/test_cli.py`)** — 서버를 스레드로 띄우고 `cli.main([...])`을 `contextlib.redirect_stdout`으로 잡아 문자열을 확인: `status` 줄에 `붙음 0 · 전체 0`; invite → register(urllib) → `workers` 출력에 `홍길동` `joonyeon` `붙음`; `send 홍길동 notify 쉬세요` → 출력 첫 줄이 `c-`로 시작하고 `/commands`로 받아 보면 그 명령; 이름이 둘이면 exit 2.
- [ ] **Step 2: 실패 확인** — `python -m unittest hub.tests.test_cli -v` → ImportError.
- [ ] **Step 3: `hub/cli.py` 구현** — `argparse` 하위 명령, `urllib`, 표는 열 너비를 데이터로 맞춤(한글 폭은 `unicodedata.east_asian_width`로 2칸). `main(argv) -> int`.
- [ ] **Step 4: 통과 확인**; `boccia-hub.cmd status`를 실제 서버 없이 실행하면 `허브에 연결할 수 없음 · 127.0.0.1:8790` 한 줄과 exit 1.
- [ ] **Step 5: 커밋** — `git add hub/cli.py boccia-hub.cmd hub/tests/test_cli.py && git commit -m "Hub CLI: status, workers, events, send, invite, logs"`

### Task 3: 빌드 정보와 `--version` (라벨러)

**Files:**
- Create: `cmake/build_info.cmake`
- Modify: `CMakeLists.txt`(`boccia_labeler` 대상 근처), `src/native_labeler_main.cpp:43`(옵션 분기), `tests/native_labeler_tests.cpp`

**Interfaces:**
- Produces: 생성 헤더 `${CMAKE_BINARY_DIR}/build_info.hpp` with `constexpr const char* BOCCIA_BUILD_TIME = "2026-10-19 15:48";` `constexpr const char* BOCCIA_GIT_HASH = "849e7a5";` (git이 없으면 `"nogit"`). 함수 `std::string boccia::versionString()` in `src/build_info.hpp`(새, 생성 헤더를 include) → `"<BUILD_TIME> · <GIT_HASH>"`.
- `boccia_labeler.exe --version` → 그 문자열 한 줄, exit 0(창 없이).

- [ ] **Step 1: 테스트** — `tests/native_labeler_tests.cpp`에 `check(std::regex_match(boccia::versionString(), std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2} · \S+)")), "version string shape")`.
- [ ] **Step 2: 실패 확인** — 빌드 실패(헤더 없음).
- [ ] **Step 3: 구현** — `cmake/build_info.cmake`: `string(TIMESTAMP now "%Y-%m-%d %H:%M")`, `execute_process(git rev-parse --short HEAD)`, 내용이 다를 때만 파일 교체(`file(WRITE tmp)` + `configure_file(... COPYONLY)`); `CMakeLists.txt`: `add_custom_target(build_info ALL COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/build_info.hpp -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake BYPRODUCTS build_info.hpp)`, `boccia_labeler`·`native_labeler_tests`·(나중에 트래커)가 `build_info`에 의존하고 `${CMAKE_BINARY_DIR}`를 include. `native_labeler_main.cpp`: `--version`이면 `std::cout << versionString() << "\n"; return 0;`를 GLFW 전에.
- [ ] **Step 4: 확인** — `cmake --build build-labeler --config Release --target boccia_labeler native_labeler_tests`, `build-labeler\Release\boccia_labeler.exe --version` → `2026-10-19 16:20 · 849e7a5` 모양, `native_labeler_tests.exe` PASS.
- [ ] **Step 5: 커밋** — `git add cmake/build_info.cmake CMakeLists.txt src/build_info.hpp src/native_labeler_main.cpp tests/native_labeler_tests.cpp && git commit -m "Labeler reports its build time and git hash (--version)"`

### Task 4: 공용 클라이언트 `hub_client` (C++)

**Files:**
- Create: `src/hub_client.hpp`, `src/hub_client.cpp`, `tests/hub_client_tests.cpp`
- Modify: `CMakeLists.txt`(라이브러리 `hub_client`: `src/hub_client.cpp`, 링크 `winhttp`; 테스트 `hub_client_tests` + `add_test(NAME hub_client …)`), `src/labeler_log.hpp/.cpp`(`setLogSink`)

**Interfaces:**
- Produces:
```cpp
namespace boccia::hub {
struct Settings { std::string address, token, worker, pc, account, app, version; };   // address "host:port"
struct Command { std::string id, name, value; };
struct Registration { Json device, summary; };   // the app's own part of /register
class Client {
public:
    // outbox: files to upload when offline; logFile: the app's log, read for the backfill after a (re)connect
    Client(Settings settings, std::filesystem::path outbox, std::function<std::filesystem::path()> logFile, std::function<Registration()> registration);
    ~Client();                                   // stops both threads (joins within 2 s)
    void setStatus(Json state);                  // replaces; sent on the next cycle when it changed
    void event(const std::string& text);         // "YYYY-MM-DD HH:MM:SS.mmm  text" or plain text (time = now); dropped while offline (the log file is the record)
    void upload(const std::string& kind, const std::filesystem::path& file);   // log|screen; copied to the outbox, sent when connected
    std::vector<Command> takeCommands();         // for the app thread; empty when none
    void finish(const Command& command, bool ok, const std::string& text);
    enum class State { Offline, Connected, Unauthorized };
    State state() const; std::string lastError() const; std::chrono::steady_clock::time_point lastSeen() const;
    void setSettings(Settings settings);         // a new address/token/name: reconnects
};
std::string hostName();                          // this PC's name (GetComputerNameExW DnsHostname)
std::string accountName();                       // GetUserNameW
}
```
- `labeler_log.hpp`: `void setLogSink(std::function<void(const std::string& line)> sink);` — `logLine()`이 쓴 줄(시각 포함)을 그대로 넘긴다. `nullptr`로 끊는다.
- 동작: **보내기 스레드** 1초 주기 — Offline이면 10초마다 `GET /ping`, 성공 시 `POST /register`(401 → Unauthorized, 다시 시도 안 함; `setSettings` 뒤에만), 응답 `since` 뒤의 로그 줄(파일 끝에서 2,000줄 안)을 100줄씩 `POST /events`, 보관함 파일 업로드, 그다음부터 상태(바뀐 것만)·이벤트 묶음. 실패하면 Offline. **명령 스레드** — Connected 동안 `GET /commands?wait=25` 반복, 받은 것을 큐에. 두 스레드는 WinHTTP 세션 하나씩. 연결 3초·보내기 5초·받기 5초(명령 대기는 30초).

- [ ] **Step 1: 테스트 쓰기 (`tests/hub_client_tests.cpp`)** — 도우미 `struct HubProcess`: `python hub/hub.py serve --port 0 --storage <temp> --bind 127.0.0.1 --print-port`를 `CreateProcessW`로 띄우고 첫 줄에서 포트를 읽음, 소멸자에서 `TerminateProcess`; 제어 API 호출용 작은 WinHTTP 함수 `control(method, path, body) -> (status, Json)`(테스트 안에만). 시나리오:
  - `connects_registers_and_sends_status_and_events`: invite → `Client` 생성 → 3초 안에 `state()==Connected`; `setStatus({{"page","review"}})`, `event("확인 · 촬영 01 · 프레임 0003")` → `/api/workers`에 id `테스트@<hostName()>`와 status.page, `/events`에 그 문장.
  - `wrong_token_stops_retrying`: 틀린 토큰 → 3초 안에 `Unauthorized`, 그 뒤 15초 동안 허브 `workers.json`에 등록 없음(`/api/workers` 빈 배열).
  - `backfills_log_lines_since_last_event_without_duplicates`: 로그 파일에 2,500줄(시각 증가) 쓰고 그중 앞 500줄은 허브에 먼저 보내 둠 → 접속 뒤 `/events?limit=5000` 길이 == 2,000(마지막 2,000줄만) 그리고 겹친 줄 중복 없음.
  - `receives_a_command_and_reports_the_result`: `/api/workers/<id>/commands` notify → 3초 안에 `takeCommands()`에 하나 → `finish(cmd,true,"띄움")` → 명령 상태 `done`.
  - `offline_calls_return_at_once_and_outbox_uploads_later`: 허브를 끈 뒤(`HubProcess` 종료) `setStatus`·`event`·`upload("log", file)`가 50 ms 안에 돌아옴; 허브를 같은 저장소로 다시 띄우면 15초 안에 Connected가 되고 `/api/workers/<id>/files/log`에 그 파일.
  - 각 검사는 `wait_until(pred, seconds)` 도우미로 기다린다. Python이 없으면(`python --version` 실패) `SKIP: python` 출력 후 exit 0.
- [ ] **Step 2: 실패 확인** — 빌드 실패(헤더 없음).
- [ ] **Step 3: `src/hub_client.cpp` 구현** — WinHTTP(`WinHttpOpen(…, WINHTTP_ACCESS_TYPE_NO_PROXY)`, `WinHttpSetTimeouts(3000,3000,5000,5000)`, UTF-8 JSON 본문, `Authorization: Bearer` 헤더), 상태는 `std::atomic<State>`, 큐는 `std::mutex`, 스레드는 `std::jthread`와 `stop_token`(대기는 `condition_variable_any::wait_for`로 끊김). 로그 이어 보내기는 파일을 끝에서부터 읽어 2,000줄, 각 줄의 앞 23자(`YYYY-MM-DD HH:MM:SS.mmm`)가 `since`보다 크면 보냄. `labeler_log.cpp`의 `logLine`은 쓰고 나서 sink를 부른다(락 밖에서).
- [ ] **Step 4: 통과 확인** — `cmake --build build-labeler --config Release --target hub_client_tests` → `build-labeler\Release\hub_client_tests.exe` → `PASS: …` 한 줄, `ctest -R hub_client`.
- [ ] **Step 5: 커밋** — `git add src/hub_client.* tests/hub_client_tests.cpp src/labeler_log.* CMakeLists.txt && git commit -m "Hub client: knock, register, status, events with log backfill, commands and outbox over WinHTTP"`

### Task 5: 라벨러 연결 — 설정, 작업자 창, 상태·이벤트, 명령, 표시

**Files:**
- Create: `src/labeler_settings.hpp/.cpp`
- Modify: `src/native_labeler_projects.cpp:44-52, 266-280`(`deviceFile/loadDevice/saveDevice` → 새 설정 모듈 사용), `src/native_labeler_app.hpp/.cpp`, `src/native_labeler_main.cpp`, `CMakeLists.txt`(`boccia_labeler`에 `hub_client` 링크), `tests/native_labeler_tests.cpp`

**Interfaces:**
- Consumes: Task 4 `hub::Client`, Task 3 `versionString()`.
- Produces:
```cpp
namespace boccia::labeler {
struct LabelerSettings { bool gpu = true; std::string hubAddress = "mark-hong-pc.tail68c059.ts.net:8790", worker, token; };
std::filesystem::path settingsFile();                  // %LOCALAPPDATA%\Boccia\labeler-settings.json
LabelerSettings loadLabelerSettings();                 // missing file or keys → defaults
void saveLabelerSettings(const LabelerSettings&);      // atomicJson, keeps unknown keys
}
```
  - 파일 모양: `{"device":"gpu","hub":{"address":"…","worker":"홍길동","token":"…"}}`.
  - `Application`: 멤버 `std::unique_ptr<hub::Client> hub_; LabelerSettings settings_; bool workerDialog_=false; std::string hubNotice_;`, 메서드 `void startHub()`(설정에 worker가 있으면 Client 생성; 등록 콜백은 `{device:{gpu,cuda:gpuReady(),…}, summary:{project,path,id,target,reviewed,hold}}`), `void hubTick()`(tick 끝에서: 상태 JSON 만들어 `setStatus`, `takeCommands()` 처리), `void openWorkerDialog()`.
  - 작업자 창(`theme::beginDialog("작업자")`): 필드 `이름`, `초대 코드`, `허브 주소`(기본값 채움). 저장 → 설정 저장 → `startHub()`(이미 있으면 `setSettings`). 처음 켤 때(`!hidden && worker.empty()`) 자동으로 뜸, 프로젝트 메뉴 "작업자…"에서도.
  - 상태 JSON: `{"page":"data|review|export","scene":{"recording":"촬영 03","index":142}|null,"job":{"kind":"import|export|redraft|circles|propagate","text":"초안 212 / 498","fraction":0.43}|null,"apply_previous":bool,"device":"GPU|CPU","error":"…"}` — 바뀔 때만 보냄(Client가 비교).
  - 이벤트: `setLogSink`로 로그 줄 전부 + 다음 지점에 문장 추가(`logLine`): 장면 확인 `확인 · 촬영 03 · 프레임 0142 · 공 6개`, 보류 `보류 · 촬영 03 · 프레임 0131`, 공 추가 `공 추가 · 모노 A · 클릭 분할|브러시|폴리곤|박스`, 공 삭제 `공 삭제 · 컬러 C · 공 3`, 공 수정(윤곽 반영이 기존 공을 바꿀 때) `공 수정 · …`, ← 이전 적용 `이전 적용 켬|끔`, 전파 `전파 → · 반복 N장 · 초안 M장`(poll 완료 메시지 자리), 프로젝트 `프로젝트 열기 · 이름`/`프로젝트 닫기`, 시작 줄은 이미 있음(`시작 · …`에 `· 버전 <versionString()>` 추가).
  - 명령: `notify <문구>` → `hubNotice_`에 두고 아래 막대 왼쪽에 문구 + `×`(닫기); `log` → `hub_->upload("log", logFile())`; `ping` → `finish(ok,"pong")`; 모르는 명령 → `finish(false,"모르는 명령")`.
  - 아래 막대 오른쪽 끝(폴더 경로 왼쪽)에 점 하나: Connected 초록 + "허브", Unauthorized 주황 + "초대 코드 확인", Offline은 표시 없음(허브 주소가 있을 때만 점 영역을 둠).
  - 숨긴 점검: `--hub <주소> --worker <이름> --token <토큰>`(설정 파일 대신 이 값으로 Client 시작), `--view worker-dialog --capture <png>`.

- [ ] **Step 1: 테스트** — `tests/native_labeler_tests.cpp`: `loadLabelerSettings()`가 파일 없을 때 기본 주소를 주고, `saveLabelerSettings`가 `device`와 모르는 키(`"extra":1`)를 보존하는지(임시 `LOCALAPPDATA`로 `_wputenv_s`), 상태 JSON 함수가 `Application` 없이도 되게 `Json hubStatusJson(page, scene, job, applyPrevious, gpu, error)`를 `native_labeler_model.hpp`에 두고 모양 검사.
- [ ] **Step 2: 실패 확인** — 빌드 실패.
- [ ] **Step 3: 구현** — 위 인터페이스대로. `native_labeler_projects.cpp`의 `loadDevice/saveDevice`는 `LabelerSettings`를 쓰도록 바꾼다(동작 같음). `hubTick()`은 `tick()` 끝(`pumpThumbs();prefetch();` 뒤). `Application` 소멸자에서 `hub_.reset()` 먼저.
- [ ] **Step 4: 확인** — 빌드, `native_labeler_tests` PASS, `ctest -R "native_labeler|labeler_engine|labeler_project|hub_client"` 전부 통과. 허브를 `python hub\hub.py serve --storage <scratch>`로 띄우고 `boccia-hub.cmd invite 점검`으로 토큰을 받은 뒤, `build-labeler\Release\boccia_labeler.exe --workspace <QA 복사본> --smoke-hidden --view overview --hub 127.0.0.1:8790 --worker 점검 --token <토큰> --capture <png> --timeout 60` → `boccia-hub.cmd workers`에 `점검 · <PC> · 붙음`, `events 점검`에 `시작 · … · 버전 …`과 `원본과 저장 라벨 표시 중`; `--view worker-dialog --capture` 캡처 한 장.
- [ ] **Step 5: 커밋** — `git add src/labeler_settings.* src/native_labeler_* tests/native_labeler_tests.cpp CMakeLists.txt && git commit -m "Labeler joins the hub: worker name and token, status and events, notify/log/ping commands"`

### Task 6: 끝까지 돌리기, 문서, 배포 폴더

**Files:**
- Create: `hub/README.md`, `tools/hub-check.ps1`
- Modify: `NATIVE_LABELER.md`(새 절 "허브 (2026-10-xx)"), `tools/collect_release.ps1`(라벨러 배포에 `hub_client` 변경 반영은 exe 안이라 없음; 확인만), `.gitignore`(`/hub/__pycache__/`, `D:\Boccia Hub`는 저장소 밖)

**Interfaces:**
- Consumes: Task 1~5 전부.
- Produces: `tools/hub-check.ps1`: 스크래치 저장소로 허브를 띄우고, 라벨러 복사본 둘(`--worker 가`, `--worker 나`, 둘 다 `--smoke-hidden --view overview --timeout 90`)을 붙인 뒤 CLI로 `workers`(둘 다 붙음), `send 가 notify 쉬세요`(완료), `send 나 log`(파일 목록에 로그), `events 가`(시작 줄)를 확인하고 PASS/FAIL을 찍는다. 끝나면 허브와 라벨러를 종료한다.

- [ ] **Step 1: `tools/hub-check.ps1` 작성**(위 순서; 실패 조건마다 `throw`).
- [ ] **Step 2: 실행** — `powershell -File tools/hub-check.ps1` → `PASS: two labelers registered, notify delivered, log uploaded, events listed`.
- [ ] **Step 3: 문서** — `hub/README.md`(켜기 `boccia-hub.cmd serve`, 초대 `invite`, 보기 `workers/events`, 명령 `send`, 저장소 구조, 토큰·바인드), `NATIVE_LABELER.md`에 작업자 창·설정 파일 키·상태 막대 점·명령 셋·`--hub/--worker/--token/--version` 옵션.
- [ ] **Step 4: 배포 확인** — `tools/collect_release.ps1`로 라벨러 배포 → `bin\release\labeler\boccia_labeler.exe --version`이 새 빌드 시각. 허브는 저장소에서 바로 돈다(배포 폴더에 넣지 않음).
- [ ] **Step 5: 커밋** — `git add hub/README.md tools/hub-check.ps1 NATIVE_LABELER.md .gitignore && git commit -m "Hub stage 1: end-to-end check and docs"`

---

## 자기 검토

- **범위**: 설계 "구현 순서 1"의 항목(허브 최소: 등록·상태·이벤트·명령·로그 보관·화면 캡처 저장 → Task 1의 `/files/screen`; CLI → Task 2; 공용 클라이언트 → Task 4; 라벨러 연결·작업자 이름·토큰 → Task 5; 이 PC의 라벨러 둘 → Task 6). 화면(2단계), 내보내기 올리기(4), 버전(5), 트래커(6)는 뺌.
- **이름 일치**: `hub::Client`, `Settings`, `Command`, `Registration`, `setLogSink`, `LabelerSettings`, `loadLabelerSettings/saveLabelerSettings`, `versionString`, `hubStatusJson` — Task 4·5·3·6에서 같은 이름.
- **Review Focus**: 1 → Task 4 `wrong_token_stops_retrying` + Task 5 상태 막대 "초대 코드 확인"; 2 → Task 1 중복 테스트 + Task 4 backfill 2,000; 3 → Task 1 `received`; 4 → Task 1 두 PC; 5 → Task 1 한 번만.
- **길이**: 설계 문서보다 짧다. 코드 본문은 테스트 단언과 인터페이스뿐이다.
