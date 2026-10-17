// SoulX Android Emulator - draws SoulX's app icon into an .iconset folder (for iconutil).
//
// Usage: swift scripts/make_icon.swift OUT.iconset
//
// The drawing matches SoulXAppIcon() in source/platform/macos/window/app.m (the icon SoulX's window shows in the
// Dock); change both together.
import AppKit

func drawIcon(_ rect: NSRect) {
    let body = rect.insetBy(dx: rect.width * 0.09, dy: rect.height * 0.09)
    let radius = body.width * 0.225
    let shape = NSBezierPath(roundedRect: body, xRadius: radius, yRadius: radius)
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor(white: 0, alpha: 0.35)
    shadow.shadowOffset = NSSize(width: 0, height: -rect.height * 0.012)
    shadow.shadowBlurRadius = rect.width * 0.03
    shadow.set()
    NSColor.black.setFill()
    shape.fill()
    NSGraphicsContext.restoreGraphicsState()
    let gradient = NSGradient(colors: [
        NSColor(srgbRed: 0.42, green: 0.30, blue: 0.98, alpha: 1),
        NSColor(srgbRed: 0.13, green: 0.62, blue: 0.98, alpha: 1),
        NSColor(srgbRed: 0.18, green: 0.86, blue: 0.74, alpha: 1),
    ])!
    gradient.draw(in: shape, angle: -60)
    var font = NSFont.systemFont(ofSize: body.height * 0.62, weight: .heavy)
    if let rounded = font.fontDescriptor.withDesign(.rounded), let f = NSFont(descriptor: rounded, size: font.pointSize) {
        font = f
    }
    let glow = NSShadow()
    glow.shadowColor = NSColor(white: 0, alpha: 0.25)
    glow.shadowOffset = NSSize(width: 0, height: -rect.height * 0.008)
    glow.shadowBlurRadius = rect.width * 0.02
    let s = NSAttributedString(string: "S", attributes: [.font: font, .foregroundColor: NSColor.white, .shadow: glow])
    let ts = s.size()
    s.draw(at: NSPoint(x: body.midX - ts.width / 2, y: body.midY - ts.height / 2))
}

func png(pixels: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    drawIcon(NSRect(x: 0, y: 0, width: pixels, height: pixels))
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

guard CommandLine.arguments.count == 2 else {
    FileHandle.standardError.write("usage: swift make_icon.swift OUT.iconset\n".data(using: .utf8)!)
    exit(2)
}
let out = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    try png(pixels: size).write(to: out.appendingPathComponent("icon_\(size)x\(size).png"))
    try png(pixels: size * 2).write(to: out.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
