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
$logo = Join-Path $root 'codex-official-icon-source.png'
$petSprite = Join-Path $root 'boba-spritesheet.png'
$cloudBubble = Join-Path $root 'cloud-bubble.png'
$voiceStart = Join-Path $root 'voice-start.mp3'
$voiceComplete = Join-Path $root 'voice-complete.mp3'
$voiceError = Join-Path $root 'voice-error.mp3'
$appOutput = Join-Path $root 'CodeXPets.exe'
$testOutput = Join-Path $root 'CodeXPets.SelfTest.exe'

@($source, $assemblyInfo, $selfTest, $manifest, $icon, $logo, $voiceStart, $voiceComplete, $voiceError, $petSprite, $cloudBubble) | ForEach-Object {
    if (-not (Test-Path -LiteralPath $_)) { throw "缺少构建文件：$_" }
}

$frameworkDir = Split-Path -Parent $csc
$wpfDir = Join-Path $frameworkDir 'WPF'
$presentationCore = Join-Path $wpfDir 'PresentationCore.dll'
$windowsBase = Join-Path $wpfDir 'WindowsBase.dll'
if (-not (Test-Path -LiteralPath $presentationCore) -or -not (Test-Path -LiteralPath $windowsBase)) {
    throw '找不到 Windows WPF 媒体组件，无法启用可靠 MP3 播放。'
}
$references = @(
    '/reference:System.dll',
    '/reference:System.Core.dll',
    '/reference:System.Drawing.dll',
    '/reference:System.Windows.Forms.dll',
    "/reference:$presentationCore",
    "/reference:$windowsBase"
)

& $csc /nologo /target:winexe /optimize+ /platform:anycpu `
    "/out:$appOutput" `
    "/win32manifest:$manifest" `
    "/win32icon:$icon" `
    "/resource:$voiceStart,voice-start.mp3" `
    "/resource:$voiceComplete,voice-complete.mp3" `
    "/resource:$voiceError,voice-error.mp3" `
    "/resource:$logo,codex-official-icon.png" `
    "/resource:$petSprite,boba-spritesheet.png" `
    "/resource:$cloudBubble,cloud-bubble.png" `
    $references `
    $source $assemblyInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $csc /nologo /target:exe /optimize+ /platform:anycpu `
    /main:CodeXPets.MonitorSelfTest `
    "/out:$testOutput" `
    "/resource:$voiceStart,voice-start.mp3" `
    "/resource:$voiceComplete,voice-complete.mp3" `
    "/resource:$voiceError,voice-error.mp3" `
    "/resource:$logo,codex-official-icon.png" `
    "/resource:$petSprite,boba-spritesheet.png" `
    "/resource:$cloudBubble,cloud-bubble.png" `
    $references `
    $source $selfTest $assemblyInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "构建完成：$appOutput"
Write-Host "自测程序：$testOutput"