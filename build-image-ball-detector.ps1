param([string]$DependencyRoot = 'D:/works/golf_launch_monitor/thirdparty')
$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot 'build'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DDEPENDENCY_ROOT=$DependencyRoot" "-DOpenCV_DIR=$DependencyRoot/opencv/build"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDirectory --config Release --target boccia_image_detector --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Image detector build failed.' }
& (Join-Path $PSScriptRoot 'package-image-ball-detector.ps1') -DependencyRoot $DependencyRoot -RuntimeOnly
