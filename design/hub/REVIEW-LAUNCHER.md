# 런처 배포 최종 검토 (2026-10-21)

범위: `157dc5e..5a8079d` (계획 `PLAN-LAUNCHER.md`, 설계 `LAUNCHER.md`). 검토자: Opus 새 컨텍스트(Fable은 사용량 한도로 시작 못 함).
판정: 치명 없음, 중요 6, 사소 16 → 중요 6을 고침(`f973895`), 사소는 아래에 남김.

## 고친 것 (테스트가 먼저 실패한 뒤 통과)

1. 끊긴 설치가 처음부터 다시 받고 재시도가 없음 → 검증한 조각은 `update\`에 남겨 다음 실행이 이어 받고, 파일마다 세 번 시도,
   잠깐 잠긴 파일(바이러스 검사)은 2초까지 기다려 옮김, 옛 목록의 조각은 지움. `a_cut_keeps_the_checked_pieces`,
   `a_fetch_that_fails_once_is_retried`, `a_briefly_locked_target_moves_after_a_retry`.
2. 초대 코드가 바뀐 작업자가 설정 창을 닫으면 깔린 라벨러도 못 켬 → 깔려 있으면 허브 없이 띄움. `wrong_code_still_starts_the_installed_labeler`.
3. 경로를 대소문자 구별로 비교 → Windows처럼 비교, 대소문자만 다른 두 경로의 목록은 거부. `two_paths_equal_but_for_case_are_refused`,
   `a_case_only_rename_keeps_the_file`.
4. 런처가 목록 모양을 보지 않음 → `labeler/data`·`labeler/logs`·`update`로 가는 경로, 두 프로그램이 없는 목록을 거부, 폴더는 옮기지 않음,
   허브 등록도 두 프로그램이 없으면 거부. `a_list_without_the_programs_is_refused`, `paths_into_worker_folders_are_refused`,
   `a_folder_at_a_file_path_is_not_moved`, `test_register_refuses_a_release_without_the_programs`.
5. 압축 파일 안에서 실행하면 `%TEMP%`에 설치 → `%TEMP%` 아래와 이동식 드라이브는 `빈 폴더에 넣어 주세요`. `launcher_refuses_a_temp_folder`
   (런처 테스트 폴더는 `build-labeler\launcher-tests`로 옮김).
6. 허브가 꺼졌을 때의 런처 테스트 없음 → `hub_off_starts_the_installed_labeler`, `hub_off_without_an_install_exits_3` 추가(처음부터 통과).

## 남긴 사소한 것

- 바꾼 뒤의 state 쓰기가 되돌리기 밖에 있고, 옛 백업을 새 state를 쓰기 전에 지움.
- `revertLastUpdate`가 원자적이지 않음, 실행 실패 뒤의 되돌림은 로그 줄을 남기지 않음.
- 실패 사유를 남기지 않음(`SyncResult.error`); 처음 설치의 모든 실패가 `허브에 연결할 수 없습니다`로 보임 → `update\launcher.log` 제안.
- 처음 설치에서 허브 주소를 잘못 넣으면 설정 창으로 돌아가지 않음.
- `update\`를 지우면 런처가 자기 설치를 빈 폴더가 아니라며 거부.
- bad 표시가 라벨러 exe의 버전으로만 정해져, exe를 다시 빌드하지 않고 고친 배포는 같은 id라 bad PC가 받지 않음.
- 환경 문제(RDP의 OpenGL 3.3 없음)로 좋은 버전이 bad가 될 수 있고, 되돌린 실행은 지켜보지 않음.
- 허브: `<app>` 검사, 실패한 `store_blob`의 임시 파일, 읽는 중 `index.json` 교체, 흘려보내기 중 오류의 JSON 500.
- 런처에 맨 바깥 예외 처리가 없음(설정 저장, 이상한 설정 값).
- `safePath`가 예약 이름(CON 등)·끝의 점이나 공백·쓸 수 없는 문자를 거부하지 않음, state 키도 검사하면 좋음.
- 버전 사이에 파일이 폴더로(또는 반대로) 바뀌면 매번 실패.
- 허브가 꺼져 있으면 목록을 기다리는 5~10초 동안 창이 없음, 두 번 누르면 라벨러가 둘 뜸.
- 테스트: `running_labeler_is_not_updated`가 5초짜리 가짜에 기댐, 한글 경로는 엔진 수준만, 여러 파일 중 끊김 테스트 없음.
- `라벨러를 실행하지 못했습니다`는 계획의 화면 문구 목록 밖(옛 런처에서 옴).
- 다른 이름으로 저장된 런처(`BocciaLauncher (1).exe`)는 스스로 갱신하지 않음.

## 검토자가 판단하지 않은 것에 대한 판정

설계대로 두기로 함: 목록 서명(Tailscale과 초대 코드가 보안 모델), 앱 안 갱신·되돌리기 화면·트래커·차분 패치(범위 밖), 모든 버전 보관,
빈 공간 미리 확인 안 함(실패해도 안전), 코드 서명·SmartScreen(전달은 범위 밖), `Local\` 뮤텍스와 네트워크 공유(대상 환경 아님),
종료 코드 겹침, Windows 10 1703 이전, 설정 파일의 초대 코드 평문(기존 설계), 같은 크기·시각이면 같은 파일로 봄(설계의 비교),
동시 등록(관리 PC 하나), 미커밋 백로그(검토 범위 밖), 라벨러 자체의 한글 경로 처리(범위 밖, 끝에서 끝 점검에서 문제 없음),
라벨러 두 개 실행(원래 동작).

## 그 밖의 판정 (실행 중)

- 런처 소스·아이콘·CMake 블록은 이 기능과 함께 처음 커밋함(다른 백로그에 기대지 않음).
- 런처 자신도 `update\backup`으로 옮겨 바꿈(설계의 `.old` 대신).
- 처음 설치는 되돌릴 이전 버전이 없어 지켜보지 않음.
- 설치 모드도 라벨러가 끝날 때 그 종료 코드를 돌려줌(옛 런처와 같음).
- `collect_release.ps1`과 `CMakeLists.txt`는 커밋된 옛 판본에 이 작업의 블록만 더함.
- 실행 중인 런처는 배포 때 `BocciaLauncher-previous.exe`로 옮겨짐(등록에서 빠짐).
- 사고: `build-labeler\Release`의 런처로 `--capture`를 돌려 옆의 라벨러가 보이는 창으로 약 10분 떠 있었음 → 런처 캡처를 먼저 처리하고
  `capture_beside_the_labeler_does_not_start_it`로 막음.
