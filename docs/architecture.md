# CodeXPets 架构设计

## 设计目标

1. Windows x64/ARM64 与 macOS x64/ARM64 使用同一业务实现。
2. 会话识别结果在不同平台保持一致。
3. 平台 API 不进入 Core，Core 不引用 Avalonia。
4. 任务状态只来自 Codex JSONL 会话文件，避免多套监控实现漂移。
5. 资源、CPU 架构、版本、签名和发布产物都可以自动校验。
6. 正式仓库只保留一套 .NET 10 + Avalonia 产品架构。

## 依赖方向

```text
CodeXPets.App  ───────>  CodeXPets.Core
     │                        │
     │ Avalonia               │ System.Text.Json / BCL
     │ Win32 / Objective-C    │ 无 UI、无平台窗口依赖
     └────────────────────────┘
```

- `CodeXPets.Core.Configuration`：Codex 路径、`CODEX_HOME` 与 JSON 设置。
- `CodeXPets.Core.Monitoring`：任务模型、有界 JSONL 文件发现、增量读取、UTF-8 解码和事件归约。
- `CodeXPets.Core.Application`：视觉状态、任务选择、吸附和精灵索引规则。
- `CodeXPets.Core.Domain`：状态、位置和几何值对象。
- `CodeXPets.App.Views`：透明窗口、桌宠 surface、设置与诊断窗口。
- `CodeXPets.App.Services`：`MonitorWorker`、资源、声音和离屏预览。
- `CodeXPets.App.Infrastructure`：单实例、旧设置迁移和 OS adapter。

## 运行时数据流

```text
Codex sessions/**/*.jsonl
          │ 创建 / 修改 / 重命名 / 删除
          ▼
  FileSystemWatcher（递归）
          │ 变化信号
          ▼
MonitorWorker
  ├─ 25 ms 事件防抖
  ├─ 120 ms 最小轮询间隔
  └─ 30 s 恢复轮询
          │ Poll()
          ▼
CodexSessionMonitor
  ├─ 当天及前两天快速发现
  ├─ 每 120 s 完整发现
  ├─ 最多跟踪最近修改的 40 个文件
  ├─ 按文件位置读取新增字节
  ├─ 有状态 UTF-8 解码与行缓冲
  └─ 任务、标题、完成/异常与计划进度归约
          │ MonitorSnapshot / MonitorEventKind
          ▼
 Dispatcher.UIThread
          ▼
AppController ── VisualStateCoordinator / TrayIcon / SoundService / PetWindow
```

`MonitorWorker` 只把文件变化转换为轮询信号，不直接解析 JSONL。多个连续文件事件会合并；即使操作系统遗漏通知、监听器暂时无法创建或监听缓冲区报错，30 秒恢复轮询仍会继续工作，并在后续尝试重新建立监听器。

`CodexSessionMonitor` 为每个文件保存读取位置、UTF-8 解码器和未完成行缓冲。文件增长时只读取新增字节；文件被截断时重置该文件状态。初次读取历史内容时抑制旧完成通知，后续新增内容才产生实时任务事件。监控器只读会话文件，不修改 Codex 数据。

监控线程不持有 UI 对象。`AppController` 接收不可变快照，并只在 Avalonia UI 线程更新窗口、托盘和声音状态。

## 平台边界

`IPlatformService` 是唯一的窗口级 OS 边界：

- Windows：`WM_NCHITTEST` 透明区域穿透、`WS_EX_NOACTIVATE`、注册表启动项、Explorer、MessageBox。
- macOS：CoreGraphics 光标、`setIgnoresMouseEvents:`、`SMAppService`、`open`。

音频服务内部仅按平台选择 Windows MCI 或 macOS `afplay`。其余状态和资源逻辑完全共享。

## 路径与设置

- `CodexPaths` 每次从当前 OS 与 `CODEX_HOME` 解析默认路径。
- 默认会话目录为 Codex Home 下的 `sessions`，也可以由设置覆盖。
- Windows 文件路径比较不区分大小写；macOS 文件路径比较区分大小写。
- 保存的位置由显示器标识、相对 X/Y 和吸附边组成，不直接保存某一平台的绝对路径。
- 设置写入平台用户数据目录，并从临时文件原子替换。

## 资源合同

`--validate-resources` 不只检查文件存在，还检查：

- 浮动精灵表 `1536x832`，8 列 × 4 行。
- 扒边精灵表 `2048x256`，左右各 4 帧。
- 所有托盘图标与应用图标尺寸。
- PNG 可解码、背景透明。
- 首帧和整表中白色身体、深色耳尾、暖色内耳和金黄色眼睛的像素阈值。
- 三个语音资源非空。

这可以阻止带棋盘背景、错误尺寸或错误配色的 AI 中间图进入发布包。

## 发布合同

- `VERSION` 是程序集版本、包名和 Info.plist 版本的单一来源。
- Windows x64/ARM64 发布为压缩 self-contained 单 EXE，禁止旁置运行时文件，并检查 apphost PE machine。
- macOS 包检查 apphost 及所有 Mach-O 文件的目标架构。
- 能在当前 runner 原生执行的产物必须通过 `--smoke-test`，并离屏渲染空闲、忙碌、完成、异常和左右扒边状态。
- macOS Developer ID 构建使用 hardened runtime 与 JIT entitlement，并执行 notarization/stapling。
- Release 汇总四个架构产物并重新生成统一 SHA-256。
