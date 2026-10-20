# PLAN-2 최종 검토 — Boccia Hub 2단계 (2991269..e7e6afc)

검토 대상은 커밋 여섯 개(fddaad2 · aa5e03a · 8bab00d · 71f6d48 · 9e7b63a · e7e6afc)의 커밋된 판(`git show e7e6afc:<파일>`)이다.
기준은 계획 `design/hub/PLAN-2.md`, 설계 `design/hub/DESIGN.md`("화면", "구현 순서" 2), 1단계 리뷰 `design/hub/REVIEW-1.md`, 원장
`.superpowers/sdd/PLAN-2/progress.md`의 `Ruling:` 줄이다. 허브(Task 1) → 라벨 파일(Task 2) → 라벨러(Task 3) → 화면(Task 4) →
exe(Task 5) → 점검·배포·문서(Task 6) 순으로 여러 번에 나눠 읽었다.

실행한 것은 `python -m unittest hub.tests.test_hub hub.tests.test_cli` 하나다. 34개가 OK였다(25.4초).
C++ 빌드·실행, `test_ui`, `tools/*.ps1`, GUI 프로그램은 지시대로 돌리지 않았다. 그 결과는 task-*-tests.log와 progress.md의 PASS 줄을 근거로 삼았다.

읽기만 해서 확인한 것:
- `C:\Program Files\Tailscale\tailscale.exe`의 PE 서브시스템은 3(콘솔), `pythonw.exe`는 2(GUI)다.
- Python 3.12 `Lib/subprocess.py:1509-1511`은 `shell=True`일 때만 SW_HIDE를 넣는다.
- 이 PC의 PATH에 `C:\Program Files\Tailscale`가 있다.
- 실제 시작프로그램 폴더에 `Boccia Hub.lnk`가 없다. `%LOCALAPPDATA%\Boccia`에는 `hub-settings.json`도 `hub-webview`도 없다.
- `%TEMP%`에 `boccia-hub-check-*`가 6개 있고, 하나에 약 10 MB다.
- 작업 복사본의 `src/labeler_export.cpp`와 `src/labeler_native.cpp`를 읽었다. 미커밋 백로그와의 관계를 보려는 것이다.
- 캡처 세 장 `design/hub/shots/hub-{list,activity,progress}.png`를 보았다.

---

### Strengths

- **허브 캐시(Task 1)**
  - 1단계에서 `received`가 작업자별로 겹치지 않고 늘어나게 했다. 그 위에서 `bisect_right`로 `since` 자리를 찾으므로, 2초마다 부르는 비용이 쌓인 줄 수와 무관하다.
  - 자를 때는 1,000줄씩 묶어 자르고 `seen`도 같이 비운다. 다시 켜면 파일에서 다시 읽고, 시계(`clock`)도 마지막 줄에서 이어 받는다.
  - `test_cache_reloads_after_restart`와 `test_events_fast_with_many`가 이를 덮는다.
- **정적 파일과 끄기**
  - 이름은 URL을 디코드한 뒤 정해진 문자로만 검사하고, `.`로만 된 이름은 거부한다. 화면도 제어 API와 같은 주소 제한(403)을 받고, 모두 `no-store`다.
  - `/api/shutdown`은 답을 보낸 뒤 다른 스레드에서 끈다. `shutdown()`은 두 번 불려도 된다. pythonw에서 `print`가 조용히 넘어가는 것도 맞게 다뤘다.
  - `classify`는 오류 → 알림 → 동작 순으로 가르므로 앞말이 겹쳐도 오류가 이긴다.
- **라벨 파일 기록(Task 2)**
  - 변경이 작다. `Project::summary()`는 값이 있을 때만 두 키를 싣는다. 그래서 옛 라벨 파일과도, 옛 라벨러와도 양쪽으로 호환된다. 옛 판은 이 키를 무시하거나, 프레임을 편집할 때 버린다.
  - 배치 도구(색·둘레·빠진 공·가림·전파)는 프레임을 제자리에서 고치므로 기록이 남는다.
  - 편집은 `saveFrame`이 프레임을 새로 만들어 editing이 되므로 기록도 같이 사라진다. 의미가 맞다.
- **hubProgress(Task 3)**
  - 7일은 정오 기준 `mktime` 정규화로 만든다. DST가 있어도 날이 빠지지 않는다. 날짜는 문자열 앞 10자로 맞춰 7일 수를 파싱 없이 센다.
  - 다른 작업자, 기록 없는 옛 확인, 보류를 뺀다.
  - 매 틱이 아니라 리비전이 바뀌거나 10초가 지났을 때만 다시 센다. 상태는 바뀌었을 때만 보낸다(`Client::setStatus` 비교).
