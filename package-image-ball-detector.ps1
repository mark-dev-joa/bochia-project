param(
    [string]$DependencyRoot = 'D:/works/golf_launch_monitor/thirdparty',
    [string]$BuildDirectory = '',
    [string]$VcRuntimeDirectory = '',
    [switch]$RuntimeOnly
)
$ErrorActionPreference = 'Stop'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot 'build' }
$release = Join-Path $BuildDirectory 'Release'
$modelSource = Join-Path $PSScriptRoot 'data/models/boccia/yolo26n-20261012-1280x960-u8.onnx'
$onnxCpu = Join-Path $DependencyRoot 'onnxruntime'
foreach ($required in @((Join-Path $release 'boccia_image_detector.exe'), $modelSource, (Join-Path $onnxCpu 'lib/onnxruntime.dll'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing package input: $required" }
}
# Keep the CPU runtime at the API version used to build the executable.
$builtVersion = (Get-Item -LiteralPath (Join-Path $release 'onnxruntime.dll')).VersionInfo.FileVersion
$cpuVersion = (Get-Item -LiteralPath (Join-Path $onnxCpu 'lib/onnxruntime.dll')).VersionInfo.FileVersion
if ($cpuVersion -ne $builtVersion) { throw "CPU ONNX Runtime version $cpuVersion does not match build version $builtVersion" }
$stageRoot = Join-Path $PSScriptRoot ('build/image-detector-package-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $stageRoot 'Boccia-Image-Detector'
foreach ($folder in @('models', 'fonts', 'licenses', 'data')) {
    New-Item -ItemType Directory -Path (Join-Path $package $folder) -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $release 'boccia_image_detector.exe') -Destination $package
Copy-Item -LiteralPath (Join-Path $release 'opencv_world4110.dll') -Destination $package
Copy-Item -LiteralPath (Join-Path $onnxCpu 'lib/onnxruntime.dll') -Destination $package
Copy-Item -LiteralPath $modelSource -Destination (Join-Path $package 'models/boccia-yolo26n-1280x960.onnx')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/fonts/NotoSansKR.ttf') -Destination (Join-Path $package 'fonts')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/fonts/NotoSansKR-OFL.txt') -Destination (Join-Path $package 'licenses')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/licenses/Ultralytics-AGPL-3.0.txt') -Destination (Join-Path $package 'licenses')
Copy-Item -LiteralPath (Join-Path $onnxCpu 'LICENSE'),(Join-Path $onnxCpu 'ThirdPartyNotices.txt') -Destination (Join-Path $package 'licenses')
foreach ($entry in @{'imgui/LICENSE.txt'='imgui.txt'; 'glfw/LICENSE.md'='glfw.md'; 'glad/LICENSE'='glad.txt'; 'nlohmann/LICENSE.MIT'='nlohmann-json.txt'; 'opencv/LICENSE.txt'='opencv.txt'}.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $DependencyRoot $entry.Key) -Destination (Join-Path $package ('licenses/' + $entry.Value))
}
$opencvNotices = Join-Path $DependencyRoot 'opencv/sources/3rdparty'
Get-ChildItem -LiteralPath $opencvNotices -Recurse -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|NOTICE|COPYRIGHT)([._-].*)?$' } | ForEach-Object {
    $relative = $_.FullName.Substring($opencvNotices.Length + 1)
    $destination = Join-Path $package ('licenses/OpenCV-thirdparty/' + $relative)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination
}
if (-not $VcRuntimeDirectory) {
    $instance = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') | Where-Object { $_ -match '^CMAKE_GENERATOR_INSTANCE:INTERNAL=' } | Select-Object -First 1
    if (-not $instance) { throw 'Specify the x64 Visual C++ redistributable directory.' }
    $vsRoot = $instance.Substring($instance.IndexOf('=') + 1)
    $redistRoot = Join-Path $vsRoot 'VC/Redist/MSVC'
    $version = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $crt = Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crt) { throw 'Visual C++ redistributable files are not installed.' }
    $VcRuntimeDirectory = $crt.FullName
}
foreach ($required in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $VcRuntimeDirectory $required))) { throw "VC runtime is incomplete: $required" }
}
Get-ChildItem -LiteralPath $VcRuntimeDirectory -File -Filter '*.dll' | Copy-Item -Destination $package
Set-Content -LiteralPath (Join-Path $package 'licenses/Microsoft-VC-Runtime.txt') -Encoding utf8 -Value 'Microsoft Visual C++ x64 Runtime. Copyright Microsoft Corporation. Redistributed from Visual Studio VC/Redist/MSVC under its redistribution terms. https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files'
Set-Content -LiteralPath (Join-Path $package 'portable.txt') -Encoding ascii -Value 'Use the bundled model and ./data/settings.json beside this executable.'
$profile = @{ball_detection=@{model='models/boccia-yolo26n-1280x960.onnx';model_confidence=0.25;cuda=$false;cuda_dlls='';inference_threads=4}}
# No personal calibration or machine-specific training paths go into the distribution.
$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $package 'data/settings.json'), ($profile | ConvertTo-Json -Depth 4), $utf8WithoutBom)
@'
Boccia Image Ball Detector - Windows 10/11 x64

Extract the entire ZIP, then run boccia_image_detector.exe.
Drag a photo onto the window. No image resizing is needed.
Ball boxes, confidence, original pixel coordinates and detection time (ms) are shown.
The trained YOLO26n 1280x960 model, Korean font, OpenCV, ONNX Runtime and Visual C++ runtime are included.
This portable package runs on CPU. Python, CUDA, camera SDK and separate VC++ installation are not required.
An OpenGL 3.3 capable graphics driver is required for the window.
Keep the executable, DLLs, models, fonts, data and licenses together when copying to another PC.
Detection time excludes file reading, model initialization/warm-up and drawing.

Model: YOLO26n fine-tuned on this project's boccia recordings; one class (ball).
Input: RGB uint8 [1,960,1280,3], letterboxed; normalization is inside the model.
The model is for detection in individual photos; 3D coordinates require camera calibration.
'@ | Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding utf8
$files = Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($package.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[ordered]@{application='boccia_image_detector.exe';platform='windows-x64';runtime='CPU';model='models/boccia-yolo26n-1280x960.onnx';onnx_runtime=$cpuVersion;files=@($files)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding utf8
$runtime = Join-Path $PSScriptRoot 'bin/release/image-detector'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
Get-ChildItem -LiteralPath $package -File | ForEach-Object {
    $destination = Join-Path $runtime $_.Name
    if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $_.FullName).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }
}
foreach ($folder in @('models','fonts','licenses')) { Copy-Item -LiteralPath (Join-Path $package $folder) -Destination $runtime -Recurse -Force }
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'data/settings.json'))) { Copy-Item -LiteralPath (Join-Path $package 'data') -Destination $runtime -Recurse }
Write-Output "Image detector: $runtime/boccia_image_detector.exe"
if ($RuntimeOnly) { return }
$releaseDirectory = Join-Path $PSScriptRoot 'releases'
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
$zip = Join-Path $releaseDirectory 'Boccia-Image-Detector-Windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $releaseDirectory 'Boccia-Image-Detector-SHA256SUMS.txt') -Encoding ascii -Value "$hash  Boccia-Image-Detector-Windows-x64.zip"
Write-Output "Package: $zip"
Write-Output "Bytes: $((Get-Item -LiteralPath $zip).Length)"
