// SPDX-License-Identifier: Apache-2.0
// mkqr <outdir> [text] — writes <outdir>/qr.png: a 1280x720 white image with a QR code of text (default M1B-QR-OK) in
// the centre.
import CoreImage
import AppKit
import Foundation

func die(_ message: String) -> Never {
  FileHandle.standardError.write(Data("mkqr: \(message)\n".utf8))
  exit(1)
}
guard CommandLine.arguments.count > 1 else {
  FileHandle.standardError.write(Data("usage: mkqr <outdir> [text]\n".utf8))
  exit(2)
}

let out = CommandLine.arguments[1]
let text = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "M1B-QR-OK"
let path = out + "/qr.png"
let width = 1280
let height = 720
let bytesPerPixel = 4
// each QR module becomes moduleScale x moduleScale pixels before the code is fitted
let moduleScale: CGFloat = 20
// the code is at most height - margin tall
let margin: CGFloat = 80

guard let filter = CIFilter(name: "CIQRCodeGenerator") else { die("no CIQRCodeGenerator filter") }
filter.setValue(text.data(using: .utf8), forKey: "inputMessage")
filter.setValue("M", forKey: "inputCorrectionLevel")   // error correction level M (15%)
guard let code = filter.outputImage else { die("CIQRCodeGenerator made no image for \(text)") }
let scaledCode = code.transformed(by: CGAffineTransform(scaleX: moduleScale, y: moduleScale))
let bitmapInfo = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                              bytesPerRow: width * bytesPerPixel, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: bitmapInfo) else {
  die("cannot create a \(width)x\(height) bitmap context")
}
context.setFillColor(.white)
context.fill(CGRect(x: 0, y: 0, width: width, height: height))
guard let codeImage = CIContext().createCGImage(scaledCode, from: scaledCode.extent) else {
  die("cannot render the QR code")
}

let side = min(CGFloat(height) - margin, scaledCode.extent.width)
context.interpolationQuality = .none   // keep the modules' edges sharp
context.draw(codeImage, in: CGRect(x: (CGFloat(width) - side) / 2, y: (CGFloat(height) - side) / 2, width: side,
                                   height: side))
guard let bitmap = context.makeImage() else { die("cannot read back the bitmap") }
guard let png = NSBitmapImageRep(cgImage: bitmap).representation(using: .png, properties: [:]) else {
  die("cannot encode PNG")
}
do {
  try png.write(to: URL(fileURLWithPath: path))
} catch {
  die("cannot write \(path): \(error.localizedDescription)")
}
print("wrote", path, text)
