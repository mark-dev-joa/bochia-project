param([string]$Image = '')
$ErrorActionPreference = 'Stop'
$application = Join-Path $PSScriptRoot 'bin/release/image-detector/boccia_image_detector.exe'
if (-not (Test-Path -LiteralPath $application)) {
    throw 'Run build-image-ball-detector.ps1 first.'
}
if ($Image) {
    $imagePath = (Resolve-Path -LiteralPath $Image).Path
    Start-Process -FilePath $application -WorkingDirectory $PSScriptRoot -ArgumentList @('--image', ('"' + $imagePath + '"'))
} else {
    Start-Process -FilePath $application -WorkingDirectory $PSScriptRoot
}
