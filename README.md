# CodeXPets

CodeXPets 是一个面向 Codex 的**原生桌面宠物**。它以只读方式监听本机 Codex 会话，把任务的空闲、执行、完成、异常和中断状态，实时显示为猫咪动画与云朵消息。

- **状态一眼可见**：不用切回终端，也能看到 Codex 是否正在工作、计划执行到哪一步以及任务是否结束。
- **常驻但不打扰**：透明、无边框、置顶且不抢焦点，可拖动、吸附到屏幕左右边缘并自动隐藏。
- **本地优先**：会话解析在本机完成，只读取 Codex 的 JSONL 会话文件，不修改、移动或删除它们。
- **原生轻量**：C++20 核心，Windows 使用 Win32 + GDI+，macOS 使用 AppKit + Core Graphics，无需安装 .NET、Electron、Qt 等额外运行时。

## 宠物状态预览

<table>
  <tr>
    <td align="center"><strong>空闲</strong><br><img src="docs/screenshots/idle.png" alt="CodeXPets 空闲状态" width="280"></td>
    <td align="center"><strong>工作中</strong><br><img src="docs/screenshots/busy.png" alt="CodeXPets 工作中状态" width="280"></td>
    <td align="center"><strong>已完成</strong><br><img src="docs/screenshots/completed.png" alt="CodeXPets 已完成状态" width="280"></td>
  </tr>
  <tr>
    <td align="center"><strong>异常</strong><br><img src="docs/screenshots/error.png" alt="CodeXPets 异常状态" width="280"></td>
    <td align="center"><strong>已中断</strong><br><img src="docs/screenshots/interrupted.png" alt="CodeXPets 已中断状态" width="280"></td>
    <td align="center">五种状态会随 Codex 任务生命周期自动切换。</td>
  </tr>
</table>

## 主要功能

### Codex 任务监控

- 自动发现并监听 Codex `sessions` 目录中的 JSONL 会话。
- 增量读取新增内容，通常可在 1 秒内反映任务状态变化。
- 识别任务开始、完成、异常、中断、任务标题和项目名称。
- 展示 `update_plan` 计划进度，例如 `1/3`。
- 支持同时跟踪多个任务，自动轮换展示，也可以点击云朵切换。
- 长文本自动换行和滚动，任务变化时优先展示最新内容。

### 桌面宠物交互

- 透明、无边框、始终置顶，不抢占当前应用焦点。
- 可拖动猫咪或云朵移动位置。
- 靠近屏幕左侧或右侧释放后自动吸附。
- 吸附状态支持自动隐藏、边缘唤出和拖动解除吸附。
- 自动保存宠物位置、所在显示器、吸附方向和垂直位置。
- 支持多显示器及显示器分辨率变化后的安全恢复。

### 系统集成

- Windows 常驻通知区域，macOS 常驻菜单栏。
- 可快速显示或隐藏宠物、开启或关闭声音、打开设置和会话目录。
- 支持登录系统后自动启动。
- 内置任务开始、完成和异常语音提醒。
- 提供更新入口、资源校验和单实例运行保护。

### 小爱音箱播报（可选）

- 可把 Codex 的开始、完成、异常和中断事件主动播报到小爱音箱。
- 设置页支持浏览器登录、扫描设备、选择目标音箱和测试播报。
- Windows 使用一次性 WebView2 会话，macOS 使用非持久化 WKWebView 会话。
- 授权信息只保存在 Windows 凭据管理器或 macOS Keychain，不保存小米账号密码。
- 功能默认关闭；未启用时不会发起相关网络请求。

## 状态说明

| 状态 | 宠物表现 | 含义 |
|---|---|---|
| 空闲 | 安静等待 | 当前没有正在执行的 Codex 任务 |
| 工作中 | 行走动画 | 存在活动任务，并显示标题或计划进度 |
| 已完成 | 开心表情 | 最近的任务已正常完成 |
| 异常 | 流泪表情 | 任务执行失败或异常结束 |
| 已中断 | 惊讶表情 | 任务被用户或系统中断 |

## 支持平台

当前版本：**4.1.0**

