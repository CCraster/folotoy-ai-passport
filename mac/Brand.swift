import AppKit

let accent = NSColor(srgbRed: 0.196, green: 0.941, blue: 0.549, alpha: 1)

// 三处都使用同一个字标路径，状态栏交给系统着色。
func drawPassportMark(in rect: NSRect, color: NSColor) {
    NSGraphicsContext.saveGraphicsState()
    let transform = NSAffineTransform()
    transform.translateX(by: rect.minX, yBy: rect.minY)
    transform.scaleX(by: rect.width / 24, yBy: rect.height / 24)
    transform.concat()
    color.setStroke()
    color.setFill()
    let center = NSPoint(x: 12, y: 12)
    let ring = NSBezierPath()
    ring.lineWidth = 1.7
    ring.lineCapStyle = .round
    ring.appendArc(withCenter: center, radius: 9.35, startAngle: 20, endAngle: 153, clockwise: false)
    ring.move(to: NSPoint(x: 3.36, y: 8.42))
    ring.appendArc(withCenter: center, radius: 9.35, startAngle: 202.5, endAngle: 333, clockwise: false)
    ring.stroke()
    NSBezierPath(ovalIn: NSRect(x: 19.45, y: 13.95, width: 2.6, height: 2.6)).fill()
    NSBezierPath(ovalIn: NSRect(x: 1.95, y: 7.45, width: 2.6, height: 2.6)).fill()

    let letter = NSBezierPath()
    letter.lineWidth = 2.15
    letter.lineCapStyle = .round
    letter.lineJoinStyle = .round
    letter.move(to: NSPoint(x: 9.2, y: 7.1))
    letter.line(to: NSPoint(x: 9.2, y: 16.9))
    letter.line(to: NSPoint(x: 13, y: 16.9))
    letter.curve(to: NSPoint(x: 13, y: 11.4), controlPoint1: NSPoint(x: 17.25, y: 16.9), controlPoint2: NSPoint(x: 17.25, y: 11.4))
    letter.line(to: NSPoint(x: 9.2, y: 11.4))
    letter.stroke()
    NSGraphicsContext.restoreGraphicsState()
}

func passportMenuSymbol() -> NSImage {
    let image = NSImage(size: NSSize(width: 20, height: 20), flipped: false) { rect in
        drawPassportMark(in: rect, color: .black)
        return true
    }
    image.accessibilityDescription = "Passport Sync"
    image.isTemplate = true
    return image
}

final class QuotaMeter: NSView {
    var value: Double = 0 { didSet { needsDisplay = true } }
    override func draw(_ dirtyRect: NSRect) {
        NSColor(white: 0.20, alpha: 1).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 3, yRadius: 3).fill()
        guard value > 0 else { return }
        accent.setFill()
        NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: bounds.width * min(value, 100) / 100, height: bounds.height), xRadius: 3, yRadius: 3).fill()
    }
}
