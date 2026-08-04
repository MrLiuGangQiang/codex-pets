param(
    [ValidateSet('win-x64', 'win-arm64', 'all')]
    [string]$RuntimeIdentifier = 'win-x64',
    [ValidateSet('auto', 'msvc', 'zig')]
    [string]$Toolchain = 'auto',
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
$dist = [IO.Path]::GetFullPath((Join-Path $root 'dist'))
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw -Encoding UTF8).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION 格式无效：$version" }
$limit = 10MB
$rids = if ($RuntimeIdentifier -eq 'all') { @('win-x64', 'win-arm64') } else { @($RuntimeIdentifier) }
if ($Toolchain -eq 'auto') {
    $hasZig = [bool](Get-Command zig -ErrorAction SilentlyContinue)
    $hasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
    $hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    $allTargetsAreNative = $true
    foreach ($rid in $rids) {
        $targetArchitecture = if ($rid -eq 'win-arm64') { 'arm64' } else { 'x64' }
        if ($targetArchitecture -ne $hostArchitecture) { $allTargetsAreNative = $false; break }
    }
    $Toolchain = if ($allTargetsAreNative -and $hasZig -and $hasNinja -and -not $env:GITHUB_ACTIONS) {
        'zig'
    } else {
        'msvc'
    }
}
$created = [Collections.Generic.List[string]]::new()

function Assert-DistPath([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not $resolved.StartsWith($dist + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝操作 dist 之外的路径：$resolved"
    }
    return $resolved
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "不是 PE 文件：$Path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "PE 头无效：$Path" }
        return $reader.ReadUInt16()
    }
    finally { $reader.Dispose() }
}

foreach ($rid in $rids) {
    $architecture = if ($rid -eq 'win-arm64') { 'arm64' } else { 'x64' }
    & (Join-Path $root 'build.ps1') -Configuration Release -Architecture $architecture `
        -Toolchain $Toolchain -SkipTests:$SkipTests
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $buildRoot = Get-Item (Join-Path $root "build\native-windows-$architecture-$Toolchain")
    if (-not $buildRoot) { throw "未找到 $rid 构建目录。" }
    $sourceExe = Get-ChildItem -LiteralPath $buildRoot.FullName -Recurse -File -Filter 'CodeXPets.exe' |
        Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $sourceExe) { throw "未找到 $rid 可执行文件。" }

    $expectedMachine = if ($rid -eq 'win-arm64') { 0xAA64 } else { 0x8664 }
    $actualMachine = Get-PeMachine $sourceExe.FullName
    if ($actualMachine -ne $expectedMachine) {
        throw ('架构不匹配：{0}，期望 0x{1:X4}，实际 0x{2:X4}' -f $rid, $expectedMachine, $actualMachine)
    }
    if ($sourceExe.Length -gt $limit) {
        throw ('可执行文件超过 10 MiB：{0:N2} MiB' -f ($sourceExe.Length / 1MB))
    }

    $directExe = Assert-DistPath (Join-Path $dist "CodeXPets-v$version-$rid.exe")
    Copy-Item -LiteralPath $sourceExe.FullName -Destination $directExe -Force
    $zip = Assert-DistPath (Join-Path $dist "CodeXPets-v$version-$rid.zip")
    Compress-Archive -LiteralPath $sourceExe.FullName -DestinationPath $zip -CompressionLevel Optimal -Force
    if ((Get-Item -LiteralPath $zip).Length -gt $limit) {
        throw ('ZIP 超过 10 MiB：{0:N2} MiB' -f ((Get-Item -LiteralPath $zip).Length / 1MB))
    }
    $created.Add($directExe)
    $created.Add($zip)
    Write-Host ('Created {0} ({1:N2} MiB)' -f $directExe, ((Get-Item $directExe).Length / 1MB))
    Write-Host ('Created {0} ({1:N2} MiB)' -f $zip, ((Get-Item $zip).Length / 1MB))
}

$checksum = Assert-DistPath (Join-Path $dist 'SHA256SUMS-windows.txt')
$lines = foreach ($file in $created) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($file))"
}
[IO.File]::WriteAllLines($checksum, $lines, $utf8)
Get-Content -LiteralPath $checksum
