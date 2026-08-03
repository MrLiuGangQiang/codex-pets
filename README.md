# CodeXPets

CodeXPets 3.0.5 是一个使用 **.NET 10 + Avalonia 12.1.1** 构建的跨平台 Codex 桌面宠物。Windows 和 macOS 现在共用同一套 C# 核心、会话解析规则、状态机、绘制逻辑与设置模型，仅把窗口穿透、登录启动、系统菜单和声音播放等能力放到平台适配层。

桌宠形象固定为一只卡通化的 **白色英国短毛猫**：**深灰色耳朵、深灰色尾巴、暖粉色内耳、金黄色眼睛**。浮动、忙碌、完成、异常和左右扒边动画均沿用原 Windows 版的帧布局与交互节奏。

## 支持矩阵

| 操作系统 | CPU 架构 | Runtime Identifier | 发布文件 |
|---|---:|---|---|
| Windows | x64 | `win-x64` | `CodeXPets-v3.0.5-win-x64.exe` / `.zip` |
| Windows | ARM64 | `win-arm64` | `CodeXPets-v3.0.5-win-arm64.exe` / `.zip` |
| macOS | Intel x64 | `osx-x64` | `CodeXPets-v3.0.5-macos-x64.zip` / `.dmg` |
| macOS | Apple Silicon ARM64 | `osx-arm64` | `CodeXPets-v3.0.5-macos-arm64.zip` / `.dmg` |

所有发布包均为 self-contained，不要求用户另行安装 .NET。Windows 版本是压缩的单 EXE；ZIP 备份中也只包含一个 `CodeXPets.exe`。首次运行时 .NET 会把 Avalonia/Skia 所需的少量原生库缓存到 `%TEMP%\.net`，程序所在目录始终只需要这一个 EXE。macOS 构建的最低系统版本为 **macOS 13**；这是登录启动适配层使用 `SMAppService` 的最低版本。

## 功能

- 通过 `FileSystemWatcher` 监听 Codex 会话目录，并对 JSONL 新增内容进行增量解析，识别空闲、进行中、完成、异常、标题和计划进度。
- 文件系统事件经过防抖和最小轮询间隔合并，并保留周期恢复轮询，降低重复读取且避免遗漏任务。
- Windows 通知区域图标与 macOS 菜单栏图标。
- 透明、无边框、置顶且不抢焦点的桌面宠物窗口。
- 云朵任务文字、自动换行、长文本滚动、多任务自动轮换与点击切换。
- 多显示器位置记忆，显示器分辨率变化后优先按显示器名称恢复。
- 左右边缘吸附、自动隐藏、固定高度局部边缘唤出、拖动解除吸附。
- 设置、诊断、语音提醒、开机/登录启动、打开会话目录和更新入口。
- 旧 Windows 注册表设置与旧 macOS `UserDefaults` 设置的一次性迁移。
- 单实例运行、资源完整性校验、架构校验和发布包 SHA-256。

CodeXPets 只读取 Codex JSONL 会话文件，不会修改、移动或删除这些文件。

## 使用

### Windows

1. 下载与 CPU 匹配的单文件 EXE；ZIP 只是仅含同一个 `CodeXPets.exe` 的备用包。
2. 直接运行 `CodeXPets-v3.0.5-win-x64.exe` 或 `CodeXPets-v3.0.5-win-arm64.exe`，无需安装 .NET、无需解压其他依赖。
3. 右键通知区域图标可显示/隐藏桌宠、切换声音、设置开机启动、打开设置或诊断窗口。
4. 如需固定入口，可自行把 EXE 放到长期目录后创建开始菜单或桌面快捷方式。

### macOS

1. 下载与 CPU 匹配的 DMG，打开后将 `CodeXPets.app` 拖入 `Applications`；ZIP 是备用分发格式。
2. 启动后应用常驻菜单栏，不显示 Dock 图标。
3. 正式 Release 工作流会使用 Developer ID 签名并完成 Apple 公证；个人 ad-hoc 构建首次启动时可能需要在 Finder 中右键选择“打开”。

## Codex 会话监听

CodeXPets 的唯一任务状态来源是 Codex sessions 目录中的 `*.jsonl` 文件，不需要额外启动或配置其他事件服务。

- `FileSystemWatcher` 递归监听会话文件的创建、修改、重命名和删除。
- 文件变化事件先进行约 25 毫秒防抖；连续读取之间至少间隔约 250 毫秒。
- 无文件事件时每 30 秒执行一次恢复轮询；监听器创建失败或通知丢失时仍可继续发现变化。
- `CodexSessionMonitor` 记录每个文件的读取位置，只读取新增字节，并使用有状态 UTF-8 解码处理跨读取块字符和不完整行。
- 快速发现覆盖当天及前两天的日期目录；完整发现每 600 秒执行一次，并最多跟踪最近修改的 40 个文件。
- 解析结果归一化为任务开始、完成、异常、标题和计划进度，再由 Avalonia UI 线程更新桌宠、云朵、托盘图标与提示音。

## Codex 目录规则

`CODEX_HOME` 始终优先。未设置时：

| 平台 | Codex Home | sessions | config |
|---|---|---|---|
| Windows | `%USERPROFILE%\.codex` | `%USERPROFILE%\.codex\sessions` | `%USERPROFILE%\.codex\config.toml` |
| macOS | `~/.codex` | `~/.codex/sessions` | `~/.codex/config.toml` |

