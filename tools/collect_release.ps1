# Puts the latest labeler build into bin/release/labeler: the program, the DLLs it runs with and the EfficientSAM
# model files, nothing else.
# Usage: powershell -File tools/collect_release.ps1
# Only the program files are replaced. bin/release/labeler/data (labels, exports) is never touched, and other
# tools get their own folder under bin/release, such as bin/release/calibration.
# A program that is running keeps its file open; that file is moved aside as boccia_labeler-previous.exe so the
# new build can take its place, and the running window keeps working until it is closed.
param([string] $HubStorage = 'D:\Boccia Hub')   # where the hub keeps the labeler versions launchers take (design/hub/LAUNCHER.md)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-labeler/Release'
$target = Join-Path $root 'bin/release/labeler'
if (-not (Test-Path -LiteralPath (Join-Path $build 'boccia_labeler.exe'))) { throw 'Build the labeler first: cmake --build build-labeler --config Release --target boccia_labeler' }
New-Item -ItemType Directory -Path $target -Force | Out-Null
$previous = Join-Path $target 'boccia_labeler-previous.exe'
if (Test-Path -LiteralPath $previous) { try { Remove-Item -LiteralPath $previous -Force } catch { } }
function Copy-Unchanged([string] $sourcePath, [string] $destination) {
    $source = Get-Item -LiteralPath $sourcePath
    $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
    if ($existing -and $existing.Length -eq $source.Length -and $existing.LastWriteTime -eq $source.LastWriteTime) { return }   # already this build
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source.FullName -Destination $destination -Force
}
foreach ($name in 'boccia_labeler.exe', 'opencv_world4110.dll', 'onnxruntime.dll') {
    $destination = Join-Path $target $name
    try { Copy-Unchanged (Join-Path $build $name) $destination }
    catch {
        if ($name -ne 'boccia_labeler.exe') { throw "Could not replace $name. Close the running labeler first. ($($_.Exception.Message))" }
        Move-Item -LiteralPath $destination -Destination $previous -Force
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
        Write-Host 'The labeler is running; it keeps the old file until it is closed. The next start uses the new build.'
    }
}
# The EfficientSAM-Ti files the program segments with; the repository copy comes from tools/setup_labeler_assist.py.
$models = Join-Path $root 'data/models/efficientsam'
foreach ($name in 'efficientsam_ti_encoder.onnx', 'efficientsam_ti_decoder.onnx', 'LICENSE-EfficientSAM.txt', 'provenance.json') {
    $source = Join-Path $models $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Model file missing: $source. Run python tools/setup_labeler_assist.py first." }
    Copy-Unchanged $source (Join-Path $target "models/efficientsam/$name")
}
Get-ChildItem -LiteralPath $target -File -Include '*.exe', '*.dll' | ForEach-Object { '{0,-30} {1:yyyy-MM-dd HH:mm}  {2,6:N1} MB' -f $_.Name, $_.LastWriteTime, ($_.Length / 1MB) }
Get-ChildItem -LiteralPath (Join-Path $target 'models/efficientsam') -File | ForEach-Object { '{0,-30} {1:yyyy-MM-dd HH:mm}  {2,6:N1} MB' -f ('models/efficientsam/' + $_.Name), $_.LastWriteTime, ($_.Length / 1MB) }
# The release becomes the labeler version the hub sends to launchers (design/hub/LAUNCHER.md): python and the hub
# storage are on this PC, the hub itself need not run. A failure leaves the deploy as it is.
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { Write-Warning 'Python not found: this release was not registered with the hub.' }
else {
    & $python.Source (Join-Path $root 'hub/cli.py') release labeler (Split-Path -Parent $target) --storage $HubStorage
    if ($LASTEXITCODE -ne 0) { Write-Warning 'This release was not registered with the hub.' }
}
