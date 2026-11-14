// SPDX-License-Identifier: Apache-2.0
// Makes fixtures/qr.png (1280x720, QR centred on white) and fixtures/frame.bin
// (SimulatorCamera SCFrameStore format: 64-byte header + BGRA, hostTime=0 = never stale).
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
// frame.bin
var hdr = Data(count: 64)
func put32(_ v: UInt32, _ o: Int) { withUnsafeBytes(of: v.littleEndian) { hdr.replaceSubrange(o..<o+4, with: $0) } }
put32(0x31464353, 0); put32(1, 4); put32(UInt32(W), 8); put32(UInt32(H), 12); put32(UInt32(W*4), 16); put32(0, 20)
withUnsafeBytes(of: UInt64(2).littleEndian) { hdr.replaceSubrange(24..<32, with: $0) }   // seq even
let px = Data(bytes: ctx.data!, count: W*H*4)
var file = hdr + px
file.append(Data(count: 64 + 1920*1080*4 - file.count))   // full SC_FRAME_FILE_SIZE so mmap is in-bounds
try! file.write(to: URL(fileURLWithPath: out + "/frame.bin"))
print("wrote", out + "/qr.png", out + "/frame.bin", text)
