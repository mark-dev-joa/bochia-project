# 보치아 보정 프로그램과 연구 문서

**Windows 10/11 x64 실행 파일:** [Boccia-Windows-x64.zip](releases/Boccia-Windows-x64.zip?raw=true)를 다운로드하고 전체 압축을 푼 뒤 `boccia_calibration.exe`를 실행한다. VC++ 런타임과 OpenCV·카메라 SDK DLL을 포함해 별도 Visual C++ 재배포 패키지나 개발 도구 설치는 필요하지 않다. 실제 카메라 연결에는 MV Viewer 드라이버가 필요하며, 그래픽 드라이버는 OpenGL 3.3을 지원해야 한다. Windows N 버전은 Media Feature Pack이 필요할 수 있다. 설정·보정값과 녹화는 압축을 푼 폴더의 `data/`에 저장된다. 개인 설정과 녹화 데이터는 배포 파일에 포함하지 않는다.

기존 Python 허프 서클 뷰어는 그대로 유지한다. 사용법과 빌드 안내는 [HOUGH_VIEWER.md](HOUGH_VIEWER.md)를 참고한다.

모노 2대와 컬러 1대, 최대 3대의 개별 카메라 보정과 2~3대 동시 상대 위치 보정 GUI를 제공한다. 바닥 원점은 보드 자동 검출이나 크기를 아는 직사각형 종이의 모서리 4점으로 지정한다. 실행은 `./run-calibration.ps1`, 빌드·검증은 `./build-calibration.ps1`이다. [프로그램 사용법](CALIBRATION.md)과 [전체 설계](DESIGN.md)를 참고한다. 체스보드·ChArUco 규격을 직접 입력하며 보정값은 단일 `data/settings.json`에 저장한다. 마지막 카메라를 자동 연결·수집하고, 캘리용 밝기 처리는 작업이 끝나면 해제한다.

개별 보정은 장치 패널에서 `Grab`을 시작한 뒤 `도구 → 개별 보정`을 연다. 영상은 창 맨 위에 크게 표시되고 샘플은 오른쪽에 모인다. 영상 아래에 작업 버튼과 카메라·패턴·규격(mm)·목표 샘플·수집 조건을 항상 펼쳐 둔다. `Auto Brightness`와 `Clear Samples`도 이곳에 있다. `Start Calibration` 한 번으로 자동 수집과 계산을 진행하고, 완료 후 같은 프레임의 보정 전·후를 나란히 확인한 뒤 `Save`한다. 작업 중 `Cancel`은 샘플을 유지하며 Grab을 멈추지 않는다.

상대 위치 보정도 영상을 맨 위, 샘플을 오른쪽에 표시하고 작업 버튼과 펼친 옵션을 아래에 둔다. 카메라 2~3대와 보드 규격을 정한 뒤 `Start Calibration`을 누르면 자동 수집부터 계산까지 진행한다. `Cancel`은 Grab과 모은 샘플을 유지하며, 결과는 `R / T`로 확인하고 `Save`로 저장한다.

카메라 영상창은 상태를 한 줄로 표시하고 영상 아래에 `Raw` / `Undistorted`, `Brightness`, `Contrast`를 둔다. 밝기·대비는 화면 표시만 바꾸며 시리얼별로 저장한다. `보기 → Stereo Compare`에서는 두 카메라 영상을 나란히 보고 `Undistorted` / `Rectified`를 비교한다. `Lines`는 같은 색·번호로 대응하는 에피폴라 선을 1~12개 표시한다. 왜곡 보정 영상에서는 기울어진 직선, 렉티피케이션 영상에서는 수평선이 된다. 비교 창도 도킹·분리할 수 있으며 카메라 Grab은 장치 패널에서 관리한다.

`도구 → Ball Detection`에서 전체 `Run` / `Overlay`와 카메라별 검출·표시를 설정한다. Mono는 Hough + 선택적 RANSAC, Color는 원 검출 후 HSV로 Red / Blue / White를 분류한다. 영상에 4점을 찍어 ROI를 지정하고 Gray / Blur / Edges를 보며 파라미터를 조절한다. 검출은 백그라운드에서 실행하며 오버레이는 검출에 사용한 동일 프레임 위에 그린다.

`도구 → Recording / Playback`에서 Grab 중인 카메라들의 원본 데이터를 `Record` / `Stop`으로 저장한다. 기본은 `Lossless` 무손실 압축과 30 FPS 녹화다. `Record FPS`를 바꾸거나 `Original FPS`로 수신한 전체 프레임을 저장할 수 있다. 선택한 프레임의 원본 바이트와 프레임 번호·시각, 장치 설정 XML, 보정값을 함께 저장한다. `Open Folder...`로 녹화 폴더를 열면 카메라 없이 같은 영상 처리·보정 표시를 사용하고 `Play` / `Pause`, 프레임 이동, 탐색을 할 수 있다. 재생 설정은 실시간 카메라의 보정값을 덮어쓰지 않는다.

