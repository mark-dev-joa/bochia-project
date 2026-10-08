# Puts the latest labeler build into bin/release/labeler: the program and the DLL it runs with, nothing else.
# Usage: powershell -File tools/collect_release.ps1
# Only the program files are replaced. bin/release/labeler/data (labels, exports) is never touched, and other
# tools get their own folder under bin/release, such as bin/release/calibration.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-labeler/Release'
$target = Join-Path $root 'bin/release/labeler'
if (-not (Test-Path -LiteralPath (Join-Path $build 'boccia_labeler.exe'))) { throw 'Build the labeler first: cmake --build build-labeler --config Release --target boccia_labeler' }
New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($name in 'boccia_labeler.exe', 'opencv_world4110.dll') {
    $destination = Join-Path $target $name
    try { Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force }
    catch { throw "Could not replace $name. Close the running labeler first. ($($_.Exception.Message))" }
}
Get-ChildItem -LiteralPath $target -File | ForEach-Object { '{0,-24} {1:yyyy-MM-dd HH:mm}  {2,6:N1} MB' -f $_.Name, $_.LastWriteTime, ($_.Length / 1MB) }
