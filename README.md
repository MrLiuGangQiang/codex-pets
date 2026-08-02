# CodeXPets

CodeXPets 是一个跨平台 Codex 桌面宠物，原生支持 **Windows** 与 **macOS**。它通过 Windows 通知区域或 macOS 菜单栏状态图标，以及桌面白猫和云朵气泡，显示本地 Codex CLI / Codex 桌面端会话的任务状态。

## 状态

- **空闲**：绿色圆点。
- **进行中**：黄色呼吸圆点。
- **异常**：红色圆点，例如任务中断、服务端错误、请求失败或异常完成。
- **完成**：短暂显示绿色对勾，并可播放内嵌提示音。

桌面助手会自动换行和滚动长任务标题；多个进行中任务会顺序轮换。点击任务文本可手动切换；吸附状态下可点击整朵云切换任务。

## 下载

请从 GitHub Releases 下载最新正式版：

- `CodeXPets-v3.0.0-win-portable.zip`：Windows 便携版，解压后运行 `CodeXPets.exe`。
- `CodeXPets.exe`：Windows 单文件主程序。
- `CodeXPets-v3.0.0-macos-universal.zip`：macOS Universal 2 版，支持 Apple Silicon 与 Intel。
- `SHA256SUMS.txt`：全部发布文件的 SHA-256 校验值。

当前正式版本：**3.0.0**。

## 使用

### Windows

1. 解压 Windows 便携包并双击 `CodeXPets.exe`。
2. 程序常驻任务栏通知区域；右键图标可显示/隐藏宠物、设置、诊断、打开会话目录、查看更新或退出。
3. 可选运行 `create-shortcuts.ps1` 创建开始菜单和桌面快捷方式。

### macOS

1. 解压 macOS 发布包，将 `CodeXPets.app` 拖入“应用程序”。
2. 启动后程序常驻菜单栏；菜单提供与 Windows 版对应的显示、声音、登录启动、设置、诊断和更新入口。
3. 当前个人发布包使用 ad-hoc 签名、未做 Apple Developer ID 公证。首次打开如被系统拦截，请在 Finder 中右键 `CodeXPets.app` 并选择“打开”。

CodeXPets 只读取本地会话 JSONL，不会修改 Codex 会话内容。

## 设置

双平台均支持：

- 吸附位置附近的边缘触发区高度。
- 吸附后自动隐藏时间；设为 `0` 表示不自动隐藏。
- 鼠标唤出后的保持时间。
- 任务变化时思考云朵的保持时间。
- 开始、完成和异常语音提醒。
- Codex 会话目录，可适配自定义 `CODEX_HOME` 或桌面端实际使用的会话位置。
- 开机/登录时自动运行。

Windows 设置保存在 `HKCU\Software\CodeXPets`；macOS 设置保存在当前用户的 `UserDefaults`。

## 桌面宠物与吸附

桌面宠物固定为黑耳白猫。拖动小猫靠近当前屏幕左侧或右侧边缘并松开，即可切换为扒边姿势；顶部不吸附。

程序会记住普通位置、显示器、吸附方向和垂直位置，并在分辨率或显示器变化后将宠物限制在可见工作区域。未吸附时，小猫会根据所在屏幕左右半区自动朝向屏幕内部。

吸附后连续一段时间没有任务变化，小猫会滑出隐藏。隐藏后只在最后吸附高度附近的局部边缘触发滑入，避免经过整条屏幕边缘时误唤出。再次拖动猫头即可解除吸附。

## 会话监测

默认目录：

- Windows：`%USERPROFILE%\.codex\sessions`
- macOS：`~/.codex/sessions`

如果设置了 `CODEX_HOME`，默认读取其 `sessions` 子目录；也可以在设置窗口中手动选择。

监测器会：

- 增量读取新增 JSONL 内容。
- 正确处理 UTF-8 中文或 emoji 跨 64 KB 读取块。
- 定期扫描最近修改的旧日期会话，避免漏掉持续多天的任务。
- 根据任务与会话文件活动时间清理失联任务。
- 识别任务开始、完成、中断、常见失败、异常完成和计划进度更新。

## 诊断

诊断窗口显示监听目录、跟踪文件、活跃任务、最近轮询/扫描/读取/事件、最近会话文件、JSON 解析错误、文件读取错误和过期任务清理计数，并支持复制。

## 构建与测试

### Windows

```powershell
.\build.ps1
.\CodeXPets.SelfTest.exe
.\package.ps1
```

### macOS

```bash
cd Mac
swift test -c release
cd ..
bash Mac/build-mac.sh
```

macOS 构建输出 `dist/CodeXPets.app` 和 `dist/CodeXPets-v3.0.0-macos-universal.zip`。详细说明见 `Mac/README.md`。

## 说明

- 白猫精灵、吸附姿势、云朵和开始/完成/异常提示音均随应用打包。
- 双平台动画约 30 FPS，状态和文本更新按活动状态节流。
- GitHub Actions 会在 Windows 与 macOS 上执行测试、构建、打包和标签发版。
- Windows 未签名构建可能触发 SmartScreen；macOS 未公证构建可能触发 Gatekeeper。
