# CodeXPets

CodeXPets 4.0.0 是一个面向 Codex 的**原生桌面宠物**。本版本将原来的 .NET/Avalonia/SkiaSharp 架构完全替换为轻量 C++20 核心和系统原生 UI：

- Windows：Win32 + GDI+
- macOS：AppKit + Core Graphics
- 核心：无第三方运行时、自研轻量 JSON 解析器、增量 JSONL 会话监控

目标是**不牺牲功能的前提下尽量降低发布体积和内存占用**。发布脚本会拒绝超过 10 MiB 的 EXE、APP、ZIP 或 DMG；实际工作集会受操作系统图形框架、字体和显示器缩放影响，不能只由应用代码绝对控制。

## 支持矩阵

| 系统 | 架构 | 构建/发布目标 | 文件 |
|---|---:|---|---|
| Windows | x64 | `win-x64` | `CodeXPets-v4.0.0-win-x64.exe` / `.zip` |
| Windows | ARM64 | `win-arm64` | `CodeXPets-v4.0.0-win-arm64.exe` / `.zip` |
| macOS | Intel | `osx-x64` | `CodeXPets-v4.0.0-macos-x64.zip` / `.dmg` |
| macOS | Apple Silicon | `osx-arm64` | `CodeXPets-v4.0.0-macos-arm64.zip` / `.dmg` |

macOS 最低支持 macOS 13。Windows 版本是一个自包含原生 EXE；macOS 版本是标准 `CodeXPets.app`。两者都不要求用户安装 .NET、Electron、Qt 或其他附加运行时。

## 保留的功能

- 读取 Codex `sessions` 目录中的 JSONL 文件，增量解析新增内容。
- 识别任务开始、完成、异常、标题、`update_plan` 计划进度和过期任务。
- Windows 通知区域图标、macOS 菜单栏图标。
- 透明、无边框、置顶、不抢焦点的桌面宠物窗口。
- 云朵文本、自动换行、长文本滚动、多任务自动轮换和点击切换。
- 左右边缘吸附、局部边缘唤出、自动隐藏、拖动解除吸附。
- 多显示器位置保存/恢复，支持显示器变化后的降级匹配。
- 设置窗口、诊断窗口、语音提醒、登录启动、打开会话目录和更新入口。
- Windows 旧注册表设置、macOS 旧 `NSUserDefaults` 设置以及旧位置格式迁移。
- 单实例、资源完整性校验、离屏冒烟渲染、真实窗口启动冒烟和 SHA-256 校验。

CodeXPets 只读取 Codex 会话文件，不会修改、移动或删除这些文件。

## 目录规则

`CODEX_HOME` 优先级最高。未设置时：

| 系统 | Codex Home | sessions | config |
|---|---|---|---|
| Windows | `%USERPROFILE%\\.codex` | `%USERPROFILE%\\.codex\\sessions` | `%USERPROFILE%\\.codex\\config.toml` |
| macOS | `~/.codex` | `~/.codex/sessions` | `~/.codex/config.toml` |

也可以在设置窗口中选择自定义 sessions 目录。

设置文件位置：

- Windows：`%LOCALAPPDATA%\\CodeXPets\\settings.json`
- macOS：`~/Library/Application Support/CodeXPets/settings.json`

## 运行监控策略

为了避免 `FileSystemWatcher`、FSEvents 和大型 UI 运行时带来的额外常驻开销，原生版本采用轻量自适应轮询：

- 默认每 500 ms 检查一次目录和最近跟踪的 JSONL 文件。
- 只从保存的字节偏移继续读取新增内容。
- 最多跟踪最近 40 个文件，并周期性执行恢复扫描。
- 文件不存在、正在写入或 JSON 行不完整时会安全延后到下一轮。

正常情况下状态变化延迟小于 1 秒，同时不要求安装额外后台服务。

## 使用

### Windows

1. 下载与 CPU 架构匹配的 EXE，直接运行。
2. 右键通知区域图标可以显示/隐藏桌宠、切换语音、设置登录启动、打开设置和诊断。
3. 拖动猫或云朵可以移动桌宠；靠近屏幕左/右边缘释放即可吸附。

### macOS

1. 打开 DMG，将 `CodeXPets.app` 拖入 `Applications`；ZIP 是备用格式。
2. 应用常驻菜单栏，不显示 Dock 图标。
3. 正式发布会使用 Developer ID 签名和 Apple 公证；个人构建可以使用 ad-hoc 签名。

## 从源码构建

### Windows PowerShell

本地若安装了 Zig 和 Ninja，脚本会优先使用轻量 Zig 工具链；GitHub Actions 使用 MSVC。

```powershell
# 构建、测试、资源校验、离屏渲染和真实窗口启动冒烟测试
./build.ps1

# 生成 Windows x64 EXE 和 ZIP
./package.ps1 -RuntimeIdentifier win-x64

# 生成 Windows ARM64（需要 ARM64 主机或 MSVC 交叉工具链）
./package.ps1 -RuntimeIdentifier win-arm64 -Toolchain msvc
```

也可以直接使用 CMake：

```powershell
cmake -S . -B build/native-windows-x64-zig
cmake --build build/native-windows-x64-zig --parallel
ctest --test-dir build/native-windows-x64-zig --output-on-failure
```

### macOS

需要 Xcode Command Line Tools、CMake、Clang；有 Ninja 时会自动使用 Ninja。

```bash
bash scripts/build-macos.sh osx-arm64   # Apple Silicon
bash scripts/build-macos.sh osx-x64     # Intel
```

脚本会完成 CMake 构建、CTest、资源校验、冒烟渲染、签名、ZIP/DMG、架构检查和 10 MiB 体积检查。签名变量：

- `CODE_SIGN_IDENTITY`：默认 `-`（ad-hoc）
- `APPLE_NOTARY_PROFILE`：Apple notarytool keychain profile
- `REQUIRE_NOTARIZATION=1`：强制要求正式签名和公证

## 原生命令行工具

原生可执行文件支持：

```text
--version
--validate-resources
--smoke-test
--preview <目录>
--test-sound
```

这些命令用于 CI 和发布前诊断，不会启动常驻桌宠。

## 体积和内存优化

- 不携带 .NET、JIT、Electron、Qt、SkiaSharp 或大型跨平台 UI 库。
- Windows 恢复旧版像素云朵外观，构建资源使用与旧版相同的 Skia 降采样结果（640×221），避免携带 2122×734 原图。
- 浮动精灵只缓存当前状态的 8 帧；扒边帧按需读取。
- Windows 链接器使用 dead-strip/折叠等尺寸优化；macOS 使用 `-dead_strip`。
- 发布脚本对 EXE、APP、ZIP、DMG 统一执行 10 MiB 硬限制。

Windows x64 原生构建的实测结果（同一台开发机、Release 构建，确认真实桌宠窗口和后台消息窗口均已创建，桌宠可见，空闲/忙碌/完成状态各连续采样）约为：EXE 1.27 MiB，私有内存 4.36–4.60 MiB，工作集 20.26–21.10 MiB，6 个线程。工作集包含系统共享的 GDI+/窗口代码页；不建议为了显示一个更小的数字强行清空工作集而造成页面抖动。macOS 的内存应在目标系统上通过“诊断信息”查看。

## 架构文档

详见 [`docs/architecture.md`](docs/architecture.md)。

## 版权与致谢

资源和第三方声明见 [`CREDITS.md`](CREDITS.md)。
