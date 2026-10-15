# Installs the NVIDIA runtime DLLs the tracker's GPU detector needs (ONNX Runtime's CUDA 12 build: CUDA 12.8 and cuDNN 9)
# from NVIDIA's public redistributable archives (developer.download.nvidia.com). Each archive is checked against the
# SHA-256 NVIDIA publishes and only the needed DLLs are taken out: 1.9 GB of downloads, 2.1 GB installed.
# The tracker ships this script beside itself and runs it from its CUDA dialog or "boccia_tracker cuda install"; it finds
# the DLLs beside itself, in its cuda\ or in the release folder's cuda\ (src/model_paths.hpp cudaRuntimeFolder).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File install-cuda.ps1 [-Target <folder>]
#   default target: the release folder's cuda\ when this script sits in one of its app folders (models\ one level up),
#   otherwise cuda\ beside this script (src/tracker_setup.cpp cudaInstallTarget).
# Output lines, read by the tracker (src/tracker_setup.cpp CudaProgress):
#   gpu <name> | total <bytes> | part <i>/<n> <archive> <bytes> | bytes <done> <size> | installed <dlls> <bytes> <folder>
#   error driver | error download <message> | error checksum <archive> | error <message>   (exit code 1)
# Downloads are kept in %TEMP%\boccia-cuda until the install is done, so a second try only fetches what is missing.
param([string]$Target = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false }   # the tracker reads UTF-8

function Save-Archive([string]$Url, [string]$Path, [long]$Size) {
    $partial = "$Path.part"
    try {
        $request = [Net.HttpWebRequest]::Create($Url)
        $request.Timeout = 30000; $request.ReadWriteTimeout = 60000
        $response = $request.GetResponse()
        try {
            $stream = $response.GetResponseStream()
            $file = [IO.File]::Create($partial)
            try {
                $buffer = New-Object byte[] (1MB)
                $done = 0L; $shown = -1
                while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $file.Write($buffer, 0, $read); $done += $read
                    $percent = [int][Math]::Floor(100 * $done / [Math]::Max($Size, 1))
                    if ($percent -ne $shown) { Write-Host "bytes $done $Size"; $shown = $percent }
                }
            } finally { $file.Dispose(); $stream.Dispose() }
        } finally { $response.Dispose() }
    } catch { throw "download $($_.Exception.Message)" }
    Move-Item -LiteralPath $partial -Destination $Path -Force
}

try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (-not $Target) {
        $release = Split-Path -Parent $PSScriptRoot
        $Target = if (Test-Path -LiteralPath (Join-Path $release 'models')) { Join-Path $release 'cuda' } else { Join-Path $PSScriptRoot 'cuda' }
    }
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    $Target = (Resolve-Path -LiteralPath $Target).Path

    $gpu = $null
    try { $gpu = & nvidia-smi --query-gpu=name --format=csv,noheader 2>$null | Select-Object -First 1 } catch {}
    if (-not $gpu) { throw 'driver' }
    Write-Host "gpu $gpu"

    $cudaBase = 'https://developer.download.nvidia.com/compute/cuda/redist/'
    $cudnnBase = 'https://developer.download.nvidia.com/compute/cudnn/redist/'
    try {
        $cuda = Invoke-RestMethod -Uri ($cudaBase + 'redistrib_12.8.1.json')
        $cudnn = Invoke-RestMethod -Uri ($cudnnBase + 'redistrib_9.10.2.json')
    } catch { throw "download $($_.Exception.Message)" }
    $parts = @(
        @{ Base = $cudaBase;  Entry = $cuda.cuda_cudart.'windows-x86_64';  Pattern = '^cudart64_12\.dll$' },
        @{ Base = $cudaBase;  Entry = $cuda.libcublas.'windows-x86_64';    Pattern = '^cublas(Lt)?64_12\.dll$' },
        @{ Base = $cudaBase;  Entry = $cuda.libcufft.'windows-x86_64';     Pattern = '^cufft64_11\.dll$' },
        @{ Base = $cudaBase;  Entry = $cuda.cuda_nvrtc.'windows-x86_64';   Pattern = '^nvrtc(64_120_0|-builtins64_128)\.dll$' },
        @{ Base = $cudaBase;  Entry = $cuda.libnvjitlink.'windows-x86_64'; Pattern = '^nvJitLink_120_0\.dll$' },
        @{ Base = $cudnnBase; Entry = $cudnn.cudnn.'windows-x86_64'.cuda12; Pattern = '^cudnn.*64_9\.dll$' }
    )
    foreach ($part in $parts) { if (-not $part.Entry.relative_path) { throw 'NVIDIA manifest changed: an archive path is missing.' } }
    $total = 0L
    foreach ($part in $parts) { $total += [long]$part.Entry.size }
    Write-Host "total $total"

    $temp = Join-Path $env:TEMP 'boccia-cuda'
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $installed = @()
    $index = 0
    foreach ($part in $parts) {
        $index++
        $relative = [string]$part.Entry.relative_path
        $name = [IO.Path]::GetFileName($relative)
        $size = [long]$part.Entry.size
        Write-Host ('part {0}/{1} {2} {3}' -f $index, $parts.Count, $name, $size)
        $file = Join-Path $temp $name
        $expected = [string]$part.Entry.sha256
        $ok = (Test-Path -LiteralPath $file) -and ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $expected)
        if (-not $ok) {
            Save-Archive ($part.Base + $relative) $file $size
            if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) { Remove-Item -LiteralPath $file; throw "checksum $name" }
        }
        $zip = [IO.Compression.ZipFile]::OpenRead($file)
        try {
            $found = 0
            foreach ($item in $zip.Entries) {
                if ($item.Name -match $part.Pattern) {
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($item, (Join-Path $Target $item.Name), $true)
                    $installed += $item.Name; $found++
                }
            }
            if ($found -eq 0) { throw "No DLL matched in $name" }
        } finally { $zip.Dispose() }
    }
    Remove-Item -LiteralPath $temp -Recurse -Force   # the downloaded archives; the DLLs are in place
    $bytes = (Get-ChildItem -LiteralPath $Target -File | Where-Object { $installed -contains $_.Name } | Measure-Object Length -Sum).Sum
    Write-Host ('installed {0} {1} {2}' -f $installed.Count, $bytes, $Target)
} catch {
    Write-Host "error $($_.Exception.Message)"
    exit 1
}
