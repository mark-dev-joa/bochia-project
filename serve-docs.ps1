param([int]$Port = 8770)
$ErrorActionPreference = 'Stop'
$projectDirectory = $PSScriptRoot
Push-Location $projectDirectory
try {
    & bundle exec jekyll serve --source docs --destination artifacts/jekyll-preview --host 127.0.0.1 --port $Port --livereload --livereload-port ($Port + 100) --force_polling
    if ($LASTEXITCODE -ne 0) { throw "Jekyll server exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