Jekyll로 빌드하는 한국어 연구 노트. 전체 계산 흐름과 단계별 설명을 한 페이지에 쌓고, 스크롤하며 손으로 계산한다. 현재는 04번 카메라 상대 포즈 초기값·손계산 문제까지 작성했다. 코드는 수식을 익히고 손계산한 뒤 사용자가 구현 단계로 진행할 때 추가한다.

프로그램의 `보기 → 3D View`에서 저장된 바닥 좌표계의 그리드, 작은 카메라 모형과 짧은 촬영 방향선을 확인한다. 상단에는 이동 속도 슬라이더만 표시한다. WASD 이동과 좌·우클릭 드래그 또는 방향키 회전을 지원하고 관찰 시점을 복원한다. 장치 목록에서 카메라 이름을 수정하면 저장되며 영상창 제목과 3D 라벨에 반영된다.

`도구 → Triangulation`에서 `Pick Point`로 두 영상을 고정한 뒤 같은 지점을 양쪽에서 클릭하면 P1, P2…가 목록에 쌓인다. XYZ와 두 광선의 최단거리 오차를 mm로 표시하며, 좌표는 최단거리 선분의 중앙점이다. Raw·왜곡 보정·렉티피케이션을 지원한다. 바닥 원점이 있으면 모든 점을 3D View에 표시하고 목록에서 선택한 점의 두 광선·최단거리 선분·중앙점을 강조한다. `Edit` / `Delete`로 개별 점을 수정·삭제하고, `Live`로 화면 고정을 해제하며 `Clear All`로 목록을 비운다.

## 이미지 드래그 앤 드롭 공 검출

`start-image-ball-detector.cmd`를 실행하거나 `bin/release/image-detector/boccia_image_detector.exe`를 연 뒤 이미지 파일을 창에 드래그 앤 드롭한다. 학습한 보치아 YOLO26n 모델로 전체 이미지에서 공을 찾고 검출 박스·중심 좌표·신뢰도와 **검출 처리 시간(ms)**을 표시한다. PNG, JPG, BMP, TIFF, WebP 및 한글 파일명을 지원한다. `이미지 열기` 버튼도 사용할 수 있다.

다른 PC에서는 [Boccia-Image-Detector-Windows-x64.zip](releases/Boccia-Image-Detector-Windows-x64.zip?raw=true)을 **전체 압축 해제**하고 `boccia_image_detector.exe`를 실행한다. 실행 파일·학습 모델·한국어 폰트·OpenCV·ONNX Runtime·VC++ 런타임을 포함하며 폴더 전체를 옮겨 사용한다. Windows 10/11 x64와 OpenGL 3.3 지원 그래픽 드라이버가 필요하다. 배포본은 CPU로 동작하며 Python, CUDA, 카메라 SDK 설치가 필요 없다.

검출 시간은 원본 이미지의 전처리·모델 추론·결과 좌표 변환을 포함한다. 이미지 파일 읽기, 최초 모델 로딩·1회 워밍업, 화면 그리기는 제외하며 파일 읽기·모델 준비·전체 처리 시간은 따로 표시한다. 작은 화면 미리보기와 관계없이 좌표는 원본 이미지의 픽셀이다. `결과 이미지 저장`으로 박스가 표시된 PNG를 별도로 저장하며 원본 이미지는 덮어쓰지 않는다. GPU가 사용 불가능하면 CPU로 실행하고 실제 실행 장치를 표시한다. 신뢰도 또는 GPU 설정을 바꾼 뒤 `다시 검출`하면 새 설정으로 측정한다.

배포본은 실행 파일 옆 `data/settings.json`의 상대 모델 경로와 CPU 설정을 읽는다. 개발 빌드에서는 기존 공 검출 설정을 사용할 수 있다. 이 프로그램은 해당 설정을 수정하지 않는다. 커밋된 학습 모델은 `data/models/boccia/yolo26n-20261012-1280x960-u8.onnx`다. 개별 빌드·실행:

```powershell
.\build-image-ball-detector.ps1
.\run-image-ball-detector.ps1
.\package-image-ball-detector.ps1 # 다른 PC로 전달할 ZIP 생성
# 화면 없이 같은 검출 경로로 JSON과 결과 PNG를 저장할 수도 있다.
.\bin\release\image-detector\boccia_image_detector.exe --headless --image '공 사진.png' --output 'output/result.json'
```

