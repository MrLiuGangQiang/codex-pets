# 素材致谢

## 白色英国短毛猫精灵

- `assets/floating/` 保留原桌宠的 8 帧状态动画，覆盖空闲、完成、忙碌和异常。
- `assets/dock/` 保留左右扒边姿势，以及正常、眨眼、开心和异常表情。
- 角色为卡通化白色英国短毛猫：纯白身体、深灰色耳朵和尾巴、暖粉色内耳、金黄色眼睛。

## 其他资源

- `assets/icons/`：菜单栏/通知区域状态图标。
- `assets/audio/`：开始、完成和异常语音提示。
- `assets/app/AppIcon.png`：macOS 构建时由系统 `sips`/`iconutil` 转换为 `AppIcon.icns`；Windows 图标由原生资源脚本编译。

CodeXPets 4.0 的运行时不依赖第三方 UI 或音频库；图片由 Win32 GDI+ 或 AppKit 按需加载，音频由 Windows MCI 或 macOS `NSSound` 播放。
