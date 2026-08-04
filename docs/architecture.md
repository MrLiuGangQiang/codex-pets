# CodeXPets 4.0 原生架构

## 总览

```text
Codex sessions/*.jsonl
          │
          ▼
  CodexSessionMonitor / MonitorWorker
          │  500 ms 自适应轮询、增量偏移读取
          ▼
  MonitorSnapshot + MonitorEventKind
          │
          ▼
  VisualStateCoordinator + AppLogic
          │
     ┌────┴────┐
     ▼         ▼
 Windows     macOS
 Win32       AppKit
 GDI+        Core Graphics
```

`src/core` 不依赖任何 UI 框架。平台层只负责窗口、输入、系统菜单、声音、登录启动和系统诊断。

## 核心模块

- `types.h`：平台无关的状态、位置和监控快照模型。
- `json.*`：自研小型 JSON 读写器，支持设置和会话中需要的 JSON 类型及 Unicode 代理对。
- `paths.*`：`CODEX_HOME`、用户目录、配置文件和 settings 路径。
- `settings.*`：设置归一化、原子写入、旧 Windows/macOS 位置迁移。
- `session_monitor.*`：JSONL 增量读取、活动 turn、异常、完成和计划进度。
- `app_logic.*`：视觉状态、吸附、边缘触发、任务选择和动画帧规则。
- `visual_state.*`：完成/异常通知窗口和新任务聚焦状态。

### 轮询而不是文件系统 watcher

原来的 UI 运行时和 watcher 都会带来较大的常驻内存和跨平台差异。原生版本每 500 ms 检查目录元数据，随后只读取最近文件的新增字节；每次扫描都保持有界的文件表，最多 40 个文件。这个设计牺牲了极少量响应延迟，换取更小的线程、句柄和平台适配成本，并且在 watcher 丢事件时可以自动恢复。

## Windows

`src/platform/windows` 使用：

- Win32 分层透明窗口和 `WM_NCHITTEST` Alpha 穿透。
- GDI+ 绘制文字、灯泡及精灵；云朵使用旧版 Skia 降采样后的 640×221 像素资源，以保持原有外观。
- 通知区域图标、注册表登录启动、MCI MP3 播放。
- `MonitorWorker` 事件通过隐藏消息窗口投递到 UI 线程。
- 屏幕标识格式包含设备名和几何信息，兼容旧注册表中的 Base64 标识。

发布时使用静态运行库/原生系统库，不携带第三方 DLL。链接阶段打开尺寸优化和 dead code elimination。

## macOS

`src/platform/macos/main.mm` 使用：

- `NSPanel` 透明无边框窗口，菜单栏 `NSStatusItem`。
- `NSImage` 按需缓存精灵，云朵和灯泡使用 AppKit 矢量绘制。
- `setIgnoresMouseEvents:` 配合 50 ms 全局鼠标轮询实现透明点击穿透。
- 手动拖动、左/右吸附、局部边缘唤出和多显示器恢复。
- `SMAppService` 登录启动，`NSSound` 播放语音，Mach task API 输出内存诊断。
- 预览使用 Core Graphics bitmap context，不依赖大型离屏渲染库。

### 坐标转换

共享设置格式使用 top-origin 的 `RelativeY`：

- Windows 工作区本来就是 top-origin。
- AppKit 全局坐标是 bottom-origin，保存时使用
  `RelativeY = (work.top - anchorY) / work.height`，恢复时反向转换。
- 旧 macOS `NSUserDefaults` 中的 bottom-origin 值由
  `deserialize_legacy_macos_position` 先镜像一次。

浮动模式保存窗口底部锚点；吸附模式保存屏幕边缘和鼠标的垂直坐标，而不是保存窗口被隐藏后的实际坐标，因此显示器分辨率变化后仍能稳定恢复。

## 资源策略

`assets` 只包含运行时真正需要的单帧 PNG 和三段 MP3：

- `floating/`：4 个状态 × 8 帧。
- `dock/`：左右扒边按需帧。
- `icons/`：菜单栏/通知区域状态图标。
- `audio/`：开始、完成、异常语音。

2122×734 云朵原图不进入发布包；Windows 携带旧版运行时实际使用的 640×221 降采样资源。macOS 构建只复制运行时目录，并由 `make-macos-icon.sh` 使用系统 `sips`/`iconutil` 生成 `AppIcon.icns`。

## 设置和迁移

统一 JSON 设置文件通过临时文件加原子替换写入。启动时：

1. 如果原生 settings.json 存在，读取并归一化。
2. Windows 若没有新文件，尝试旧注册表路径。
3. macOS 若没有新文件，尝试旧 `com.mrliugangqiang.codexpets` defaults 域。
4. 迁移成功后立即写入原生 JSON，后续不再依赖旧架构。

## 体积/内存边界

原生程序没有可携带的托管运行时或 JIT。发布脚本对以下对象逐一执行 10 MiB 检查：

- Windows EXE 和 ZIP。
- macOS `.app`、ZIP 和 DMG。

应用内不主动调用 `EmptyWorkingSet` 或类似 API。工作集中的系统共享代码页不是应用私有分配；诊断窗口同时显示工作集和 macOS `phys_footprint`/Windows PrivateUsage，便于区分两者。

## 测试层次

- `tests/native_tests.cpp`：核心路径、设置、JSON、状态机和 JSONL 生命周期。
- `--validate-resources`：运行时资源存在性和尺寸合同。
- `--smoke-test`：四种状态、左右吸附和离屏渲染。
- `--startup-smoke-test`：在隔离配置下真实创建 Windows 后台消息窗口、桌宠窗口、托盘和监控线程，完成首次渲染后清理退出。
- CTest：平台核心测试以及目标系统上的资源/冒烟命令。
- 发布脚本：PE/Mach-O 架构、Info.plist、签名、公证、ZIP/DMG 结构和 SHA-256。