- **화면(Task 4)**
  - 데이터에서 온 글자는 모두 `esc()`를 거친다. 작업자·PC·버전·프로젝트·활동 문장·주소·저장소·초대 코드가 다 여기에 해당한다.
  - 링크는 `encodeURIComponent`로 만들어 큰따옴표 속성에 넣는다. style 값은 `Math.round` 숫자뿐이다. 인라인 이벤트 처리기 없이 `data-act`로 위임한다.
  - 다른 출처의 페이지는 JSON POST를 사전 요청 없이 보낼 수 없다(OPTIONS가 501이다). GET에는 부작용이 없다.
  - 초대 창을 `#app` 밖에 두어 2초 새로 고침이 입력을 건드리지 않는다.
  - 새로 고침이 겹쳐도 요청 전에 `known`을 잡아 두므로 같은 줄이 두 번 붙지 않는다.
- **exe(Task 5)**
  - 뮤텍스를 `ensureHub`보다 먼저 잡는다.
  - `--check`는 창을 보이지 않고, 시작프로그램과 사용자 프로필을 건드리지 않으며, 자기가 켠 허브만 끈다. 실제 시작프로그램에 바로 가기가 없고 `hub-settings.json`도 없으므로, 점검·테스트가 첫 실행 자동 시작을 부른 적이 없다.
  - `ping`이 허브 / 다른 프로그램 / 없음을 가른다. 저장소 끝 `\`가 따옴표를 깨는 경우를 막았다. breakaway가 거절되면 빼고 다시 시도한다.
  - WebView2가 없으면 기본 브라우저로 넘어간다. 모니터별 DPI를 따른다.
- **점검·배포(Task 6)**
  - 끝까지 점검은 숨긴 라벨러 둘의 라벨 파일 숫자(`progress.today ≥ 1`)를 조건으로 삼는다. 활동 줄보다 단단한 조건이다.
  - PowerShell 5.1 함정(BOM, WebClient UTF-8, JSON 배열 펼치기)을 챙겼다.
  - SDK는 크기와 SHA-512를 확인한 뒤 필요한 파일만 푼다.
- **원장**: Ruling마다 이유와 비용을 적었다. `status.project` 문제(등록이 프로젝트를 열기 전에 나감)는 끝까지 점검으로 찾아냈고, RED→GREEN 테스트를 붙였다.

---

### Issues

#### Critical (Must Fix)

없음. 데이터를 잃거나 라벨러·허브가 죽는 경로는 찾지 못했다.

#### Important (Should Fix)

**I1. 허브를 `pythonw`로 켜면 Tailscale 명령마다 콘솔 창이 뜬다 (로그인, 창 열기, 점검 때마다)**

- 위치
  - `hub/hub.py:656-664`(`tailscale_ip`), `:667-677`(`tailscale_name`), `:680-688`, `:720-721`
  - 켜는 쪽: `src/hub_launch.cpp:99-107`(`pythonw` + `--bind auto`)
- 무엇
  - 2단계부터 허브는 콘솔이 없는 GUI 프로그램 `pythonw.exe`로 뜬다. `--bind auto`라서 `main()`이 `tailscale ip -4`와 `tailscale status --json`을 `subprocess.run`으로 부른다.
  - `tailscale.exe`는 콘솔 프로그램이고 이 PC의 PATH에 있다.
  - 부모에게 콘솔이 없고 `creationflags`에 `CREATE_NO_WINDOW`가 없으면, Windows는 콘솔 자식마다 새 콘솔 창을 만든다. Python은 `shell=True`일 때만 SW_HIDE를 넣는다.
  - 그래서 허브가 켜질 때마다 창 둘이 잠깐 뜬다. Windows 11 기본 터미널이 Windows Terminal이면 그 창이 뜨고 포커스도 가져간다.
- 언제 생기나
  - 로그인할 때. 첫 창 실행이 자동 시작을 켜 두므로 이것이 기본이다.
  - 허브가 꺼진 채 창을 열 때.
  - `Boccia Hub.exe --check`, `hub_launch_tests`의 `ensure_hub_twice_starts_one`.
  - 1단계는 `boccia-hub.cmd serve`(python.exe가 콘솔을 함께 씀)로 켜서 이 창이 보이지 않았다.
- 왜
  - `hub/README.md:8`의 "(`pythonw`, 검은 창 없음)"과 다르다.
  - 계획 Global Constraints의 "점검 중 화면에 창을 띄우지 않는다"를 Task 5 점검이 어겼다.
  - 로그인 직후 사용자가 입력 중인 창에서 포커스를 뺏을 수 있다.
- 고칠 것
  - 두 `subprocess.run`에 `creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)`를 준다.
  - 테스트: `subprocess.run`을 바꿔치기해, 넘어온 `creationflags`에 `0x08000000`이 있는지 보는 허브 단위 테스트 하나.

**I2. 로그인 때 Tailscale보다 먼저 뜬 허브는 그 세션 내내 127.0.0.1만 연다 (계획 공백)**

- 위치
  - `hub/hub.py:680-689`: `bind_addresses`가 시작할 때 한 번만 돈다.
  - `hub/hub.py:707-721`: 묶기 실패는 stderr로만 가고, `public_name`도 한 번만 읽는다.
  - `src/hub_app_main.cpp:200`(`--background`), `:218-221`(첫 실행에 자동 시작을 켬)
- 무엇
  - 묶을 주소와 Tailscale 이름을 시작할 때 한 번만 정한다.
  - 그때 `tailscale ip -4`가 실패하거나(서비스가 아직 Starting, 무선망 연결이 늦음 등) Tailscale 주소 묶기가 실패하면, 허브는 localhost만 열고 다시 시도하지 않는다.
  - pythonw라 `묶기 실패` 줄도 사라진다.
- 왜
  - 1단계 리뷰는 "자동 시작은 1단계 밖"이라며 이 문제를 제외했다. 2단계가 로그인 시작을 기본으로 만들었으므로 이제 범위 안이다.
  - 이렇게 되면 joonyeon 같은 다른 PC의 라벨러는 그날 붙지 못한다.
  - 화면은 주소를 `127.0.0.1:8790`(location.host)으로 보일 뿐 이유를 알려 주지 않는다. 허브의 목적인 원격 라벨러 보기가 조용히 꺼진다.
- 고칠 것
  - `--bind auto`인데 Tailscale 서버가 없으면, `main()`의 대기 고리에서 30초마다 `tailscale_ip()`를 다시 본다. 주소가 생기면 묶고 `public_name`도 다시 읽는다.
  - 묶기 실패는 `hub.say`로 `hub.log`에 남긴다.
  - 테스트: `tailscale_ip`가 처음엔 `""`, 다음엔 주소를 주도록 바꿔치기한 뒤 `hub.servers`가 늘어나는지 본다.

#### Minor (Nice to Have)

**M1. 활동 줄을 끝까지 읽을 수 없고, 2초마다 화면 전체를 다시 만든다**
- 위치: `hub/ui/style.css:103`(`.e .t`가 한 줄·말줄임), `hub/ui/app.js:223-229`(`render()`가 매번 `#app.innerHTML`을 바꿈)
- 긴 오류 문장은 잘린다. 프로젝트 열기 실패나 경로가 든 내보내기 오류가 그렇다. 툴팁도 없다.
- 복사하려고 선택해도 2초 안에 지워진다. 활동 탭은 최대 2,000줄을 2초마다 새로 만든다.
- 원격 문제 해결이 허브의 첫 목적이므로 오류 전문을 볼 수 있어야 한다.
- 고칠 것: `.t`에 `title="${esc(event.text)}"`를 붙이고, 새 HTML이 지난번과 같으면 `innerHTML`을 건드리지 않는다.

