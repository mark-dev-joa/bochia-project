param(
    [string]$DependencyRoot = 'D:/works/golf_launch_monitor/thirdparty',
    [string]$SdkRoot = 'C:/Program Files/HuarayTech/MV Viewer',
    [string]$VcRuntimeDirectory = '',
    [string]$CudaDlls = '',   # a folder with the CUDA 12 / cuDNN 9 runtime DLLs to ship beside the program (about 2 GB); empty ships none
    [switch]$RuntimeOnly
)
$ErrorActionPreference = 'Stop'
$release = Join-Path $PSScriptRoot 'build/Release'
$executable = Join-Path $release 'boccia_calibration.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the Release application first.' }

# Always use a new staging directory. Never copy the live profile or build cache.
$stageRoot = Join-Path $PSScriptRoot ('build/package-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $stageRoot 'Boccia-Windows-x64'
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination $package
Copy-Item -LiteralPath (Join-Path $release 'opencv_world4110.dll') -Destination $package
# The learned ball detector: ONNX Runtime beside the program, the model under models/, its licences with the others.
Copy-Item -LiteralPath (Join-Path $release 'onnxruntime.dll') -Destination $package
# The GPU build of ONNX Runtime keeps the CUDA provider in its own DLLs; the CPU build has only the shared one.
$onnxRoot = Join-Path $DependencyRoot 'onnxruntime-gpu-cuda12'   # the GPU build when unpacked beside the CPU one, as CMake prefers it
if (-not (Test-Path -LiteralPath (Join-Path $onnxRoot 'lib/onnxruntime.dll'))) { $onnxRoot = Join-Path $DependencyRoot 'onnxruntime' }
$onnxLib = Join-Path $onnxRoot 'lib'
foreach ($provider in @('onnxruntime_providers_shared.dll', 'onnxruntime_providers_cuda.dll')) {
    if (Test-Path -LiteralPath (Join-Path $onnxLib $provider)) { Copy-Item -LiteralPath (Join-Path $onnxLib $provider) -Destination $package }
}
if ($CudaDlls) {
    foreach ($pattern in @('cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll', 'cudnn*64_9.dll', 'cufft64_11.dll', 'curand64_10.dll', 'nvrtc64_120_0.dll', 'nvrtc-builtins64_*.dll')) {
        Get-ChildItem -LiteralPath $CudaDlls -File -Filter $pattern | Copy-Item -Destination $package
    }
}
# The models: the byte-input fp32 files (CPU and GPU). An fp16 sibling (<name>-fp16.onnx) would be picked on CUDA, but
# tools/train/onnx_variants.py showed no gain from it on the RTX 5060 Ti, so none ships.
$models = [ordered]@{
    'data/models/boccia/yolo26n-20261009-u8.onnx' = 'models/boccia-yolo26n.onnx'
    'data/models/boccia/yolo26s-20261009-u8.onnx' = 'models/boccia-yolo26s.onnx'
}
foreach ($entry in $models.GetEnumerator()) {
    $model = Join-Path $PSScriptRoot $entry.Key
    if (Test-Path -LiteralPath $model) {
        New-Item -ItemType Directory -Path (Join-Path $package 'models') -Force | Out-Null
        Copy-Item -LiteralPath $model -Destination (Join-Path $package $entry.Value)
    }
}
Copy-Item -LiteralPath (Join-Path $onnxRoot 'LICENSE') -Destination (Join-Path $licenses 'ONNX-Runtime-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $onnxRoot 'ThirdPartyNotices.txt') -Destination (Join-Path $licenses 'ONNX-Runtime-ThirdPartyNotices.txt')

$sdkRuntime = Join-Path $SdkRoot 'Runtime/x64'
Get-ChildItem -LiteralPath $sdkRuntime -File | Where-Object { $_.Extension -in '.dll', '.cti', '.properties' } |
    Copy-Item -Destination $package
Copy-Item -LiteralPath (Join-Path $sdkRuntime 'xml') -Destination (Join-Path $package 'xml') -Recurse
Copy-Item -LiteralPath (Join-Path $sdkRuntime 'userset') -Destination (Join-Path $package 'userset') -Recurse
Copy-Item -LiteralPath (Join-Path $SdkRoot 'Licenses') -Destination (Join-Path $licenses 'MV-Viewer') -Recurse
Copy-Item -LiteralPath (Join-Path $release 'LICENSE-zstd.txt') -Destination $licenses

# Deploy one complete x64 VC runtime after the SDK files, replacing its older VC14 DLLs.
# Use the licensed VS redistributable directory, never DLLs harvested from System32.
if (-not $VcRuntimeDirectory) {
    $cache = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build/CMakeCache.txt')
    $instance = $cache | Where-Object { $_ -match '^CMAKE_GENERATOR_INSTANCE:INTERNAL=' } | Select-Object -First 1
    if (-not $instance) { throw 'Specify -VcRuntimeDirectory with the x64 Microsoft.VC*.CRT redistributable folder.' }
    $vsRoot = $instance.Substring($instance.IndexOf('=') + 1)
    $redistRoot = Join-Path $vsRoot 'VC/Redist/MSVC'
    $version = Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $version) { throw 'Visual C++ redistributable files are not installed.' }
    $crt = Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crt) { throw 'The x64 VC runtime folder is missing.' }
    $VcRuntimeDirectory = $crt.FullName
}
foreach ($required in @('msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $VcRuntimeDirectory $required))) { throw "VC runtime is incomplete: $required" }
}
Get-ChildItem -LiteralPath $VcRuntimeDirectory -File -Filter '*.dll' | Copy-Item -Destination $package -Force
$vcRuntimeVersion = (Get-Item -LiteralPath (Join-Path $package 'msvcp140.dll')).VersionInfo.FileVersion
@"
Microsoft Visual C++ Runtime (x64) — app-local deployment
Version: $vcRuntimeVersion
Copyright Microsoft Corporation. All rights reserved.
Distributed from the Visual Studio VC/Redist/MSVC x64 CRT directory.
These DLLs are included beside the executable; no separate VC++ installer is required on Windows 10/11 x64.
Keep these files together and update them when publishing future application packages.
Redistribution information: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files
Distributable code list: https://aka.ms/vs/17/redist.txt
"@ | Set-Content -LiteralPath (Join-Path $licenses 'Microsoft-VC-Runtime.txt') -Encoding utf8

$notices = @{
    'imgui/LICENSE.txt' = 'imgui.txt'
    'glfw/LICENSE.md' = 'glfw.md'
    'glad/LICENSE' = 'glad.txt'
    'nlohmann/LICENSE.MIT' = 'nlohmann-json.txt'
    'opencv/LICENSE.txt' = 'opencv.txt'
}
foreach ($entry in $notices.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $DependencyRoot $entry.Key) -Destination (Join-Path $licenses $entry.Value)
}
$opencvThirdParty = Join-Path $DependencyRoot 'opencv/sources/3rdparty'
Get-ChildItem -LiteralPath $opencvThirdParty -Recurse -File | Where-Object {
    $_.Name -match '^(LICENSE|COPYING|NOTICE|COPYRIGHT)([._-].*)?$'
} | ForEach-Object {
    $relative = $_.FullName.Substring($opencvThirdParty.Length + 1)
    $destination = Join-Path $licenses ('OpenCV-thirdparty/' + $relative)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination
}
# Keep runnable files apart from compiler outputs, tests, and the labeler.
# Preserve any settings and recordings already present in this destination.
$runtimeOutput = Join-Path $PSScriptRoot 'bin/release/calibration'
New-Item -ItemType Directory -Path $runtimeOutput -Force | Out-Null
Get-ChildItem -LiteralPath $package -File | ForEach-Object {
    $runtimeFile = Join-Path $runtimeOutput $_.Name
    if (-not (Test-Path -LiteralPath $runtimeFile) -or
        (Get-FileHash -LiteralPath $_.FullName).Hash -ne (Get-FileHash -LiteralPath $runtimeFile).Hash) {
        Copy-Item -LiteralPath $_.FullName -Destination $runtimeFile -Force
    }
}
foreach ($runtimeFolder in @('xml', 'userset', 'licenses', 'models')) {
    if (Test-Path -LiteralPath (Join-Path $package $runtimeFolder)) {
        Copy-Item -LiteralPath (Join-Path $package $runtimeFolder) -Destination $runtimeOutput -Recurse -Force
    }
}
Write-Output "Executable: $(Join-Path $runtimeOutput 'boccia_calibration.exe')"
if ($RuntimeOnly) { return }

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CALIBRATION.md') -Destination $package
Set-Content -LiteralPath (Join-Path $package 'portable.txt') -Encoding utf8 -Value 'Store settings, layouts, calibration captures and recordings in ./data beside this application.'
@'
Boccia — Windows 10/11 x64

