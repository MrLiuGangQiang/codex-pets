param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [ValidateSet('', 'win-x64', 'win-arm64', 'osx-x64', 'osx-arm64')]
    [string]$RuntimeIdentifier = '',
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$utf8 = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$localDotnet = Join-Path $root '.dotnet\dotnet.exe'
$dotnet = if (Test-Path -LiteralPath $localDotnet) {
    $localDotnet
} else {
    $command = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $command) { throw '需要 .NET 10 SDK。请安装后重新运行。' }
    $command.Source
}

$solution = Join-Path $root 'CodeXPets.slnx'
$appProject = Join-Path $root 'src\CodeXPets.App\CodeXPets.App.csproj'
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw -Encoding UTF8).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION 格式无效：$version" }

& $dotnet restore $solution
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $dotnet build $solution -c $Configuration --no-restore
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipTests) {
    & $dotnet test $solution -c $Configuration --no-build --no-restore
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$appDll = Join-Path $root "src\CodeXPets.App\bin\$Configuration\net10.0\CodeXPets.dll"
& $dotnet $appDll --validate-resources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $dotnet $appDll --smoke-test
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not [string]::IsNullOrWhiteSpace($RuntimeIdentifier)) {
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $root "dist\publish\$RuntimeIdentifier"
    }
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
    & $dotnet publish $appProject -c $Configuration -r $RuntimeIdentifier --self-contained true `
        -o $OutputPath -p:DebugType=None -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "发布输出：$OutputPath"
}

Write-Host "CodeXPets $version 构建完成（$Configuration）。"