**M2. `last10`이 자정 직후 10분 동안 어제 확인을 뺀다**
- 위치: `src/native_labeler_model.cpp:64`(10분 검사를 오늘 날짜 프레임에만 함)
- 계획의 정의는 `now-600 ≤ t ≤ now`다. 이 코드로는 00:05에 23:58의 확인이 속도에서 빠진다. 원장에 Ruling이 없는 이탈이다.
- 고칠 것: 이 작업자의 기록이 있는 모든 프레임(최소한 오늘과 어제)에 10분 검사를 한다. 테스트: now=00:05, 23:58 프레임 → `last10 == 1`.

**M3. 작업을 멈추면 `오류`로 적히고, 둘레 다듬기·색 채우기 실패는 이유 없이 적힌다**
- 다음 줄들은 상태가 `complete`가 아니면 모두 `오류 · …`로 적는다.
  - `src/native_labeler_app.cpp:789`: 가려진 공 표시. 사용자가 `stop-occluded`로 멈출 수 있다.
  - `:813`: 빠진 공 찾기·원형 검출. `stop-missing`으로 멈출 수 있다.
  - `:795`: 전파.
- `:818-819`는 엔진에 `circleJob["error"]`·`colorJob["error"]`가 있는데도 `오류 · 둘레 다듬기`만 적는다.
- 그래서 사용자가 누른 중단이 활동 탭에 빨간 줄로 나오고 `오류 M`에 더해진다. 계획 표의 형식은 `오류 · <작업 이름> · <이유>`다.
- 고칠 것: `stopped`면 `<작업> 중단`(job 종류)으로 적는다. 초안 다시 만들기가 이미 그렇게 한다. 실패에는 이유를 붙인다.

