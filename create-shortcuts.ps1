param([switch]$StartMenuOnly)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'CodeXPets.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "缺少 CodeXPets.exe：$exe" }

function New-CodeXPetsShortcut([string]$shortcutPath) {
    $parent = Split-Path -Parent $shortcutPath
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $root
    $shortcut.IconLocation = "$exe,0"
    $shortcut.Description = 'CodeXPets Codex 桌面宠物'
    $shortcut.Save()
    Write-Host "已创建：$shortcutPath"
}

$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\CodeXPets.lnk'
New-CodeXPetsShortcut $startMenu
if (-not $StartMenuOnly) {
    $desktop = [Environment]::GetFolderPath('DesktopDirectory')
    if (-not [string]::IsNullOrWhiteSpace($desktop)) {
        New-CodeXPetsShortcut (Join-Path $desktop 'CodeXPets.lnk')
    }
}
