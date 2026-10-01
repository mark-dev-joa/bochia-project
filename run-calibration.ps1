$ErrorActionPreference = 'Stop'
$application = Join-Path $PSScriptRoot 'build\Release\boccia_calibration.exe'
if (-not (Test-Path -LiteralPath $application)) {
    throw 'Executable is missing. Run build-calibration.ps1 first.'
}
Start-Process -FilePath $application -WorkingDirectory $PSScriptRoot
