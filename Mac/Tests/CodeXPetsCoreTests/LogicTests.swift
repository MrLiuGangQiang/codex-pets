import CoreGraphics
import Foundation
import XCTest
@testable import CodeXPetsCore

final class LogicTests: XCTestCase {
    func testSettingsDefaultsAndNormalization() {
        var settings = AppSettings()
        XCTAssertEqual(settings.dockHoverHeight, 240)
        settings.dockHoverHeight = 5
        settings.dockIdleHideSeconds = -1
        settings.dockRevealSeconds = 0
        settings.dockNotificationSeconds = 500
        settings.normalize()
        XCTAssertEqual(settings.dockHoverHeight, 40)
        XCTAssertEqual(settings.dockIdleHideSeconds, 0)
        XCTAssertEqual(settings.dockRevealSeconds, 1)
        XCTAssertEqual(settings.dockNotificationSeconds, 120)
    }

    func testLatestStateAndOneShotTaskFocus() {
        XCTAssertEqual(AppLogic.selectVisualState(activeCount: 1,
            abnormalRecently: true, completedRecently: false,
            latestChangedState: .error), .error)
        XCTAssertEqual(AppLogic.selectVisualState(activeCount: 1,
            abnormalRecently: false, completedRecently: false,
            latestChangedState: .error), .busy)
        XCTAssertEqual(AppLogic.selectPreferredTaskIndex(
            focusLatestTask: true, latestTaskIndex: 2), 2)
        XCTAssertEqual(AppLogic.selectPreferredTaskIndex(
            focusLatestTask: false, latestTaskIndex: 2), -1)
    }

    func testDockedCloudUsesWholeBubbleAsTaskSelector() {
        let cloud = CGRect(x: 10, y: 20, width: 300, height: 150)
        let content = CGRect(x: 100, y: 65, width: 150, height: 60)
        let edgePoint = CGPoint(x: 15, y: 25)
        XCTAssertTrue(AppLogic.isTaskSwitchPoint(isDocked: true,
            bubbleVisible: true, state: .busy, taskCount: 2,
            bubbleBounds: cloud, contentBounds: content, point: edgePoint))
        XCTAssertFalse(AppLogic.isTaskSwitchPoint(isDocked: false,
            bubbleVisible: true, state: .busy, taskCount: 2,
            bubbleBounds: cloud, contentBounds: content, point: edgePoint))
        XCTAssertFalse(AppLogic.isTaskSwitchPoint(isDocked: true,
            bubbleVisible: false, state: .busy, taskCount: 2,
            bubbleBounds: cloud, contentBounds: content, point: edgePoint))
        XCTAssertFalse(AppLogic.isTaskSwitchPoint(isDocked: true,
            bubbleVisible: true, state: .busy, taskCount: 1,
            bubbleBounds: cloud, contentBounds: content, point: edgePoint))
    }

    func testDockingGeometryAndVisibility() {
        let work = CGRect(x: 100, y: 50, width: 1200, height: 800)
        XCTAssertEqual(AppLogic.selectSnapEdge(cursor: CGPoint(x: 112, y: 400),
                                              workArea: work, snapDistance: 24), .left)
        XCTAssertEqual(AppLogic.selectSnapEdge(cursor: CGPoint(x: 1285, y: 400),
                                              workArea: work, snapDistance: 24), .right)
        XCTAssertEqual(AppLogic.selectSnapEdge(cursor: CGPoint(x: 700, y: 400),
                                              workArea: work, snapDistance: 24), .none)
        let bounds = AppLogic.dockHoverBounds(edge: .left, workArea: work,
            dockY: 400, scale: 1, fullyHidden: true, hoverHeight: 240)
        XCTAssertEqual(bounds.height, 240, accuracy: 0.01)
        XCTAssertTrue(bounds.contains(CGPoint(x: 101, y: 510)))
        XCTAssertFalse(bounds.contains(CGPoint(x: 101, y: 530)))

        let start = Date(timeIntervalSince1970: 1_000)
        XCTAssertTrue(AppLogic.shouldKeepDockVisible(lastContentChange: start,
            now: start.addingTimeInterval(9.9), idleHideSeconds: 10))
        XCTAssertFalse(AppLogic.shouldKeepDockVisible(lastContentChange: start,
            now: start.addingTimeInterval(10.1), idleHideSeconds: 10))
        XCTAssertTrue(AppLogic.shouldKeepDockVisible(lastContentChange: start,
            now: start.addingTimeInterval(3_600), idleHideSeconds: 0))
    }

    func testCloudNotificationAndHeaders() {
        XCTAssertEqual(AppLogic.cloudNotificationSeconds(state: .error,
                                                          configuredSeconds: 1), 10)
        XCTAssertEqual(AppLogic.cloudNotificationSeconds(state: .busy,
                                                          configuredSeconds: 5), 5)
        XCTAssertEqual(AppLogic.formatBusyHeader(stepProgress: "1/2",
                                                  sessionIndex: 0,
                                                  sessionCount: 2),
                       "进行中(1/2)·会话(1/2)")
        XCTAssertEqual(AppLogic.formatBusyMetadata(stepProgress: nil,
                                                    sessionIndex: 1,
                                                    sessionCount: 2),
                       "会话(2/2)")
        XCTAssertEqual(AppLogic.formatAbnormalTaskText("构建安装包"),
                       "任务失败：构建安装包")
    }

    func testPositionClampsInvalidRatios() throws {
        let state = PetPositionState(dockEdge: .right, screenIdentifier: "1",
                                     relativeX: 8, relativeY: -3)
        XCTAssertEqual(state.relativeX, 1)
        XCTAssertEqual(state.relativeY, 0)
        let decoded = try JSONDecoder().decode(PetPositionState.self,
            from: JSONEncoder().encode(state))
        XCTAssertEqual(decoded, state)
    }
}