1. ZIP 전체를 쓰기 가능한 폴더에 압축 해제하세요.
2. boccia_calibration.exe를 실행하세요. DLL과 xml, userset 폴더를 함께 유지하세요.
3. 실제 카메라 연결에는 해당 카메라용 MV Viewer 드라이버가 설치되어 있어야 합니다.
   카메라 없이 녹화 폴더를 재생할 수도 있습니다.
4. VC++ 런타임 DLL을 포함하므로 별도의 Visual C++ 재배포 패키지, Python, Visual Studio 설치는 필요하지 않습니다.
   그래픽 드라이버는 OpenGL 3.3을 지원해야 합니다. Windows N 버전은 Media Feature Pack이 필요할 수 있습니다.

설정·보정값·레이아웃: 이 폴더의 data/ 아래에 생성됩니다.
녹화 기본 위치: data/recordings/
도구 → Recording / Playback: 기본 Lossless / 30 FPS. Record FPS 또는 Original FPS로 변경합니다.
도구 → Ball Detection: 녹화와 실시간 영상에 같은 검출을 적용합니다.
공 자동 3D 위치의 기본 Position은 Mono pair + Color입니다. 모노 2대에서 위치를 계산하고 컬러는 색상과 겹침·가림 보완에 사용합니다. 컬러 보완 위치는 Mono+C로 표시합니다.
공의 XYZ·바닥까지 거리는 m, 재투영 RMS·에피폴 오차는 px, 광선 간 RMS는 mm 단위입니다. Matching Limits에서 제한값을 직접 조절할 수 있습니다.
공 라벨에는 ID만 표시하며 기본 선택은 없음입니다. 목록에서 선택한 공에만 삼각측량 선을 표시하고 Clear Selection으로 해제합니다. Draw Trajectory를 켜면 같은 추적 ID의 관측 궤적을 그립니다.
처리한 매 프레임의 위치·시간·추적 ID는 data/ball-tracks/tracks-*/positions.jsonl과 positions.csv에 자동 저장합니다. 보정값과 메타데이터를 함께 보관합니다.
floor? 표시는 보정값과 바닥 원점의 불일치 가능성을 뜻합니다. 절대 위치 정확도는 실제 거리 기준으로 보정 후 확인하세요.
도구 → Triangulation: Pick Point 후 양쪽 영상의 같은 지점을 클릭하면 P1, P2…가 목록에 추가됩니다. XYZ와 광선 간 최단거리는 mm 단위입니다. 목록 선택 시 3D View에 두 광선, 최단거리 선분, 중앙점을 표시합니다.
보기 → Simulation: 볼 프리셋과 v0·각도·높이를 설정한 뒤 Launch로 던지기·바운스를 실행합니다. 카메라 연결 없이 사용할 수 있습니다.
시뮬레이션 히스토리는 data/simulation-history.json에 저장됩니다. 모든 궤적을 함께 표시하며 기록 선택으로 비교, 선택 삭제 또는 Clear가 가능합니다.
기존 data/settings.json과 data/ui-layout.json이 있다면 종료 상태에서 data/로 옮길 수 있습니다.

사용법: CALIBRATION.md
빌드 정보: build-info.json
외부 라이브러리 고지: licenses/

이 패키지에는 개인 카메라 보정값, 녹화 영상, 계정 정보가 포함되지 않습니다.
'@ | Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding utf8

$manifest = Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($package.Length + 1).Replace('\','/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$buildInfo = [ordered]@{
    createdUtc = [DateTime]::UtcNow.ToString('o')
    platform = 'windows-x64'
    application = 'boccia_calibration.exe'
    vcRuntime = [ordered]@{ deployment = 'app-local'; version = $vcRuntimeVersion }
    files = @($manifest)
}
$buildInfo | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding utf8

$output = Join-Path $PSScriptRoot 'releases'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$zip = Join-Path $output 'Boccia-Windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii -Value "$hash  Boccia-Windows-x64.zip"
Write-Output "Package: $zip"
Write-Output "Staging: $package"
Write-Output "Bytes: $((Get-Item -LiteralPath $zip).Length)"
Write-Output "SHA256: $hash"
