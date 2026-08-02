import AppKit
import CodeXPetsCore

@MainActor
final class PetWindowController: NSWindowController {
    static let windowSize = NSSize(width: 420, height: 260)
    static let bubbleSize = NSSize(width: 320, height: 112)
    static let petSize = NSSize(width: 130, height: 140)
    static let dockSize: CGFloat = 104
    static let scrollStartHold: TimeInterval = 1.9
    static let scrollEndHold: TimeInterval = 1.7
    static let shortTaskDisplay: TimeInterval = 6
    static let scrollSpeed: CGFloat = 15

    let petView: PetView
    let contextMenu: NSMenu
    var settings: AppSettings

    var statusText = "空闲"
    var thoughtText = "等你交给我下一个任务"
    var currentState = ReminderState.idle
    var taskTitles: [String] = []
    var taskProgressLabels: [String?] = []
    var taskIndex = 0

    var animationTick = 0
    var scrollOffset: CGFloat = 0
    var scrollHold = PetWindowController.scrollStartHold
    var scrollCycle: TimeInterval = 0
    var scrollAtEnd = false
    var lastDisplayedText = ""

    var dockEdge = DockEdge.none
    var dockY: CGFloat = 0
    var dockScreenIdentifier = ""
    var dockBubbleBelow = false
    var dockVisibility: CGFloat = 1
    var dockLastContentChange = Date()
    var dockThoughtUntil = Date.distantPast
    var dockHoverRevealUntil = Date.distantPast

    var dragPending = false
    var dragging = false
    var dragStartedDocked = false
    var dragStartMouse = CGPoint.zero
    var dragStartOrigin = CGPoint.zero

    init(menu: NSMenu, settings: AppSettings) {
        self.contextMenu = menu
        self.settings = settings
        self.petView = PetView(frame: NSRect(origin: .zero, size: Self.windowSize))
        let panel = NSPanel(contentRect: NSRect(origin: .zero, size: Self.windowSize),
                            styleMask: [.borderless, .nonactivatingPanel],
                            backing: .buffered, defer: false)
        panel.level = .floating
        panel.backgroundColor = .clear
        panel.isOpaque = false
        panel.hasShadow = false
        panel.hidesOnDeactivate = false
        panel.isMovable = false
        panel.isReleasedWhenClosed = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.contentView = petView
        super.init(window: panel)
        petView.owner = self
        petView.menu = menu
        placeAtDefaultLocation()
        _ = restoreSavedPosition()
        updateMousePassThrough()
    }

    required init?(coder: NSCoder) { nil }

    var isDocked: Bool { dockEdge != .none }
    var selectedTaskIndex: Int { taskIndex }

    func showInactive() {
        guard let window else { return }
        window.orderFrontRegardless()
        updateMousePassThrough()
    }

    func hidePet() { window?.orderOut(nil) }

    func applySettings(_ next: AppSettings) {
        settings = next
        if isDocked { positionDockedWindow() }
        petView.needsDisplay = true
    }

    func updateStatus(status: String, thought: String, state: ReminderState,
                      titles: [String], progressLabels: [String?],
                      selectNewestTask: Bool, preferredTaskIndex: Int) {
        let nextProgress = normalizedProgress(progressLabels, count: titles.count)
        let selectedTitle = selectedTaskTitle
        let previousIndex = taskIndex
        let changed = statusText != status || thoughtText != thought || currentState != state ||
            taskTitles != titles || !optionalStringArraysEqual(taskProgressLabels, nextProgress)

        if changed && isDocked && state != .idle {
            let now = Date()
            dockLastContentChange = now
            dockThoughtUntil = now.addingTimeInterval(Double(AppLogic.cloudNotificationSeconds(
                state: state, configuredSeconds: settings.dockNotificationSeconds)))
            dockVisibility = 1
        }

        statusText = status
        thoughtText = thought
        currentState = state
        taskTitles = titles
        taskProgressLabels = nextProgress

        let nextIndex: Int
        if state != .busy {
            nextIndex = 0
        } else if preferredTaskIndex >= 0, preferredTaskIndex < titles.count {
            nextIndex = preferredTaskIndex
        } else if selectNewestTask, !titles.isEmpty {
            nextIndex = titles.count - 1
        } else if let selectedTitle,
                  let matching = titles.firstIndex(of: selectedTitle) {
            nextIndex = matching
        } else {
            nextIndex = min(max(0, previousIndex), max(0, titles.count - 1))
        }

        let selectionChanged = taskIndex != nextIndex
        taskIndex = nextIndex
        if changed || selectionChanged { resetScroll() }
        if changed || selectionChanged { petView.needsDisplay = true }
    }

    func animate(tick: Int, elapsed: TimeInterval) {
        let frameChanged = animationTick != tick
        animationTick = tick
        let scrollChanged = currentState == .busy && advanceScroll(elapsed: elapsed)
        let dockChanged = advanceDockAnimation(elapsed: elapsed)
        updateMousePassThrough()
        if frameChanged || scrollChanged || dockChanged { petView.needsDisplay = true }
    }

    @discardableResult
    func switchFromCloudClick() -> Bool {
        guard taskTitles.count > 1 else { return false }
        moveToNextTask()
        if isDocked {
            let now = Date()
            dockVisibility = 1
            dockLastContentChange = now
            dockThoughtUntil = now.addingTimeInterval(Double(AppLogic.cloudNotificationSeconds(
                state: currentState, configuredSeconds: settings.dockNotificationSeconds)))
            dockHoverRevealUntil = now.addingTimeInterval(Double(settings.dockRevealSeconds))
        }
        petView.needsDisplay = true
        return true
    }

    var selectedTaskTitle: String? {
        guard !taskTitles.isEmpty else { return nil }
        return taskTitles[min(max(0, taskIndex), taskTitles.count - 1)]
    }

    var displayedText: String { selectedTaskTitle ?? thoughtText }

    func resetScroll() {
        scrollOffset = 0
        scrollHold = Self.scrollStartHold
        scrollCycle = 0
        scrollAtEnd = false
        lastDisplayedText = normalizedDisplayText(displayedText)
    }

    func moveToNextTask() {
        taskIndex = taskTitles.count > 1 ? (taskIndex + 1) % taskTitles.count : 0
        resetScroll()
    }

    func normalizedDisplayText(_ text: String) -> String {
        text.replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
            .split(separator: "\n")
            .map { line in line.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ") }
            .filter { !$0.isEmpty }
            .joined(separator: "\n")
    }

    func normalizedProgress(_ values: [String?], count: Int) -> [String?] {
        if values.count == count { return values }
        if values.count > count { return Array(values.prefix(count)) }
        return values + Array(repeating: nil, count: count - values.count)
    }

    func optionalStringArraysEqual(_ lhs: [String?], _ rhs: [String?]) -> Bool {
        guard lhs.count == rhs.count else { return false }
        return zip(lhs, rhs).allSatisfy { $0 == $1 }
    }
}

@MainActor
final class PetView: NSView {
    weak var owner: PetWindowController?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        owner?.draw(in: bounds)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func mouseDown(with event: NSEvent) { owner?.handleMouseDown(event) }
    override func mouseDragged(with event: NSEvent) { owner?.handleMouseDragged(event) }
    override func mouseUp(with event: NSEvent) { owner?.handleMouseUp(event) }
    override func rightMouseDown(with event: NSEvent) { owner?.showContextMenu(with: event) }
}
