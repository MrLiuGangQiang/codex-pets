param(
    [ValidateSet('win-x64', 'win-arm64', 'all')]
    [string]$RuntimeIdentifier = 'all',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$utf8 = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root 'dist'
$distFull = [IO.Path]::GetFullPath($dist)
New-Item -ItemType Directory -Force -Path $distFull | Out-Null
$localDotnet = Join-Path $root '.dotnet\dotnet.exe'
$dotnet = if (Test-Path -LiteralPath $localDotnet) {
    $localDotnet
} else {
    $command = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $command) { throw '需要 .NET 10 SDK。' }
    $command.Source
}
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw -Encoding UTF8).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION 格式无效：$version" }
$appProject = Join-Path $root 'src\CodeXPets.App\CodeXPets.App.csproj'
$rids = if ($RuntimeIdentifier -eq 'all') { @('win-x64', 'win-arm64') } else { @($RuntimeIdentifier) }

& (Join-Path $root 'build.ps1') -Configuration Release -SkipTests:$SkipTests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Assert-DistChild([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not $resolved.StartsWith($distFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝操作 dist 之外的路径：$resolved"
    }
    return $resolved
}

function Reset-SafeDirectory([string]$Path) {
    $resolved = Assert-DistChild $Path
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $resolved | Out-Null
    return $resolved
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
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

function Invoke-NativeSmokeTest([string]$Executable) {
    $process = Start-Process -FilePath $Executable -ArgumentList '--smoke-test' `
        -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0) {
        throw "冒烟测试失败（退出码 $($process.ExitCode)）：$Executable"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$created = @()
foreach ($rid in $rids) {
    $artifactPattern = '^CodeXPets-v.+-' + [Regex]::Escape($rid) + '\.(exe|zip)$'
    $staleArtifacts = Get-ChildItem -LiteralPath $distFull -File |
        Where-Object { $_.Name -match $artifactPattern }
    foreach ($artifact in $staleArtifacts) {
        [IO.File]::Delete((Assert-DistChild $artifact.FullName))
    }

    $stage = Reset-SafeDirectory (Join-Path $distFull "stage-$rid")
    & $dotnet publish $appProject -c Release -r $rid --self-contained true -o $stage `
        -p:DebugType=None -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.pdb' | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force
    }

    $exe = Join-Path $stage 'CodeXPets.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "缺少单文件发布程序：$exe" }
    $publishedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
    if ($publishedFiles.Count -ne 1 -or
        -not [string]::Equals($publishedFiles[0].FullName, $exe,
            [StringComparison]::OrdinalIgnoreCase)) {
        $unexpected = ($publishedFiles | ForEach-Object FullName) -join [Environment]::NewLine
        throw "Windows 发布目录必须只包含 CodeXPets.exe：$unexpected"
    }
    $expectedMachine = if ($rid -eq 'win-arm64') { 0xAA64 } else { 0x8664 }
    $actualMachine = Get-PeMachine $exe
    if ($actualMachine -ne $expectedMachine) {
        throw ('架构校验失败：{0} machine=0x{1:X4} expected=0x{2:X4}' -f $rid,$actualMachine,$expectedMachine)
    }

    $fileVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
    if ($fileVersion -notlike "$version*") {
        throw "版本校验失败：$rid fileVersion=$fileVersion expected=$version"
    }

    $hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    $canRun = ($rid -eq 'win-x64' -and $hostArchitecture -in @('X64', 'Arm64')) -or
              ($rid -eq 'win-arm64' -and $hostArchitecture -eq 'Arm64')
    if ($canRun) { Invoke-NativeSmokeTest $exe }

    $singleExe = Join-Path $distFull "CodeXPets-v$version-$rid.exe"
    Copy-Item -LiteralPath $exe -Destination $singleExe -Force

    $zip = Join-Path $distFull "CodeXPets-v$version-$rid.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath $exe -DestinationPath $zip -CompressionLevel Optimal
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        if ($archive.Entries.Count -ne 1 -or $archive.Entries[0].FullName -ne 'CodeXPets.exe') {
            $entries = ($archive.Entries | ForEach-Object FullName) -join ', '
            throw "ZIP 必须只包含 CodeXPets.exe，实际内容：$entries"
        }
    }
    finally { $archive.Dispose() }

    $created += $singleExe
    $created += $zip
    Write-Host "单文件程序：$singleExe"
    Write-Host "单文件 ZIP：$zip"

    $verifiedStage = Assert-DistChild $stage
    [IO.Directory]::Delete($verifiedStage, $true)
}

$checksum = Join-Path $distFull 'SHA256SUMS-windows.txt'
$lines = foreach ($file in $created) {
    $hash = Get-FileHash -LiteralPath $file -Algorithm SHA256
    "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($file))"
}
$lines | Set-Content -LiteralPath $checksum -Encoding ASCII
Write-Host "校验文件：$checksum"
