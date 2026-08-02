import AppKit
import CodeXPetsCore

@MainActor
final class SettingsWindowController: NSWindowController, NSWindowDelegate {
    private let settingsViewController: SettingsViewController

    init(settings: AppSettings) {
        settingsViewController = SettingsViewController(settings: settings)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 420),
                              styleMask: [.titled, .closable],
                              backing: .buffered, defer: false)
        window.title = "\(AppInfo.displayName) 设置"
        window.isReleasedWhenClosed = false
        window.contentViewController = settingsViewController
        super.init(window: window)
        window.delegate = self
    }

    required init?(coder: NSCoder) { nil }

    func runModal() -> AppSettings? {
        guard let window else { return nil }
        window.center()
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        let response = NSApp.runModal(for: window)
        window.orderOut(nil)
        return response == .OK ? settingsViewController.resultSettings() : nil
    }

    func windowWillClose(_ notification: Notification) {
        if NSApp.modalWindow === window { NSApp.stopModal(withCode: .cancel) }
    }
}

@MainActor
private final class SettingsViewController: NSViewController {
    private var current: AppSettings
    private let hoverHeightField = SettingsViewController.numberField(minimum: 40, maximum: 1000)
    private let idleHideField = SettingsViewController.numberField(minimum: 0, maximum: 3600)
    private let revealField = SettingsViewController.numberField(minimum: 1, maximum: 60)
    private let notificationField = SettingsViewController.numberField(minimum: 1, maximum: 120)
    private let soundButton = NSButton(checkboxWithTitle: "播放开始、完成和异常语音提醒",
                                       target: nil, action: nil)
    private let sessionsRootField = NSTextField()

    init(settings: AppSettings) {
        current = settings
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { nil }

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 680, height: 420))
        buildInterface()
        apply(current)
    }

    func resultSettings() -> AppSettings {
        var result = current
        result.dockHoverHeight = Int(hoverHeightField.stringValue) ?? current.dockHoverHeight
        result.dockIdleHideSeconds = Int(idleHideField.stringValue) ?? current.dockIdleHideSeconds
        result.dockRevealSeconds = Int(revealField.stringValue) ?? current.dockRevealSeconds
        result.dockNotificationSeconds = Int(notificationField.stringValue) ?? current.dockNotificationSeconds
        result.soundEnabled = soundButton.state == .on
        result.sessionsRoot = sessionsRootField.stringValue
        result.normalize()
        return result
    }

    private func buildInterface() {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)

        stack.addArrangedSubview(row(label: "边缘触发区高度：", control: hoverHeightField,
                                     suffix: "px"))
        stack.addArrangedSubview(row(label: "吸附自动隐藏：", control: idleHideField,
                                     suffix: "秒（0 表示不隐藏）"))
        stack.addArrangedSubview(row(label: "鼠标唤出保持：", control: revealField, suffix: "秒"))
        stack.addArrangedSubview(row(label: "任务云朵保持：", control: notificationField, suffix: "秒"))
        stack.addArrangedSubview(row(label: "声音：", control: soundButton, suffix: nil))

        sessionsRootField.translatesAutoresizingMaskIntoConstraints = false
        sessionsRootField.widthAnchor.constraint(greaterThanOrEqualToConstant: 360).isActive = true
        let browseButton = NSButton(title: "浏览…", target: self, action: #selector(browseSessionsRoot))
        let folderStack = NSStackView(views: [sessionsRootField, browseButton])
        folderStack.orientation = .horizontal
        folderStack.alignment = .centerY
        folderStack.spacing = 8
        stack.addArrangedSubview(row(label: "Codex 会话目录：", control: folderStack, suffix: nil))

        let note = NSTextField(wrappingLabelWithString:
            "自动隐藏仅影响吸附状态。会话目录修改后会立即重新扫描；默认目录为 ~/.codex/sessions，也支持 CODEX_HOME。")
        note.textColor = .secondaryLabelColor
        note.maximumNumberOfLines = 0
        note.translatesAutoresizingMaskIntoConstraints = false
        note.widthAnchor.constraint(equalToConstant: 620).isActive = true
        stack.addArrangedSubview(note)

        let defaultsButton = NSButton(title: "恢复默认", target: self, action: #selector(restoreDefaults))
        let cancelButton = NSButton(title: "取消", target: self, action: #selector(cancel))
        let okButton = NSButton(title: "确定", target: self, action: #selector(confirm))
        okButton.keyEquivalent = "\r"
        let buttons = NSStackView(views: [defaultsButton, NSView(), cancelButton, okButton])
        buttons.orientation = .horizontal
        buttons.alignment = .centerY
        buttons.spacing = 10
        buttons.translatesAutoresizingMaskIntoConstraints = false
        buttons.widthAnchor.constraint(equalToConstant: 620).isActive = true
        stack.addArrangedSubview(buttons)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 26),
            stack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -26),
            stack.topAnchor.constraint(equalTo: view.topAnchor, constant: 24),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: view.bottomAnchor, constant: -20)
        ])
    }

    private func row(label text: String, control: NSView, suffix: String?) -> NSView {
        let label = NSTextField(labelWithString: text)
        label.alignment = .right
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 170).isActive = true
        var views: [NSView] = [label, control]
        if let suffix {
            let suffixLabel = NSTextField(labelWithString: suffix)
            suffixLabel.textColor = .secondaryLabelColor
            views.append(suffixLabel)
        }
        let row = NSStackView(views: views)
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        row.heightAnchor.constraint(greaterThanOrEqualToConstant: 30).isActive = true
        return row
    }

    private func apply(_ settings: AppSettings) {
        current = settings
        hoverHeightField.stringValue = String(settings.dockHoverHeight)
        idleHideField.stringValue = String(settings.dockIdleHideSeconds)
        revealField.stringValue = String(settings.dockRevealSeconds)
        notificationField.stringValue = String(settings.dockNotificationSeconds)
        soundButton.state = settings.soundEnabled ? .on : .off
        sessionsRootField.stringValue = settings.sessionsRoot
    }

    @objc private func browseSessionsRoot() {
        let panel = NSOpenPanel()
        panel.title = "选择 Codex 会话目录"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = URL(fileURLWithPath: sessionsRootField.stringValue, isDirectory: true)
        if panel.runModal() == .OK, let url = panel.url {
            sessionsRootField.stringValue = url.path
        }
    }

    @objc private func restoreDefaults() { apply(AppSettings()) }
    @objc private func cancel() { NSApp.stopModal(withCode: .cancel) }
    @objc private func confirm() { NSApp.stopModal(withCode: .OK) }

    private static func numberField(minimum: Int, maximum: Int) -> NSTextField {
        let field = NSTextField()
        let formatter = NumberFormatter()
        formatter.numberStyle = .none
        formatter.allowsFloats = false
        formatter.minimum = NSNumber(value: minimum)
        formatter.maximum = NSNumber(value: maximum)
        field.formatter = formatter
        field.alignment = .right
        field.translatesAutoresizingMaskIntoConstraints = false
        field.widthAnchor.constraint(equalToConstant: 90).isActive = true
        return field
    }
}

