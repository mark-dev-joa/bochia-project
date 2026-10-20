param([switch] $Desktop)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'bin/release/labeler'
# Keep source ASCII for Windows PowerShell 5.1's script decoding.
$launcherName = 'BocciaLauncher.exe'   # 2026-10-20 user request; it was 'Boccia Labeler.exe', which looked like the program itself
$legacyLauncherName = (-join ([char[]]@(0xB77C,0xBCA8,0xB7EC,0x20,0xC2E4,0xD589)))+'.exe'
$build = Join-Path $root 'build-labeler/Release/boccia_labeler_launcher.exe'
if (-not (Test-Path -LiteralPath $build)) { throw 'Build boccia_labeler_launcher first.' }
Copy-Item -LiteralPath $build -Destination (Join-Path $target $launcherName) -Force
foreach ($earlier in @('Boccia Labeler.exe', $legacyLauncherName)) {   # this launcher under its earlier names
    $path = Join-Path $target $earlier
    if (Test-Path -LiteralPath $path) { try { Remove-Item -LiteralPath $path -Force } catch { Write-Host "In use, kept until it is closed: $path" } }
}
if ($Desktop) {
    $name = 'Boccia Labeler'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) ($name+'.lnk')))
    $shortcut.TargetPath = Join-Path $target $launcherName
    $shortcut.WorkingDirectory = $target
    $shortcut.IconLocation = (Join-Path $target $launcherName)+',0'
    $shortcut.Description = 'Boccia Labeler'
    $shortcut.Save()
    $oldName = (-join ([char[]]@(0xBCF4,0xCE58,0xC544,0x20,0xB77C,0xBCA8,0xB7EC)))+'.lnk'
    $oldPath = Join-Path ([Environment]::GetFolderPath('Desktop')) $oldName
    if(Test-Path -LiteralPath $oldPath){$old=$shell.CreateShortcut($oldPath);if($old.TargetPath -eq (Join-Path $target $legacyLauncherName)){Remove-Item -LiteralPath $oldPath}}
    Write-Host "Desktop shortcut: $($shortcut.FullName)"
}
Write-Host "Portable launch icon: $(Join-Path $target $launcherName)"
