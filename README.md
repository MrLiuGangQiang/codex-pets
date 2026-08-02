# CodeXPets

一个无需安装的 Windows 桌面宠物，用任务栏通知区域图标和云朵气泡显示 Codex CLI 的任务状态。

## 状态

- **空闲**：绿色圆点。
- **进行中**：黄色圆点，带呼吸效果。
- **异常**：红色圆点（例如任务被中断）。
- **完成**：短暂显示绿色对勾圆点，并播放内嵌的完成提示音。

提示：鼠标悬停到托盘图标时，还会显示当前任务标题。桌面助手会自动将任务内容换成多行；较长内容从顶部向上滚动，滚动到底后顺序切换任务或重新从顶部开始。

## 使用

1. 双击 `CodeXPets.exe`。
2. 程序常驻 Windows 任务栏通知区域。
3. 右键图标可查看状态、设置开机自动运行、打开 Codex 会话目录或退出。
4. 不需要安装；退出程序后可直接删除整个文件夹。


## 下载

请从 GitHub Releases 下载最新的正式版：

- `CodeXPets-v2.0.1-win-portable.zip`：推荐，解压后直接运行。
- `CodeXPets.exe`：单独的主程序文件。

当前正式版本：**2.0.1**。
## 构建

在 Windows PowerShell 中运行：

```powershell
.\build.ps1
```

构建输出：

- `CodeXPets.exe`：主程序。
- `CodeXPets.SelfTest.exe`：自测程序。

执行自测：

```powershell
.\CodeXPets.SelfTest.exe
```

## 监测位置

默认读取：

```text
%USERPROFILE%\.codex\sessions
```

如果设置了 `CODEX_HOME`，则读取 `%CODEX_HOME%\sessions`。

## 说明

- 程序为单文件 EXE，Codex 图标与开始、完成、异常提示音均已内嵌。
- 播放使用 Windows 系统媒体组件，MP3 无需额外安装播放器。
- 图标源自已验证发布者 OpenAI 的官方 Codex 扩展。
- 本工具是本地绿色提醒工具，不会修改 Codex 会话内容。
