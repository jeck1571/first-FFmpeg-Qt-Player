param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$QtRoot = $env:QTDIR,
    [string]$VsDevCmd,
    [string]$CMake,
    [string]$Ninja,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $projectRoot "build\$Configuration"

if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    $qtCandidates = @(
        'C:\Qt\6.11.1\msvc2022_64',
        'C:\Qt\6.10.0\msvc2022_64',
        'C:\Qt\6.9.3\msvc2022_64'
    )
    $QtRoot = $qtCandidates |
        Where-Object { Test-Path (Join-Path $_ 'lib\cmake\Qt6\Qt6Config.cmake') } |
        Select-Object -First 1
}
if (-not $QtRoot -or -not (Test-Path (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw 'Qt 6 MSVC was not found. Pass -QtRoot or set QTDIR.'
}

if ([string]::IsNullOrWhiteSpace($VsDevCmd)) {
    $VsDevCmd = @(
        'D:\vs2022\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $VsDevCmd -or -not (Test-Path $VsDevCmd)) {
    throw 'VsDevCmd.bat was not found. Install Visual Studio 2022 C++ tools or pass -VsDevCmd.'
}

if ([string]::IsNullOrWhiteSpace($CMake)) {
    $CMake = @(
        'D:\bin\cmake.exe',
        'D:\vs2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $CMake) {
        $CMake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
    }
}
if (-not $CMake -or -not (Test-Path $CMake)) {
    throw 'CMake was not found. Install CMake or pass -CMake.'
}

if ([string]::IsNullOrWhiteSpace($Ninja)) {
    $Ninja = @(
        'D:\vs2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Ninja) {
        $Ninja = (Get-Command ninja.exe -ErrorAction SilentlyContinue).Source
    }
}
if (-not $Ninja -or -not (Test-Path $Ninja)) {
    throw 'Ninja was not found. Install CMake/Ninja or pass -Ninja.'
}

if ($Clean -and (Test-Path $buildRoot)) {
    $resolvedProject = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\') + '\'
    $resolvedBuild = [IO.Path]::GetFullPath($buildRoot)
    if (-not $resolvedBuild.StartsWith($resolvedProject, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the project: $resolvedBuild"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$configureCommand = @(
    "`"$CMake`" -S `"$projectRoot`" -B `"$buildRoot`" -G Ninja",
    "-DCMAKE_MAKE_PROGRAM=`"$Ninja`"",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_PREFIX_PATH=`"$QtRoot`""
) -join ' '

$commandLine = @(
    "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >NUL",
    'set "VSLANG=1033"',
    $configureCommand,
    "`"$CMake`" --build `"$buildRoot`" --parallel"
) -join ' && '

Write-Host "Configuring and building $Configuration with Qt FFmpeg Player..."
& $env:ComSpec /d /s /c $commandLine
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$exe = Join-Path $buildRoot 'FFmpegQtPlayer.exe'
if (-not (Test-Path $exe)) {
    throw "Build completed but the executable was not found at $exe."
}
Write-Host "Built: $exe"
