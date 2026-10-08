# 라벨러 엔진 C++ 통합 설계

2026-10-08. 사용자 결정: "c++ 통합하자". 네이티브 라벨러(`boccia_labeler.exe`)가 Python 엔진(`ball_labeler.py`) 없이 혼자 도는 구조로 바꾼다.

## 배경

지금의 네이티브 라벨러는 Dear ImGui 화면(C++)이 localhost의 Python HTTP 서버에 요청을 보내는 구조다. 엔진은 원본 촬영 해독, 라벨 저장, 장면 색인, 검수·분할·내보내기 규칙, EfficientSAM 추론, 자동 초안, 대표 프레임 필터를 맡는다. 1차 화면 교체([labeler-v2/DESIGN.md](../labeler-v2/DESIGN.md))에서 엔진은 그대로 두었다.

이 구조의 문제는 다음과 같다.

- 라벨러를 쓰는 PC에 Python 3.12와 패키지(opencv, numpy, zstandard, onnxruntime)가 있어야 한다. `bin/release/labeler` 폴더만 복사해서는 돌지 않는다.
- 엔진 프로세스를 따로 띄우고 포트를 맞추고 수명을 묶어야 한다. 엔진 코드가 바뀌면 남아 있는 엔진을 내려야 한다(2026-10-08에 두 번 겪음).
- 이미지가 PNG로 인코딩되어 HTTP를 지나므로 장면 하나 읽는 데 0.16초가 든다. 썸네일과 다음 장면 미리 읽기도 같은 길을 지난다.
- 같은 규칙(마스크 RLE, 박스 보정)이 두 언어에 나뉘어 있어 한쪽만 고치기 쉽다.

## 결정 사항

- Python 엔진 5개 모듈(`ball_labeler.py`, `labeler_native.py`, `labeler_assist.py`, `labeler_auto.py`, `labeler_selection.py`)의 기능을 C++ 정적 라이브러리 `labeler_engine`으로 옮기고 `boccia_labeler.exe`에 링크한다. HTTP, 포트, 엔진 프로세스, 엔진 로그는 없어진다.
- **파일 형식은 그대로다.** `project.json`, `<프로젝트>-selection.json`, `<프로젝트>-auto-status.json`, 내보내기 폴더(COCO, 검출용 COCO, YOLO, crops, manifest.json)는 Python 엔진이 쓰던 형식 그대로 읽고 쓴다. 웹 라벨러와 `tools/*.py`는 저장소에 남고 같은 파일을 계속 읽을 수 있다.
- **응답 모양도 그대로다.** 화면 코드는 지금 HTTP 응답으로 받던 JSON과 같은 모양을 함수 반환값으로 받는다. `Client`가 `Engine`으로 바뀌고 호출 한 줄씩만 바뀐다.
- EfficientSAM은 ONNX Runtime 1.30.0(win-x64, CPU) C++ API로 돌린다. 모델 파일은 실행 파일 옆 `models/efficientsam/`에서 찾고, 없으면 저장소의 `data/models/efficientsam/`을 쓴다.
- 원본 해독은 OpenCV와 zstd만 쓴다. 캘리브레이션 앱이 쓰는 카메라 SDK의 `ImageConvert.dll`은 쓰지 않는다(Python `decode_raw`와 같은 길).
- 옮기는 범위: 촬영 카탈로그와 원본 해독, 라벨 저장소(원자적 저장, revision, 충돌), 장면 색인, 검수·보류, 묶음 분할과 지문, 내보내기, 마스크 넓히기, 색상 채우기, 썸네일, 촬영 통계, 내보내기 기록, 대표 프레임 사이드카 읽기, SAM 클릭·박스 분할, 원 후보, 이전 프레임 추적, 폴리곤 마스크, 자동 초안(씨앗 캐시 포함), 대표 프레임 필터 만들기.
- 범위 밖: 이미지 폴더 입력(웹 라벨러만 지원), HTTP 서버, 촬영 추가와 내용 기반 촬영 ID(2차), 원 라벨, 학습 스크립트.

## 고려한 방식

