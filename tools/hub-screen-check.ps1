# Stage-2 end-to-end check of the hub screen (design/hub/PLAN-2.md Task 6): a hub on a scratch store, two hidden labelers
# on copies of a project that each confirm a scene as 가 and 나, the hub's numbers for them, pictures of the screen, and
# Boccia Hub.exe --check against the same hub. Prints PASS or throws.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/hub-screen-check.ps1 -Project <QA project folder> [-Shots <folder>]
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Shots = '',
    [string]$Labeler = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Labeler) { $Labeler = Join-Path $root 'build-labeler\Release\boccia_labeler.exe' }
if (-not $Shots) { $Shots = Join-Path $root 'design\hub\shots' }
$window = Join-Path (Split-Path -Parent $Labeler) 'Boccia Hub.exe'
foreach ($path in $Project, $Labeler, $window) { if (-not (Test-Path -LiteralPath $path)) { throw "not found: $path" } }
$cli = Join-Path $root 'hub\cli.py'
$scratch = Join-Path $env:TEMP ('boccia-hub-screen-' + $PID)
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$hubOut = Join-Path $scratch 'hub.out'
$hubArguments = @(('"' + (Join-Path $root 'hub\hub.py') + '"'), 'serve', '--port', '0', '--bind', '127.0.0.1', '--print-port', '--storage', ('"' + (Join-Path $scratch 'store') + '"'))
$hub = Start-Process -FilePath 'python' -ArgumentList $hubArguments -RedirectStandardOutput $hubOut -RedirectStandardError (Join-Path $scratch 'hub.err') -NoNewWindow -PassThru
$labelers = @()
$address = ''

function Hub([string[]]$Arguments) {
    $text = (& python $cli --hub $address @Arguments | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "hub cli failed: $($Arguments -join ' ')`n$text" }
    $text
}
function Get-Text([string]$Path) {   # Windows PowerShell 5.1 misreads UTF-8 JSON through Invoke-WebRequest; WebClient is told the encoding
    $client = New-Object Net.WebClient
    $client.Encoding = [Text.Encoding]::UTF8
    try { $client.DownloadString("http://$address$Path") } finally { $client.Dispose() }
}
function Workers { $parsed = Get-Text '/api/workers' | ConvertFrom-Json; $parsed | ForEach-Object { $_ } }   # 5.1 passes a JSON array on as one object
function WaitFor([scriptblock]$Check, [int]$Seconds, [string]$What) {
    foreach ($i in 1..($Seconds * 2)) { $result = & $Check; if ($result) { return $result }; Start-Sleep -Milliseconds 500 }
    $seen = try { Hub @('workers') } catch { $_.Exception.Message }
    throw "timed out: $What`n$seen"
}

try {
    $port = 0
    foreach ($i in 1..100) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $hubOut) -and ((Get-Content -LiteralPath $hubOut -TotalCount 1) -match '^port (\d+)')) { $port = [int]$Matches[1]; break }
    }
    if ($port -eq 0) { throw 'hub did not start' }
    $address = "127.0.0.1:$port"
    foreach ($name in '가', '나') {
        $token = (Hub @('invite', $name)).Trim()
        $copy = Join-Path $scratch "project-$name"
        & robocopy $Project $copy /E /R:0 /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "copy failed: $Project" }
        $arguments = @('--workspace', ('"' + $copy + '"'), '--smoke-hidden', '--view', 'overview', '--prompt', '--timeout', '120',
                       '--hub', $address, '--worker', $name, '--token', $token, '--hub-wait', '20')
        $labelers += Start-Process -FilePath $Labeler -ArgumentList $arguments -PassThru
    }
    $workers = WaitFor -Seconds 60 -What 'two confirmations reported' -Check {
        $all = @(Workers)
        $done = @($all | Where-Object { $_.last_action -and $_.last_action.text -like '장면 확인 · *' -and $_.status.progress -and $_.status.progress.today -ge 1 })
        if ($done.Count -ge 2) { $all } else { $null }
    }
    foreach ($worker in $workers) { Write-Host ("{0}  {1}  확인 {2} / {3}  오늘 {4}" -f $worker.id, $worker.last_action.text, $worker.status.progress.reviewed, $worker.status.progress.target, $worker.status.progress.today) }
    $first = ($workers | Where-Object { $_.worker -eq '가' } | Select-Object -First 1).id
    & python (Join-Path $root 'tools\hub-capture.py') $port $first $Shots
    if ($LASTEXITCODE -ne 0) { throw 'capture failed' }
    $check = (& $window --check --port $port | Out-String)
    if ($check -notmatch 'PASS:') { throw "Boccia Hub.exe --check: $check" }
    if (-not (Get-Text '/ping').Contains('boccia')) { throw 'the check must leave a hub it did not start running' }
    Write-Host 'PASS: two labelers confirmed, screens captured, Boccia Hub.exe loaded the screen'
}
finally {
    foreach ($process in $labelers) {
        if ($process -and -not $process.HasExited) { $process.WaitForExit(90000) | Out-Null; if (-not $process.HasExited) { $process.Kill() } }
    }
    if ($hub -and -not $hub.HasExited) { $hub.Kill() }
    Start-Sleep -Milliseconds 500
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
