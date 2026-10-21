# Puts the latest hub into bin/release/hub (design/hub/PLAN-2.md Task 6): Boccia Hub.exe, the hub's Python files and its
# screen, the screen's font, boccia-hub.cmd for the command line, and the licenses. The hub's data stays where the hub
# keeps it (D:\Boccia Hub by default), never here.
# Usage: powershell -File tools/collect_hub.ps1
# A running Boccia Hub.exe keeps its file open; it is moved aside as Boccia Hub-previous.exe so the new build can take its place.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-labeler/Release'
$target = Join-Path $root 'bin/release/hub'
$exe = Join-Path $build 'Boccia Hub.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build the hub window first: cmake --build build-labeler --config Release --target boccia_hub' }
New-Item -ItemType Directory -Path $target, (Join-Path $target 'hub/ui/fonts'), (Join-Path $target 'licenses') -Force | Out-Null

function Copy-Changed([string]$from, [string]$to) {
    if ((Test-Path -LiteralPath $to) -and ((Get-FileHash -LiteralPath $from).Hash -eq (Get-FileHash -LiteralPath $to).Hash)) { return }
    try { Copy-Item -LiteralPath $from -Destination $to -Force }
    catch {   # the running window holds its exe: move it aside, then copy
        $aside = [IO.Path]::ChangeExtension($to, $null).TrimEnd('.') + '-previous' + [IO.Path]::GetExtension($to)
        if (Test-Path -LiteralPath $aside) { Remove-Item -LiteralPath $aside -Force }
        Move-Item -LiteralPath $to -Destination $aside
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}
Copy-Changed $exe (Join-Path $target 'Boccia Hub.exe')
foreach ($name in '__init__.py', 'hub.py', 'cli.py', 'release.py') { Copy-Changed (Join-Path $root "hub/$name") (Join-Path $target "hub/$name") }
foreach ($name in 'index.html', 'app.js', 'style.css') { Copy-Changed (Join-Path $root "hub/ui/$name") (Join-Path $target "hub/ui/$name") }
Copy-Changed (Join-Path $root 'assets/fonts/NotoSansKR.ttf') (Join-Path $target 'hub/ui/fonts/NotoSansKR.ttf')
Copy-Changed (Join-Path $root 'assets/fonts/NotoSansKR-OFL.txt') (Join-Path $target 'licenses/NotoSansKR-OFL.txt')
$webview = Join-Path $root 'third_party/webview2/LICENSE.txt'
if (Test-Path -LiteralPath $webview) { Copy-Changed $webview (Join-Path $target 'licenses/WebView2-LICENSE.txt') }
Set-Content -LiteralPath (Join-Path $target 'boccia-hub.cmd') -Value "@echo off`r`npython `"%~dp0hub\cli.py`" %*" -Encoding ASCII -NoNewline
Get-ChildItem -LiteralPath $target -Recurse -File | ForEach-Object {
    '{0,-36} {1:yyyy-MM-dd HH:mm} {2,9:N0} B' -f $_.FullName.Substring($target.Length + 1), $_.LastWriteTime, $_.Length
}