| 방식 | 내용 | 판단 |
| --- | --- | --- |
| **A. 같은 JSON 모양의 인프로세스 엔진** | Python 함수를 하나씩 C++로 옮기고, 반환값은 지금의 HTTP 응답과 같은 `nlohmann::json`으로 둔다. | **채택.** 화면 코드 변경이 기계적이다. 위험은 엔진 이식에만 있고, 그 위험은 Python 테스트를 옮긴 C++ 테스트와 실데이터 대조로 막는다. |
| B. 타입 구조체로 전면 재작성 | 엔진과 화면 사이를 `struct Scene`, `struct Ball` 같은 타입으로 다시 설계한다. | 모든 화면 파일을 건드리고 일정이 두 배가 된다. 사용자에게 보이는 이득이 없다. 필요해지면 A 위에서 모듈별로 할 수 있다. |
| C. Python 임베딩 | 실행 파일 안에서 CPython을 띄워 지금 코드를 그대로 부른다. | 배포 폴더에 Python 런타임과 패키지(수백 MB)를 넣어야 하고 DLL 충돌이 생긴다. 통합의 목적에 맞지 않는다. |

## 구조

네임스페이스는 지금처럼 `boccia::labeler`. 새 파일은 `src/`에 평평하게 둔다(저장소 관례).

| 새 파일 | 옮기는 Python | 내용 |
| --- | --- | --- |
| `src/labeler_util.hpp/.cpp` | `ball_labeler.atomic_json` | SHA-256(Windows CNG `BCryptHash`, 새 의존성 없음), 128비트 난수 hex(uuid4 대신), UTC ISO 시각과 지역 시각 문자열, 원자적 JSON 저장(임시 파일 → flush → `MoveFileExW`), Python `Path` 정렬과 같은 경로 정렬 |
| `src/labeler_catalog.hpp/.cpp` | `ball_labeler.decode_raw`, `Catalog` | `session.json` 탐색, `frames.jsonl` 읽기, 촬영 ID(상대 경로 sha256 앞 16자)와 원본 서명, 프레임 해독(Mono8, RGB8, BGR8, BayerRG8, padding, zstd와 바이트 델타), `nearest`, `metadata` |
| `src/labeler_store.hpp/.cpp` | `ball_labeler.validate_boxes`, `Project`, `selected_reviewed`, `labeler_selection.SelectionFile` | 프로젝트 열기와 서명 검사, `summary`, `mutate`(revision 검사와 `Conflict`), `commitAutomatic`(auto_revision), `frame`, `saveFrame`, `saveSplits`, 대표 프레임 사이드카 읽기와 `view` |
| `src/labeler_export.hpp/.cpp` | `ball_labeler.export_dataset`, `Project.start_export` | 내보내기 폴더 생성, PNG, COCO 두 벌, YOLO, crops, data.yaml, manifest, INCOMPLETE 표시, 백그라운드 작업과 진행률 |
| `src/labeler_native.hpp/.cpp` | `labeler_native.py` 전체 | 장면 색인, 검수 대상, 지문, 촬영 통계, 썸네일, 내보내기 기록, growable/colorable, 색상 채우기 작업, 묶음 분할 계획·확정, 검수, 마스크 넓히기, `nativeCatalog` |
| `src/labeler_assist.hpp/.cpp` | `labeler_assist.py`(RLE는 `native_labeler_model`에 이미 있음) | `maskContours`, `overlap`, `ballColor`, `polygonProposal`, `BallAssist`(ONNX 세션, 크롭 임베딩 캐시, 클릭·박스 분할, 후보 선택), 원 후보(Hough), 이전 프레임 추적(템플릿 매칭) |
| `src/labeler_selection.hpp/.cpp` | `labeler_selection.match_boxes/features/ball_patch/compare_frames/build_selection` | 대표 프레임 필터 v2 만들기 |
| `src/labeler_auto.hpp/.cpp` | `labeler_auto.py` 전체 | 씨앗 캐시(검출 jsonl과 스테레오 matches 인증), 외형 비교와 마스크 이동, 카메라별 작업 스레드, 32장 단위 커밋, 상태 파일 |
| `src/labeler_engine.hpp/.cpp` | `ball_labeler.Handler`의 라우팅, `LabelServer` | 화면이 쓰는 파사드 `Engine`. 위 모듈을 묶고 잠금을 건다. |

### Engine 인터페이스

