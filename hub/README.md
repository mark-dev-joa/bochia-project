# Boccia Hub

연구소의 라벨러(나중엔 트래커도)를 한 PC에서 지켜보고 명령을 보내는 허브(설계 `design/hub/DESIGN.md`, 1단계
`PLAN-1.md`, 2단계 화면 `PLAN-2.md`). Python 3.10 이상이 허브 PC에만 있으면 됩니다. 앱은 허브가 켜져 있을 때만 붙습니다.

## 창 (`Boccia Hub.exe`)

`bin\release\hub\Boccia Hub.exe`를 더블클릭하면 허브가 꺼져 있을 때 뒤에서 켜고(`pythonw`, 검은 창 없음) 화면을 자기 창에
띄웁니다(WebView2). 창을 닫아도 허브는 계속 켜져 있어 라벨러들이 끊기지 않습니다. 이미 열려 있으면 그 창이 앞으로 옵니다.

- 화면: **작업자**(네 칸: 붙은 라벨러 · 오늘 확인 · 지금 속도 · 전체 진행, 표, 초대) → 줄을 누르면 **활동**(전체·작업·동작·오류)
  · **진행**(촬영별 확인, 날짜별 확인 7일, 요약). **설정**: 주소 · 저장소 · 켠 시각 · 로그인할 때 켜기 · 허브 끄기(두 번 눌러 끔).
- 처음 실행할 때 로그인하면 허브가 켜지도록 시작프로그램에 `Boccia Hub.lnk`(`--background`)를 만듭니다. 설정에서 끕니다
  (`%LOCALAPPDATA%\Boccia\hub-settings.json`의 `autostart`).
- 옵션: `--background`(허브만 켜고 끝), `--check`(숨긴 점검: 허브 → WebView2 → 화면 제목, 창을 띄우지 않음),
  `--port <n>`(8790), `--storage <폴더>`(`D:\Boccia Hub`).
- Python 3.10 이상이 없거나, 8790 포트를 다른 프로그램이 쓰면 그 사실 한 줄을 띄우고 끝납니다.
- 같은 화면을 브라우저로도 봅니다: `http://127.0.0.1:8790/`(다른 기기는 `--control-from`에 주소를 넣은 경우만).
- 빌드: `powershell -File tools/fetch-webview2.ps1`(WebView2 SDK 1.0.4258.31, SHA-512 확인) 뒤
  `cmake --build build-labeler --config Release --target boccia_hub`, 배포 `powershell -File tools/collect_hub.ps1`.

## 켜기 (명령줄)

```
boccia-hub.cmd serve
boccia-hub.cmd serve --port 8790 --storage "D:\Boccia Hub" --bind auto
```

`--bind auto`는 127.0.0.1과 Tailscale 주소(`tailscale ip -4`)에 묶습니다. `all`은 모든 주소(LAN 전체가 앱 API에 닿음),
아니면 주소를 쉼표로. 앱 쪽 주소: `mark-hong-pc.tail68c059.ts.net:8790`.

제어 API(`/api/…`, 아래 CLI가 쓰는 것)는 이 PC(127.0.0.1)에서만 답합니다. 다른 PC에서도 CLI를 쓰려면 그 PC의 Tailscale
주소를 `--control-from 100.x.y.z,100.a.b.c`로 적어 줍니다. 작업자 PC는 앱 API만 씁니다.

## 작업자 초대

```
boccia-hub.cmd invite 홍길동
```

초대 코드 한 줄이 나옵니다. 라벨러의 작업자 창(프로젝트 메뉴 → 작업자)에 이름과 함께 붙여 넣습니다.
같은 이름을 다시 초대하면 옛 코드는 더 쓰지 못합니다. 같은 이름이 PC 둘에서 붙으면 `홍길동@PC이름`으로 구분됩니다.

## 보기

```
boccia-hub.cmd status
boccia-hub.cmd workers
boccia-hub.cmd events 홍길동 [--follow] [--limit 50]
boccia-hub.cmd logs 홍길동
```

`workers`: 이름 · PC · 앱 · 버전 · 상태(붙음 / 떨어짐 14:02) · 마지막(마지막 활동 시각) · 지금(하는 일).
`events`: 활동 한 줄씩(시각 + 문장). `logs`: 가장 최근에 올라온 로그 파일을 그대로.
이름 대신 `이름@PC`도 됩니다. 이름이 둘 이상의 PC에 있으면 목록을 보이고 끝납니다(exit 2).

