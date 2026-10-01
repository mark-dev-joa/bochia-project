param(
    [string]$DependencyRoot = 'D:/works/golf_launch_monitor/thirdparty',
    [string]$SdkRoot = 'C:/Program Files/HuarayTech/MV Viewer'
)
$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot 'build'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DDEPENDENCY_ROOT=$DependencyRoot" "-DOpenCV_DIR=$DependencyRoot/opencv/build" "-DIMV_ROOT=$SdkRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDirectory --config Release --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $buildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Calibration verification failed.' }