```cpp
namespace boccia::labeler {
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };   // 옛 HTTP 409
struct EnginePaths { std::filesystem::path source, project, exports, models, root; };
class Engine {
public:
    explicit Engine(EnginePaths paths);        // 카탈로그 탐색, 프로젝트 열기. 실패하면 Python과 같은 문구로 throw
    ~Engine();                                 // 백그라운드 작업을 멈추고 기다린다
    Json catalog() const;                      // GET /api/catalog
    Json nativeCatalog() const;                // GET /api/native/catalog (api_version 5)
    Json frame(const std::string& recording, const std::string& camera, int index) const;         // GET /api/frame
    cv::Mat image(const std::string& recording, const std::string& camera, int index) const;      // GET /api/image, BGR 그대로
    cv::Mat thumbnail(const std::string& recording, const std::string& camera, int index, int width = 128) const;
    Json saveAnnotation(const Json& payload);  // POST /api/annotation → {"summary": …}
    Json saveSplits(const Json& payload);      // POST /api/splits
    Json nativeAction(const std::string& action, const Json& payload);   // POST /api/native/<action>
    Json assist(const std::string& method, const Json& payload);        // POST /api/assist/<method>
    Json assistStatus() const;  Json exportStatus() const;  Json exportHistory() const;  Json colorFillStatus() const;
    Json autoStatus() const;    Json autoStart();           Json autoStop();
};
}
```

- 반환 JSON의 키와 값은 지금 HTTP 응답과 같다. 다른 점은 둘뿐이다. `image`와 `thumbnail`은 PNG·JPEG 바이트가 아니라 `cv::Mat`를 돌려주고, `nativeCatalog`의 `api_version`은 5가 된다(화면이 옛 엔진에 붙을 일이 없으니 검사는 지운다).
- 오류는 Python의 `ValueError` 문구 그대로 `std::runtime_error`로, revision 불일치와 "본 것과 다른 라벨"은 `Conflict`로 던진다. 화면의 `catch (const Conflict&)`는 그대로 쓴다.

## 데이터 흐름

- **시작.** `Application::connect()`가 작업 스레드에서 `Engine`을 만든다. 카탈로그 탐색(27개 `frames.jsonl`, 6.6 MB `project.json`)은 1초 안에 끝난다. 실패 문구("기존 라벨 프로젝트와 촬영 데이터가 다릅니다…")는 지금처럼 화면 상단에 뜬다.
- **장면 읽기.** `fetchScene`이 카메라마다 `engine->frame()`과 `engine->image()`를 부른다. 원본 해독 결과 `cv::Mat`가 바로 텍스처가 된다. PNG 왕복이 없어 장면 하나가 수십 ms로 줄어든다.
- **저장.** `saveAnnotation`은 `mutate(expected_revision, …)`을 거친다. 상태 전체를 복사해 바꾼 뒤 파일에 쓰고 교체한다(Python과 같은 의미: 쓰기에 실패하면 메모리 상태는 그대로). 6.6 MB 상태 복사는 수십 ms다.
- **백그라운드 작업.** 내보내기, 색상 채우기, 자동 초안은 `std::jthread`로 돈다. 상태는 `Json`이고 화면은 지금처럼 3초마다 `exportStatus`·`colorFillStatus`·`autoStatus`·`nativeCatalog`를 읽는다. 마스크 넓히기는 요청 안에서 동기로 끝낸다(2,435개 마스크 팽창은 몇 초).
- **동시성.** `Engine`에 `std::recursive_mutex` 하나(Python의 `RLock`과 같은 자리). 화면의 모든 호출은 `launch()`가 만든 작업 스레드에서 들어오고, 작업 스레드끼리는 잠금으로 줄을 선다. `Application`은 소멸 전에 모든 future를 기다리므로 `Engine*`를 람다에 잡아도 안전하다.
- **SAM.** `BallAssist`는 첫 분할 요청에 세션을 만든다(인코더 24.8 MB, 디코더 16.6 MB). 임베딩 캐시는 Python과 같이 `(프레임 키, 크롭 사각형)` 하나다. 자동 초안은 작업 스레드마다 `BallAssist`를 따로 둔다(Python과 같음).

## 그대로 유지하는 규칙

