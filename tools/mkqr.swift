// SPDX-License-Identifier: Apache-2.0
// mkqr <outdir> [text] — writes <outdir>/qr.png (1280x720, a QR code of text, default M1B-QR-OK, centred on white).
import CoreImage
import AppKit
import Foundation
let out = CommandLine.arguments[1]
let text = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "M1B-QR-OK"
let W = 1280, H = 720
let f = CIFilter(name: "CIQRCodeGenerator")!
f.setValue(text.data(using: .utf8), forKey: "inputMessage"); f.setValue("M", forKey: "inputCorrectionLevel")
let qr = f.outputImage!.transformed(by: CGAffineTransform(scaleX: 20, y: 20))
let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W*4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
ctx.setFillColor(.white); ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
let cg = CIContext().createCGImage(qr, from: qr.extent)!
let s = min(CGFloat(H) - 80, qr.extent.width)
ctx.interpolationQuality = .none
ctx.draw(cg, in: CGRect(x: (CGFloat(W)-s)/2, y: (CGFloat(H)-s)/2, width: s, height: s))
let img = ctx.makeImage()!
let rep = NSBitmapImageRep(cgImage: img)
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out + "/qr.png"))
print("wrote", out + "/qr.png", text)