## Windows 앱 빌드·패키징

Visual Studio 2022의 C++ 빌드 도구, CMake, MV Viewer SDK와 OpenCV 4.11, Dear ImGui docking, GLFW, GLAD, nlohmann/json 소스가 필요하다. 기존 의존성 폴더의 구성은 [CMakeLists.txt](CMakeLists.txt)를 참고한다. Zstandard 1.5.7은 CMake가 검증된 해시로 다운로드해 정적으로 링크한다.

```powershell
.\build-calibration.ps1 -DependencyRoot 'D:/path/to/thirdparty' -SdkRoot 'C:/Program Files/HuarayTech/MV Viewer'
.\package-calibration.ps1 -DependencyRoot 'D:/path/to/thirdparty' -SdkRoot 'C:/Program Files/HuarayTech/MV Viewer'
```

패키징 결과는 `releases/Boccia-Windows-x64.zip`이며 SHA-256은 `releases/SHA256SUMS.txt`에 기록한다. ZIP에는 외부 라이브러리 고지와 파일별 해시를 포함한다. 개발 실행은 프로젝트의 `data/`, `portable.txt`를 포함한 배포 실행은 실행 파일 옆의 `data/`를 사용한다.

## 연구 문서 로컬 실행

Ruby 3.3 계열과 Bundler가 설치된 PowerShell에서:

```powershell
bundle install
.\serve-docs.ps1
```

브라우저 주소: http://127.0.0.1:8770/ . 종료는 실행한 터미널에서 Ctrl+C. 다른 포트는 `./serve-docs.ps1 -Port 8771`로 지정한다. 라이브리로드 포트는 문서 포트 + 100이다. 원본은 `docs/`, 로컬 생성물은 `artifacts/jekyll-preview/`다.

```powershell
.\build-docs.ps1
```

배포용 정적 결과는 `_site/`에 생성된다. 문서 수정은 `docs/`에서 하고 생성물을 직접 편집하지 않는다. 수식은 MathJax, 흐름도는 Mermaid를 사용하며, 두 라이브러리는 버전을 고정한 jsDelivr URL에서 브라우저가 불러온다. 오프라인에서는 수식·흐름도 렌더링이 제한되고 절차 표와 본문은 계속 읽을 수 있다.

## 문서 추가 규칙

- 학습 내용은 `docs/index.md` 한 페이지에 번호 순서대로 추가한다. 02번부터는 01번 다음, 프로젝트 참고 자료 앞에 이어 붙인다.
- 각 단계는 기호와 단위 → 수식 → 숫자를 대입한 손계산 예제 → 직접 풀 문제 → 펼쳐 보는 답 순서로 구성한다.
- 학습 본문에 코드·의사코드·함수 이름을 넣지 않는다. 사용자가 구현 단계로 진행할 때 코드를 추가한다. 문서 서버 실행 명령은 별개다.
- 예제의 가정과 실제 측정 결과를 구분한다.
- 왜곡 적용·제거와 반복법은 본문 단계와 분리하여 문서 하단의 참고 A에 유지한다. 기존 `#distorted-projection` 앵커는 참고 A의 정방향 투영식으로 연결된다.
- 기존 단계·프로젝트 URL은 통합 문서의 해당 앵커로 이동시킨다.
- 새 단계를 추가하면 탐색과 현재 학습 위치도 갱신한다.

## 나중에 Cloudflare Pages로 배포

아직 외부 배포하지 않았다. Git 연결을 사용하는 경우 저장소 루트를 기준으로 다음 설정을 사용한다.

| 항목 | 값 |
|---|---|
| 프로젝트 루트 | 저장소 루트 |
| 빌드 명령 | `bundle exec jekyll build --source docs --destination _site` |
| 출력 디렉터리 | `_site` |
| 환경 변수 | `RUBY_VERSION=3.3.3` |

배포 시 해당 Pages 빌드 이미지가 지원하는 Ruby 버전을 확인한다. 직접 업로드할 경우에도 로컬에서 빌드한 `_site`만 전달한다. 이미지 수집 데이터, 로컬 로그와 원본 코드 전체를 정적 사이트 출력에 넣지 않는다. 경로에는 Jekyll `relative_url` 필터를 사용한다.

공식 문서: https://developers.cloudflare.com/pages/framework-guides/deploy-a-jekyll-site/

장치 패널은 선택한 카메라의 Connect / Disconnect / Grab / Stop과 Cal / Pose / World 상태 아이콘으로 구성한다. 아이콘을 누르면 저장된 값을 확인할 수 있다. World는 바닥 원점 기준의 카메라 위치와 회전이다.
