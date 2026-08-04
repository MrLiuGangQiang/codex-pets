param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('native', 'x64', 'arm64')]
    [string]$Architecture = 'native',
    [ValidateSet('auto', 'msvc', 'zig')]
    [string]$Toolchain = 'auto',
    [string]$Generator = '',
    [string]$BuildDirectory = '',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'User') + ';' +
    [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + $env:PATH

$root = [IO.Path]::GetFullPath((Split-Path -Parent $MyInvocation.MyCommand.Path))
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { $ctest = (Get-Command ctest -ErrorAction Stop).Source }

$hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($Architecture -eq 'native') {
    $Architecture = if ($hostArchitecture -eq 'arm64') { 'arm64' } else { 'x64' }
}

if ($Toolchain -eq 'auto') {
    $hasZig = [bool](Get-Command zig -ErrorAction SilentlyContinue)
    $hasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
    $matchesHost = ($Architecture -eq 'arm64' -and $hostArchitecture -eq 'arm64') -or
                   ($Architecture -eq 'x64' -and $hostArchitecture -eq 'x64')
    $Toolchain = if ($matchesHost -and $hasZig -and $hasNinja -and -not $env:GITHUB_ACTIONS) {
        'zig'
    } else {
        'msvc'
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $root "build\native-windows-$Architecture-$Toolchain"
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null

$configure = @('-S', $root, '-B', $BuildDirectory,
    '-DCODEXPETS_BUILD_APP=ON', '-DCODEXPETS_BUILD_TESTS=ON')

if ($Toolchain -eq 'zig') {
    if (($hostArchitecture -eq 'arm64' -and $Architecture -ne 'arm64') -or
        ($hostArchitecture -ne 'arm64' -and $Architecture -ne 'x64')) {
        throw 'Zig 本地构建仅支持当前 Windows 主机架构；交叉架构请使用 MSVC。'
    }
    $zig = (Get-Command zig -ErrorAction Stop).Source
    $ninja = (Get-Command ninja -ErrorAction Stop).Source
    $toolDir = Join-Path $BuildDirectory 'toolchain'
    New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
    $cxx = Join-Path $toolDir 'zig-cxx.cmd'
    $ar = Join-Path $toolDir 'zig-ar.cmd'
    $ranlib = Join-Path $toolDir 'zig-ranlib.cmd'
    $rc = Join-Path $toolDir 'zig-rc.cmd'
    Set-Content -LiteralPath $cxx -Encoding ASCII -Value "@echo off`r`n`"$zig`" c++ %*"
    Set-Content -LiteralPath $ar -Encoding ASCII -Value "@echo off`r`n`"$zig`" ar %*"
    Set-Content -LiteralPath $ranlib -Encoding ASCII -Value "@echo off`r`n`"$zig`" ranlib %*"
    Set-Content -LiteralPath $rc -Encoding ASCII -Value "@echo off`r`n`"$zig`" rc %*"
    if ([string]::IsNullOrWhiteSpace($Generator)) { $Generator = 'Ninja' }
    $ninjaCmake = $ninja.Replace([char]92, [char]47)
    $cxxCmake = $cxx.Replace([char]92, [char]47)
    $arCmake = $ar.Replace([char]92, [char]47)
    $ranlibCmake = $ranlib.Replace([char]92, [char]47)
    $rcCmake = $rc.Replace([char]92, [char]47)
    $configure += @('-G', $Generator, "-DCMAKE_MAKE_PROGRAM=$ninjaCmake",
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_CXX_COMPILER=$cxxCmake",
        "-DCMAKE_AR=$arCmake", "-DCMAKE_RANLIB=$ranlibCmake", "-DCMAKE_RC_COMPILER=$rcCmake")
}
else {
    if ([string]::IsNullOrWhiteSpace($Generator)) { $Generator = 'Visual Studio 17 2022' }
    $cmakeArchitecture = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x64' }
    $configure += @('-G', $Generator, '-A', $cmakeArchitecture)
}

& $cmake @configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipTests) {
    & $ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$executables = Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File -Filter 'CodeXPets.exe' |
    Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' }
if (-not $executables) { throw "未找到原生可执行文件：$BuildDirectory" }
$executable = $executables | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1

$canRun = ($Architecture -eq 'arm64' -and $hostArchitecture -eq 'arm64') -or
          ($Architecture -eq 'x64' -and $hostArchitecture -ne 'arm64')
if ($canRun) {
    foreach ($argument in @('--validate-resources', '--smoke-test')) {
        & $executable.FullName $argument
        if ($LASTEXITCODE -ne 0) { throw "$argument 失败：$($executable.FullName)" }
    }

    $startupRoot = Join-Path $BuildDirectory ("startup-smoke-" + [guid]::NewGuid().ToString('N'))
    $startupLocal = Join-Path $startupRoot 'local'
    $startupCodex = Join-Path $startupRoot 'codex'
    $startupSessions = Join-Path $startupCodex 'sessions'
    $startupSettingsDirectory = Join-Path $startupLocal 'CodeXPets'
    New-Item -ItemType Directory -Force -Path $startupSettingsDirectory, $startupSessions | Out-Null
    $startupSettings = [ordered]@{
        DockHoverHeight = 240
        DockIdleHideSeconds = 10
        DockRevealSeconds = 3
        DockNotificationSeconds = 5
        SoundEnabled = $false
        PetVisible = $false
        SessionsRoot = $startupSessions
        PetPosition = $null
    }
    [IO.File]::WriteAllText(
        (Join-Path $startupSettingsDirectory 'settings.json'),
        ($startupSettings | ConvertTo-Json -Depth 4),
        $utf8)
    $previousLocalAppData = $env:LOCALAPPDATA
    $previousCodexHome = $env:CODEX_HOME
    try {
        $env:LOCALAPPDATA = $startupLocal
        $env:CODEX_HOME = $startupCodex
        & $executable.FullName --startup-smoke-test
        if ($LASTEXITCODE -ne 0) { throw "--startup-smoke-test 失败：$($executable.FullName)" }
    }
    finally {
        $env:LOCALAPPDATA = $previousLocalAppData
        $env:CODEX_HOME = $previousCodexHome
    }
}
else {
    Write-Host "跳过 $Architecture 可执行文件运行检查（主机架构：$hostArchitecture）。"
}

Write-Host "CodeXPets $Configuration/$Architecture 构建完成：$($executable.FullName)"
