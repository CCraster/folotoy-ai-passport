import AppKit

let folder = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let size = base * scale
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
            bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        let t = NSAffineTransform(); t.scale(by: CGFloat(size) / 1024); t.concat()
        NSColor(srgbRed: 0.04, green: 0.055, blue: 0.055, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: 32, y: 32, width: 960, height: 960), xRadius: 210, yRadius: 210).fill()
        drawPassportMark(in: NSRect(x: 134, y: 134, width: 756, height: 756), color: accent)
        NSGraphicsContext.restoreGraphicsState()
        let name = "icon_\(base)x\(base)\(scale == 2 ? "@2x" : "").png"
        try bitmap.representation(using: .png, properties: [:])!.write(to: folder.appendingPathComponent(name))
    }
}