**M4. 내보내기 끝 줄과 '이미지 자료' 이름이 미커밋 백로그의 데이터 모양에 기댄다 (Ruling의 비용 서술이 틀림)**
- 내보내기 끝 줄
  - `src/native_labeler_app.cpp:755`는 `exportStatus_["path"]`를 본다.
  - 작업에 위쪽 `path`를 넣는 것은 작업 복사본의 평면 내보내기(`src/labeler_export.cpp:272`, 미커밋)뿐이다.
  - 커밋된 `startExport`(HEAD `src/labeler_export.cpp:131-138`)는 폴더를 `result.path`에 둔다.
  - 그래서 HEAD에서는 어느 내보내기도 `내보내기 · …`로 적히지 않는다. 작업 복사본에서도 학습 모드 분할 화면의 내보내기는 적히지 않는다.
  - Ruling은 "분할 화면 내보내기는 내보내기 탭의 형식으로 적힌다"고 했지만, 실제로는 아예 적히지 않는다.
- '이미지 자료' 이름
  - `hubProgress`의 `이미지 자료 NN`은 `scene.source_type`에 기댄다.
  - HEAD의 `sceneIndex`에는 이 키가 없다. 작업 복사본 `src/labeler_native.cpp:77`에만 있다.
- 빌드를 새로 깨지는 않는다. 하지만 1단계 I1을 정리할 때 함께 커밋하지 않으면 조용히 틀린 결과가 나온다.
- 고칠 것
  - `path`가 없으면 `result.path`를 쓴다.
  - '한 번만' 기준은 실패에도 있는 값(예: `started_at`)으로 잡는다.
  - 이름은 검수 화면처럼 `members[0].camera == "images"`도 본다.

**M5. 허브를 못 켤 때 문구가 틀리거나 이유가 남지 않는다**
- 문구가 틀리는 경우
  - `src/hub_launch.cpp:83`: 8790을 HTTP가 아닌 프로그램이 쓰거나 2초 안에 답하지 않으면 `Ping::None`이 된다.
  - 그러면 허브를 띄우고, 허브는 묶기에 실패해 죽고, 약 10초 뒤 `허브를 켜지 못함`이 뜬다. 거기에 붙지는 않는다.
  - `pythonOnPath`(`:50-63`, `:70`)는 버전을 보지 않는다. PATH에 옛 pythonw가 있으면 같은 문구가 된다.
- 이유가 남지 않는 경우
  - 표준 입출력 핸들을 주지 않으므로(`:100-107`) pythonw에는 stderr가 없다. 계획은 NUL로 보내고 `CREATE_NO_WINDOW`를 쓰라고 했는데, 원장에 Ruling이 없다.
  - 그래서 `hub/hub.py:711`의 `묶기 실패`와, `Hub()`가 저장소 생성·권한에서 던진 예외의 traceback이 사라진다.
- 고칠 것
  - `None`이면 TCP 연결을 한 번 더 시도해서, 붙으면 `포트를 다른 프로그램이 씀`을 띄운다.
  - PATH 후보는 버전을 확인한다.
  - 허브의 stderr를 `%LOCALAPPDATA%\Boccia\hub-start.log`로 돌리고, 묶기 실패는 `say`로 남긴다.

**M6. 이미 열린 창 대신 숨긴 점검 창을 앞으로 가져올 수 있다**
- `src/hub_app_main.cpp:206`의 `FindWindowW(WINDOW_CLASS)`는 숨긴 창도 찾는다.
- `--check`도 같은 클래스(`:28`)로 창을 만들고 뮤텍스는 잡지 않는다. 그래서 점검이 도는 동안 사용자가 exe를 다시 누르면 점검 창이 보이게 되고, 진짜 창은 앞으로 오지 않을 수 있다. 드문 경우다.
- 고칠 것: 점검은 다른 클래스 이름을 쓰거나, 보이는 창만 찾는다(EnumWindows + IsWindowVisible).

