// Renders app-icon.png from the SF Symbol apple.terminal, in the style of the
// other Outer Shell app icons. Run on a Mac: swift Scripts/render_app_icon.swift
import AppKit

let size = 512
let glyphExtent = CGFloat(size) * 0.76
let color = NSColor(srgbRed: 0, green: 136.0 / 255.0, blue: 1, alpha: 1)
let output = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "app-icon.png")

guard let base = NSImage(systemSymbolName: "apple.terminal", accessibilityDescription: nil) else {
    fatalError("apple.terminal is not available on this system")
}
let configuration = NSImage.SymbolConfiguration(pointSize: 256, weight: .regular)
    .applying(NSImage.SymbolConfiguration(paletteColors: [color]))
guard let symbol = base.withSymbolConfiguration(configuration) else {
    fatalError("Could not configure apple.terminal")
}

let scale = glyphExtent / max(symbol.size.width, symbol.size.height)
let glyphSize = CGSize(width: symbol.size.width * scale, height: symbol.size.height * scale)
guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                                    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
    fatalError("Could not create the bitmap")
}
bitmap.size = CGSize(width: size, height: size)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
symbol.draw(in: CGRect(x: (CGFloat(size) - glyphSize.width) / 2,
                       y: (CGFloat(size) - glyphSize.height) / 2,
                       width: glyphSize.width,
                       height: glyphSize.height))
NSGraphicsContext.restoreGraphicsState()

guard let png = bitmap.converting(to: .sRGB, renderingIntent: .default)?.representation(using: .png, properties: [:]) else {
    fatalError("Could not encode the PNG")
}
try png.write(to: output)
