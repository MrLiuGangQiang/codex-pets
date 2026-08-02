import AppKit
import CodeXPetsCore

@MainActor
extension PetWindowController {
    func draw(in bounds: CGRect) {
        guard let context = NSGraphicsContext.current else { return }
        context.cgContext.clear(bounds)
        if shouldShowThoughtBubble { drawThoughtBubble() }
        drawPet()
    }

    func drawThoughtBubble() {
        guard let context = NSGraphicsContext.current else { return }
        let cloud = bubbleRect
        context.imageInterpolation = .none
        AppResources.image(named: "cloud-bubble")?.draw(
            in: cloud, from: .zero, operation: .sourceOver,
            fraction: 1, respectFlipped: true, hints: nil)

        let dots = thoughtDotRects()
        drawThoughtDot(dots.0)
        drawThoughtDot(dots.1)

        let header: String
        if currentState == .busy {
            let progress: String?
            if taskIndex >= 0, taskIndex < taskProgressLabels.count {
                progress = taskProgressLabels[taskIndex]
            } else {
                progress = nil
            }
            header = AppLogic.formatBusyHeader(stepProgress: progress,
                                               sessionIndex: taskIndex,
                                               sessionCount: taskTitles.count)
        } else if currentState == .completed {
            header = "已完成"
        } else if currentState == .error {
            header = "异常"
        } else {
            header = statusText
        }

        let headerParagraph = NSMutableParagraphStyle()
        headerParagraph.alignment = .center
        headerParagraph.lineBreakMode = .byTruncatingTail
        let headerAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: currentState == .busy ? 11.5 : 12.5,
                                     weight: .bold),
            .foregroundColor: NSColor(calibratedRed: 0.13, green: 0.19,
                                      blue: 0.26, alpha: 1),
            .paragraphStyle: headerParagraph
        ]
        (header as NSString).draw(with: headerRect,
                                  options: [.usesLineFragmentOrigin],
                                  attributes: headerAttributes)

        if currentState != .idle { drawLightBulb(error: currentState == .error) }

        let body = normalizedDisplayText(displayedText)
        let bodyAttributes = bodyTextAttributes(includeColor: true)
        let textHeight = measuredTextHeight(body, width: contentRect.width,
                                            attributes: bodyAttributes)
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: contentRect).addClip()
        let drawRect = CGRect(x: contentRect.minX, y: contentRect.minY - scrollOffset,
                              width: contentRect.width,
                              height: max(contentRect.height, textHeight + 4))
        (body as NSString).draw(with: drawRect,
                                options: [.usesLineFragmentOrigin, .usesFontLeading],
                                attributes: bodyAttributes)
        NSGraphicsContext.restoreGraphicsState()
    }

    func drawPet() {
        guard let context = NSGraphicsContext.current else { return }
        context.imageInterpolation = .high
        if isDocked {
            let directionOffset = dockEdge == .right ? 4 : 0
            AppResources.dockImage(index: directionOffset + dockExpressionIndex)?.draw(
                in: dockPetRect, from: .zero, operation: .sourceOver,
                fraction: 1, respectFlipped: true, hints: nil)
            return
        }
        guard let image = AppResources.catImage(state: currentState,
                                                frame: floatingSpriteFrame) else { return }
        let destination = floatingPetRect
        if shouldMirrorFloatingSprite {
            context.saveGraphicsState()
            context.cgContext.translateBy(x: destination.minX + destination.maxX, y: 0)
            context.cgContext.scaleBy(x: -1, y: 1)
            image.draw(in: destination, from: .zero, operation: .sourceOver,
                       fraction: 1, respectFlipped: true, hints: nil)
            context.restoreGraphicsState()
        } else {
            image.draw(in: destination, from: .zero, operation: .sourceOver,
                       fraction: 1, respectFlipped: true, hints: nil)
        }
    }

    func drawLightBulb(error: Bool) {
        let cloud = bubbleRect
        let origin = CGPoint(x: cloud.minX + cloud.width * 0.125,
                             y: cloud.minY + cloud.height * 0.285)
        let bulb = CGRect(x: origin.x, y: origin.y, width: 24, height: 24)
        let glow = error
            ? NSColor(calibratedRed: 0.89, green: 0.24, blue: 0.22, alpha: 1)
            : NSColor(calibratedRed: 0.33, green: 0.66, blue: 0.93, alpha: 1)
        NSColor(calibratedRed: 0.27, green: 0.19, blue: 0.12, alpha: 1).setStroke()
        glow.setFill()
        let head = NSBezierPath(ovalIn: bulb.insetBy(dx: 3, dy: 3))
        head.lineWidth = 2
        head.fill()
        head.stroke()
        let base = NSBezierPath(roundedRect: CGRect(x: bulb.midX - 5,
                                                    y: bulb.maxY - 3,
                                                    width: 10, height: 6),
                                xRadius: 2, yRadius: 2)
        NSColor(calibratedWhite: 0.35, alpha: 1).setFill()
        base.fill()
    }

    func drawThoughtDot(_ rect: CGRect) {
        let outer = octagon(in: rect, notch: 3)
        NSColor(calibratedRed: 0.16, green: 0.20, blue: 0.24, alpha: 1).setFill()
        outer.fill()
        let innerRect = rect.insetBy(dx: 3, dy: 3)
        guard innerRect.width > 0, innerRect.height > 0 else { return }
        NSColor(calibratedRed: 0.95, green: 0.97, blue: 1, alpha: 1).setFill()
        octagon(in: innerRect, notch: 1).fill()
    }

    func octagon(in rect: CGRect, notch: CGFloat) -> NSBezierPath {
        let n = min(notch, min(rect.width, rect.height) / 3)
        let path = NSBezierPath()
        path.move(to: CGPoint(x: rect.minX + n, y: rect.minY))
        path.line(to: CGPoint(x: rect.maxX - n, y: rect.minY))
        path.line(to: CGPoint(x: rect.maxX, y: rect.minY + n))
        path.line(to: CGPoint(x: rect.maxX, y: rect.maxY - n))
        path.line(to: CGPoint(x: rect.maxX - n, y: rect.maxY))
        path.line(to: CGPoint(x: rect.minX + n, y: rect.maxY))
        path.line(to: CGPoint(x: rect.minX, y: rect.maxY - n))
        path.line(to: CGPoint(x: rect.minX, y: rect.minY + n))
        path.close()
        return path
    }

    func bodyTextAttributes(includeColor: Bool) -> [NSAttributedString.Key: Any] {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .left
        paragraph.lineBreakMode = .byWordWrapping
        paragraph.lineSpacing = 1
        var attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11.5, weight: .semibold),
            .paragraphStyle: paragraph
        ]
        if includeColor {
            attributes[.foregroundColor] = NSColor(calibratedRed: 0.18,
                green: 0.24, blue: 0.31, alpha: 1)
        }
        return attributes
    }

    func measuredTextHeight(_ text: String, width: CGFloat,
                            attributes: [NSAttributedString.Key: Any]) -> CGFloat {
        let bounds = (text as NSString).boundingRect(
            with: NSSize(width: max(1, width), height: 10_000),
            options: [.usesLineFragmentOrigin, .usesFontLeading],
            attributes: attributes)
        return ceil(max(1, bounds.height))
    }
}