**M7. `--check`마다 WebView2 프로필이 `%TEMP%`에 남는다**
- 위치: `src/hub_app_main.cpp:242`. 지금 이 PC에 `boccia-hub-check-*`가 6개 있고, 하나에 약 10 MB다.
- 고칠 것: 끝낼 때 지운다(브라우저 프로세스가 끝난 뒤 잠깐 재시도). 또는 점검용 프로필 폴더를 하나로 고정한다.

**M8. 로그인 바로 가기가 처음 실행한 복사본을 계속 가리킨다**
- `src/hub_app_main.cpp:220`은 `autostart` 키가 없을 때 한 번만 바로 가기를 만든다.
- 개발 빌드(`build-labeler\Release`)를 먼저 열면, 그 뒤 `bin\release\hub`를 써도 로그인할 때는 개발 빌드가 뜬다. 설정에서 껐다 켜야 바뀐다.
- 고칠 것: `autostart`가 true인데 바로 가기 대상이 `app.exe`와 다르면, 창을 열 때 바로 가기를 다시 만든다.

**M9. 'N초 전'을 라벨러 PC의 시계로 잰다**
- `hub/ui/app.js:115`의 `ago(worker.last_action.time)`에서 `time`은 라벨러 시계이고 `Date.now()`는 허브 PC 시계다.
- 두 PC의 시계가 다르면(1단계 집중 검토 3의 상황) "0초 전"이나 엉뚱한 분이 나온다.
- 고칠 것: `_last_action`(`hub/hub.py:278-284`)에 `received`를 싣고 그것으로 잰다.

**M10. 허용되지 않은 기기는 JSON 오류 본문을 본다 (계획 안의 모순)**
- `/`와 `/ui/*`도 403이다(`hub/hub.py:548-549`). 그래서 app.js가 실리지 않고, `허용되지 않은 기기`(`hub/ui/app.js:41, 226`)는 나올 길이 없다.
- 브라우저에는 `{"error": "screen: only this PC …"}`가 그대로 보인다. 계획이 두 요구를 함께 적었기 때문이다.
- 고칠 것: `/`의 403은 `text/html`로 `허용되지 않은 기기` 한 줄만 보낸다.

**M11. `Host`를 보지 않아 DNS 리바인딩 페이지가 같은 출처가 된다 (1단계 I3 잔여)**
- `hub/hub.py:546-564`는 접속 주소와 Content-Type만 본다.
- 리바인딩된 페이지는 브라우저가 보기엔 같은 출처이고, 허브가 보기엔 127.0.0.1에서 온 요청이다. 그래서 초대 코드를 만들어 읽고, 명령을 넣을 수 있다. 2단계부터는 `/api/shutdown`도 부를 수 있다.
- Chromium의 사설망 보호가 위험을 줄이지만 없애지는 않는다. Firefox에는 그런 보호가 없다.
- 고칠 것: `Host`가 `127.0.0.1`, `localhost`, `[::1]`, `public_name`, Tailscale IP, `--control-from` 주소에 포트를 붙인 값일 때만 받는다.

**M12. 캐시를 처음 채울 때 작업자의 전체 기록을 전역 잠금 안에서 읽는다**
- `hub/hub.py:152-171`은 날짜 파일을 모두 읽은 뒤 끝 20,000줄만 남긴다.
- 처음 오는 `/api/workers`가 `_last_action` → `_cache`(`:273, 280`)를 거쳐 모든 작업자에 대해 이 일을 `self.lock` 안에서 한다.
- 그동안 앱 요청(등록·상태·이벤트)이 기다리고, 메모리가 기록량만큼 잠깐 는다. 기록이 몇 달 쌓이면 허브를 켤 때마다 몇 초가 걸린다.
- 고칠 것: 최신 파일부터 거꾸로 20,000줄만 읽는다(`deque(maxlen)`). 파일 읽기는 잠금 밖에서 한다.

**M13. `reviewed_at`·`reviewed_by`가 데이터 버전에 섞인다**
- `src/labeler_native.cpp:93`이 지문을 만들 때 `state["frames"]`를 통째로 해시한다.
- 그래서 라벨은 그대로 두고 다시 확인만 해도 데이터 버전(내보내기 지문)이 바뀐다. 누가·언제 확인했는지는 데이터가 아니다.
- 고칠 것: 데이터 버전을 '라벨'로 정의한다면 지문 스냅샷에서 두 키를 뺀다.

