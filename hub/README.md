# Boccia Hub

연구소의 라벨러(나중엔 트래커도)를 한 PC에서 지켜보고 명령을 보내는 허브의 1단계(설계 `design/hub/DESIGN.md`).
Python 3.12만 있으면 되고, 허브 PC에만 필요합니다. 앱은 허브가 켜져 있을 때만 붙습니다.

## 켜기

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
- 앱 API는 작업자 코드(`Authorization: Bearer`)가 있어야 하고, 제어 API(`/api/...`)는 코드 대신 출처로 막습니다: 127.0.0.1과 `--control-from` 주소만, POST는 `Content-Type: application/json`만. `--bind auto`면 허브는 127.0.0.1과 Tailscale 주소에만 묶이므로 Tailscale 밖에서는 닿지 않습니다.

## 점검

```
python -m unittest hub.tests.test_hub hub.tests.test_cli
build-labeler\Release\hub_client_tests.exe
powershell -File tools/hub-check.ps1
```

마지막 것은 스크래치 저장소에 허브를 띄우고 숨긴 라벨러 둘을 붙여 `workers`, `send notify`, `send log`, `logs`, `events`를 확인합니다.
