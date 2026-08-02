$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root 'build.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$assemblyInfo = Get-Content -LiteralPath (Join-Path $root 'AssemblyInfo.cs') -Raw
$match = [regex]::Match($assemblyInfo, 'AssemblyFileVersion\("(?<version>\d+\.\d+\.\d+)\.\d+"\)')
if (-not $match.Success) { throw '无法从 AssemblyInfo.cs 读取版本号。' }
$version = $match.Groups['version'].Value
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$zip = Join-Path $dist "CodeXPets-v$version-win-portable.zip"
$packageFiles = @(
    (Join-Path $root 'CodeXPets.exe'),
    (Join-Path $root 'README.md'),
    (Join-Path $root 'CHANGELOG.md'),
    (Join-Path $root 'CREDITS.md'),
    (Join-Path $root 'create-shortcuts.ps1')
)
foreach ($file in $packageFiles) {
    if (-not (Test-Path -LiteralPath $file)) { throw "缺少发布文件：$file" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$expectedEntries = $packageFiles | ForEach-Object { [IO.Path]::GetFileName($_) }
$packageCreated = $false
for ($attempt = 1; $attempt -le 10; $attempt++) {
    try {
        if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
        Compress-Archive -LiteralPath $packageFiles -DestinationPath $zip `
            -CompressionLevel Optimal -ErrorAction Stop

        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $actualEntries = @($archive.Entries | ForEach-Object { $_.FullName })
            foreach ($expectedEntry in $expectedEntries) {
                if ($actualEntries -notcontains $expectedEntry) {
                    throw "发布包缺少文件：$expectedEntry"
                }
            }
        }
        finally { $archive.Dispose() }
        $packageCreated = $true
        break
    }
    catch {
        if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
        if ($attempt -ge 10) { throw }
        Start-Sleep -Milliseconds 300
    }
}
if (-not $packageCreated) { throw '发布包创建失败。' }

$hash = Get-FileHash -LiteralPath $zip -Algorithm SHA256
$hashLine = "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zip))"
Set-Content -LiteralPath (Join-Path $dist 'SHA256SUMS.txt') -Value $hashLine -Encoding ASCII
Write-Host "发布包：$zip"
Write-Host "SHA256：$($hash.Hash)"
