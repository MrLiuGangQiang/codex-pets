import Foundation

public final class CodexSessionMonitor {
    private static let readBufferSize = 65_536
    private static let maximumBufferedLineBytes = 1_048_576
    private static let maximumTrackedFiles = 40
    private static let fullDiscoveryInterval: TimeInterval = 120
    private static let staleTurnGrace: TimeInterval = 600

    private static let failureMessagePrefixes = [
        "traceback (most recent call last):",
        "unhandled exception",
        "fatal error",
        "internal server error",
        "server error",
        "api error",
        "request failed",
        "stream error",
        "something went wrong",
        "服务端错误",
        "服务器错误",
        "内部服务器错误",
        "请求失败"
    ]

    private static let failureMessageFragments = [
        "stream disconnected before completion",
        "connection reset by peer",
        "upstream service error",
        "service unavailable",
        "bad gateway",
        "gateway timeout",
        "rate limit exceeded",
        "error sending request"
    ]

    private static let failureEventTypes: Set<String> = [
        "turn_aborted", "task_failed", "turn_failed", "stream_error",
        "request_error", "error"
    ]

    private static let httpServerErrorRegex = try! NSRegularExpression(
        pattern: #"\bhttp(?: status)?\s*5\d\d\b"#)
    private static let failedTestsRegex = try! NSRegularExpression(
        pattern: #"\b[1-9]\d*\s+test\(s\)\s+failed\b"#)
    private static let nonZeroExitCodeRegex = try! NSRegularExpression(
        pattern: #"(?:^|[\r\n])\s*exit code:\s*[1-9]\d*\b"#)

    public var onTaskStarted: (() -> Void)?
    public var onTaskCompleted: (() -> Void)?
    public var onTaskAborted: (() -> Void)?
    public var onStateChanged: (() -> Void)?

    private var sessionsRoot: String
    private var files: [String: TailState] = [:]
    private var activeTurns: [String: ActiveTurn] = [:]
    private var plansByFile: [String: TaskPlanProgress] = [:]
    private var nextStartedSequence: Int64 = 0

    public private(set) var lastCompletedTitle: String?
    public private(set) var lastAbortedTitle: String?
    public private(set) var lastEventType: String?
    public private(set) var lastEventFile: String?

    private var lastReadFile: String?
    private var lastError: String?
    private var parseErrorCount = 0
    private var readErrorCount = 0
    private var staleTurnCleanupCount = 0
    private var lastDiscovery = Date.distantPast
    private var lastRead = Date.distantPast
    private var lastEvent = Date.distantPast
    private var lastPoll = Date.distantPast
    private var nextDiscovery = Date.distantPast
    private var nextFullDiscovery = Date.distantPast
    private var nextStaleTurnCheck = Date.distantPast
    private var disposed = false

    public init(sessionsRoot: String = AppSettings.defaultSessionsRoot()) {
        self.sessionsRoot = AppSettings.normalizeSessionsRoot(sessionsRoot)
        discoverFiles(initial: true)
    }

    public var activeCount: Int { activeTurns.count }

    public var activeTitles: [String] {
        orderedActiveTurns.map { turn in
            guard let title = turn.title, !title.isEmpty else { return "正在处理任务…" }
            return title
        }
    }

    public var activePlanProgressLabels: [String?] {
        orderedActiveTurns.map { turn in
            guard let plan = turn.plan, plan.totalSteps > 1 else { return nil }
            return "\(plan.completedSteps)/\(plan.totalSteps)"
        }
    }

    public var totalPlanStepCount: Int {
        activeTurns.values.reduce(0) { $0 + ($1.plan?.totalSteps ?? 0) }
    }

    public var completedPlanStepCount: Int {
        activeTurns.values.reduce(0) { $0 + ($1.plan?.completedSteps ?? 0) }
    }

    public var primaryCurrentPlanStep: String? {
        orderedActiveTurns.compactMap { $0.plan }.first(where: { $0.totalSteps > 0 })?.currentStep
    }

    public var primaryActiveTitle: String? {
        orderedActiveTurns.compactMap { turn in
            guard let title = turn.title, !title.isEmpty else { return nil }
            return title
        }.first
    }

    public func activeTitleIndex(sourcePath: String?) -> Int {
        guard let sourcePath, !sourcePath.isEmpty else { return -1 }
        return orderedActiveTurns.firstIndex { pathsEqual($0.sourcePath, sourcePath) } ?? -1
    }

    public func setSessionsRoot(_ path: String) {
        guard !disposed else { return }
        let normalized = AppSettings.normalizeSessionsRoot(path)
        guard !pathsEqual(normalized, sessionsRoot) else { return }
        sessionsRoot = normalized
        files.removeAll()
        activeTurns.removeAll()
        plansByFile.removeAll()
        lastCompletedTitle = nil
        lastAbortedTitle = nil
        nextDiscovery = .distantPast
        nextFullDiscovery = .distantPast
        nextStaleTurnCheck = .distantPast
        discoverFiles(initial: true)
        onStateChanged?()
    }

    public func poll() {
        guard !disposed else { return }
        let now = Date()
        if now >= nextDiscovery {
            discoverFiles(initial: false)
            nextDiscovery = now.addingTimeInterval(1.2)
        }
        for state in Array(files.values) {
            readNewBytes(state, suppressNotifications: false)
        }
        if now >= nextStaleTurnCheck {
            nextStaleTurnCheck = now.addingTimeInterval(30)
            clearStaleTurns(now: now)
        }
        lastPoll = now
    }

    public func dispose() {
        disposed = true
        files.removeAll()
        activeTurns.removeAll()
        plansByFile.removeAll()
    }

    public func reportUnexpectedError(_ operation: String, error: Error?) {
        readErrorCount += 1
        lastError = "\(operation)：\(error?.localizedDescription ?? "未知错误")"
    }

    public func diagnosticsText() -> String {
        var lines = [
            "会话监听",
            "  目录：\(sessionsRoot)",
            "  目录存在：\(FileManager.default.fileExists(atPath: sessionsRoot) ? "是" : "否")",
            "  已跟踪文件：\(files.count)",
            "  活跃任务：\(activeTurns.count)",
            "  最近轮询：\(formatDate(lastPoll))",
            "  最近扫描：\(formatDate(lastDiscovery))",
            "  最近读取：\(formatDate(lastRead))"
        ]
        if let lastReadFile { lines.append("  最近读取文件：\(lastReadFile)") }
        let eventDescription = lastEventType.map { "\($0) · \(formatDate(lastEvent))" } ?? "无"
        lines.append("  最近事件：\(eventDescription)")
        if let lastEventFile { lines.append("  最近事件文件：\(lastEventFile)") }
        lines.append("  JSON 解析错误：\(parseErrorCount)")
        lines.append("  文件读取错误：\(readErrorCount)")
        lines.append("  过期任务清理：\(staleTurnCleanupCount)")
        if let lastError { lines.append("  最近错误：\(lastError)") }
        if !activeTurns.isEmpty {
            lines.append("")
            lines.append("活跃任务明细")
            for turn in orderedActiveTurns {
                lines.append("  \(turn.turnID) | \(turn.title?.isEmpty == false ? turn.title! : "未命名任务")")
                lines.append("    最近活动：\(formatDate(turn.lastActivity))")
                lines.append("    文件：\(turn.sourcePath)")
            }
        }
        return lines.joined(separator: "\n")
    }

    public static func looksLikeFailureMessage(_ message: String?) -> Bool {
        guard let message else { return false }
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        let lower = trimmed.lowercased()
        if failureMessagePrefixes.contains(where: { lower.hasPrefix($0) }) { return true }
        if failureMessageFragments.contains(where: { lower.contains($0) }) { return true }
        let range = NSRange(lower.startIndex..<lower.endIndex, in: lower)
        return httpServerErrorRegex.firstMatch(in: lower, range: range) != nil ||
            failedTestsRegex.firstMatch(in: lower, range: range) != nil ||
            nonZeroExitCodeRegex.firstMatch(in: lower, range: range) != nil
    }

    public static func isTurnStale(lastActivity: Date, fileWrite: Date, now: Date,
                                   graceSeconds: TimeInterval) -> Bool {
        guard graceSeconds > 0 else { return false }
        let cutoff = now.addingTimeInterval(-graceSeconds)
        return lastActivity < cutoff && fileWrite < cutoff
    }

    private var orderedActiveTurns: [ActiveTurn] {
        activeTurns.values.sorted { $0.startSequence < $1.startSequence }
    }

    private func discoverFiles(initial: Bool) {
        let now = Date()
        lastDiscovery = now
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: sessionsRoot, isDirectory: &isDirectory),
              isDirectory.boolValue else { return }

        var candidates = Set<String>()
        let calendar = Calendar.current
        for daysAgo in 0...2 {
            guard let day = calendar.date(byAdding: .day, value: -daysAgo, to: now) else { continue }
            let components = calendar.dateComponents([.year, .month, .day], from: day)
            guard let year = components.year, let month = components.month, let dayNumber = components.day else { continue }
            let folder = URL(fileURLWithPath: sessionsRoot, isDirectory: true)
                .appendingPathComponent(String(format: "%04d", year), isDirectory: true)
                .appendingPathComponent(String(format: "%02d", month), isDirectory: true)
                .appendingPathComponent(String(format: "%02d", dayNumber), isDirectory: true)
            do {
                let names = try FileManager.default.contentsOfDirectory(at: folder,
                    includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
                for url in names where url.pathExtension.lowercased() == "jsonl" {
                    candidates.insert(url.path)
                }
            } catch let error as NSError where error.code == NSFileNoSuchFileError {
                // A missing date folder is normal.
            } catch {
                reportUnexpectedError("扫描日期目录", error: error)
            }
        }

        if initial || now >= nextFullDiscovery {
            nextFullDiscovery = now.addingTimeInterval(Self.fullDiscoveryInterval)
            if let enumerator = FileManager.default.enumerator(at: URL(fileURLWithPath: sessionsRoot),
                includingPropertiesForKeys: [.isRegularFileKey],
                options: [.skipsHiddenFiles, .skipsPackageDescendants],
                errorHandler: { [weak self] _, error in
                    self?.reportUnexpectedError("完整扫描会话目录", error: error)
                    return true
                }) {
                for case let url as URL in enumerator where url.pathExtension.lowercased() == "jsonl" {
                    candidates.insert(url.path)
                }
            }
        }

        let newest = candidates.sorted {
            safeLastWriteDate($0) > safeLastWriteDate($1)
        }.prefix(Self.maximumTrackedFiles)
        for path in newest where files[path] == nil {
            let state = TailState(path: path, lastWrite: safeLastWriteDate(path))
            files[path] = state
            readNewBytes(state, suppressNotifications: initial)
        }
        pruneTrackedFiles()
    }

    private func pruneTrackedFiles() {
        guard files.count > Self.maximumTrackedFiles else { return }
        let activePaths = Set(activeTurns.values.map { normalizedPath($0.sourcePath) })
        let removable = files.values
            .filter { !activePaths.contains(normalizedPath($0.path)) }
            .sorted { $0.lastActivity < $1.lastActivity }
        for state in removable {
            guard files.count > Self.maximumTrackedFiles else { break }
            files.removeValue(forKey: state.path)
            plansByFile.removeValue(forKey: state.path)
        }
    }

    private func readNewBytes(_ state: TailState, suppressNotifications: Bool) {
        do {
            let attributes = try FileManager.default.attributesOfItem(atPath: state.path)
            let length = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
            let writeDate = attributes[.modificationDate] as? Date ?? .distantPast
            if length < state.position {
                removeTurns(forFile: state.path)
                plansByFile.removeValue(forKey: state.path)
                state.reset()
            }
            if length == state.position && writeDate <= state.lastWrite { return }
            if length == state.position {
                state.lastWrite = writeDate
                return
            }

            let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: state.path))
            defer { try? handle.close() }
            try handle.seek(toOffset: state.position)
            while let data = try handle.read(upToCount: Self.readBufferSize), !data.isEmpty {
                consume(data: data, state: state, suppressNotifications: suppressNotifications)
                state.position += UInt64(data.count)
            }

            let readDate = Date()
            let activityDate = suppressNotifications && writeDate != .distantPast ? writeDate : readDate
            state.lastWrite = writeDate
            state.lastActivity = activityDate
            lastReadFile = state.path
            lastRead = readDate
            touchTurns(forFile: state.path, activityDate: activityDate)
        } catch {
            reportUnexpectedError("读取会话文件", error: error)
        }
    }

    private func consume(data: Data, state: TailState, suppressNotifications: Bool) {
        var incoming = data
        if state.skipCurrentLine {
            if let newline = incoming.firstIndex(of: 0x0A) {
                incoming = incoming.subdata(in: incoming.index(after: newline)..<incoming.endIndex)
                state.skipCurrentLine = false
            } else {
                return
            }
        }
        state.lineData.append(incoming)

        while let newline = state.lineData.firstIndex(of: 0x0A) {
            let line = state.lineData.subdata(in: state.lineData.startIndex..<newline)
            state.lineData.removeSubrange(state.lineData.startIndex...newline)
            if !line.isEmpty {
                processLine(line, sourcePath: state.path,
                            suppressNotifications: suppressNotifications)
            }
        }

        if state.lineData.count >= Self.maximumBufferedLineBytes {
            state.lineData.removeAll(keepingCapacity: true)
            state.skipCurrentLine = true
        }
    }

    private func processLine(_ data: Data, sourcePath: String,
                             suppressNotifications: Bool) {
        guard var line = String(data: data, encoding: .utf8) else {
            recordParseError("解析 UTF-8 会话内容", error: nil)
            return
        }
        line = line.trimmingCharacters(in: CharacterSet(charactersIn: "\u{FEFF} \t\r"))
        guard !line.isEmpty else { return }
        _ = tryProcessEvent(line, sourcePath: sourcePath,
                            suppressNotifications: suppressNotifications)
    }

    @discardableResult
    private func tryProcessEvent(_ line: String, sourcePath: String,
                                 suppressNotifications: Bool) -> Bool {
        if tryProcessPlanUpdate(line, sourcePath: sourcePath,
                                suppressNotifications: suppressNotifications) { return true }
        guard line.contains("event_msg") else { return false }
        guard let root = parseObject(line, operation: "解析 event_msg") else { return false }
        guard string(root, "type") == "event_msg" else { return false }
        guard let payload = object(root, "payload") else { return true }
        guard let eventType = string(payload, "type"), !eventType.isEmpty else { return true }

        let eventDate = eventDate(root)
        recordEvent(eventType, sourcePath: sourcePath)
        if eventType == "user_message" {
            if let title = string(payload, "message")?.trimmingCharacters(in: .whitespacesAndNewlines),
               !title.isEmpty {
                setTitle(forFile: sourcePath, title: title, activityDate: eventDate)
            }
            return true
        }

        let turnID = string(payload, "turn_id") ?? string(root, "turn_id")
        guard let turnID, !turnID.isEmpty else { return true }
        let before = activeTurns.count

        if eventType == "task_started" {
            var added = false
            let turn: ActiveTurn
            if let existing = activeTurns[turnID] {
                turn = existing
            } else {
                turn = ActiveTurn(turnID: turnID, sourcePath: sourcePath,
                                  startSequence: nextStartedSequence)
                nextStartedSequence += 1
                turn.plan = plansByFile[sourcePath]
                activeTurns[turnID] = turn
                added = true
            }
            touch(turn, activityDate: eventDate)
            if added && !suppressNotifications { onTaskStarted?() }
        } else if eventType == "task_complete" {
            let completedTurn = activeTurns[turnID]
            let wasActive = completedTurn != nil
            let abnormal = isAbnormalTaskCompletion(line)
            if wasActive { activeTurns.removeValue(forKey: turnID) }
            if let completedTurn, !suppressNotifications {
                if abnormal {
                    lastAbortedTitle = nonempty(completedTurn.title) ?? "发生异常的任务"
                    onTaskAborted?()
                } else {
                    lastCompletedTitle = nonempty(completedTurn.title) ?? "已完成的任务"
                    onTaskCompleted?()
                }
            }
        } else if Self.failureEventTypes.contains(eventType) {
            let abortedTurn = activeTurns.removeValue(forKey: turnID)
            if let abortedTurn, !suppressNotifications {
                lastAbortedTitle = nonempty(abortedTurn.title) ?? "发生异常的任务"
                onTaskAborted?()
            }
        } else if let turn = activeTurns[turnID] {
            touch(turn, activityDate: eventDate)
        }

        if before != activeTurns.count { onStateChanged?() }
        return true
    }

    @discardableResult
    private func tryProcessPlanUpdate(_ line: String, sourcePath: String,
                                      suppressNotifications: Bool) -> Bool {
        guard line.contains("update_plan"), line.contains("function_call") else { return false }
        guard let root = parseObject(line, operation: "解析 update_plan"),
              let payload = object(root, "payload") else { return false }
        guard string(payload, "type") == "function_call",
              string(payload, "name") == "update_plan" else { return false }

        recordEvent("update_plan", sourcePath: sourcePath)
        let activityDate = eventDate(root)
        guard let arguments = string(payload, "arguments"),
              let argumentsData = arguments.data(using: .utf8),
              let argumentsRoot = (try? JSONSerialization.jsonObject(with: argumentsData)) as? [String: Any],
              let planItems = argumentsRoot["plan"] as? [Any] else { return true }

        let metadata = object(payload, "internal_chat_message_metadata_passthrough")
        let requestedTurnID = string(metadata, "turn_id")
        let turn: ActiveTurn?
        if let requestedTurnID, let exact = activeTurns[requestedTurnID] {
            turn = exact
        } else {
            turn = activeTurns.values
                .filter { pathsEqual($0.sourcePath, sourcePath) }
                .max { $0.startSequence < $1.startSequence }
        }
        guard let turn else { return true }
        touch(turn, activityDate: activityDate)

        var completed = 0
        var currentStep: String?
        var firstPendingStep: String?
        for item in planItems {
            guard let step = item as? [String: Any] else { continue }
            let status = string(step, "status")
            let stepText = string(step, "step")
            if status == "completed" {
                completed += 1
            } else if status == "in_progress", currentStep == nil {
                currentStep = stepText
            } else if status == "pending", firstPendingStep == nil {
                firstPendingStep = stepText
            }
        }
        if currentStep == nil { currentStep = firstPendingStep }
        let next = TaskPlanProgress(totalSteps: planItems.count,
                                    completedSteps: completed,
                                    currentStep: currentStep)
        plansByFile[sourcePath] = next
        let changed = turn.plan != next
        turn.plan = next
        if changed && !suppressNotifications { onStateChanged?() }
        return true
    }

    private func isAbnormalTaskCompletion(_ line: String) -> Bool {
        guard let data = line.data(using: .utf8),
              let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let payload = object(root, "payload") else { return false }
        let status = string(payload, "status")?.lowercased()
        if ["failed", "error", "aborted", "cancelled"].contains(status ?? "") { return true }
        if let error = payload["error"], !(error is NSNull),
           !String(describing: error).trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return true
        }
        return Self.looksLikeFailureMessage(string(payload, "last_agent_message"))
    }

