# The WebView2 SDK that Boccia Hub.exe is built with (design/hub/PLAN-2.md Task 5): the headers and the static loader of
# Microsoft.Web.WebView2 1.0.4258.31 from nuget.org, checked by size and SHA-512 before anything is unpacked.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-webview2.ps1
$ErrorActionPreference = 'Stop'
$version = '1.0.4258.31'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg"
$size = 9801922
$sha512 = 'HlGVwvyP/IWiXAU8K57Vmg59khY3DWMOxMnH4AHusFRy0/vDAjKItXUIVNXiXXaq4CLfSZyNUic1wOIlnPLsPQ=='
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'third_party\webview2'
$download = Join-Path ([IO.Path]::GetTempPath()) ('webview2-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $download | Out-Null
try {
    $package = Join-Path $download "microsoft.web.webview2.$version.nupkg"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $package
    $length = (Get-Item -LiteralPath $package).Length
    if ($length -ne $size) { Write-Host "size $length, expected $size"; exit 1 }
    $stream = [IO.File]::OpenRead($package)
    try { $hash = [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash($stream)) } finally { $stream.Dispose() }
    if ($hash -ne $sha512) { Write-Host "SHA-512 $hash, expected $sha512"; exit 1 }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $target 'include'), (Join-Path $target 'lib\x64') -Force | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($package)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName -replace '\\', '/'
            if ($name -like 'build/native/include/*.h') { $out = Join-Path $target ('include\' + $entry.Name) }
            elseif ($name -eq 'build/native/x64/WebView2LoaderStatic.lib') { $out = Join-Path $target 'lib\x64\WebView2LoaderStatic.lib' }
            elseif ($name -eq 'LICENSE.txt') { $out = Join-Path $target 'LICENSE.txt' }
            else { continue }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
        }
    } finally { $zip.Dispose() }
    if (-not (Test-Path -LiteralPath (Join-Path $target 'include\WebView2.h'))) { Write-Host 'WebView2.h missing in the package'; exit 1 }
    Write-Host "WebView2 SDK $version -> third_party\webview2"
} finally {
    Remove-Item -LiteralPath $download -Recurse -Force -ErrorAction SilentlyContinue
}
