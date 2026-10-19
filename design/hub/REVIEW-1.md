# PLAN-1 최종 검토 — Boccia Hub 1단계 (849e7a5..c5f2331)

검토 대상: `git show c5f2331:<파일>`의 커밋된 판. hub/hub.py, hub/cli.py, boccia-hub.cmd, hub/tests/*, src/hub_client.*,
tests/hub_client_tests.cpp, src/labeler_log.*, src/labeler_settings.*, src/native_labeler_hub.cpp, src/build_info.hpp,
cmake/build_info.cmake, CMakeLists.txt 변경, tools/hub-check.ps1, hub/README.md, NATIVE_LABELER.md 허브 절, 그리고
범위가 정해진 세 파일(app.hpp/.cpp, main.cpp, projects.cpp)의 허브·설정·버전 부분. 서버 → CLI → 클라이언트 → 라벨러 연결 →
빌드·문서 순으로 여러 번에 나눠 읽었다.

실행한 것: `python -m unittest hub.tests.test_hub hub.tests.test_cli` → 22개 OK(15.9초). C++ 빌드·실행·hub-check는 지시대로
하지 않았고 task-4-run.txt, task-5-build*.txt, task-6-tests.log, progress.md의 PASS를 근거로 삼았다. 판단 근거로
nlohmann 3.11.3 소스(json.hpp:1275, detail/output/serializer.hpp:513)와 MSVC 14.44 STL 소스(`<filesystem>`:1813-1827,
crt/src/stl/filesystem.cpp:269-281, syserror_import_lib.cpp:38-68)를 읽었고, 이 PC의 ACP가 949임을 확인했다.

참고: 작업 복사본의 `src/hub_client.cpp`는 HEAD와 다르다(Task 5 Ruling의 "첫 바퀴는 기다리지 않음"이 커밋되지 않음 — I1).

---

### Strengths

- **허브(hub.py)**: 표준 라이브러리만으로 깔끔하다. workers/tokens는 임시 파일 + `os.replace`로 원자적 교체, 명령은
  append-only `commands.jsonl`(마지막 줄이 최신 상태)이라 재시작에도 "보냄"이 유지된다. 토큰은 SHA-256만 저장한다.
  `received`를 작업자별로 겹치지 않고 늘어나게(+1 ms) 찍어 `since` 페이지가 빠짐·반복 없이 된다(Ruling 타당).
  파일 이름 검사가 URL 디코드 뒤에 이뤄지고(`..%5c..%5cevil` 테스트), 올리는 중 파일은 `~`로 숨긴다. Windows에서
  `SO_REUSEADDR`를 끈 것(두 번째 허브가 같은 포트를 조용히 잡지 않게)도 좋다. 세션 교대(409)와 재시작 보존이 테스트된다.
- **CLI**: 한글 폭을 맞춘 표, 이름/이름@PC 해석과 exit 2, 연결 실패 한 줄 + exit 1. 테스트가 실제 허브를 상대로 돈다.
- **클라이언트(hub_client)**: 화면 스레드에서 HTTP를 하지 않고, 스레드마다 WinHTTP 세션, 프록시 없음, 타임아웃 설정,
  `Core`를 shared_ptr로 두어 detach된 스레드도 안전하다. 진행 중인 노크 도중 설정이 바뀌어도 `settingsChanged`로 다음
  바퀴에서 바로잡힌다. 결과 재전송은 "응답 없음"일 때만. X-Boccia-Pc를 퍼센트 인코딩.
- **라벨러 연결**: 허브 코드를 `native_labeler_hub.cpp` 하나에 모음, 등록 내용은 `HubShared` 복사본(잠금)으로 읽고 로그
  싱크는 `weak_ptr`만 잡는다(Ruling 타당). 설정 모듈은 모르는 키를 지키고 `{"device": …}`만 있던 옛 파일과 호환되며
  테스트가 있다. 숨긴 실행은 설정 파일을 읽지도 쓰지도 않고(`--hub`는 메모리에만), `saveDevice`도 load-modify-save로
  허브 키를 지킨다. 붙여 넣은 코드의 앞뒤 공백·줄바꿈을 자른다.
- **테스트·점검**: C++ 테스트가 가짜가 아니라 실제 `hub.py` 프로세스를 상대로 한다. hub-check.ps1은 UTF-8 BOM,
  `[Console]::OutputEncoding`, `PYTHONIOENCODING`까지 챙겼다. Ruling마다 이유와 비용을 적은 것도 좋다.

---

### Issues

#### Critical (Must Fix)

**C1. 잘못된 UTF-8 한 줄이 라벨러를 죽이고, 이어 보내기 때문에 허브가 켜져 있는 한 다시 켤 때마다 또 죽는다**

- 위치: `src/hub_client.cpp:220`(postEvents `Json{{"events", batch}}.dump()`), `:262`(상태 `state_.dump()`), `:269`(결과),
  `:296`(등록 `body.dump()`). 스레드 함수 `sendLoop`(:310-335)·`pollLoop`(:337-379)에 바깥 try/catch가 없다.
- 무엇: nlohmann 3.11.3의 `dump()` 기본값은 `error_handler_t::strict`라서 문자열에 잘못된 UTF-8이 있으면
  `type_error.316`을 던진다. 보내기 스레드에서 던져지면 `std::thread` 밖으로 나가 `std::terminate` → 라벨러 프로세스 종료.
- 어떻게 들어오나: 로그 줄과 상태의 `error`는 예외 문구를 그대로 쓴다 — `native_labeler_app.cpp:752`("오류 · "+error_),
  `native_labeler_projects.cpp:416`("임포트 복사 실패 · "+what()), `native_labeler_hub.cpp:160`(상태의 error_),
  `:106`(등록의 gpuProblem). MSVC의 `std::filesystem_error::what()`은 경로를 `__std_fs_code_page()` = CP_ACP로 바꾸고
  (라벨러에는 `setlocale(".UTF-8")`도 activeCodePage 매니페스트도 없다), en-US 메시지 자원이 없으면 시스템 언어 메시지를
  `FormatMessageA`로 넣는다. 이 PC의 ACP는 949다. 한글 경로(프로젝트·내보내기 폴더)가 든 filesystem 오류 하나 — 가져오기의
  `fs::rename`/`fs::space`/`fs::create_directories`(백신이 파일을 잡아 rename이 실패하는 흔한 경우), `labeler_export.cpp:31`의
  `create_directories` 등 — 면 CP949 바이트가 로그 줄과 상태에 들어간다. 또 `tailLines`(:79-88)는 `\n`으로 끝나지 않은
  마지막 조각(쓰는 중인 줄)도 줄로 넣으므로, 버퍼보다 긴 줄을 쓰는 순간 읽으면 UTF-8 중간에서 잘린 줄이 생길 수 있다.
- 왜 Critical: 계획의 첫 제약은 "앱은 허브가 없어도 절대 멈추지 않는다"인데 여기서는 허브가 *켜져 있어서* 멈춘다.
  그 줄은 허브에 저장되지 않았으므로 `since`보다 뒤다 → 다시 켜면 첫 노크 뒤 이어 보내기가 같은 줄을 다시 보내다 또
  죽는다. 그 줄이 마지막 2,000줄 밖으로 밀리거나 날짜가 바뀔 때까지(보통 그날 내내) 허브가 켜져 있으면 라벨러를 쓸 수
  없고, 확인하지 않은 장면의 편집을 잃는다.
- 고칠 것: (1) hub_client.cpp의 모든 dump를 `dump(-1, ' ', false, Json::error_handler_t::replace)`로. (2) 두 스레드의 한
  바퀴를 try/catch로 감싸 예외를 `fail()`(Offline)로. (3) `tailLines`는 파일이 `\n`으로 끝나지 않으면 마지막 조각을 버림.
  (4) 원인 쪽: 라벨러 시작에 `setlocale(LC_ALL, ".UTF-8")`(MSVC STL이 filesystem_error 경로를 UTF-8로 냄 — ImGui의 '?'도
  해결) 또는 what()을 UTF-8로 바꾸는 도우미. 테스트: `client.event("오류 · \xBA\xB8")`, 그리고 같은 바이트가 든 로그 줄의
  이어 보내기 → 클라이언트가 살아 있고 허브에 U+FFFD로 저장되는지.

#### Important (Should Fix)

**I1. HEAD(c5f2331)는 깨끗한 체크아웃에서 라벨러가 빌드되지 않고, 커밋은 시험·배포한 빌드와 다르다** (병합 전 필수)

- 근거: `src/native_labeler_app.hpp:3`은 `labeler_workspace.hpp`, `src/native_labeler_projects.cpp:4-5`는
  `labeler_detector.hpp`·`model_paths.hpp`, `src/native_labeler_main.cpp:5`는 `model_paths.hpp`를 include하는데 셋 다 git에
  없다. `src/native_labeler_hub.cpp:106`의 `gpuProblem()`·`yoloCudaRuntime()`은 커밋된 어떤 헤더에도 선언이 없다.
  커밋된 `CMakeLists.txt:122,124,187,189`는 `src/native_labeler_projects.cpp`도, Task 4의 산출물인 `src/labeler_log.cpp`
  (`setLogSink`)도 빌드하지 않는다(작업 복사본 CMakeLists.txt:190·260에만 있음). Task 5 Ruling의 "첫 바퀴는 기다리지
  않음"(`bool first`)은 작업 복사본 `src/hub_client.cpp`에만 있다(파일 시각 23:00, 커밋 2d12056은 23:04; 계획의 Task 5
  `git add` 목록에 hub_client.cpp가 없다). 기준 849e7a5에는 이런 참조가 없었으므로 이 범위의 커밋(주로 2d12056)이
  브랜치를 빌드 불가로 만들었다.
- 왜: progress.md의 PASS와 배포된 `bin/release/labeler/boccia_labeler.exe`(23:08, 빌드 결과와 같은 파일)는 작업 복사본에서
  나왔고 커밋으로는 재현되지 않는다. Task 3("include 순서 차이")·Task 5("한 커밋에 섞임") Ruling이 적은 비용보다 크다 —
  이 Ruling들은 틀렸다. 이대로 main에 합치면 main이 빌드되지 않는다.
- 고칠 것: 허브 커밋이 기대는 파일(labeler_workspace.*, labeler_detector.*, model_paths.*, labeler_sam.* 변경, CMake 소스
  목록)을 사용자가 정해 커밋하거나 허브 커밋이 그것 없이 서게 하고, hub_client.cpp 변경을 커밋한 뒤
  `git worktree add <temp> HEAD`에서 깨끗이 빌드 + `ctest`로 확인.

**I2. 이어 보내기가 다른 작업자(다른 프로세스)의 줄을 이 작업자의 활동으로 올린다**

- 위치: `src/hub_client.cpp:226-237`(since 뒤의 마지막 2,000줄을 누가 썼든 보냄), `src/labeler_log.cpp:28-45`(로그는 프로그램
  폴더 + 날짜 하나 — 작업자·프로세스 구분 없음), `src/native_labeler_hub.cpp:65-74`(이름을 바꾸면 `setSettings` → 새
  식별자로 등록 → since "" → 마지막 2,000줄 전부).
- 무엇: 공용 PC에서 홍길동이 오전에 일한 뒤 김민수가 작업자 창에서 이름을 바꾸면, 김민수@PC로 등록하자마자 오늘 로그의
  마지막 2,000줄(홍길동의 확인·공 추가 …)이 김민수의 활동으로 저장된다. 다시 홍길동으로 바꾸면 홍길동의 since 뒤
  줄(김민수의 오후 작업)이 홍길동에게 간다. 같은 PC의 라벨러 창 둘도 같은 파일을 쓴다. hub-check.ps1의 두 숨긴 라벨러도
  같은 `build-labeler\Release\logs\labeler-<날짜>.log`를 쓰므로 각자의 첫 이어 보내기가 상대와 그날 앞선 점검들의 줄을
  싣는다(task-6-tests.log의 가·나 `마지막`이 23:11:09로 같은 것과도 맞는다). 그래서 `events 가`의 확인은 나의 줄로도
  통과할 수 있다.
- 왜: 허브의 목적인 "작업 진행 관리"(작업자별 확인 수·활동)가 공용 PC에서 틀린다. 식별자를 이름@PC로 정한 설계(결정 6)와
  메뉴의 "작업자" 항목이 이름 바꾸기를 정상 동작으로 만든다.
- 고칠 것: 이어 보내기를 현재 작업자의 줄로 제한 — 예: saveWorker가 이미 쓰는 `작업자 · <이름>` 줄을 경계로, 마지막
  경계가 현재 이름이 아니면 그 뒤만 보냄(또는 작업자 이름을 로그 파일 이름이나 각 줄에 넣음). 숨긴 `--hub` 실행은 자기
  로그 파일을 쓰게(아래 M13과 함께).

**I3. 제어 API(`/api/`)에 인증도 출처 확인도 없어, 작업자 PC와 같은 망의 누구나 초대·명령·로그 읽기를 할 수 있다** (계획 문제)

- 위치: `hub/hub.py:417-418, 450-472`(경로로만 제어/앱을 가름, `client_address`·Host·Origin 확인 없음), `:492-500`
  (Content-Type을 보지 않고 JSON으로 읽음), `:546-555`(`--bind all` = 0.0.0.0), `hub/README.md:13, 65`.
- 무엇: 설계 "보안"은 "명령을 만드는 쪽은 localhost와 Tailscale에서만, 밖에서 온 요청으로는 명령을 만들 수 없다"면서
  작업자 PC를 같은 tailnet에 tag:boccia로 넣고 "허브 PC의 8790만 보게" 한다. 계획은 두 면을 한 포트에 두었으므로 작업자
  PC(그 위의 누구든 curl로)가 `POST /api/invites`로 아무 작업자의 토큰을 바꿔(옛 것 무효 → 그 사람은 "초대 코드 확인"에
  갇힘), `POST /api/workers/<id>/commands`로 남의 라벨러에 문구를 띄우고, 남의 로그를 받을 수 있다. `--bind all`이면 LAN
  전체가 그렇다(README:65는 "Tailscale 밖에서는 닿지 않습니다"라고 함). 허브 PC의 브라우저에서 연 웹 페이지도
  `fetch(…, {mode: "no-cors", body: …})`(text/plain이라 사전 요청 없음)로 같은 일을 할 수 있다(브라우저의 로컬 네트워크
  차단에만 기댐).
- 왜: 1단계 명령은 알림·로그·ping이라 피해는 토큰 무효화·가짜 알림·로그 열람이지만, 5단계 `update`가 같은 통로로 오면
  설계가 지키려던 "작업자 PC에 아무 실행 파일이나 심기"가 된다. 2단계 화면이 이 API 위에 지어지기 전에 정해야 한다.
- 고칠 것: 제어 API를 loopback 전용 별도 포트로(원격 보기는 `tailscale serve`로 그 포트만, ACL은 작업자 태그에 8790만),
  또는 `/api/`에서 `client_address`가 loopback이거나 허용한 Tailscale 기기(`tailscale whois`로 태그 확인)인지 검사.
  `/api/` POST는 `Content-Type: application/json`을 요구하고(교차 출처에 사전 요청 강제) Host를 확인. README에
  `--bind all`의 의미를 적는다.

**I4. 허브에 붙은 라벨러를 닫으면 창이 2초 멈춘다**

- 위치: `src/hub_client.cpp:387-399`(`~Client`가 두 스레드를 최대 2초 기다림), `:351`(명령 스레드는 거의 항상 25초 대기
  요청 안에 있고 그것을 끊을 수단이 없음), `src/native_labeler_app.cpp:38`(`~Application`에서 `hub_.reset()`),
  `src/native_labeler_main.cpp:207-208`(창은 Application이 사라진 뒤에 닫힘), `src/native_labeler_hub.cpp:37`(이름을 비우고
  저장해도 화면 스레드에서 같은 2초).
- 무엇: 붙어 있는 동안의 닫기는 매번 2초를 다 기다린 뒤 명령 스레드를 detach한다. 테스트도 `< 2.5 s`로 이 2초를
  받아들인다(`tests/hub_client_tests.cpp:159-161`).
- 왜: "허브 때문에 화면이 멈추지 않는다"는 계획 제약의 취지에 어긋나고, 매일 닫을 때마다 보인다.
- 고칠 것: 진행 중인 요청 핸들을 Core에 잠금 아래 두고 소멸자에서 `WinHttpCloseHandle`로 끊거나(동기 호출이 취소되어
  돌아옴), 명령 스레드는 기다리지 않고 바로 detach(Core만 만지므로 안전). 테스트 기준을 예: 0.5초로.

**I5. 집중 검토 1의 자동 확인이 실패할 수 없다**

- 위치: `tests/hub_client_tests.cpp:101-105`.
- 무엇: 클라이언트는 끝까지 "wrong-token"을 쓰므로 10초마다 다시 노크하는 클라이언트라도 계속 401을 받아 등록되지
  않는다 → `/api/workers`는 어느 쪽이든 비어 있다. 지금 코드는 맞다(`sendLoop`는 Offline일 때만 노크하고 Unauthorized는
  `setSettings`까지 유지 — hub_client.cpp:297, 327-331, 435-437). 그러나 회귀를 잡지 못한다. 상태 막대의 주황 "초대 코드
  확인"(Task 5 몫)도 캡처·테스트가 없다.
- 고칠 것: 재시도가 보이게 — 예: Unauthorized가 된 뒤 같은 포트·저장소로 허브를 다시 띄우되 `tokens.json`에
  sha256("wrong-token") → "테스트"를 넣어 둔다. 재시도하는 클라이언트라면 10초 안에 등록되고, 맞는 클라이언트는 그대로
  빈 목록. 주황 상태 캡처 한 장(`--hub … --token <틀린 값> --capture`).

#### Minor (Nice to Have)

- **M1** `src/hub_client.cpp:232` `time <= since`: since와 같은 밀리초의 줄은 보내지 않는다. 허브가 (시각, 문장)으로
  거르므로 `<`가 손실이 없다. 또 `logLine`은 시각을 잠금 밖에서 찍어(`src/labeler_log.cpp:48-58`) 여러 스레드의 줄이
  파일에 시각 역순으로 들어갈 수 있고, since보다 앞선 시각의 줄은 빠진다. `<`로, 시각은 잠금 안에서.
- **M2** 부분 겹침 이어 보내기가 테스트되지 않는다(Task 4 Ruling — 이 Ruling은 틀렸다). 계획의 숫자(2,500줄 중 500줄 먼저
  → 2,000)는 틀렸지만 고칠 것은 기대값(전체 2,500, 501~2,500 각 한 번)이었지 시나리오를 빼는 것이 아니었다. 지금 since
  자르기는 "전부 앞" 경우로만 확인된다.
- **M3** 이벤트를 1초마다 묶지 않는다: 보내기 스레드가 이벤트 하나마다 깨어 보낸다(`src/hub_client.cpp:318-321`). 계획은
  "1초마다 묶어". 한가할 때 로그 한 줄 = POST 하나, DERP 중계에선 요청 수만 늘어난다.
- **M4** 줄바꿈이 든 로그 문장(여러 줄 오류 메시지)은 실시간 이벤트에선 전체, 이어 보내기에선 첫 줄만(나머지는 시각이
  없어 버려짐) → 겹치는 구간에서 서로 다른 (시각, 문장)으로 둘 다 저장된다. `logLine`에서 `\n`을 공백으로.
- **M5** 다시 등록해도 상태를 다시 보내지 않는다(`knock`이 `statusDirty`를 세우지 않음, `src/hub_client.cpp:290-307`).
  허브 주소를 바꾸거나 저장소가 빈 허브에 붙으면 화면이 바뀌기 전까지 그 허브엔 상태가 없다(한가한 라벨러는 계속).
- **M6** `--version`의 git 해시가 바뀐 작업 복사본의 빌드도 깨끗한 커밋처럼 보인다(배포 exe는 `2d12056`이라 하지만 그 뒤의
  작업 복사본에서 빌드; 이 저장소는 늘 dirty). 5단계 버전 관리의 근거이므로 `git describe --always --dirty` 또는
  `git status --porcelain`이 비지 않으면 `+`. 또 `sayVersion`(`src/native_labeler_main.cpp:23-25`)은 UTF-8 "·"를 CONOUT$에
  바이트로 써서 CP949 콘솔에선 깨진다.
- **M7** `hub_client_tests`는 허브가 안 떠도(예: hub.py 오류) "SKIP: python"을 찍고 exit 0(`tests/hub_client_tests.cpp:99-100`)
  → ctest가 통과로 본다. 계획대로 `python --version`을 따로 확인.
- **M8** 같은 작업자의 라벨러 창 둘(한 PC)은 서로 다시 등록하며 10초마다 409로 상대의 대기를 끊는다(Task 4 Ruling의 비용).
  상태가 두 창 사이에서 번갈아 바뀌고 명령은 아무 창에나 가며 등록마다 이어 보내기를 다시 돈다. 3단계 명령(화면·내보내기)
  전에 "다른 곳에서 접속" 상태나 인스턴스 구분이 필요하다.
- **M9** 허브가 400으로 거절하는 이름(`/:*?"<>|@`, 64자 초과 — `hub/hub.py:171-172`는 토큰 확인보다 먼저)이나 주소 오타는
  Offline으로 남아 아무 표시 없이 10초마다 노크한다(`src/hub_client.cpp:297`은 401/403만 거절 상태로). 작업자 창에서 허브와
  같은 규칙으로 막거나 /register의 400도 거절 상태로.
- **M10** 작업자 창이 이름이 빈 동안 켤 때마다 뜬다(`src/native_labeler_app.cpp:34`) — 설계는 "처음 켤 때 한 번 묻는다".
  창의 "초대 코드가 맞지 않습니다"(`src/native_labeler_hub.cpp:93`)는 문장이다(계획: 라벨·숫자·버튼만; 상태 막대처럼
  "초대 코드 확인").
- **M11** 허브 알림이 아래 막대 왼쪽의 진행·메시지(바쁨 표시, "이전 적용 · 프레임 0012" 출처 등)를 닫을 때까지 가린다
  (`src/native_labeler_app.cpp:1183-1190`). 긴 문구는 잘리지 않아 오른쪽 표시와 겹친다.
- **M12** `log` 명령은 파일을 보관함에 복사만 하고 바로 `완료`를 보고한다(`src/native_labeler_hub.cpp:164-167`). 그 뒤 5xx가
  아닌 거절(401·413 …)이면 보관함 파일을 지운다(`src/hub_client.cpp:249-251`) — 토큰을 바꾸는 사이의 401도 영구 거절 취급.
- **M13** 숨긴 `--hub` 실행도 사용자의 실제 보관함 `%LOCALAPPDATA%\Boccia\hub-outbox`를 쓴다(`src/native_labeler_hub.cpp:43-46`).
  올리기 전에 끝난 점검의 파일은 사용자의 실제 라벨러가 다음 접속 때 사용자 이름으로 실제 허브에 올린다. `--hub`면 스크래치
  보관함을.
- **M14** 허브 비용이 쌓인 양에 비례한다: `events()`는 부를 때마다 그 작업자의 모든 events 파일을 전역 잠금 아래 읽고
  (`hub/hub.py:264-271`; CLI `--follow`는 2초마다), `_seen`은 지금까지의 모든 (시각, 문장)을 메모리에(`:132-145`),
  `take_commands`는 깰 때마다 모든 명령을 훑는다(`:301`). 1단계엔 충분하지만 2단계 화면이 주기적으로 부르면 앱 요청이 막힌다.
- **M15** CLI 출력을 파이프로 받으면 UTF-8인데 PowerShell 5.1은 949로 읽는다 → `boccia-hub.cmd workers | Select-String 홍길동`,
  `$x = boccia-hub.cmd …`에서 한글이 깨진다(hub-check.ps1은 스스로 `[Console]::OutputEncoding`을 바꿔 가려짐). 사용자 터미널이
  PowerShell이므로 README에 적거나 리다이렉트일 때 처리.
- **M16** `Handler`에 소켓 타임아웃이 없다: WinHTTP가 풀에 둔 연결마다 허브 스레드가 하나씩 남고, 잠자기·망 끊김으로 사라진
  클라이언트의 스레드는 영원히 남는다. `timeout`(예: 120초, 명령 대기보다 길게).
- **M17** 스레드 수명 세부: (a) 2초 뒤 detach된 보내기 스레드는 `stop` 확인 없이 `knock`→`backfill`을 이어가 main이 끝난 뒤
  `logFile()`(labeler_log의 정적 객체)을 부를 수 있다 — 단계 사이마다 `stop` 확인. (b) `state`를 잠금 밖에서 바꾼 뒤
  notify하므로(`src/hub_client.cpp:215, 302-303`) 명령 스레드가 깨움을 놓칠 수 있다(라벨러에선 다음 `setStatus` 0.5초 안에
  회복) — 잠금 안에서 바꾸고 notify.

---

### Rulings 평가

| Ruling | 판단 |
| --- | --- |
| worktree 없이 공유 폴더에서 작업 | 사정상 이해되지만 I1(커밋이 시험한 빌드를 재현하지 못함)의 뿌리. 비용 추정이 작다. |
| T1 X-Boccia-Pc로 PC 지정 | 타당. 한 토큰이 여러 PC를 쓰는 설계와 맞다. |
| T1 `received` +1 ms | 타당. since 페이지가 정확해진다. |
| T2 `지금`=status.text, `마지막`=since | 타당. |
| T3 생성 헤더 이름 build_stamp.hpp | 타당. |
| T3 작업마다 자기 hunk만 커밋 | **틀림** — T5의 통째 커밋과 합쳐져 HEAD가 빌드되지 않는다(I1). |
| T4 세션 id + 409 | 바뀐 인스턴스 문제에는 타당. 창 둘의 핑퐁 비용(M8)은 1단계에선 받아들일 만. |
| T4 Session/request를 헤더에 | 타당. |
| T4 이어 보내기 테스트에서 먼저 보내기 제거 | **틀림** — 기대값을 고쳤어야 했다(M2). |
| T4 명령은 많아야 한 번 | 집중 검토 5가 요구하는 동작. 타당. |
| T4 labeler_log 통째 커밋 | 내용은 괜찮으나 커밋된 CMake가 그것을 빌드하지 않는다(I1). |
| T5 native_labeler_hub.cpp 분리 | 타당. |
| T5 shared_ptr hub_ + HubShared | 타당, 좋은 결정. |
| T5 첫 바퀴 대기 없음 + hubSettled | hubSettled는 커밋됨, hub_client.cpp 변경은 커밋 안 됨(I1). |
| T5 상태에 text | 타당. |
| T5 app/main/projects 통째 커밋 | **틀림** — 커밋되지 않은 헤더·심볼에 기대 HEAD가 빌드되지 않는다(I1). |
| T6 .gitignore 그대로 | 타당(`__pycache__/` 이미 무시). |
| T6 hub-check는 시작 화면 + `--hub-wait` | 받아들일 만. 다만 두 라벨러가 로그 하나를 같이 써 I2를 가린다. |
| T6 NATIVE_LABELER.md 통째 + 설계 문서 커밋 | 타당. |

---

### Declined to judge

- app.hpp/.cpp·main.cpp·projects.cpp·NATIVE_LABELER.md의 허브 밖 변경 — 지시된 범위 밖(T5·T6 Ruling의 이전 작업).
- 동작마다 "알림 · …" 줄이 하나 더 생겨 허브 활동이 겹쳐 보이는 것 — 이전 작업의 로깅(범위 밖). 그 줄이 허브를 죽이는 경로는 C1에서 다룸.
- 숨긴 `--create-ui-test`가 `saveDevice`로 사용자의 실제 설정 파일의 `device`를 바꾸는 것 — 허브 이전부터의 동작이고, 이번 변경은 허브 키를 지키게 했다.
- 날짜를 넘긴 이어 보내기(다음 날 다시 켜면 전날 파일의 줄은 안 감) — 계획이 `logFile` 하나로 범위를 정함(설계와의 차이는 Recommendations).
- 업로드 개수 제한(설계 "요청 크기·개수 제한") — 계획 Task 1은 크기(50 MB)만 요구.
- 라벨러와 트래커가 같은 이름@PC로 부딪힘 — 6단계 범위(Recommendations).
- 명령 만료·취소가 없어 "보냄"이 영원히 남음 — 1단계 계획 밖, 집중 검토 5가 이 동작을 요구.
- 허브 저장소 기본값 `D:\Boccia Hub`(D: 여유 공간) — 설계 결정 5.
- Windows 예약 이름(CON·NUL 등)이 파일 이름 규칙을 통과해 400 대신 500 — 저장소 밖으로 못 나가고 피해 없음.
- 작업자 창에서 초대 코드가 평문으로 보임 — 설계가 한 번 붙여 넣는 코드로 정함.
- 로그인 때 Tailscale보다 먼저 뜬 허브는 127.0.0.1만 묶음 — 자동 시작은 1단계 밖.
- 같은 이름 + 같은 호스트 이름(복제 이미지) PC의 충돌 — 드물고 설계의 식별자 규칙 그대로.
- 500 응답에 예외 문구를 실어 보냄 — tailnet 안, 정보 가치 낮음.
- CLI `events`·`workers`가 날짜 없이 시:분(:초)만 — 계획이 정한 출력 모양.
- 이름에 `%xx`가 들어가면 CLI 경로에서 디코드되어 404 — 드묾.

---

### Review Focus coverage (1–5)

1. **틀린 토큰** — 코드 경로: `knock()`이 401/403에 Unauthorized(`src/hub_client.cpp:297`), `sendLoop`는 Offline일 때만 노크
   (`:327-331`), 벗어나는 길은 `setSettings`뿐(`:435-437`), 명령 대기 401도 Unauthorized(`:374`); 상태 막대 주황 "초대 코드
   확인"(`src/native_labeler_app.cpp:1202-1209`), 작업자 창 저장 → `startHub` → `setSettings`. 테스트:
   `wrong_token_stops_retrying`은 3초 안에 Unauthorized까지는 확인하지만 "다시 시도하지 않음"은 실패할 수 없는 확인이다
   (I5). 주황 상태의 캡처·테스트 없음. → **코드는 맞음, 테스트 공백.**
2. **허브가 꺼진 채 몇 시간 뒤 켬** — `tailLines(file, 2000)` + `time <= since`(`src/hub_client.cpp:226-237`), 허브의 (시각, 문장)
   중복 제거(`hub/hub.py:240-262`)와 늘어나는 `received`(`:147-155`). 테스트: `test_events_dedupe_and_keep_received_time`,
   `test_state_survives_a_restart`, C++ `backfills_log_lines_since_last_event_without_duplicates`(정확히 마지막 2,000줄, 다시
   붙어도 0). 공백: 부분 겹침 미검증(M2), 같은 밀리초 줄 손실(M1), 다른 작업자 줄 섞임(I2), 잘못된 UTF-8 줄에서 죽음(C1).
   → **기본 경로는 덮임, 위 네 공백.**
3. **두 PC의 시계 다름** — `add_events`가 `time`과 `received`를 둘 다 저장, `events()`는 받은 순서. 테스트:
   `test_events_listed_by_received_time_with_since_and_limit`(옛 시계의 줄이 나중에 오면 끝에), `test_events_dedupe_and_keep_received_time`.
   → **덮임.**
4. **같은 이름, 두 PC** — 식별자 `이름@PC`(`hub/hub.py:179`), X-Boccia-Pc로 요청 귀속(`:191-202`). 테스트:
   `test_same_name_two_pcs_are_two_workers`, `test_pc_header_picks_the_pc_when_one_token_serves_two`, CLI
   `test_ambiguous_name_lists_workers_and_exits_2`. → **덮임.** (같은 PC의 창 둘은 M8.)
5. **명령 처리 중 죽음** — `take_commands`가 응답 전에 queued → sent로 바꾸고(`hub/hub.py:306-308`) "sent"는 다시 주지 않음;
   `_load_commands`가 재시작 뒤에도 마지막 상태 유지. 테스트: `test_command_longpoll_is_delivered_once`(두 번째 대기는 빈 목록,
   결과 전까지 "sent"). 재등록·재시작 뒤 다시 안 주는 것은 별도 테스트 없이 코드로 분명. → **덮임.**

---

### Recommendations

1. C1을 먼저(작은 수정 + 잘못된 UTF-8 테스트 두 개). 허브가 켜져 있으면 라벨러가 죽는 길은 joonyeon에 배포하기 전에 막아야 한다.
2. 병합 전에 브랜치가 깨끗한 체크아웃에서 빌드되게(I1) — 어떤 미커밋 파일을 같이 커밋할지는 사용자가 정할 일이므로 물어볼 것.
3. 2단계 화면을 `/api/` 위에 짓기 전에 제어면 분리 방식을 정한다(I3). 별도 loopback 포트가 가장 단순하다.
4. 이어 보내기의 작업자 경계(I2)와 닫을 때 2초(I4)를 고치고, I5의 테스트를 실패할 수 있게.
5. 계획에 대한 메모(다음 단계 전에): 식별자에 앱 종류가 없어 6단계에서 라벨러와 트래커가 같은 이름@PC로 세션을 뺏는다;
   설계의 "떨어져 있던 동안의 동작은 로그 파일이 기록"은 하루를 넘기거나 이름이 바뀌면 지켜지지 않는다(계획이 파일 하나로
   좁힘); 버전 문자열에 dirty 표시가 필요하다(M6, 5단계 롤백의 근거); 계획의 Task 5 `git add` 목록이 Ruling으로 바뀐 파일을
   따라가지 못했다.

---

### Assessment

**Ready to merge?** With fixes

**Reasoning:** 허브·CLI·클라이언트·라벨러 연결은 계획대로 끝까지 돌고 있는 테스트도 통과하지만, 내용에 따라 라벨러가 죽고
다시 켤 때마다 또 죽는 C1을 고쳐야 하고, HEAD가 깨끗한 체크아웃에서 빌드되지 않으므로(I1) 그대로는 합칠 수 없다. 제어 API
보안(I3)과 작업자 귀속(I2)은 연구소 PC에 배포하기 전에 정리해야 한다.
