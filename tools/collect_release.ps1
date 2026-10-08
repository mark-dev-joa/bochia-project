# Puts the latest labeler build into bin/release/labeler: the program and the DLL it runs with, nothing else.
# Usage: powershell -File tools/collect_release.ps1
# Only the program files are replaced. bin/release/labeler/data (labels, exports) is never touched, and other
# tools get their own folder under bin/release, such as bin/release/calibration.
# A program that is running keeps its file open; that file is moved aside as boccia_labeler-previous.exe so the
# new build can take its place, and the running window keeps working until it is closed.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-labeler/Release'
$target = Join-Path $root 'bin/release/labeler'
if (-not (Test-Path -LiteralPath (Join-Path $build 'boccia_labeler.exe'))) { throw 'Build the labeler first: cmake --build build-labeler --config Release --target boccia_labeler' }
New-Item -ItemType Directory -Path $target -Force | Out-Null
$previous = Join-Path $target 'boccia_labeler-previous.exe'
if (Test-Path -LiteralPath $previous) { try { Remove-Item -LiteralPath $previous -Force } catch { } }
foreach ($name in 'boccia_labeler.exe', 'opencv_world4110.dll') {
    $source = Get-Item -LiteralPath (Join-Path $build $name)
    $destination = Join-Path $target $name
    $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
    if ($existing -and $existing.Length -eq $source.Length -and $existing.LastWriteTime -eq $source.LastWriteTime) { continue }   # already this build
    try { Copy-Item -LiteralPath $source.FullName -Destination $destination -Force }
    catch {
        if ($name -ne 'boccia_labeler.exe') { throw "Could not replace $name. Close the running labeler first. ($($_.Exception.Message))" }
        Move-Item -LiteralPath $destination -Destination $previous -Force
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
        Write-Host 'The labeler is running; it keeps the old file until it is closed. The next start uses the new build.'
    }
}
Get-ChildItem -LiteralPath $target -File -Include '*.exe', '*.dll' | ForEach-Object { '{0,-30} {1:yyyy-MM-dd HH:mm}  {2,6:N1} MB' -f $_.Name, $_.LastWriteTime, ($_.Length / 1MB) }
