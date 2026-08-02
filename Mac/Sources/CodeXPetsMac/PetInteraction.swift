import AppKit
import CodeXPetsCore

@MainActor
extension PetWindowController {
    func handleMouseDown(_ event: NSEvent) {
        guard event.type == .leftMouseDown, let window else { return }
        let point = petView.convert(event.locationInWindow, from: nil)
        if isTaskSwitchPoint(point) {
            switchFromCloudClick()
            return
        }
        guard isInteractivePoint(point) else { return }
        dragStartedDocked = isDocked
        if dragStartedDocked {
            dockVisibility = 1
            petView.needsDisplay = true
        }
        dragPending = true
        dragging = false
        dragStartMouse = NSEvent.mouseLocation
        dragStartOrigin = window.frame.origin
    }

    func handleMouseDragged(_ event: NSEvent) {
        guard dragPending || dragging, let window else { return }
        var cursor = NSEvent.mouseLocation
        var dx = cursor.x - dragStartMouse.x
        var dy = cursor.y - dragStartMouse.y
        if !dragging, abs(dx) + abs(dy) < 4 { return }
        if !dragging, isDocked {
            undockForDrag(cursor: cursor)
            cursor = NSEvent.mouseLocation
            dragStartMouse = cursor
            dragStartOrigin = window.frame.origin
            dx = 0
            dy = 0
        }
        dragging = true
        window.setFrameOrigin(NSPoint(x: dragStartOrigin.x + dx,
                                      y: dragStartOrigin.y + dy))
        petView.needsDisplay = true
    }

    func handleMouseUp(_ event: NSEvent) {
        guard dragPending || dragging else { return }
        let moved = dragging
        let startedDocked = dragStartedDocked
        dragPending = false
        dragging = false
        dragStartedDocked = false
        if !moved {
            if startedDocked && isDocked {
                dockLastContentChange = Date()
                dockVisibility = 1
                petView.needsDisplay = true
            }
            return
        }
        let cursor = NSEvent.mouseLocation
        if !trySnapToEdge(cursor: cursor) { clampToWorkingArea() }
        saveCurrentPosition()
    }

    func showContextMenu(with event: NSEvent) {
        NSMenu.popUpContextMenu(contextMenu, with: event, for: petView)
    }

    func advanceDockAnimation(elapsed: TimeInterval) -> Bool {
        guard isDocked else { return false }
        let now = Date()
        let hovering = isDockHovering(globalPoint: NSEvent.mouseLocation)
        if hovering {
            dockHoverRevealUntil = now.addingTimeInterval(Double(settings.dockRevealSeconds))
        }
        let shouldBeVisible = AppLogic.shouldShowDock(
            lastContentChange: dockLastContentChange, now: now,
            isDragging: dragPending || dragging, isHovering: hovering,
            hoverRevealUntil: dockHoverRevealUntil,
            idleHideSeconds: settings.dockIdleHideSeconds)
        let target: CGFloat = shouldBeVisible ? 1 : 0
        guard abs(target - dockVisibility) > 0.001 else { return false }
        let duration: CGFloat = target > dockVisibility ? 0.30 : 0.55
        let previous = dockVisibility
        let delta = CGFloat(max(0.001, min(0.1, elapsed))) / duration
        dockVisibility = target > dockVisibility
            ? min(target, dockVisibility + delta)
            : max(target, dockVisibility - delta)
        return abs(previous - dockVisibility) > 0.001
    }

    func isDockHovering(globalPoint: CGPoint) -> Bool {
        guard isDocked, let screen = dockScreen else { return false }
        let hover = AppLogic.dockHoverBounds(edge: dockEdge,
                                             workArea: screen.visibleFrame,
                                             dockY: dockY, scale: 1,
                                             fullyHidden: dockVisibility <= 0.01,
                                             hoverHeight: settings.dockHoverHeight)
        if hover.contains(globalPoint) { return true }
        if screenRect(forViewRect: visiblePetRect).contains(globalPoint) { return true }
        return shouldShowThoughtBubble &&
            screenRect(forViewRect: bubbleRect).contains(globalPoint)
    }

    func updateMousePassThrough() {
        guard let window, window.isVisible else { return }
        if dragPending || dragging {
            window.ignoresMouseEvents = false
            return
        }
        let global = NSEvent.mouseLocation
        if isDocked, isDockHovering(globalPoint: global) {
            window.ignoresMouseEvents = false
            return
        }
        let windowPoint = window.convertPoint(fromScreen: global)
        let viewPoint = petView.convert(windowPoint, from: nil)
        window.ignoresMouseEvents = !isInteractivePoint(viewPoint)
    }

    func screenRect(forViewRect rect: CGRect) -> CGRect {
        guard let window else { return .zero }
        let windowRect = petView.convert(rect, to: nil)
        return window.convertToScreen(windowRect)
    }

    func advanceScroll(elapsed: TimeInterval) -> Bool {
        let text = normalizedDisplayText(displayedText)
        if text != lastDisplayedText {
            resetScroll()
            return true
        }
        let attributes = bodyTextAttributes(includeColor: false)
        let maxOffset = max(0, measuredTextHeight(text, width: contentRect.width,
                                                  attributes: attributes) - contentRect.height)
        if maxOffset <= 0 {
            let changed = scrollOffset != 0
            scrollOffset = 0
            scrollCycle += elapsed
            if scrollCycle >= Self.shortTaskDisplay {
                moveToNextTask()
                return true
            }
            return changed
        }
        if scrollHold > 0 {
            scrollHold = max(0, scrollHold - elapsed)
            return false
        }
        if !scrollAtEnd {
            let previous = scrollOffset
            scrollOffset = min(maxOffset,
                               scrollOffset + Self.scrollSpeed * CGFloat(elapsed))
            if scrollOffset >= maxOffset {
                scrollAtEnd = true
                scrollHold = Self.scrollEndHold
            }
            return abs(previous - scrollOffset) > 0.01
        }
        if scrollHold > 0 {
            scrollHold = max(0, scrollHold - elapsed)
            return false
        }
        moveToNextTask()
        return true
    }
}
