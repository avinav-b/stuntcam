// SPDX-License-Identifier: Apache-2.0
// M5APP_MODE=qr: the QR payloads two plain-AVFoundation scanners report, for scripts/qr-test.sh. Opens the back (or
// front) camera with an AVCaptureMetadataOutput for .qr (the engine's barcode metadata) and an
// AVCaptureVideoDataOutput whose frames go to Vision's VNDetectBarcodesRequest (every 5th frame).
// Env: QR_POS back|front (back), QR_SECS (10), QR_W / QR_H (the format; default: the session's preset).
// Prints `QRSEEN src=<meta|vision> t=<ms since the session started> at=<Unix ms> pct=<payload, percent-encoded like
// JavaScript's encodeURIComponent>` each time a scanner's payload differs from its last one, `INFO qr.frame WxH` once,
// then DONE.
import UIKit
import AVFoundation
import Vision

// encodeURIComponent's unreserved set: what scripts/qr-test.sh decodes with urllib.parse.unquote
let qrUnreserved = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.!~*'()")

final class QRWatch: NSObject, AVCaptureMetadataOutputObjectsDelegate, AVCaptureVideoDataOutputSampleBufferDelegate {
  let lock = NSLock(); var last: [String: String] = [:], t0 = Date(), frames = 0
  func report(_ src: String, _ v: String?) {
    guard let v else { return }
    lock.lock(); let isNew = last[src] != v; if isNew { last[src] = v }; lock.unlock()
    if isNew {
      P("QRSEEN src=\(src) t=\(Int(Date().timeIntervalSince(t0) * 1000)) at=\(Int(Date().timeIntervalSince1970 * 1000)) pct=\(v.addingPercentEncoding(withAllowedCharacters: qrUnreserved) ?? "UNENCODABLE")")
    }
  }
  func metadataOutput(_ o: AVCaptureMetadataOutput, didOutput objs: [AVMetadataObject], from c: AVCaptureConnection) {
    for m in objs where m.type == .qr { report("meta", (m as? AVMetadataMachineReadableCodeObject)?.stringValue) }
  }
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
    frames += 1
    if frames == 1 { P("INFO qr.frame \(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb))") }
    if frames % 5 != 0 { return }
    let r = VNDetectBarcodesRequest(); r.symbologies = [.qr]
    try? VNImageRequestHandler(cvPixelBuffer: pb, options: [:]).perform([r])
    for o in r.results ?? [] where o.symbology == .qr { report("vision", o.payloadStringValue) }
  }
}
var qrKeep: QRWatch?

extension VC {
  func qr() {
    let env = ProcessInfo.processInfo.environment
    let front = env["QR_POS"] == "front", secs = Double(env["QR_SECS"] ?? "10") ?? 10
    guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: front ? .front : .back) else { R(false, "qr.device"); done(); return }
    let m = QRWatch(); qrKeep = m
    let s = session
    s.beginConfiguration()
    guard let inp = try? AVCaptureDeviceInput(device: dev), s.canAddInput(inp) else { R(false, "qr.input"); done(); return }
    s.addInput(inp)
    let meta = AVCaptureMetadataOutput()
    if s.canAddOutput(meta) { s.addOutput(meta) } else { R(false, "qr.metadataOutput") }
    meta.setMetadataObjectsDelegate(m, queue: DispatchQueue(label: "qr.meta"))
    let vdo = AVCaptureVideoDataOutput(); vdo.alwaysDiscardsLateVideoFrames = true
    vdo.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    vdo.setSampleBufferDelegate(m, queue: DispatchQueue(label: "qr.vdo"))
    if s.canAddOutput(vdo) { s.addOutput(vdo) } else { R(false, "qr.videoDataOutput") }
    s.commitConfiguration()
    if meta.availableMetadataObjectTypes.contains(.qr) { meta.metadataObjectTypes = [.qr] } else { R(false, "qr.metadataTypes", "\(meta.availableMetadataObjectTypes)") }
    if let w = env["QR_W"].flatMap(Int32.init), let h = env["QR_H"].flatMap(Int32.init) {
      if let f = dev.formats.first(where: { let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription); return d.width == w && d.height == h }) {
        do { try dev.lockForConfiguration(); dev.activeFormat = f; dev.unlockForConfiguration() } catch { R(false, "qr.format", "\(error)") }
      } else { R(false, "qr.format", "no \(w)x\(h)") }
    }
    P("INFO qr.begin pos=\(front ? "front" : "back") secs=\(secs) format=\(fmtDesc(dev.activeFormat))")
    DispatchQueue.global().async {
      m.t0 = Date()
      s.startRunning()
      DispatchQueue.global().asyncAfter(deadline: .now() + secs) {
        m.lock.lock(); let seen = m.last; m.lock.unlock()
        P("INFO qr.end seen=\(seen.keys.sorted()) frames=\(m.frames)")
        s.stopRunning(); self.done()
      }
    }
  }
}