| 系统 | 架构 | 构建目标 | 发布文件 |
|---|---:|---|---|
| Windows | x64 | `win-x64` | `CodeXPets-v4.1.0-win-x64.exe` / `.zip` |
| Windows | ARM64 | `win-arm64` | `CodeXPets-v4.1.0-win-arm64.exe` / `.zip` |
| macOS | Intel | `osx-x64` | `CodeXPets-v4.1.0-macos-x64.zip` / `.dmg` |
| macOS | Apple Silicon | `osx-arm64` | `CodeXPets-v4.1.0-macos-arm64.zip` / `.dmg` |

macOS 最低支持 macOS 13。Windows 版本为自包含原生 EXE，macOS 版本为标准 `CodeXPets.app`。

## 使用方法

### Windows

1. 下载与 CPU 架构匹配的 EXE，直接运行，无需安装。
2. 右键通知区域图标可显示或隐藏宠物、切换声音、设置登录启动和打开设置。
3. 拖动猫咪或云朵可调整位置；拖到屏幕左侧或右侧即可吸附。

### macOS

1. 打开 DMG，将 `CodeXPets.app` 拖入 `Applications`；也可以使用 ZIP 包。
2. 启动后应用常驻菜单栏，不显示 Dock 图标。
3. 通过菜单栏图标管理显示、声音、登录启动、设置和退出。

## 会话与设置目录

`CODEX_HOME` 的优先级最高。未设置时使用以下默认目录：

| 系统 | Codex Home | sessions | config |
|---|---|---|---|
| Windows | `%USERPROFILE%\.codex` | `%USERPROFILE%\.codex\sessions` | `%USERPROFILE%\.codex\config.toml` |
| macOS | `~/.codex` | `~/.codex/sessions` | `~/.codex/config.toml` |

也可以在设置窗口中选择自定义 `sessions` 目录。

CodeXPets 自身设置保存在：

- Windows：`%LOCALAPPDATA%\CodeXPets\settings.json`
- macOS：`~/Library/Application Support/CodeXPets/settings.json`

## 轻量运行设计

- 默认每 500 ms 检查会话目录和最近跟踪的 JSONL 文件。
- 只从已记录的字节偏移继续读取新增内容，不重复加载完整会话。
- 最多跟踪最近 40 个文件，并周期性执行恢复扫描。
- 文件正在写入、JSON 行不完整或文件暂时不可用时，会安全延后到下一轮。
- 浮动宠物只缓存当前状态的动画帧，吸附资源按需加载。
- 发布流程对 Windows EXE/ZIP 和 macOS APP/ZIP/DMG 执行 10 MiB 体积限制。

## 从源码构建

### Windows PowerShell

需要 CMake；本机安装 Zig 和 Ninja 时，构建脚本会优先使用轻量 Zig 工具链，也可以使用 MSVC。

```powershell
# 构建、测试、资源校验、离屏渲染和真实窗口启动冒烟测试
./build.ps1

# 生成 Windows x64 EXE 和 ZIP
./package.ps1 -RuntimeIdentifier win-x64

# 生成 Windows ARM64 包
./package.ps1 -RuntimeIdentifier win-arm64 -Toolchain msvc
```

也可以直接使用 CMake：

```powershell
cmake -S . -B build/native-windows-x64-zig
cmake --build build/native-windows-x64-zig --parallel
ctest --test-dir build/native-windows-x64-zig --output-on-failure
```

### macOS

需要 Xcode Command Line Tools、CMake 和 Clang；安装 Ninja 后会自动使用 Ninja。

```bash
bash scripts/build-macos.sh osx-arm64   # Apple Silicon
bash scripts/build-macos.sh osx-x64     # Intel
```

构建脚本会执行 CMake 构建、CTest、资源校验、渲染冒烟测试、签名、ZIP/DMG 打包、架构检查和体积检查。

签名相关环境变量：

- `CODE_SIGN_IDENTITY`：默认 `-`，使用 ad-hoc 签名。
- `APPLE_NOTARY_PROFILE`：Apple `notarytool` Keychain Profile。
- `REQUIRE_NOTARIZATION=1`：要求正式签名和 Apple 公证。

## 命令行工具

```text
--version
--validate-resources
--smoke-test
--preview <目录>
--test-sound
--expression-demo
```

这些命令用于版本检查、资源验证、五状态渲染、预览图生成、声音测试和状态轮换演示。Windows 还支持 `--startup-smoke-test`，用于在隔离配置中验证真实窗口、通知区域和监控线程启动。

## 文档

- [架构说明](docs/architecture.md)
- [版本记录](CHANGELOG.md)
- [资源与第三方声明](CREDITS.md)
