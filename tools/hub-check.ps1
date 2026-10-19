# Stage-1 end-to-end check of the hub (design/hub/PLAN-1.md Task 6): a hub on a scratch store, two hidden labelers that
# join as 가 and 나, then the CLI: workers (both joined), send notify, send log, logs, events. Prints PASS or throws.
#   powershell -File tools/hub-check.ps1 [-Workspace <project folder>] [-Labeler <boccia_labeler.exe>]
# Without -Workspace the labelers sit on the start screen; with one they open its overview.
param(
    [string]$Workspace = '',
    [string]$Labeler = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Labeler) { $Labeler = Join-Path $root 'build-labeler\Release\boccia_labeler.exe' }
if (-not (Test-Path -LiteralPath $Labeler)) { throw "labeler not found: $Labeler" }
$cli = Join-Path $root 'hub\cli.py'
$scratch = Join-Path $env:TEMP ('boccia-hub-check-' + $PID)
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$store = Join-Path $scratch 'hub'
$hubOut = Join-Path $scratch 'hub.out'
$hubErr = Join-Path $scratch 'hub.err'
$hubArguments = @(('"' + (Join-Path $root 'hub\hub.py') + '"'), 'serve', '--port', '0', '--bind', '127.0.0.1', '--print-port', '--storage', ('"' + $store + '"'))
$hub = Start-Process -FilePath 'python' -ArgumentList $hubArguments -RedirectStandardOutput $hubOut -RedirectStandardError $hubErr -NoNewWindow -PassThru
$labelers = @()
$address = ''

function Hub {
    param([string[]]$Arguments)
    $text = (& python $cli --hub $address @Arguments | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "hub cli failed: $($Arguments -join ' ')`n$text" }
    $text
}
function WaitFor {
    param([scriptblock]$Check, [int]$Seconds, [string]$What)
    foreach ($i in 1..($Seconds * 2)) {
        $result = & $Check
        if ($result) { return $result }
        Start-Sleep -Milliseconds 500
    }
    throw "timed out: $What"
}

try {
    $port = 0
    foreach ($i in 1..100) {
        Start-Sleep -Milliseconds 100
        if (Test-Path -LiteralPath $hubOut) {
            $line = Get-Content -LiteralPath $hubOut -TotalCount 1
            if ($line -match '^port (\d+)') { $port = [int]$Matches[1]; break }
        }
    }
    if ($port -eq 0) { throw "hub did not start: $(Get-Content -LiteralPath $hubErr -Raw)" }
    $address = "127.0.0.1:$port"
    $tokens = @{}
    foreach ($name in '가', '나') {
        $tokens[$name] = (Hub @('invite', $name)).Trim()
        if ($tokens[$name].Length -ne 43) { throw "invite failed for $name" }
    }
    $view = if ($Workspace) { @('--workspace', ('"' + $Workspace + '"'), '--view', 'overview') } else { @('--view', 'welcome') }
    foreach ($name in '가', '나') {
        $arguments = @('--smoke-hidden', '--timeout', '90', '--hub', $address, '--worker', $name, '--token', $tokens[$name], '--hub-wait', '25') + $view
        $labelers += Start-Process -FilePath $Labeler -ArgumentList $arguments -PassThru
    }
    $table = WaitFor -Seconds 30 -What 'two labelers joining' -Check {
        $text = Hub @('workers')
        $joined = @($text -split "`n" | Where-Object { $_ -match '^(가|나)\s' -and $_ -match '붙음' })
        if ($joined.Count -ge 2) { $text } else { $null }
    }
    Write-Host $table
    $notify = Hub @('send', '가', 'notify', '쉬세요')
    if ($notify -notmatch '완료 · 띄움') { throw "notify not finished:`n$notify" }
    $log = Hub @('send', '나', 'log')
    if ($log -notmatch '완료 · labeler-') { throw "log not uploaded:`n$log" }
    $logText = WaitFor -Seconds 10 -What 'the uploaded log' -Check { $text = Hub @('logs', '나'); if ($text -match '시작 · ') { $text } else { $null } }
    $events = WaitFor -Seconds 10 -What 'the events of 가' -Check {
        $text = Hub @('events', '가', '--limit', '2000')
        if (($text -match '시작 · ') -and ($text -match '허브 알림 · 쉬세요')) { $text } else { $null }
    }
    Write-Host 'PASS: two labelers registered, notify delivered, log uploaded, events listed'
}
finally {
    foreach ($process in $labelers) {
        if ($process -and -not $process.HasExited) { $process.WaitForExit(60000) | Out-Null; if (-not $process.HasExited) { $process.Kill() } }
    }
    if ($hub -and -not $hub.HasExited) { $hub.Kill() }
    Start-Sleep -Milliseconds 300
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
