// SPDX-License-Identifier: Apache-2.0
// avaudio <movie>: per-track first-sample PTS + timeRange; audio codec/rate/channels, peak/RMS dBFS; A/V start offset.
import AVFoundation
import Foundation
let url = URL(fileURLWithPath: CommandLine.arguments[1])
let a = AVURLAsset(url: url)
let sem = DispatchSemaphore(value: 0)
func fourcc(_ v: FourCharCode) -> String { String(format: "%c%c%c%c", (v>>24)&255, (v>>16)&255, (v>>8)&255, v&255) }
Task {
  do {
    let ts = try await a.load(.tracks)
    var first: [AVMediaType: Double] = [:]
    var out = "  avaudio:"
    for t in ts {
      let tr = try await t.load(.timeRange)
      let fds = try await t.load(.formatDescriptions)
      let r = try AVAssetReader(asset: a)
      var settings: [String: Any]? = nil
      if t.mediaType == .audio { settings = [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMBitDepthKey: 32, AVLinearPCMIsFloatKey: true, AVLinearPCMIsNonInterleaved: false, AVLinearPCMIsBigEndianKey: false] }
      let o = AVAssetReaderTrackOutput(track: t, outputSettings: settings)
      r.add(o); r.startReading()
      var pts0 = Double.nan, ptsLast = 0.0, peak: Float = 0, sumsq = 0.0, n = 0, count = 0
      while let sb = o.copyNextSampleBuffer() {
        let p = CMSampleBufferGetPresentationTimeStamp(sb).seconds
        if pts0.isNaN { pts0 = p }; ptsLast = p + CMSampleBufferGetDuration(sb).seconds; count += 1
        if t.mediaType == .audio, let bb = CMSampleBufferGetDataBuffer(sb) {
          var len = 0; var ptr: UnsafeMutablePointer<Int8>? = nil
          CMBlockBufferGetDataPointer(bb, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr)
          if let ptr { ptr.withMemoryRebound(to: Float.self, capacity: len / 4) { f in for i in 0..<(len / 4) { let v = abs(f[i]); if v > peak { peak = v }; sumsq += Double(v * v); n += 1 } } }
        }
      }
      first[t.mediaType] = pts0
      var desc = ""
      if let fd = fds.first {
        desc = fourcc(CMFormatDescriptionGetMediaSubType(fd))
        if t.mediaType == .audio, let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd)?.pointee { desc += " \(Int(asbd.mSampleRate))Hz ch=\(asbd.mChannelsPerFrame)" }
      }
      out += "\n   [\(t.mediaType.rawValue) \(desc) timeRange.start=\(String(format: "%.3f", tr.start.seconds)) dur=\(String(format: "%.3f", tr.duration.seconds)) firstPTS=\(String(format: "%.3f", pts0)) endPTS=\(String(format: "%.3f", ptsLast)) samples=\(count)"
      if t.mediaType == .audio { out += String(format: " peak=%.1fdBFS rms=%.1fdBFS", 20 * log10(Double(max(peak, 1e-9))), 10 * log10(max(sumsq / Double(max(n, 1)), 1e-18))) }
      out += "]"
    }
    if let v = first[.video], let au = first[.audio] { out += String(format: "\n   A/V start offset (audio firstPTS - video firstPTS) = %+.1f ms", (au - v) * 1000) }
    else { out += "\n   no audio track" }
    print(out)
  } catch { print("  avaudio ERROR \(error)") }
  sem.signal()
}
sem.wait()
