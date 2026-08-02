import CoreGraphics
import Foundation

public enum ReminderState: String, Codable, CaseIterable {
    case idle
    case busy
    case completed
    case error
}

public enum DockEdge: String, Codable {
    case none
    case left
    case right
}

public struct PetPositionState: Codable, Equatable {
    public var dockEdge: DockEdge
    public var screenIdentifier: String
    public var relativeX: Double
    public var relativeY: Double

    public init(dockEdge: DockEdge, screenIdentifier: String, relativeX: Double,
                relativeY: Double) {
        self.dockEdge = dockEdge
        self.screenIdentifier = screenIdentifier
        self.relativeX = Self.clamp01(relativeX)
        self.relativeY = Self.clamp01(relativeY)
    }

    public static func clamp01(_ value: Double) -> Double {
        min(1, max(0, value.isFinite ? value : 0.5))
    }
}

public struct AppSettings: Equatable {
    public var dockHoverHeight: Int
    public var dockIdleHideSeconds: Int
    public var dockRevealSeconds: Int
    public var dockNotificationSeconds: Int
    public var soundEnabled: Bool
    public var sessionsRoot: String

    private enum Key {
        static let dockHoverHeight = "DockHoverHeight"
        static let dockIdleHideSeconds = "DockIdleHideSeconds"
        static let dockRevealSeconds = "DockRevealSeconds"
        static let dockNotificationSeconds = "DockNotificationSeconds"
        static let soundEnabled = "SoundEnabled"
        static let sessionsRoot = "SessionsRoot"
        static let petPosition = "PetPositionV1"
    }

    public init(dockHoverHeight: Int = 240, dockIdleHideSeconds: Int = 10,
                dockRevealSeconds: Int = 3, dockNotificationSeconds: Int = 5,
                soundEnabled: Bool = true,
                sessionsRoot: String = AppSettings.defaultSessionsRoot()) {
        self.dockHoverHeight = dockHoverHeight
        self.dockIdleHideSeconds = dockIdleHideSeconds
        self.dockRevealSeconds = dockRevealSeconds
        self.dockNotificationSeconds = dockNotificationSeconds
        self.soundEnabled = soundEnabled
        self.sessionsRoot = sessionsRoot
        normalize()
    }

    public static func load(defaults: UserDefaults = .standard) -> AppSettings {
        var value = AppSettings()
        if defaults.object(forKey: Key.dockHoverHeight) != nil {
            value.dockHoverHeight = defaults.integer(forKey: Key.dockHoverHeight)
        }
        if defaults.object(forKey: Key.dockIdleHideSeconds) != nil {
            value.dockIdleHideSeconds = defaults.integer(forKey: Key.dockIdleHideSeconds)
        }
        if defaults.object(forKey: Key.dockRevealSeconds) != nil {
            value.dockRevealSeconds = defaults.integer(forKey: Key.dockRevealSeconds)
        }
        if defaults.object(forKey: Key.dockNotificationSeconds) != nil {
            value.dockNotificationSeconds = defaults.integer(forKey: Key.dockNotificationSeconds)
        }
        if defaults.object(forKey: Key.soundEnabled) != nil {
            value.soundEnabled = defaults.bool(forKey: Key.soundEnabled)
        }
        if let root = defaults.string(forKey: Key.sessionsRoot), !root.isEmpty {
            value.sessionsRoot = root
        }
        value.normalize()
        return value
    }

    public func save(defaults: UserDefaults = .standard) {
        var normalized = self
        normalized.normalize()
        defaults.set(normalized.dockHoverHeight, forKey: Key.dockHoverHeight)
        defaults.set(normalized.dockIdleHideSeconds, forKey: Key.dockIdleHideSeconds)
        defaults.set(normalized.dockRevealSeconds, forKey: Key.dockRevealSeconds)
        defaults.set(normalized.dockNotificationSeconds, forKey: Key.dockNotificationSeconds)
        defaults.set(normalized.soundEnabled, forKey: Key.soundEnabled)
        defaults.set(normalized.sessionsRoot, forKey: Key.sessionsRoot)
    }

    public mutating func normalize() {
        dockHoverHeight = min(1000, max(40, dockHoverHeight))
        dockIdleHideSeconds = min(3600, max(0, dockIdleHideSeconds))
        dockRevealSeconds = min(60, max(1, dockRevealSeconds))
        dockNotificationSeconds = min(120, max(1, dockNotificationSeconds))
        sessionsRoot = Self.normalizeSessionsRoot(sessionsRoot)
    }

    public static func defaultSessionsRoot(environment: [String: String] =
        ProcessInfo.processInfo.environment) -> String {
        let home: String
        if let codexHome = environment["CODEX_HOME"], !codexHome.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            home = codexHome
        } else if let userHome = environment["HOME"], !userHome.isEmpty {
            home = (userHome as NSString).appendingPathComponent(".codex")
        } else {
            home = (NSHomeDirectory() as NSString).appendingPathComponent(".codex")
        }
        return (home as NSString).appendingPathComponent("sessions")
    }

    public static func normalizeSessionsRoot(_ rawPath: String) -> String {
        let trimmed = rawPath.trimmingCharacters(in: .whitespacesAndNewlines)
        let source = trimmed.isEmpty ? defaultSessionsRoot() : trimmed
        let expanded = (source as NSString).expandingTildeInPath
        return (expanded as NSString).standardizingPath
    }

    public static func loadPetPosition(defaults: UserDefaults = .standard) -> PetPositionState? {
        guard let data = defaults.data(forKey: Key.petPosition) else { return nil }
        return try? JSONDecoder().decode(PetPositionState.self, from: data)
    }

    public static func savePetPosition(_ state: PetPositionState,
                                       defaults: UserDefaults = .standard) {
        if let data = try? JSONEncoder().encode(state) {
            defaults.set(data, forKey: Key.petPosition)
        }
    }
}