**M14. 모양이 틀린 값 하나가 상세 화면을 멈춘다**
- `render()`에 보호 장치가 없고, 앱이 보낸 JSON을 그대로 쓴다.
  - `shortGpu(name).replace`(`hub/ui/app.js:66, 164`): `device.name`이 문자열이 아니면 예외
  - `days.map`·`recordings.map`(`:188-192`): 배열이 아니면 예외
  - `(d.date || "").slice`(`:192`): 문자열이 아니면 예외
- 다른 판의 라벨러·트래커가 모양을 바꾸면 2초마다 예외가 나고, 화면이 옛 내용에 머문다.
- 고칠 것: `String()`·`Array.isArray`로 맞추거나, render를 try/catch로 감싸 마지막으로 잘 그린 화면을 둔다.

**M15. 테스트 공백**
- (a) 집중 검토 4의 "다시 확인한 프레임은 한 번만 센다"를, 실제로 두 번 확인해서 보는 테스트가 없다.
  - 구조상으로는 맞다.
  - 그러나 어제 확인한 프레임을 오늘 다시 확인하면 어제 막대가 줄어든다. 이 성질도 문서에 없다.
- (b) 창 경로는 한 번도 돌지 않았다. 이 PC에 `hub-webview`·`hub-settings.json`이 없다.
  - 웹 메시지 `autostart?`·`autostart`·`close`
  - 첫 실행 바로 가기
  - 뮤텍스로 열린 창 앞으로 가져오기
  - `--check`에서 `ExecuteScript("chrome.webview.postMessage({type:'autostart?'})")`로 메시지 왕복만 확인할 수 있다. 시작프로그램은 건드리지 않는다.
- (c) `hub/tests/edge.py`의 정리
  - Edge 프로세스를 거두지 않는다(task-4-tests.log의 `ResourceWarning`).
  - 정리에 실패하면 프로필을 남긴다(`%TEMP%\boccia-edge-i2c7pdiv`).
- (d) 계획의 `CREATE_NO_WINDOW`·NUL 입출력과 다르게 구현했는데, 원장에 Ruling으로 남지 않았다.

**M16. 문서**
- `design/hub/DESIGN.md:34, 141, 155`에 아직 "창(Edge 앱 모드)"이 남아 있다. 결정 11(`:19`)은 기록이므로 그대로 둔다.
- `hub/README.md:8`의 "검은 창 없음"은 I1을 고치기 전까지 틀린 말이다.

---

### Declined to judge

