# CodeXPets for macOS

macOS 版本使用 AppKit + Swift Package Manager 实现，与 Windows 版共用相同的会话识别规则和交互语义。

## 功能

- 菜单栏状态图标：空闲、进行中、完成、异常。
- 透明桌面宠物、任务云朵、多会话点击切换、长文本滚动。
- 左右屏幕边缘吸附、自动隐藏、局部边缘唤出、多显示器位置记忆。
- Codex JSONL 增量监测、计划进度、异常完成识别、诊断信息。
- 开始/完成/异常内嵌提示音、自定义会话目录、登录时自动运行。
- 原生 Apple Silicon + Intel Universal 2 应用包。

## 要求

- macOS 13 Ventura 或更高版本。
- 构建需要 Xcode 15 或更高版本提供的 Swift 工具链。

## 测试

```bash
cd Mac
swift test -c release
```

## 构建与打包

```bash
bash Mac/build-mac.sh
```

输出：

- `dist/CodeXPets.app`
- `dist/CodeXPets-v<version>-macos-universal.dmg`（推荐安装包，包含 Applications 快捷入口）
- `dist/CodeXPets-v<version>-macos-universal.zip`（备用压缩包）
- `dist/SHA256SUMS-macos.txt`

构建脚本会创建并挂载 DMG，验证应用版本、Applications 快捷入口、arm64 + x86_64 架构、资源完整性与代码签名。

发布包使用 ad-hoc 签名，未做 Apple Developer ID 公证。安装时打开 DMG，将 `CodeXPets.app` 拖到 `Applications`；首次打开如被系统拦截，请在 Finder 中右键应用并选择“打开”。