## 명령

```
boccia-hub.cmd send 홍길동 notify 쉬세요
boccia-hub.cmd send 홍길동 log
boccia-hub.cmd send 홍길동 ping
```

`notify`: 라벨러 아래 막대 왼쪽에 문구(×로 닫음). `log`: 오늘 로그 파일을 허브로 올림. 10초 안에 결과가 오면 `완료 · …`.
`boccia-hub.cmd stop`: 허브를 끕니다(`허브 끔`).

## 저장소 (`D:\Boccia Hub`)

```
workers.json            작업자(이름@PC): 앱, 버전, 마지막 접속, 마지막 활동 시각
tokens.json             초대 코드의 SHA-256 → 이름 (코드 자체는 없음)
commands.jsonl          모든 명령과 결과
status\<id>.json        마지막 상태
events\<id>\<날짜>.jsonl 활동 (시각, 받은 시각, 문장; 같은 줄은 한 번만)
log\<id>\               올라온 로그 파일
screen\<id>\            화면 캡처 (2단계)
```

## 동작

- 앱은 10초마다 `/ping`을 두드리고, 허브가 답하면 `/register`로 붙습니다. 코드가 틀리면(401/403) 설정을 바꿀 때까지 다시 시도하지 않습니다.
- 붙으면 로그 파일의 마지막 2,000줄 중 허브가 아직 못 받은 줄부터 보냅니다. 앱은 붙을 때마다 `작업자 · 이름@PC · 버전 …` 줄을 로그에 적고, 다른 이름의 그 줄 뒤에 있는 줄은 보내지 않습니다(같은 PC에서 이름을 바꿔도 남의 활동이 섞이지 않음). 로그 줄에 UTF-8이 아닌 바이트가 있으면 U+FFFD로 바꿔 보냅니다. 상태는 바뀔 때만(붙을 때마다 한 번 다시), 활동은 생기는 대로, 파일은 보관함(`%LOCALAPPDATA%\Boccia\hub-outbox`)에 두었다가 올립니다.
- 명령은 `GET /commands?wait=25`로 기다렸다 받고, 결과를 `POST /commands/<id>/result`로 돌려줍니다. 같은 작업자가 새로 붙으면 옛 접속의 대기는 바로 끝납니다.
- 앱이 꺼질 때는 `POST /bye`를 보내고(0.5초만 기다림) 허브는 그 작업자를 바로 떨어짐으로 표시합니다. 다시 `/register`하면 붙음으로 돌아옵니다. 앱이 멈추거나 강제로 끝나면 명령 대기가 끝나고 마지막 요청 뒤 15초가 지나야 떨어짐이 됩니다(2026-10-20).
- 앱 API는 작업자 코드(`Authorization: Bearer`)가 있어야 하고, 제어 API(`/api/...`)는 코드 대신 출처로 막습니다: 127.0.0.1과 `--control-from` 주소만, POST는 `Content-Type: application/json`만. `--bind auto`면 허브는 127.0.0.1과 Tailscale 주소에만 묶이므로 Tailscale 밖에서는 닿지 않습니다.

## 점검

```
python -m unittest hub.tests.test_hub hub.tests.test_cli hub.tests.test_ui
build-labeler\Release\hub_client_tests.exe
build-labeler\Release\hub_launch_tests.exe
powershell -File tools/hub-check.ps1
powershell -File tools/hub-screen-check.ps1 -Project <QA 프로젝트 폴더>
```

`hub-check.ps1`은 스크래치 저장소에 허브를 띄우고 숨긴 라벨러 둘을 붙여 `workers`, `send notify`, `send log`, `logs`, `events`를
확인합니다. `hub-screen-check.ps1`은 프로젝트 복사본 둘에서 숨긴 라벨러가 장면을 하나씩 확인하게 한 뒤 허브의 숫자와 활동을
보고, 화면 셋을 `design/hub/shots/hub-{list,activity,progress}.png`로 찍고, `Boccia Hub.exe --check`를 같은 허브에 돌립니다.
화면 테스트(`test_ui`)와 캡처는 headless Edge를 DevTools로 다룹니다(`hub/tests/edge.py`; 이 PC의 `msedge --dump-dom`은 출력이 없음).