    private func parseObject(_ line: String, operation: String) -> [String: Any]? {
        guard let data = line.data(using: .utf8) else { return nil }
        do {
            return try JSONSerialization.jsonObject(with: data) as? [String: Any]
        } catch {
            if line.trimmingCharacters(in: .whitespacesAndNewlines).hasSuffix("}") {
                recordParseError(operation, error: error)
            }
            return nil
        }
    }

    private func recordEvent(_ type: String, sourcePath: String) {
        lastEventType = type
        lastEventFile = sourcePath
        lastEvent = Date()
    }

    private func recordParseError(_ operation: String, error: Error?) {
        parseErrorCount += 1
        lastError = "\(operation)：\(error?.localizedDescription ?? "未知错误")"
    }

    private func removeTurns(forFile path: String) {
        let ids = activeTurns.values.filter { pathsEqual($0.sourcePath, path) }.map(\.turnID)
        guard !ids.isEmpty else { return }
        for id in ids { activeTurns.removeValue(forKey: id) }
        onStateChanged?()
    }

    private func setTitle(forFile path: String, title: String, activityDate: Date) {
        guard let turn = activeTurns.values
            .filter({ pathsEqual($0.sourcePath, path) })
            .max(by: { $0.startSequence < $1.startSequence }) else { return }
        touch(turn, activityDate: activityDate)
        guard turn.title?.isEmpty != false else { return }
        turn.title = title
        onStateChanged?()
    }