- **촬영 ID와 원본 서명.** `sha256(상대 경로 posix)[:16]`, 서명은 `sha256(원본 절대 경로 문자열)`에 촬영마다 `상대 경로 + 카메라 ID + frames.jsonl 바이트 + frames.raw 크기`를 순서대로 더한 값. 순서는 Python `sorted(Path)`와 같아야 하므로 경로를 소문자 조각 단위로 비교해 정렬한다. 절대 경로는 `weakly_canonical`로 얻어 `D:\works\bochia\data\imports` 형태(역슬래시)가 되게 한다. 현재 프로젝트의 서명 `f5c3ffc0…`와 같아야 열린다.
- **JSON 저장.** nlohmann은 키를 알파벳 순으로 쓰고 Python은 넣은 순으로 쓴다. 두 엔진 다 순서에 의존하지 않는다. 숫자는 정수·실수가 섞여도 된다(Python `validate_boxes`는 실수로, C++ `boundingRect`는 정수로 쓴다). 프레임이 128개 미만이면 들여쓰기 2, 아니면 한 줄, 끝에 줄바꿈.
- **지문.** 같은 스냅샷 객체(`source`, `frames`, `selected`, `splits`, `groups`)를 키 정렬·구분자 없음·ASCII 이스케이프로 직렬화한 SHA-256. Python `json.dumps(sort_keys=True, separators=(",", ":"))`와 바이트 단위로 같게 하는 것을 목표로 하고 QA 사본에서 비교한다. 아직 확정한 분할이 없으므로(현재 프로젝트에 `native_split` 없음) 달라도 잃는 것은 없다.
- **검수 규칙.** 한 장면의 촬영만 함께, 같은 시점의 프레임만, `seen` 공 수가 다르면 `Conflict`, 저장 시 `contours` 제거, `native_split` 무효화. 분할 확정은 미검수 0장, 그룹 3개 이상, 그룹은 한 분할, 세 분할 모두 검수 이미지 보유. 내보내기는 확정된 지문이 유효할 때만.
- **대표 프레임.** 사이드카가 있으면 사람이 손댄 프레임은 항상 포함(`human_preserved`). 필터 v2 규칙값(`center_pixels` 8 등)과 비교 순서를 그대로 옮긴다.
- **SAM 전후 처리.** 크롭 반경 `max(32, r×3.5)`, RGB/255, 박스 꼭짓점 라벨 2·3, 후보는 `logit ≥ 0`의 연결 성분 중 첫 클릭을 품은 것, 모든 클릭의 포함·제외가 맞는 것, 면적 4 이상·크롭의 70% 이하·한 변이 크롭의 95% 이하, 그중 IoU 예측이 가장 큰 것.
- **색상 분류.** 5×5 침식 안쪽 HSV, 채도 >120 픽셀이 40% 이상이면 최빈 색상 <12 또는 >165는 빨강, 95~135는 파랑, 아니면 명도 중앙값 <22는 검정, 나머지 흰색. 통합 뒤 추가(2026-10-08 사용자 요청 "검출된 공 컬러 자동으로"): 컬러 카메라의 색상 없는 공은 앱 시작 때 백그라운드로, 분할 제안 때 그 자리에서 채운다. 백그라운드 채움은 `auto_revision`에 쓴다.
- **공 둘레 다시 재기(2026-10-08 사용자 요청 "캐니 + 랜삭 원 피팅").** `refineCircle`: 마스크 주변 크롭 → 3×3 블러 → Canny(20/60) → 마스크 윤곽 ±3px 띠의 에지 → 3점 외접원 RANSAC(300회, 인라이어 1px) → 인라이어 기하 최소제곱(가우스·뉴턴) → 각 인라이어를 그래디언트 포물선 보간으로 서브픽셀 이동 뒤 재맞춤 → 인라이어 12개 이상, 36구간 중 절반 이상, 마스크와 크기 35%·위치 0.5r 안이면 채택. 채택되면 마스크는 원 안의 픽셀, `detection_bbox`는 원의 상자, `circle`에 [x, y, r]. 일괄 작업(`refine-circles`: 확인한 이미지 포함·이미 잰 공 다시 재기·둘레 여유 옵션, `auto_revision`, 여유는 `circle_margin`으로 저장)과 SAM 제안에 적용한다. 손으로 그린 윤곽·직접 보정한 박스·가려진 공은 제외하고 공을 지우거나 더하지 않는다. 검수 화면(`Project::frame()`)은 저장된 라벨만 보여 준다: 사전 작업과 본작업의 분리(2026-10-08 사용자 요청). 복사본 400개에서 영상 경계와 0.5px 넘게 어긋난 공 24%→5%, 1px 넘게 5%→0%, 파라미터(임계값 10~40, 띠 2~4)에 둔감.
- **OpenCV 버전.** Python은 OpenCV 5.0, C++는 4.11이다. Bayer 해독, INTER_AREA 축소, Hough, CLAHE, 템플릿 매칭은 실제 프레임에서 결과를 대조해 차이가 있으면 기록한다.