也可以在设置窗口中选择自定义 sessions 目录。路径层会拒绝把 Windows 盘符路径用于 macOS，或把 Unix/macOS 绝对路径用于 Windows；文件身份比较在 Windows 上不区分大小写，在 macOS 上区分大小写。

## 设置文件

- Windows：`%LOCALAPPDATA%\CodeXPets\settings.json`
- macOS：`~/Library/Application Support/CodeXPets/settings.json`

保存采用临时文件后原子替换。首次运行新架构时，程序会尝试迁移旧设置：

- Windows：`HKCU\Software\CodeXPets` 与 `HKCU\Software\CodeXPets\Windows`
- macOS：bundle id `com.mrliugangqiang.codexpets` 对应的 `UserDefaults`

## 架构

```text
CodeXPets.slnx
├─ src/CodeXPets.Core/          # 平台无关：路径、设置、状态机与 JSONL 会话解析
├─ src/CodeXPets.App/           # Avalonia UI、桌宠绘制、托盘、声音与平台适配器
│  ├─ Infrastructure/
│  │  ├─ WindowsPlatformService.cs
│  │  └─ MacPlatformService.cs
│  ├─ Services/
│  └─ Views/
├─ tests/CodeXPets.Core.Tests/  # 路径、状态、交互规则和会话生命周期回归测试
├─ packaging/macos/             # Info.plist 与 hardened runtime entitlement
├─ scripts/build-macos.sh       # macOS x64/ARM64 原生打包
└─ package.ps1                  # Windows x64/ARM64 原生打包
```

更详细的依赖方向、运行时数据流和平台边界见 `docs/architecture.md`。


## 构建

需要 .NET SDK `10.0.302`；仓库中的 `global.json` 固定该 feature band。

### Windows

```powershell
# 构建、测试、校验全部资源并离屏渲染所有状态
.\build.ps1

# 生成当前 Windows 可执行的两个架构包
.\package.ps1 -RuntimeIdentifier all

# 也可以只生成一个架构
.\package.ps1 -RuntimeIdentifier win-x64
.\package.ps1 -RuntimeIdentifier win-arm64
```

`package.ps1` 会：

1. 构建并运行测试。
2. 运行白色英短资源尺寸、透明背景、配色校验和全部浮动/扒边状态离屏渲染。
3. 生成包含 .NET、Avalonia、Skia 和原生依赖的压缩 self-contained 单 EXE。
4. 强制发布目录只能包含一个 `CodeXPets.exe`，并校验 PE machine：x64 为 `0x8664`，ARM64 为 `0xAA64`。
5. 在宿主机能够原生执行该架构时运行发布产物冒烟测试。
6. 删除调试符号，生成可直接运行的版本化 EXE、仅含一个 EXE 的 ZIP 和 `SHA256SUMS-windows.txt`。

### macOS

```bash
# 当前机器架构
bash scripts/build-macos.sh osx-arm64
# 或
bash scripts/build-macos.sh osx-x64

# 同时生成两个架构（交叉架构仅发布，不默认执行冒烟测试）
bash scripts/build-macos.sh all
```

脚本会生成标准 `CodeXPets.app`、ICNS、ZIP、DMG 和 `SHA256SUMS-macos.txt`，并用 `lipo` 检查 apphost 以及 bundle 内所有 Mach-O 文件。默认使用 ad-hoc 签名。

正式签名与公证：

```bash
export CODE_SIGN_IDENTITY='Developer ID Application: ...'
export APPLE_NOTARY_PROFILE='codexpets-release-notary'
export REQUIRE_NOTARIZATION=1
bash scripts/build-macos.sh osx-arm64
```

hardened runtime 使用 `packaging/macos/CodeXPets.entitlements` 中最小化的 JIT entitlement。脚本先公证并 staple 应用，再生成最终 ZIP/DMG；DMG 也会签名、公证和 staple。

## 开发命令

```powershell
# 输出版本
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --version

# 校验资源，包括白身、深色耳尾、暖色内耳、金黄色眼睛
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --validate-resources

# 启动并离屏渲染空闲、忙碌、完成、异常和左右扒边状态
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --smoke-test

# 离屏生成全部状态预览
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --preview .\preview

# 实际播放并等待 Windows/macOS 提示音完成
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --test-sound start
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --test-sound complete
.\.dotnet\dotnet.exe src\CodeXPets.App\bin\Release\net10.0\CodeXPets.dll --test-sound error
```

## CI 与发版

GitHub Actions 在四个原生 runner 上分别验证和打包：

- `windows-2025` → `win-x64`
- `windows-11-arm` → `win-arm64`
- `macos-15-intel` → `osx-x64`
- `macos-15` → `osx-arm64`

标签 `v<VERSION>` 会触发 Release 工作流。macOS 正式发布需要配置：

- `MACOS_CERTIFICATE_P12_BASE64`
- `MACOS_CERTIFICATE_PASSWORD`
- `MACOS_CODE_SIGN_IDENTITY`
- `APPLE_API_KEY_ID`
- `APPLE_API_ISSUER_ID`
- `APPLE_API_PRIVATE_KEY`

最终 Release 包含四个平台/架构构建和统一的 `SHA256SUMS.txt`。