- 날짜별·오늘 확인이 작업자 전체가 아니라 열린 프로젝트 하나를 기준으로 한다. 프로젝트를 바꾸면 다른 프로젝트의 오늘 확인이 안 보인다. — 계획이 열린 catalog로 정의했다.
- '전체 진행'은 작업자들의 합이다. 같은 프로젝트의 복사본을 둘이 하면 두 번 센다. — 계획의 정의다.
- 활동 탭에 같은 PC 다른 프로세스의 줄이 섞인다. 캡처 `hub-activity.png`의 13:06:13 `시작` 두 줄과 앞선 점검들의 `장면 확인`이 그 예다. — 1단계 I2·M8의 잔여이며 2단계 범위 밖이다.
- 설치된 Microsoft Store Python은 PEP 514에 등록돼 있어도 `is_regular_file`이 실행 별칭이라 거부한다. — 허브는 python.org 3.12가 있는 이 PC에서만 돌고, 계획의 인터페이스가 그렇게 정했다.
- 기본 저장소가 `D:\Boccia Hub`인데 D:의 여유 공간이 적다. — 설계 결정 5이고 1단계에서도 제외했다.
- `--storage D:\`(드라이브 루트)의 따옴표 처리, `--port`에 숫자가 아닌 값을 주면 0번 포트로 뜨는 허브. — 드문 입력이다.
- 시스템 시계가 날짜를 거꾸로 넘은 뒤 허브를 다시 켜면 `received` 순서가 파일 순서와 어긋날 수 있다. — 드문 경우다.
- `hub.log` 회전. — 계획에 없다.
- 6 MB 글꼴까지 `no-store`라 페이지를 열 때마다 다시 받는다. 원격 `--control-from` 기기에서는 느리다. — 계획이 모두 `no-store`로 정했다.
- 흐리게 하는 꼬리(4조각 이상일 때 마지막 조각)가 계획 문구("첫 ` · ` 뒤")와 다르다. — 시안과 같다.
- 허브를 끈 뒤나 허브가 죽은 뒤 창에 다시 켜기 버튼이 없다. — 계획 범위 밖이다.
- 시안과의 시각적 차이. — 실행자가 캡처를 시안과 나란히 보였으므로 사용자가 판단할 일이다.
- `fetch-webview2.ps1`이 PowerShell 5.1의 진행 표시줄 때문에 느릴 수 있다. — 기능에 영향이 없다.
- 창이 다시 활성화될 때 WebView로 포커스를 넘기지 않는다(WM_ACTIVATE). — 사소한 조작감 문제다.
- `say()`의 UTF-8 출력이 CP949 콘솔에서 깨진다(예: `FAIL: Python 3.10 이상 필요`). — 점검 출력은 파이프로 받는다.

---

### Review Focus coverage (1–5)

1. **허브가 켜진 채 누르거나 두 번 누름**
   - 코드
     - `ensureHub`가 먼저 핑하고, 허브면 켜지 않고 `""`를 돌려준다(`src/hub_launch.cpp:90-91`).
     - 뮤텍스 `Local\BocciaHubWindow`를 `ensureHub` 앞에서 잡는다(`src/hub_app_main.cpp:202-210`). 이미 있으면 `FindWindowW` → `ShowWindow`/`SetForegroundWindow`.
     - `--background`와 창이 동시에 켜지면 둘 다 허브를 띄운다. 그러나 나중 것은 묶기에 실패하므로(Windows에서 SO_REUSEADDR 끔) 허브는 하나만 남는다.
   - 테스트: `ensure_hub_twice_starts_one`(순서대로 두 번 부름)의 통과 기록이 있다.
   - 공백
     - 뮤텍스·앞으로 가져오기는 자동 테스트도 실행 기록도 없다(M15b).
     - 첫 창이 `ensureHub`(최대 10초) 안에 있을 때 두 번째 클릭은 아무것도 하지 않고 끝난다.
     - 숨긴 점검 창을 집을 수 있다(M6).
   - 결론: **코드는 맞다. 창 경로는 검증되지 않았다.**
2. **Python이 없거나 Store 별칭만 있음**
   - 코드
     - `findPython`은 동봉 → PEP 514(3.10 이상, 최신부터) → PATH 순으로 찾고, PATH에서는 `\windowsapps`를 건너뛴다.
     - 비면 `Python 3.10 이상 필요`를 MessageBox로 띄우고 끝난다. 멈추지 않는다.
   - 테스트: `python_on_path_skips_store_alias`, `ensureHub({}, …)`
   - 공백: PATH 후보의 버전은 확인하지 않는다(M5).
   - 결론: **덮였다. 작은 공백 하나.**
3. **8790을 다른 프로그램이 씀**
   - 코드: `ping`이 `{"hub":"boccia"}`가 아닌 HTTP 답을 Other로 보고 문구를 띄운다.
   - 테스트: `ping_tells_other_from_hub`(http.server)와, 포트가 차 있을 때의 문구 확인
   - 공백: HTTP가 아닌 프로그램이나 답이 없는 프로그램은 None이 되고, 약 10초 뒤 `허브를 켜지 못함`이 뜬다. 거기에 붙지는 않는다(M5).
   - 결론: **HTTP인 경우만 덮였다.**
4. **허브가 꺼져 있던 날의 확인**
   - 코드: `hubProgress`(`src/native_labeler_model.cpp:54-70`), 기록은 `src/labeler_native.cpp:409-417`
   - 테스트
     - `hubProgressTests`: 다른 작업자(d), 기록 없음(e), 보류(f), 8일 전(h), 0인 날, last10
     - 엔진 테스트: 확인·보류의 기록과 요약
   - 다시 확인한 프레임
     - 프레임은 키 하나이므로 구조상 한 번만 센다. 마지막 확인의 날짜·작업자로 옮겨 간다.
     - 테스트는 없다(M15a).
   - 공백: 자정 직후의 last10(M2)
   - 화면은 오늘 받은 상태일 때만 오늘 수를 센다(Ruling).
   - 결론: **덮였다. 공백 둘.**
5. **활동이 수만 줄**
   - 코드: 작업자별 캐시 + `bisect_right`(`hub/hub.py:333-343`), 자르기(`:186-192`)
   - 테스트: `test_events_fast_with_many`(20,000줄, `since` 20번에 1초 미만). 이번에 직접 돌려 OK였다.
   - 공백
     - 첫 적재가 전체 기록을 잠금 안에서 읽는다(M12).
     - 화면은 2,000줄을 2초마다 다시 만든다(M1).
   - 결론: **정상 상태는 덮였다.**

---

### Rulings assessment

| Ruling | 판단 |
| --- | --- |
| 준비: worktree 없이 공유 폴더, 기능 브랜치 | 사정상 이해된다. 다만 2단계도 미커밋 백로그의 데이터 모양에 새로 기댔다(M4). 1단계처럼 비용이 실제로 생겼다. |
| T1 Tailscale 이름은 `main()`이 한 번 읽음 | 선택은 타당하다(테스트 허브는 tailscale을 부르지 않음). 비용 서술이 빠뜨린 것: pythonw에서 창이 뜸(I1), Tailscale이 늦으면 끝까지 빈 주소(I2). |
| T2 기록 테스트 위치 | 타당하다. |
| T3 dxgi를 `#pragma comment`로 | 타당하다(MSVC 전용, hub_client를 쓰는 대상에 모두 따라감). |
| T3 `native_labeler_app.hpp` 멤버 | 타당하다. |
| T3 내보내기 끝을 `tick()`에서 한 번 | 방식은 타당하다. 비용 서술은 틀렸다: 분할 화면 내보내기는 위쪽 `path`가 없어 아예 적히지 않고, HEAD에서는 어느 내보내기도 적히지 않는다(M4). |
| T4 headless Edge를 DevTools로 | 타당하다(이 PC의 사정, 도구 하나 추가). 프로세스 정리는 M15c. |
| T4 '지금' = page + scene | 타당하다. 시안 문구와 같고, 작업이 두 번 나오지 않는다. |
| T4 오늘·10분은 오늘 받은 상태만, 허브 끄기는 두 번 | 타당하다. |
| T5 IShellLinkW·Winsock 도우미, ensureHub 추가 확인 | 타당하다. 계획보다 단단하다. |
| T5 모니터별 DPI | 타당하다. |
| T5 make-ico를 Edge로 | 타당하다. |
| T6 상태에 `project` | 타당하다. 좋은 발견이다(RED→GREEN). |
| T6 WebClient UTF-8·배열 펼치기 | 타당하다. |
| T6 `hub-capture.py`로 캡처 | 타당하다. |
| T6 `bin/release/hub` 미커밋 | 타당하다(배포 커밋은 요청이 있을 때). |
| (원장에 없음) CreateProcess 플래그·NUL | `DETACHED_PROCESS`와 함께 쓰면 `CREATE_NO_WINDOW`는 무시되므로 해는 없다. 그래도 Ruling으로 남겼어야 한다. NUL 대신 핸들을 아예 주지 않은 것은 M5의 이유 소실로 이어진다. |
| (원장에 없음) last10을 오늘로 제한 | 계획과 다르다. M2. |

