param(
    [string]$DependencyRoot = 'D:/works/golf_launch_monitor/thirdparty',
    [string]$SdkRoot = 'C:/Program Files/HuarayTech/MV Viewer'
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

$sdkRuntime = Join-Path $SdkRoot 'Runtime/x64'
Get-ChildItem -LiteralPath $sdkRuntime -File | Where-Object { $_.Extension -in '.dll', '.cti', '.properties' } |
    Copy-Item -Destination $package
Copy-Item -LiteralPath (Join-Path $sdkRuntime 'xml') -Destination (Join-Path $package 'xml') -Recurse
Copy-Item -LiteralPath (Join-Path $sdkRuntime 'userset') -Destination (Join-Path $package 'userset') -Recurse
Copy-Item -LiteralPath (Join-Path $SdkRoot 'Licenses') -Destination (Join-Path $licenses 'MV-Viewer') -Recurse
Copy-Item -LiteralPath (Join-Path $release 'LICENSE-zstd.txt') -Destination $licenses

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
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CALIBRATION.md') -Destination $package
Set-Content -LiteralPath (Join-Path $package 'portable.txt') -Encoding utf8 -Value 'Store settings, layouts, calibration captures and recordings in ./data beside this application.'
@'
Boccia — Windows x64

1. ZIP 전체를 쓰기 가능한 폴더에 압축 해제하세요.
2. boccia_calibration.exe를 실행하세요. DLL과 xml, userset 폴더를 함께 유지하세요.
3. 실제 카메라 연결에는 해당 카메라용 MV Viewer 드라이버가 설치되어 있어야 합니다.
   카메라 없이 녹화 폴더를 재생할 수도 있습니다.

설정·보정값·레이아웃: 이 폴더의 data/ 아래에 생성됩니다.
녹화 기본 위치: data/recordings/
도구 → Recording / Playback: 기본 Lossless / 30 FPS. Record FPS 또는 Original FPS로 변경합니다.
도구 → Ball Detection: 녹화와 실시간 영상에 같은 검출을 적용합니다.
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