## 배포 폴더

`bin/release/labeler/`에 `boccia_labeler.exe`, `opencv_world4110.dll`, `onnxruntime.dll`, `models/efficientsam/`(onnx 두 개, LICENSE, provenance), `data/`(labels, exports). `tools/collect_release.ps1`이 모델과 ONNX Runtime DLL도 복사한다. `onnxruntime_providers_shared.dll`은 CPU만 쓰면 필요 없음을 확인한 뒤 뺀다.

CMake: `ONNXRUNTIME_ROOT`(기본 `${DEPENDENCY_ROOT}/onnxruntime`)에서 임포트 타깃을 만들고 `labeler_engine`에 링크한다. 빌드 후 DLL을 실행 파일 옆에 복사한다. `BOCCIA_LABELER_PYTHON`과 `winhttp` 링크는 지운다.

## 화면 쪽 변경

- `Client client_` → `std::unique_ptr<Engine> engine_`. `connect()`가 작업 스레드에서 만들고 완료 콜백이 보관한다. 람다는 `Engine*`를 잡는다.
- `client.get("/api/…")`, `client.post(…)` 호출 19곳을 `Engine` 메서드로 바꾼다. `Client::frameQuery`, `client.image`, `client.thumbnail`은 `engine->frame/image/thumbnail(recording, camera, index)`가 된다.
- 지우는 것: `native_labeler_client.hpp/.cpp`(WinHTTP, 엔진 시작, Job Object), `--port`, `--no-start-server`, 엔진 로그와 `engineError`, api_version 검사. 옵션 `--source`, `--project`, `--export-root`, `--view` 등은 그대로.
- `native_labeler_tests`의 HTTP 모드(포트 인자)는 실제 촬영이 있을 때 인프로세스 엔진으로 같은 검사를 한다.

## 오류 처리

- 모델 파일 없음: 분할 요청에 "분할 모델이 준비되지 않았습니다. models/efficientsam 폴더에 efficientsam_ti_encoder.onnx와 decoder.onnx를 두세요." `assistStatus().ready`가 false면 자동 초안 시작도 같은 이유로 거부.
- 프로젝트와 원본 불일치, 프레임 범위 밖, 잘린 원본, 지원하지 않는 픽셀 형식: Python 문구 그대로.
- 내보내기 실패: 폴더에 `INCOMPLETE.txt`를 남기고 작업 상태에 `error`.
- 자동 초안: 상태 파일을 32장마다 쓰고, 다음 시작 때 `running`이면 `interrupted`로 바꾼다.
- 앱 종료: `~Engine`이 자동 초안과 색상 채우기에 중단을 알리고 스레드를 합류한다. 내보내기는 끝까지 기다린다(폴더가 깨지지 않게).

## 검증

- **C++ 테스트 `labeler_engine_tests`.** Python 테스트 64개(`test_ball_labeler` 20, `test_native_labeler` 17, `test_labeler_selection` 7, 나머지는 보조 도구 테스트)를 같은 이름의 검사로 옮긴다. 픽스처는 Python `setUp`과 같은 합성 촬영(3 촬영 × 2 카메라 × 3 프레임, 8×6 Mono8, zstd와 델타)을 임시 폴더에 만든다. 모델 파일이 없으면 SAM 검사는 건너뛰고 그 사실을 출력한다.
- **실데이터 대조(QA 사본 `output/native-labeler/v2-qa-project.json`).** 원본 서명 일치, 장면 색인 JSON 비교, 프레임 해독 해시(모노·Bayer 각 3장), 선택 뷰 수(검수 대상 3,866 / 제외 1,303), SAM 마스크 IoU(클릭 10곳), 내보내기 이미지·박스 수, 지문.
- **앱.** `--smoke-hidden`, `--self-test`, `--stage-test`를 QA 사본으로 돌리고 1920·1280 캡처를 지금 캡처와 비교한다.
- **사용자 프로젝트**(`bin/release/labeler/data/labels/project.json`)는 끝까지 손대지 않는다. 완성 뒤 사용자가 새 빌드로 열면 그때 처음 읽는다.

## 2차로 미루는 것

촬영 추가와 내용 기반 촬영 ID, 원본 위치 다시 지정, 이미지 폴더 입력, 원 라벨(가려진 공의 전체 원), 학습 스크립트, 캘리브레이션 도구 배포 폴더.
