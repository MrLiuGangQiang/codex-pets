import AppKit
import CodeXPetsCore

@MainActor
extension PetWindowController {
    func placeAtDefaultLocation() {
        guard let window, let work = NSScreen.main?.visibleFrame else { return }
        window.setFrameOrigin(NSPoint(x: work.maxX - Self.windowSize.width - 24,
                                      y: work.minY + 24))
    }

    @discardableResult
    func restoreSavedPosition() -> Bool {
        guard let state = AppSettings.loadPetPosition() else { return false }
        let targetScreen = screen(identifier: state.screenIdentifier) ?? NSScreen.main
        guard let targetScreen else { return false }
        let work = targetScreen.visibleFrame
        if state.dockEdge != .none {
            dockEdge = state.dockEdge
            dockScreenIdentifier = screenIdentifier(targetScreen)
            dockY = work.minY + CGFloat(state.relativeY) * work.height
            dockVisibility = 1
            dockLastContentChange = Date()
            positionDockedWindow()
            return true
        }
        let anchorX = work.minX + CGFloat(state.relativeX) * work.width
        let anchorY = work.minY + CGFloat(state.relativeY) * work.height
        window?.setFrameOrigin(NSPoint(x: anchorX - Self.windowSize.width / 2,
                                       y: anchorY))
        clampToWorkingArea()
        return true
    }

    func saveCurrentPosition() {
        guard let window else { return }
        let targetScreen = dockScreen ?? screen(containing: CGPoint(x: window.frame.midX,
                                                                     y: window.frame.midY)) ?? NSScreen.main
        guard let targetScreen else { return }
        let work = targetScreen.visibleFrame
        if isDocked {
            let relativeY = work.height <= 0 ? 0.5 :
                Double((dockY - work.minY) / work.height)
            AppSettings.savePetPosition(PetPositionState(
                dockEdge: dockEdge,
                screenIdentifier: screenIdentifier(targetScreen),
                relativeX: dockEdge == .left ? 0 : 1,
                relativeY: relativeY))
        } else {
            let relativeX = work.width <= 0 ? 0.5 :
                Double((window.frame.midX - work.minX) / work.width)
            let relativeY = work.height <= 0 ? 0.5 :
                Double((window.frame.minY - work.minY) / work.height)
            AppSettings.savePetPosition(PetPositionState(
                dockEdge: .none,
                screenIdentifier: screenIdentifier(targetScreen),
                relativeX: relativeX,
                relativeY: relativeY))
        }
    }

    func clampToWorkingArea() {
        guard let window else { return }
        if isDocked {
            positionDockedWindow()
            return
        }
        let work = screen(containing: CGPoint(x: window.frame.midX,
                                              y: window.frame.midY))?.visibleFrame
            ?? NSScreen.main?.visibleFrame ?? .zero
        let x = min(max(work.minX, window.frame.minX), work.maxX - window.frame.width)
        let y = min(max(work.minY, window.frame.minY), work.maxY - window.frame.height)
        window.setFrameOrigin(NSPoint(x: x, y: y))
    }

    func positionDockedWindow() {
        guard isDocked, let window, let targetScreen = dockScreen ?? NSScreen.main else { return }
        dockScreenIdentifier = screenIdentifier(targetScreen)
        let work = targetScreen.visibleFrame
        dockBubbleBelow = false
        var originY = dockY - Self.dockSize / 2
        if originY + Self.windowSize.height > work.maxY {
            dockBubbleBelow = true
            originY = dockY - Self.windowSize.height + Self.dockSize / 2
        }
        let originX = dockEdge == .left ? work.minX : work.maxX - Self.windowSize.width
        originY = min(max(work.minY, originY), work.maxY - Self.windowSize.height)
        window.setFrameOrigin(NSPoint(x: originX, y: originY))
        petView.needsDisplay = true
    }

    @discardableResult
    func trySnapToEdge(cursor: CGPoint) -> Bool {
        guard let targetScreen = screen(containing: cursor) else { return false }
        let edge = AppLogic.selectSnapEdge(cursor: cursor,
                                           workArea: targetScreen.visibleFrame,
                                           snapDistance: 36)
        guard edge != .none else { return false }
        dockEdge = edge
        dockScreenIdentifier = screenIdentifier(targetScreen)
        dockY = cursor.y
        dockVisibility = 1
        dockLastContentChange = Date()
        dockHoverRevealUntil = .distantPast
        if currentState != .idle {
            dockThoughtUntil = Date().addingTimeInterval(Double(
                AppLogic.cloudNotificationSeconds(
                    state: currentState,
                    configuredSeconds: settings.dockNotificationSeconds)))
        } else {
            dockThoughtUntil = .distantPast
        }
        positionDockedWindow()
        resetScroll()
        petView.needsDisplay = true
        return true
    }

    func undockForDrag(cursor: CGPoint) {
        guard isDocked, let window else { return }
        dockEdge = .none
        dockScreenIdentifier = ""
        dockBubbleBelow = false
        dockVisibility = 1
        dockThoughtUntil = .distantPast
        dockHoverRevealUntil = .distantPast
        window.setFrameOrigin(NSPoint(x: cursor.x - Self.windowSize.width / 2,
                                      y: cursor.y - Self.petSize.height / 2))
        petView.needsDisplay = true
    }

    var dockScreen: NSScreen? { screen(identifier: dockScreenIdentifier) }

    func screen(identifier: String) -> NSScreen? {
        guard !identifier.isEmpty else { return nil }
        return NSScreen.screens.first { screenIdentifier($0) == identifier }
    }

    func screen(containing point: CGPoint) -> NSScreen? {
        NSScreen.screens.first { $0.frame.contains(point) } ?? NSScreen.main
    }

    func screenIdentifier(_ screen: NSScreen) -> String {
        let key = NSDeviceDescriptionKey("NSScreenNumber")
        if let number = screen.deviceDescription[key] as? NSNumber {
            return number.stringValue
        }
        return screen.localizedName
    }
}