public enum AppLogic {
    public static func formatAbnormalTaskText(_ title: String?) -> String {
        let normalized = title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return "任务失败：" + (normalized.isEmpty ? "未知任务" : normalized)
    }

    public static func selectPreferredTaskIndex(focusLatestTask: Bool,
                                                latestTaskIndex: Int) -> Int {
        focusLatestTask ? latestTaskIndex : -1
    }

    public static func selectVisualState(activeCount: Int, abnormalRecently: Bool,
                                         completedRecently: Bool,
                                         latestChangedState: ReminderState) -> ReminderState {
        if latestChangedState == .error && abnormalRecently { return .error }
        if latestChangedState == .completed && completedRecently { return .completed }
        if latestChangedState == .busy && activeCount > 0 { return .busy }
        return activeCount > 0 ? .busy : .idle
    }

    public static func cloudNotificationSeconds(state: ReminderState,
                                                configuredSeconds: Int) -> Int {
        let safe = max(1, configuredSeconds)
        return state == .error ? max(10, safe) : safe
    }

    public static func shouldShowThoughtBubble(isDocked: Bool, state: ReminderState,
                                               now: Date, dockThoughtUntil: Date) -> Bool {
        if !isDocked { return true }
        let hasTaskState = state == .busy || state == .completed || state == .error
        return hasTaskState && now < dockThoughtUntil
    }

    public static func shouldKeepDockVisible(lastContentChange: Date, now: Date,
                                             idleHideSeconds: Int) -> Bool {
        idleHideSeconds <= 0 || now.timeIntervalSince(lastContentChange) < Double(idleHideSeconds)
    }

    public static func shouldShowDock(lastContentChange: Date, now: Date,
                                      isDragging: Bool, isHovering: Bool,
                                      hoverRevealUntil: Date,
                                      idleHideSeconds: Int) -> Bool {
        isDragging || isHovering || now < hoverRevealUntil ||
            shouldKeepDockVisible(lastContentChange: lastContentChange, now: now,
                                  idleHideSeconds: idleHideSeconds)
    }

    public static func selectSnapEdge(cursor: CGPoint, workArea: CGRect,
                                      snapDistance: CGFloat) -> DockEdge {
        if abs(cursor.x - workArea.minX) <= snapDistance { return .left }
        if abs(cursor.x - workArea.maxX) <= snapDistance { return .right }
        return .none
    }

    public static func shouldMirrorFloatingSprite(anchor: CGPoint,
                                                   workArea: CGRect) -> Bool {
        anchor.x < workArea.midX
    }

    public static func dockHoverBounds(edge: DockEdge, workArea: CGRect,
                                       dockY: CGFloat, scale: CGFloat,
                                       fullyHidden: Bool, hoverHeight: Int) -> CGRect {
        let width = fullyHidden ? max(18, 28 * scale) : max(40, 56 * scale)
        let normalizedHeight = min(1000, max(40, hoverHeight))
        let halfHeight = max(20, CGFloat(normalizedHeight) * scale / 2)
        let x = edge == .left ? workArea.minX : workArea.maxX - width
        let bottom = max(workArea.minY, dockY - halfHeight)
        let top = min(workArea.maxY, dockY + halfHeight)
        return CGRect(x: x, y: bottom, width: width, height: max(1, top - bottom))
    }

    public static func isTaskSwitchPoint(isDocked: Bool, bubbleVisible: Bool,
                                         state: ReminderState, taskCount: Int,
                                         bubbleBounds: CGRect, contentBounds: CGRect,
                                         point: CGPoint) -> Bool {
        guard bubbleVisible, taskCount > 1, state != .idle else { return false }
        return isDocked ? bubbleBounds.contains(point) : contentBounds.contains(point)
    }

    public static func formatBusyMetadata(stepProgress: String?, sessionIndex: Int,
                                          sessionCount: Int) -> String {
        var parts: [String] = []
        if let stepProgress, !stepProgress.isEmpty { parts.append("(\(stepProgress))") }
        if sessionCount > 1 {
            let safeIndex = min(max(0, sessionIndex), sessionCount - 1)
            parts.append("会话(\(safeIndex + 1)/\(sessionCount))")
        }
        return parts.joined(separator: "·")
    }

    public static func formatBusyHeader(stepProgress: String?, sessionIndex: Int,
                                        sessionCount: Int) -> String {
        let metadata = formatBusyMetadata(stepProgress: stepProgress,
                                          sessionIndex: sessionIndex,
                                          sessionCount: sessionCount)
        guard !metadata.isEmpty else { return "进行中" }
        return metadata.hasPrefix("(") ? "进行中\(metadata)" : "进行中·\(metadata)"
    }
}
