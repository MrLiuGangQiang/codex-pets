import Foundation
import XCTest
@testable import CodeXPetsCore

final class SessionMonitorTests: XCTestCase {
    private var temporaryRoots: [URL] = []

    override func tearDown() {
        for root in temporaryRoots { try? FileManager.default.removeItem(at: root) }
        temporaryRoots.removeAll()
        super.tearDown()
    }

    func testLifecyclePlanAndFailureDetection() throws {
        let root = try makeRoot(prefix: "Lifecycle")
        let file = try makeTodayFile(root: root, name: "rollout-test.jsonl")
        try write([
            #"{"timestamp":"2026-08-01T00:00:00Z","type":"response_item","payload":{"text":"type task_started fake"}}"#,
            #"{"timestamp":"2026-08-01T00:00:01Z","type":"event_msg","payload":{"type":"task_started","turn_id":"A"}}"#,
            #"{"timestamp":"2026-08-01T00:00:01Z","type":"event_msg","payload":{"type":"user_message","message":"Hello title test"}}"#
        ], to: file)

        let monitor = CodexSessionMonitor(sessionsRoot: root.path)
        var started = 0
        var completed = 0
        var aborted = 0
        monitor.onTaskStarted = { started += 1 }
        monitor.onTaskCompleted = { completed += 1 }
        monitor.onTaskAborted = { aborted += 1 }
        XCTAssertEqual(monitor.activeCount, 1)
        XCTAssertEqual(monitor.primaryActiveTitle, "Hello title test")
        XCTAssertEqual(started, 0, "Startup scan must remain silent")

        try append(#"{"timestamp":"2026-08-01T00:00:02Z","type":"response_item","payload":{"type":"function_call","name":"update_plan","arguments":"{\"plan\":[{\"step\":\"Inspect\",\"status\":\"completed\"},{\"step\":\"Build feature\",\"status\":\"in_progress\"},{\"step\":\"Test\",\"status\":\"pending\"}]}","internal_chat_message_metadata_passthrough":{"turn_id":"A"}}}"#,
                   to: file)
        monitor.poll()
        XCTAssertEqual(monitor.totalPlanStepCount, 3)
        XCTAssertEqual(monitor.completedPlanStepCount, 1)
        XCTAssertEqual(monitor.primaryCurrentPlanStep, "Build feature")
        XCTAssertEqual(monitor.activePlanProgressLabels.first!, "1/3")

        try append(#"{"timestamp":"2026-08-01T00:00:03Z","type":"event_msg","payload":{"type":"task_complete","turn_id":"A","last_agent_message":"ok"}}"#,
                   to: file)
        monitor.poll()
        XCTAssertEqual(monitor.activeCount, 0)
        XCTAssertEqual(completed, 1)
        XCTAssertEqual(monitor.lastCompletedTitle, "Hello title test")

        try append([
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"B"}}"#,
            #"{"type":"event_msg","payload":{"type":"user_message","message":"Server failure title"}}"#,
            #"{"type":"event_msg","payload":{"type":"task_complete","turn_id":"B","last_agent_message":"Traceback (most recent call last):\nRuntimeError: upstream exploded"}}"#
        ], to: file)
        monitor.poll()
        XCTAssertEqual(monitor.activeCount, 0)
        XCTAssertEqual(aborted, 1)
        XCTAssertEqual(monitor.lastAbortedTitle, "Server failure title")
        XCTAssertTrue(CodexSessionMonitor.looksLikeFailureMessage("Internal Server Error"))
        XCTAssertFalse(CodexSessionMonitor.looksLikeFailureMessage("已修复服务端错误并完成全部测试。"))
    }

    func testPlanCarriesAcrossFollowUpTurnInSameFile() throws {
        let root = try makeRoot(prefix: "Carry")
        let file = try makeTodayFile(root: root, name: "rollout-carry.jsonl")
        try write([
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"OLD"}}"#,
            #"{"type":"response_item","payload":{"type":"function_call","name":"update_plan","arguments":"{\"plan\":[{\"step\":\"One\",\"status\":\"completed\"},{\"step\":\"Two\",\"status\":\"in_progress\"}]}","internal_chat_message_metadata_passthrough":{"turn_id":"OLD"}}}"#,
            #"{"type":"event_msg","payload":{"type":"task_complete","turn_id":"OLD"}}"#,
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"NEW"}}"#,
            #"{"type":"event_msg","payload":{"type":"user_message","message":"Follow-up"}}"#
        ], to: file)
        let monitor = CodexSessionMonitor(sessionsRoot: root.path)
        XCTAssertEqual(monitor.activeCount, 1)
        XCTAssertEqual(monitor.primaryActiveTitle, "Follow-up")
        XCTAssertEqual(monitor.activePlanProgressLabels.first!, "1/2")
    }

    func testUTF8SplitAcrossReadBoundaryAndPartialLine() throws {
        let root = try makeRoot(prefix: "UTF8")
        let file = try makeTodayFile(root: root, name: "rollout-utf8.jsonl")
        let started = #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"UTF"}}"# + "\n"
        let prefix = #"{"type":"event_msg","payload":{"type":"user_message","message":""#
        let suffix = #"猫跨块"}}"# + "\n"
        let targetOffset = 65_535
        let paddingCount = max(0, targetOffset - started.utf8.count - prefix.utf8.count)
        let data = Data((started + prefix + String(repeating: "a", count: paddingCount) + suffix).utf8)
        try data.write(to: file)

        let monitor = CodexSessionMonitor(sessionsRoot: root.path)
        XCTAssertEqual(monitor.activeCount, 1)
        XCTAssertTrue(monitor.primaryActiveTitle?.hasSuffix("猫跨块") == true)

        try appendRaw(#"{"type":"event_msg","payload":{"type":"task_started","turn_id":"PARTIAL"}"#,
                      to: file, addNewline: false)
        monitor.poll()
        XCTAssertTrue(monitor.diagnosticsText().contains("JSON 解析错误：0"))
    }

    func testLiveAppendWorksWhenModificationTimeIsFrozen() throws {
        let root = try makeRoot(prefix: "Live")
        let file = try makeTodayFile(root: root, name: "rollout-live.jsonl")
        try Data("{}\n".utf8).write(to: file)
        let frozen = Date().addingTimeInterval(-3600)
        try FileManager.default.setAttributes([.modificationDate: frozen],
                                              ofItemAtPath: file.path)
        let monitor = CodexSessionMonitor(sessionsRoot: root.path)
        try append([
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"LIVE"}}"#,
            #"{"type":"event_msg","payload":{"type":"user_message","message":"Live title"}}"#
        ], to: file)
        try FileManager.default.setAttributes([.modificationDate: frozen],
                                              ofItemAtPath: file.path)
        monitor.poll()
        XCTAssertEqual(monitor.activeCount, 1)
        XCTAssertEqual(monitor.primaryActiveTitle, "Live title")
    }

    func testRecentlyModifiedOldDateFolderIsDiscovered() throws {
        let root = try makeRoot(prefix: "Old")
        let oldFolder = root.appendingPathComponent("2020/01/01", isDirectory: true)
        try FileManager.default.createDirectory(at: oldFolder,
                                                withIntermediateDirectories: true)
        let file = oldFolder.appendingPathComponent("rollout-old.jsonl")
        try write([
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"OLD"}}"#,
            #"{"type":"event_msg","payload":{"type":"user_message","message":"Old active"}}"#
        ], to: file)
        let monitor = CodexSessionMonitor(sessionsRoot: root.path)
        XCTAssertEqual(monitor.activeCount, 1)
        XCTAssertEqual(monitor.primaryActiveTitle, "Old active")
    }

    func testStaleRequiresBothTurnAndFileToBeOld() {
        let now = Date(timeIntervalSince1970: 10_000)
        XCTAssertTrue(CodexSessionMonitor.isTurnStale(
            lastActivity: now.addingTimeInterval(-661),
            fileWrite: now.addingTimeInterval(-661), now: now, graceSeconds: 600))
        XCTAssertFalse(CodexSessionMonitor.isTurnStale(
            lastActivity: now.addingTimeInterval(-661),
            fileWrite: now.addingTimeInterval(-60), now: now, graceSeconds: 600))
    }

    private func makeRoot(prefix: String) throws -> URL {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("CodeXPetsMacTests-\(prefix)-\(UUID().uuidString)",
                                    isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        temporaryRoots.append(root)
        return root
    }

    private func makeTodayFile(root: URL, name: String) throws -> URL {
        let components = Calendar.current.dateComponents([.year, .month, .day], from: Date())
        let folder = root
            .appendingPathComponent(String(format: "%04d", components.year!), isDirectory: true)
            .appendingPathComponent(String(format: "%02d", components.month!), isDirectory: true)
            .appendingPathComponent(String(format: "%02d", components.day!), isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.appendingPathComponent(name)
    }

    private func write(_ lines: [String], to url: URL) throws {
        try Data((lines.joined(separator: "\n") + "\n").utf8).write(to: url)
    }

    private func append(_ line: String, to url: URL) throws {
        try appendRaw(line, to: url, addNewline: true)
    }

    private func append(_ lines: [String], to url: URL) throws {
        try appendRaw(lines.joined(separator: "\n"), to: url, addNewline: true)
    }

    private func appendRaw(_ text: String, to url: URL, addNewline: Bool) throws {
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        try handle.seekToEnd()
        try handle.write(contentsOf: Data((text + (addNewline ? "\n" : "")).utf8))
    }
}