    private func touch(_ turn: ActiveTurn, activityDate: Date) {
        if activityDate > turn.lastActivity { turn.lastActivity = activityDate }
    }

    private func touchTurns(forFile path: String, activityDate: Date) {
        for turn in activeTurns.values where pathsEqual(turn.sourcePath, path) {
            touch(turn, activityDate: activityDate)
        }
    }

    private func clearStaleTurns(now: Date) {
        let staleIDs = activeTurns.values.filter { turn in
            Self.isTurnStale(lastActivity: turn.lastActivity,
                             fileWrite: safeLastWriteDate(turn.sourcePath),
                             now: now, graceSeconds: Self.staleTurnGrace)
        }.map(\.turnID)
        guard !staleIDs.isEmpty else { return }
        for id in staleIDs { activeTurns.removeValue(forKey: id) }
        staleTurnCleanupCount += staleIDs.count
        onStateChanged?()
    }

    private func eventDate(_ root: [String: Any]) -> Date {
        guard let raw = string(root, "timestamp") else { return Date() }
        if let date = Self.iso8601Fractional.date(from: raw) { return date }
        if let date = Self.iso8601Basic.date(from: raw) { return date }
        return Date()
    }

    private static let iso8601Fractional: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    private static let iso8601Basic: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter
    }()

    private func object(_ map: [String: Any]?, _ key: String) -> [String: Any]? {
        map?[key] as? [String: Any]
    }

    private func string(_ map: [String: Any]?, _ key: String) -> String? {
        guard let value = map?[key], !(value is NSNull) else { return nil }
        if let string = value as? String { return string }
        return String(describing: value)
    }

    private func nonempty(_ value: String?) -> String? {
        guard let value else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    private func safeLastWriteDate(_ path: String) -> Date {
        (try? FileManager.default.attributesOfItem(atPath: path)[.modificationDate] as? Date) ?? .distantPast
    }

    private func normalizedPath(_ path: String) -> String {
        (path as NSString).standardizingPath.lowercased()
    }

    private func pathsEqual(_ lhs: String, _ rhs: String) -> Bool {
        normalizedPath(lhs) == normalizedPath(rhs)
    }

    private func formatDate(_ date: Date) -> String {
        guard date != .distantPast else { return "无" }
        return Self.diagnosticsDateFormatter.string(from: date)
    }

    private static let diagnosticsDateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "zh_CN")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter
    }()

    private final class TailState {
        let path: String
        var position: UInt64 = 0
        var lineData = Data()
        var lastWrite: Date
        var lastActivity: Date
        var skipCurrentLine = false

        init(path: String, lastWrite: Date) {
            self.path = path
            self.lastWrite = lastWrite
            self.lastActivity = lastWrite == .distantPast ? Date() : lastWrite
        }

        func reset() {
            position = 0
            lineData.removeAll(keepingCapacity: true)
            lastWrite = .distantPast
            lastActivity = Date()
            skipCurrentLine = false
        }
    }

    private final class ActiveTurn {
        let turnID: String
        let sourcePath: String
        let startSequence: Int64
        var title: String?
        var plan: TaskPlanProgress?
        var lastActivity = Date.distantPast

        init(turnID: String, sourcePath: String, startSequence: Int64) {
            self.turnID = turnID
            self.sourcePath = sourcePath
            self.startSequence = startSequence
        }
    }

    private struct TaskPlanProgress: Equatable {
        let totalSteps: Int
        let completedSteps: Int
        let currentStep: String?
    }
}
