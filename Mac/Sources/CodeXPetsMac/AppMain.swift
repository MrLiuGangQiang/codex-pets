import AppKit
import CodeXPetsCore
import Darwin
import Foundation

@main
enum CodeXPetsApplication {
    @MainActor
    static func main() {
        let arguments = CommandLine.arguments
        if arguments.contains("--version") {
            print(AppInfo.version)
            return
        }
        if arguments.contains("--validate-resources") {
            let missing = AppResources.missingResources()
            if missing.isEmpty {
                print("CodeXPets macOS resources: OK")
                return
            }
            fputs("Missing resources: \(missing.joined(separator: ", "))\n", stderr)
            exit(1)
        }

        let application = NSApplication.shared
        application.setActivationPolicy(.accessory)
        let delegate = AppDelegate()
        application.delegate = delegate
        application.run()
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var settings = AppSettings.load()
    private var monitor: CodexSessionMonitor!
    private var petController: PetWindowController!
    private let soundPlayer = SoundPlayer()

    private var statusItem: NSStatusItem!
    private let statusMenu = NSMenu()
    private var statusMenuItem: NSMenuItem!
    private var assistantMenuItem: NSMenuItem!
    private var soundMenuItem: NSMenuItem!
    private var launchAtLoginMenuItem: NSMenuItem!
    private var diagnosticsController: DiagnosticsWindowController?

    private var pollTimer: Timer?
    private var animationTimer: Timer?
    private var lastAnimationTime = ProcessInfo.processInfo.systemUptime
    private var animationAccumulator: TimeInterval = 0
    private var animationFrame = 0
    private var petAnimationTick = 0
    private var lastVisualRefresh = Date.distantPast

    private var completedUntil = Date.distantPast
    private var abnormalUntil = Date.distantPast
    private var latestChangedState = ReminderState.idle
    private var latestChangedSourcePath: String?
    private var showNewestTaskOnNextRefresh = false
    private var lastVisualState: ReminderState?

    func applicationDidFinishLaunching(_ notification: Notification) {
        if SingleInstanceChecker.hasAnotherInstance() {
            let alert = NSAlert()
            alert.messageText = "CodeXPets 已经在菜单栏里啦。"
            alert.runModal()
            NSApp.terminate(nil)
            return
        }

        buildStatusMenu()
        petController = PetWindowController(menu: statusMenu, settings: settings)
        petController.showInactive()

        monitor = CodexSessionMonitor(sessionsRoot: settings.sessionsRoot)
        monitor.onTaskStarted = { [weak self] in self?.taskStarted() }
        monitor.onTaskCompleted = { [weak self] in self?.taskCompleted() }
        monitor.onTaskAborted = { [weak self] in self?.taskAborted() }
        monitor.onStateChanged = { [weak self] in self?.monitorStateChanged() }

        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) {
            [weak self] _ in
            Task { @MainActor [weak self] in self?.poll() }
        }
        animationTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) {
            [weak self] _ in
            Task { @MainActor [weak self] in self?.animate() }
        }
        RunLoop.main.add(pollTimer!, forMode: .common)
        RunLoop.main.add(animationTimer!, forMode: .common)
        refreshVisual(forceText: true)
    }

    func applicationWillTerminate(_ notification: Notification) {
        pollTimer?.invalidate()
        animationTimer?.invalidate()
        petController?.saveCurrentPosition()
        monitor?.dispose()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func menuWillOpen(_ menu: NSMenu) {
        launchAtLoginMenuItem.state = LaunchAtLoginManager.isEnabled ? .on : .off
        soundMenuItem.state = settings.soundEnabled ? .on : .off
        assistantMenuItem.state = petController?.window?.isVisible == true ? .on : .off
    }

    private func buildStatusMenu() {
        statusMenu.delegate = self
        statusMenuItem = NSMenuItem(title: "状态：正在检查 Codex…", action: nil,
                                    keyEquivalent: "")
        statusMenuItem.isEnabled = false
        assistantMenuItem = NSMenuItem(title: "显示桌面助手",
                                       action: #selector(toggleAssistant),
                                       keyEquivalent: "")
        assistantMenuItem.target = self
        assistantMenuItem.state = .on
        soundMenuItem = NSMenuItem(title: "播放语音提醒",
                                   action: #selector(toggleSound),
                                   keyEquivalent: "")
        soundMenuItem.target = self
        soundMenuItem.state = settings.soundEnabled ? .on : .off
        launchAtLoginMenuItem = NSMenuItem(title: "登录时自动运行",
                                           action: #selector(toggleLaunchAtLogin),
                                           keyEquivalent: "")
        launchAtLoginMenuItem.target = self
        launchAtLoginMenuItem.state = LaunchAtLoginManager.isEnabled ? .on : .off

        statusMenu.addItem(statusMenuItem)
        statusMenu.addItem(.separator())
        statusMenu.addItem(assistantMenuItem)
        statusMenu.addItem(soundMenuItem)
        statusMenu.addItem(launchAtLoginMenuItem)
        statusMenu.addItem(menuItem("打开 Codex 会话目录",
                                    action: #selector(openSessionsFolder)))
        statusMenu.addItem(menuItem("设置…", action: #selector(showSettings)))
        statusMenu.addItem(menuItem("诊断信息…", action: #selector(showDiagnostics)))
        statusMenu.addItem(menuItem("查看更新…", action: #selector(openLatestRelease)))
        statusMenu.addItem(menuItem("关于 CodeXPets", action: #selector(showAbout)))
        statusMenu.addItem(.separator())
        statusMenu.addItem(menuItem("退出", action: #selector(quit)))

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = StatusIconRenderer.image(state: .idle, frame: 0)
        statusItem.button?.toolTip = "正在检查 Codex"
        statusItem.menu = statusMenu
    }

    private func menuItem(_ title: String, action: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        return item
    }

    @objc private func toggleAssistant() {
        let shouldShow = assistantMenuItem.state != .on
        assistantMenuItem.state = shouldShow ? .on : .off
        if shouldShow { petController.showInactive() } else { petController.hidePet() }
    }

    @objc private func toggleSound() {
        settings.soundEnabled.toggle()
        settings.save()
        soundMenuItem.state = settings.soundEnabled ? .on : .off
    }

    @objc private func toggleLaunchAtLogin() {
        let enable = !LaunchAtLoginManager.isEnabled
        do {
            try LaunchAtLoginManager.setEnabled(enable)
        } catch {
            showError(title: "设置登录启动失败", error: error)
        }
        launchAtLoginMenuItem.state = LaunchAtLoginManager.isEnabled ? .on : .off
    }

    @objc private func openSessionsFolder() {
        let root = settings.sessionsRoot
        if FileManager.default.fileExists(atPath: root) {
            NSWorkspace.shared.open(URL(fileURLWithPath: root, isDirectory: true))
        } else {
            let alert = NSAlert()
            alert.messageText = "还没有找到 Codex 会话目录"
            alert.informativeText = root
            alert.runModal()
        }
    }

    @objc private func showSettings() {
        let controller = SettingsWindowController(settings: settings)
        guard let next = controller.runModal() else { return }
        let previousRoot = settings.sessionsRoot
        settings = next
        settings.save()
        soundMenuItem.state = settings.soundEnabled ? .on : .off
        petController.applySettings(settings)
        if previousRoot != settings.sessionsRoot {
            monitor.setSessionsRoot(settings.sessionsRoot)
        }
        refreshVisual(forceText: true)
    }

    @objc private func showDiagnostics() {
        if diagnosticsController == nil {
            diagnosticsController = DiagnosticsWindowController(
                diagnosticsProvider: { [weak self] in self?.diagnosticsText() ?? "" },
                sessionsRootProvider: { [weak self] in self?.settings.sessionsRoot ?? "" })
        }
        diagnosticsController?.showWindow(nil)
    }

    @objc private func openLatestRelease() {
        let url = AppInfo.repository.appendingPathComponent("releases/latest")
        NSWorkspace.shared.open(url)
    }

    @objc private func showAbout() {
        let alert = NSAlert()
        alert.messageText = AppInfo.displayName
        alert.informativeText = "macOS 菜单栏与桌面宠物版\n监测本地 Codex CLI / Codex 桌面端会话状态。"
        alert.icon = AppResources.image(named: "AppIcon")
        alert.addButton(withTitle: "好")
        alert.runModal()
    }

    @objc private func quit() { NSApp.terminate(nil) }

    private func poll() {
        monitor.poll()
    }

    private func animate() {
        let nowUptime = ProcessInfo.processInfo.systemUptime
        let elapsed = max(0.001, min(0.1, nowUptime - lastAnimationTime))
        lastAnimationTime = nowUptime
        animationAccumulator += elapsed
        while animationAccumulator >= 0.12 {
            animationAccumulator -= 0.12
            animationFrame = (animationFrame + 1) % 8
            petAnimationTick = (petAnimationTick + 1) % 6400
        }
        petController.animate(tick: petAnimationTick, elapsed: elapsed)
        let now = Date()
        let refreshInterval: TimeInterval = monitor.activeCount > 0 ? 0.12 : 0.25
        if now.timeIntervalSince(lastVisualRefresh) >= refreshInterval {
            refreshVisual(forceText: false)
        }
    }

    private func taskStarted() {
        recordLatestTaskChange(.busy)
        latestChangedSourcePath = monitor.lastEventFile
        showNewestTaskOnNextRefresh = true
        revealAssistant()
        refreshVisual(forceText: true)
        if settings.soundEnabled { soundPlayer.enqueue(.start) }
    }

    private func taskCompleted() {
        recordLatestTaskChange(.completed)
        revealAssistant()
        refreshVisual(forceText: true)
        if settings.soundEnabled { soundPlayer.enqueue(.complete) }
    }

    private func taskAborted() {
        recordLatestTaskChange(.error)
        revealAssistant()
        refreshVisual(forceText: true)
        if settings.soundEnabled { soundPlayer.enqueue(.error) }
    }

    private func monitorStateChanged() {
        if monitor.lastEventType == "update_plan" {
            recordLatestTaskChange(.busy)
            latestChangedSourcePath = monitor.lastEventFile
            showNewestTaskOnNextRefresh = true
        }
        refreshVisual(forceText: true)
    }

    private func recordLatestTaskChange(_ state: ReminderState) {
        latestChangedState = state
        if state == .busy {
            completedUntil = .distantPast
            abnormalUntil = .distantPast
        } else if state == .completed {
            completedUntil = Date().addingTimeInterval(5)
            abnormalUntil = .distantPast
        } else if state == .error {
            abnormalUntil = Date().addingTimeInterval(10)
            completedUntil = .distantPast
        }
    }

    private func revealAssistant() {
        if assistantMenuItem.state == .on { petController.showInactive() }
    }

    private func refreshVisual(forceText: Bool) {
        let active = monitor.activeCount
        let now = Date()
        if now >= completedUntil { completedUntil = .distantPast }
        if now >= abnormalUntil { abnormalUntil = .distantPast }
        lastVisualRefresh = now
        let visualState = AppLogic.selectVisualState(
            activeCount: active, abnormalRecently: now < abnormalUntil,
            completedRecently: now < completedUntil,
            latestChangedState: latestChangedState)
        if visualState != lastVisualState {
            petAnimationTick = 0
            lastVisualState = visualState
        }

        statusItem.button?.image = StatusIconRenderer.image(state: visualState,
                                                            frame: animationFrame)
        let stateText: String
        if visualState == .busy {
            stateText = busyStateText(active: active)
        } else if visualState == .error {
            stateText = "异常"
        } else if visualState == .completed {
            stateText = "已完成"
        } else {
            stateText = "空闲"
        }
        statusMenuItem.title = "状态：\(stateText)"
        statusItem.button?.toolTip = stateText

        let thought: String
        if visualState == .error {
            thought = "任务异常了"
        } else if visualState == .completed {
            thought = "任务完成啦！"
        } else if visualState == .busy {
            thought = monitor.primaryActiveTitle ?? "正在认真处理你的任务…"
        } else {
            thought = "等你交给我下一个任务"
        }

        let titles: [String]
        let progress: [String?]
        if visualState == .error {
            titles = [AppLogic.formatAbnormalTaskText(monitor.lastAbortedTitle)]
            progress = []
        } else if visualState == .completed, let title = monitor.lastCompletedTitle {
            titles = [title]
            progress = []
        } else if active > 0 {
            titles = monitor.activeTitles
            progress = monitor.activePlanProgressLabels
        } else {
            titles = monitor.activeTitles
            progress = []
        }

        let focusLatest = showNewestTaskOnNextRefresh && visualState == .busy
        let latestIndex = monitor.activeTitleIndex(sourcePath: latestChangedSourcePath)
        let preferredIndex = AppLogic.selectPreferredTaskIndex(
            focusLatestTask: focusLatest, latestTaskIndex: latestIndex)
        petController.updateStatus(status: stateText, thought: thought,
            state: visualState, titles: titles, progressLabels: progress,
            selectNewestTask: focusLatest, preferredTaskIndex: preferredIndex)
        if focusLatest { showNewestTaskOnNextRefresh = false }
    }

    private func busyStateText(active: Int) -> String {
        var value = active == 1 ? "进行中" : "进行中（\(active) 个会话）"
        let total = monitor.totalPlanStepCount
        if active == 1, total > 0 {
            value += " · 步骤 \(monitor.completedPlanStepCount)/\(total)"
        }
        return value
    }

    private func diagnosticsText() -> String {
        [
            AppInfo.displayName,
            "平台：macOS",
            "声音提醒：\(settings.soundEnabled ? "开启" : "关闭")",
            "登录启动：\(LaunchAtLoginManager.isEnabled ? "开启" : "关闭")",
            "触发区高度：\(settings.dockHoverHeight) px",
            "自动隐藏：\(settings.dockIdleHideSeconds <= 0 ? "关闭" : "\(settings.dockIdleHideSeconds) 秒")",
            "",
            monitor.diagnosticsText()
        ].joined(separator: "\n")
    }

    private func showError(title: String, error: Error) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = error.localizedDescription
        alert.runModal()
    }
}