@MainActor
final class DiagnosticsWindowController: NSWindowController, NSWindowDelegate {
    private let diagnosticsProvider: () -> String
    private let sessionsRootProvider: () -> String
    private let textView = NSTextView()
    private var refreshTimer: Timer?

    init(diagnosticsProvider: @escaping () -> String,
         sessionsRootProvider: @escaping () -> String) {
        self.diagnosticsProvider = diagnosticsProvider
        self.sessionsRootProvider = sessionsRootProvider
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 540),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable],
                              backing: .buffered, defer: false)
        window.title = "\(AppInfo.displayName) 诊断信息"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        buildInterface(in: window)
    }

    required init?(coder: NSCoder) { nil }

    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
        refresh()
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.refresh() }
        }
    }

    func windowWillClose(_ notification: Notification) {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    private func buildInterface(in window: NSWindow) {
        let content = NSView()
        window.contentView = content

        textView.isEditable = false
        textView.isSelectable = true
        textView.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.textContainerInset = NSSize(width: 10, height: 10)
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.borderType = .bezelBorder
        scroll.documentView = textView
        scroll.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(scroll)

        let copyButton = NSButton(title: "复制", target: self, action: #selector(copyDiagnostics))
        let refreshButton = NSButton(title: "刷新", target: self, action: #selector(refreshAction))
        let folderButton = NSButton(title: "打开会话目录", target: self, action: #selector(openSessionsFolder))
        let closeButton = NSButton(title: "关闭", target: self, action: #selector(closeWindow))
        let buttons = NSStackView(views: [copyButton, refreshButton, folderButton, NSView(), closeButton])
        buttons.orientation = .horizontal
        buttons.alignment = .centerY
        buttons.spacing = 10
        buttons.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(buttons)

        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            scroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            scroll.topAnchor.constraint(equalTo: content.topAnchor, constant: 16),
            scroll.bottomAnchor.constraint(equalTo: buttons.topAnchor, constant: -12),
            buttons.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            buttons.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            buttons.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -14),
            buttons.heightAnchor.constraint(equalToConstant: 32)
        ])
    }

    private func refresh() {
        textView.string = diagnosticsProvider()
        textView.scrollToBeginningOfDocument(nil)
    }

    @objc private func refreshAction() { refresh() }

    @objc private func copyDiagnostics() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(textView.string, forType: .string)
    }

    @objc private func openSessionsFolder() {
        let path = sessionsRootProvider()
        if FileManager.default.fileExists(atPath: path) {
            NSWorkspace.shared.open(URL(fileURLWithPath: path, isDirectory: true))
        } else {
            let alert = NSAlert()
            alert.messageText = "尚未找到 Codex 会话目录"
            alert.informativeText = path
            alert.runModal()
        }
    }

    @objc private func closeWindow() { close() }
}
