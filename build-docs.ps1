$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & bundle exec jekyll build --source docs --destination _site --strict_front_matter
    if ($LASTEXITCODE -ne 0) { throw "Jekyll build failed with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
