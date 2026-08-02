$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidates = @(
    'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe',
    'C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe'
)
$csc = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $csc) { throw '找不到 Windows .NET Framework C# 编译器。' }

$source = Join-Path $root 'CodeXPets.cs'
$assemblyInfo = Join-Path $root 'AssemblyInfo.cs'
$selfTest = Join-Path $root 'CodeXPets.SelfTest.cs'
$manifest = Join-Path $root 'app.manifest'
$icon = Join-Path $root 'CodeXPets.ico'
$whiteCatSprite = Join-Path $root 'white-cat-spritesheet.png'
$dockSprite = Join-Path $root 'cat-dock-spritesheet.png'
$cloudBubble = Join-Path $root 'cloud-bubble.png'
$voiceStart = Join-Path $root 'voice-start.mp3'
$voiceComplete = Join-Path $root 'voice-complete.mp3'
$voiceError = Join-Path $root 'voice-error.mp3'
$appOutput = Join-Path $root 'CodeXPets.exe'
$testOutput = Join-Path $root 'CodeXPets.SelfTest.exe'

$requiredFiles = @(
    $source, $assemblyInfo, $selfTest, $manifest, $icon,
    $whiteCatSprite, $dockSprite, $cloudBubble,
    $voiceStart, $voiceComplete, $voiceError
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "缺少构建文件：$requiredFile"
    }
}

$frameworkDir = Split-Path -Parent $csc
$presentationCore = Join-Path $frameworkDir 'WPF\PresentationCore.dll'
$windowsBase = Join-Path $frameworkDir 'WPF\WindowsBase.dll'
$webExtensions = Join-Path $frameworkDir 'System.Web.Extensions.dll'
foreach ($frameworkAssembly in @($presentationCore, $windowsBase, $webExtensions)) {
    if (-not (Test-Path -LiteralPath $frameworkAssembly)) {
        throw "缺少 .NET Framework 组件：$frameworkAssembly"
    }
}

$references = @(
    '/reference:System.dll',
    '/reference:System.Core.dll',
    '/reference:System.Drawing.dll',
    '/reference:System.Windows.Forms.dll',
    "/reference:$presentationCore",
    "/reference:$windowsBase",
    "/reference:$webExtensions"
)
$resources = @(
    "/resource:$voiceStart,voice-start.mp3",
    "/resource:$voiceComplete,voice-complete.mp3",
    "/resource:$voiceError,voice-error.mp3",
    "/resource:$whiteCatSprite,white-cat-spritesheet.png",
    "/resource:$dockSprite,cat-dock-spritesheet.png",
    "/resource:$cloudBubble,cloud-bubble.png"
)
$commonCompilerArgs = @(
    '/nologo',
    '/optimize+',
    '/warn:4',
    '/warnaserror+',
    '/platform:anycpu'
) + $references + $resources

& $csc @commonCompilerArgs '/target:winexe' `
    "/out:$appOutput" `
    "/win32manifest:$manifest" `
    "/win32icon:$icon" `
    $source $assemblyInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $csc @commonCompilerArgs '/target:exe' `
    '/main:CodeXPets.MonitorSelfTest' `
    "/out:$testOutput" `
    $source $selfTest $assemblyInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "构建完成：$appOutput"
Write-Host "自测程序：$testOutput"
