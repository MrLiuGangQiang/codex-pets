import AppKit
import CodeXPetsCore

@MainActor
extension PetWindowController {
    var shouldShowThoughtBubble: Bool {
        AppLogic.shouldShowThoughtBubble(isDocked: isDocked, state: currentState,
                                         now: Date(), dockThoughtUntil: dockThoughtUntil)
    }

    var bubbleRect: CGRect {
        let y = dockBubbleBelow ? Self.windowSize.height - Self.bubbleSize.height : 0
        return CGRect(x: (Self.windowSize.width - Self.bubbleSize.width) / 2,
                      y: y, width: Self.bubbleSize.width, height: Self.bubbleSize.height)
    }

    var contentRect: CGRect {
        let cloud = bubbleRect
        let reserveBulb = currentState != .idle
        let x = cloud.minX + cloud.width * (reserveBulb ? 0.30 : 0.22)
        let y = cloud.minY + cloud.height * 0.31
        let maxRight = cloud.minX + cloud.width * 0.80
        let maxBottom = cloud.minY + cloud.height * 0.76
        return CGRect(x: x, y: y, width: max(1, min(205, maxRight - x)),
                      height: max(1, min(52, maxBottom - y)))
    }

    var headerRect: CGRect {
        let cloud = bubbleRect
        return CGRect(x: cloud.minX + cloud.width * 0.20,
                      y: cloud.minY + cloud.height * 0.14,
                      width: cloud.width * 0.60, height: cloud.height * 0.20)
    }

    var floatingPetRect: CGRect {
        CGRect(x: (Self.windowSize.width - Self.petSize.width) / 2,
               y: Self.windowSize.height - Self.petSize.height,
               width: Self.petSize.width, height: Self.petSize.height)
    }

    var dockPetRect: CGRect {
        let smooth = smoothStep(dockVisibility)
        let hiddenOffset = Self.dockSize * (1 - smooth)
        let x = dockEdge == .left ? -hiddenOffset : Self.windowSize.width - Self.dockSize + hiddenOffset
        let y: CGFloat = dockBubbleBelow ? 0 : Self.windowSize.height - Self.dockSize
        return CGRect(x: x, y: y, width: Self.dockSize, height: Self.dockSize)
    }

    var visiblePetRect: CGRect { isDocked ? dockPetRect : floatingPetRect }

    func thoughtDotRects() -> (CGRect, CGRect) {
        let cloud = bubbleRect
        let pet = visiblePetRect
        let smallSize = CGSize(width: 11, height: 10)
        let largeSize = CGSize(width: 17, height: 15)
        let petCenterX = pet.midX
        let cloudCenterX = cloud.midX
        let smallCenterX = isDocked ? petCenterX : cloudCenterX - 11
        let largeCenterX = isDocked ? (petCenterX * 2 + cloudCenterX) / 3 : cloudCenterX - 7
        let smallY: CGFloat
        let largeY: CGFloat
        if dockBubbleBelow {
            smallY = pet.maxY + 6
            largeY = min(cloud.minY - largeSize.height - 5,
                         smallY + smallSize.height + 5)
        } else {
            smallY = max(cloud.maxY + 10,
                         min(cloud.maxY + 30, pet.minY - smallSize.height - 6))
            largeY = max(cloud.maxY - 6, smallY - largeSize.height - 5)
        }
        return (
            CGRect(x: largeCenterX - largeSize.width / 2, y: largeY,
                   width: largeSize.width, height: largeSize.height),
            CGRect(x: smallCenterX - smallSize.width / 2, y: smallY,
                   width: smallSize.width, height: smallSize.height)
        )
    }

    var floatingSpriteFrame: Int {
        if currentState == .busy {
            let phase = abs(animationTick) % 64
            if phase < 40 { return (phase / 2) % 4 }
            if phase < 44 { return 4 }
            if phase < 60 { return 5 + ((phase - 44) / 4) % 2 }
            return 7
        }
        switch currentState {
        case .completed: return (animationTick / 3) % 4
        case .idle, .error: return (animationTick / 3) % 8
        case .busy: return 0
        }
    }

    var dockExpressionIndex: Int {
        let phase = abs(animationTick) % 20
        let blink = phase == 11 || phase == 14
        if currentState == .completed { return blink ? 1 : 2 }
        if currentState == .error { return blink ? 1 : 3 }
        return blink ? 1 : 0
    }

    var shouldMirrorFloatingSprite: Bool {
        guard let window else { return false }
        let anchor = CGPoint(x: window.frame.midX, y: window.frame.minY)
        let workArea = screen(containing: anchor)?.visibleFrame ??
            NSScreen.main?.visibleFrame ?? .zero
        return AppLogic.shouldMirrorFloatingSprite(anchor: anchor, workArea: workArea)
    }

    func isTaskSwitchPoint(_ point: CGPoint) -> Bool {
        AppLogic.isTaskSwitchPoint(isDocked: isDocked,
                                   bubbleVisible: shouldShowThoughtBubble,
                                   state: currentState, taskCount: taskTitles.count,
                                   bubbleBounds: bubbleRect, contentBounds: contentRect,
                                   point: point)
    }

    func isInteractivePoint(_ point: CGPoint) -> Bool {
        if visiblePetRect.insetBy(dx: -3, dy: -3).contains(point) { return true }
        guard shouldShowThoughtBubble else { return false }
        if bubbleRect.contains(point) { return true }
        let dots = thoughtDotRects()
        return dots.0.contains(point) || dots.1.contains(point)
    }

    func smoothStep(_ value: CGFloat) -> CGFloat {
        let x = min(1, max(0, value))
        return x * x * (3 - 2 * x)
    }
}
