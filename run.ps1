param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$MediaPath
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $projectRoot "build\$Configuration\FFmpegQtPlayer.exe"
if (-not (Test-Path $exe)) {
    & (Join-Path $projectRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if ($MediaPath) {
    & $exe $MediaPath
} else {
    & $exe
}