---

### Recommendations

1. I1(한 줄 수정)과 I2를 고친 뒤에 이 PC에서 자동 시작을 믿고 쓴다. `%TEMP%`에 남은 `boccia-hub-check-*`, `boccia-hub-check-store`, `boccia-edge-*`는 지워도 된다.
2. 사용자가 `bin\release\hub\Boccia Hub.exe`를 한 번 직접 열어 창 경로를 확인한다. 점검이 보이는 창을 띄우지 않는다는 제약 때문에, 이 경로는 아직 아무도 본 적이 없다. 볼 것:
   - 첫 실행 바로 가기(그 exe를 가리키게 됨)
   - 설정의 로그인 토글
   - 허브 끄기 뒤 창이 닫히는지
   - 두 번 눌렀을 때 열린 창이 앞으로 오는지
3. 1단계 I1(백로그 커밋)을 정리할 때, 평면 내보내기(`labeler_export.cpp`의 위쪽 `path`)와 `sceneIndex`의 `source_type`을 함께 커밋한다. 아니면 M4처럼 대체 경로를 둔다.
4. 다음 단계 전에 계획에 반영할 것:
   - 403 화면 요구와 `허용되지 않은 기기` 요구가 서로 막는다(M10).
   - 로그인 시작은 Tailscale이 준비되는 시점을 다뤄야 한다(I2).
   - 하위 프로세스를 띄우는 코드는 pythonw 아래에서도 창이 뜨지 않아야 한다는 것을 요구 사항으로 적는다(I1).
   - 집중 검토 항목마다 실패할 수 있는 테스트 사례를 계획에 넣는다(M15a).

---

### Assessment

**Ready to merge?** With fixes

**Reasoning:** 계획의 여섯 작업이 모두 들어왔고 허브 테스트는 이번에도 통과했으며, 데이터를 잃거나 앱이 죽는 경로는 찾지 못했다. 다만 2단계가 기본으로 켜는 로그인 시작 경로에서 매번 콘솔 창이 뜨고(I1), Tailscale이 늦으면 그날 원격 라벨러를 받지 못하므로(I2), 자동 시작을 믿기 전에 이 둘을 고쳐야 한다.
