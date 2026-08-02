import AppKit
import CodeXPetsCore

@MainActor
enum StatusIconRenderer {
    static func image(state: ReminderState, frame: Int) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let image = NSImage(size: size, flipped: false) { rect in
            let center = NSPoint(x: rect.midX, y: rect.midY)
            switch state {
            case .idle:
                drawDot(center: center, radius: 5.2,
                        color: NSColor(calibratedRed: 0.20, green: 0.76,
                                       blue: 0.46, alpha: 1))
            case .busy:
                let phase = CGFloat(abs(frame) % 8) / 7
                let wave = (1 - cos(phase * .pi * 2)) / 2
                drawDot(center: center, radius: 4.7 + wave * 1.2,
                        color: NSColor(calibratedRed: 0.96, green: 0.70,
                                       blue: 0.18, alpha: 0.72 + wave * 0.28))
            case .completed:
                drawDot(center: center, radius: 6.2,
                        color: NSColor(calibratedRed: 0.18, green: 0.73,
                                       blue: 0.42, alpha: 1))
                let check = NSBezierPath()
                check.move(to: NSPoint(x: 5.2, y: 9.0))
                check.line(to: NSPoint(x: 8.0, y: 6.4))
                check.line(to: NSPoint(x: 13.0, y: 11.8))
                check.lineWidth = 1.8
                check.lineCapStyle = .round
                check.lineJoinStyle = .round
                NSColor.white.setStroke()
                check.stroke()
            case .error:
                drawDot(center: center, radius: 6.0,
                        color: NSColor(calibratedRed: 0.89, green: 0.25,
                                       blue: 0.24, alpha: 1))
                let cross = NSBezierPath()
                cross.move(to: NSPoint(x: 6.2, y: 6.2))
                cross.line(to: NSPoint(x: 11.8, y: 11.8))
                cross.move(to: NSPoint(x: 11.8, y: 6.2))
                cross.line(to: NSPoint(x: 6.2, y: 11.8))
                cross.lineWidth = 1.7
                cross.lineCapStyle = .round
                NSColor.white.setStroke()
                cross.stroke()
            }
            return true
        }
        image.isTemplate = false
        return image
    }

    private static func drawDot(center: NSPoint, radius: CGFloat, color: NSColor) {
        let rect = NSRect(x: center.x - radius, y: center.y - radius,
                          width: radius * 2, height: radius * 2)
        color.setFill()
        NSBezierPath(ovalIn: rect).fill()
        NSColor(calibratedWhite: 0.12, alpha: 0.22).setStroke()
        let outline = NSBezierPath(ovalIn: rect.insetBy(dx: 0.3, dy: 0.3))
        outline.lineWidth = 0.6
        outline.stroke()
    }
}
