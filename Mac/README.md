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
- `dist/CodeXPets-v<version>-macos-universal.zip`
- `dist/SHA256SUMS-macos.txt`

发布包使用 ad-hoc 签名，未做 Apple Developer ID 公证。首次打开下载版时，如系统提示来源未知，请在 Finder 中右键应用并选择“打开”。
