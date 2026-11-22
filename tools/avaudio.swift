// SPDX-License-Identifier: Apache-2.0
// avaudio <movie> — for each track: the first sample's presentation time and the track's time range; for audio, the
// codec, sample rate and channels, and the peak and RMS levels in dBFS; then the offset between the audio and video
// starts.
import AVFoundation
import Foundation

guard CommandLine.arguments.count > 1 else {
  FileHandle.standardError.write(Data("usage: avaudio <movie>\n".utf8))
  exit(2)
}

let url = URL(fileURLWithPath: CommandLine.arguments[1])
let asset = AVURLAsset(url: url)
let done = DispatchSemaphore(value: 0)
// the smallest peak and mean square that are converted to dBFS, so that silence prints a finite level
let peakFloor: Float = 1e-9
let meanSquareFloor = 1e-18

// a four-character code as text ("aac ", "avc1")
func fourCC(_ code: FourCharCode) -> String {
  String(format: "%c%c%c%c", (code >> 24) & 255, (code >> 16) & 255, (code >> 8) & 255, code & 255)
}

// seconds with 3 decimals
func secs3(_ seconds: Double) -> String { String(format: "%.3f", seconds) }

// what reading one track's samples found
struct TrackScan {
  var firstPTS = Double.nan
  var endPTS = 0.0
  var samples = 0
  var peak: Float = 0      // the largest absolute audio sample value (1.0 = full scale)
  var sumSquares = 0.0     // over every audio sample value
  var values = 0           // audio sample values seen
}

// Reads every sample buffer of the track: the start and end times and the count, and for audio (read as 32-bit float
// PCM) the peak and the sum of squares.
func scan(_ track: AVAssetTrack) throws -> TrackScan {
  let reader = try AVAssetReader(asset: asset)
  var settings: [String: Any]? = nil
  if track.mediaType == .audio {
    settings = [
      AVFormatIDKey: kAudioFormatLinearPCM,
      AVLinearPCMBitDepthKey: 32,
      AVLinearPCMIsFloatKey: true,
      AVLinearPCMIsNonInterleaved: false,
      AVLinearPCMIsBigEndianKey: false,
    ]
  }

  let output = AVAssetReaderTrackOutput(track: track, outputSettings: settings)
  reader.add(output)
  reader.startReading()
  var result = TrackScan()
  while let sampleBuffer = output.copyNextSampleBuffer() {
    let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer).seconds
    if result.firstPTS.isNaN {
      result.firstPTS = pts
    }

    result.endPTS = pts + CMSampleBufferGetDuration(sampleBuffer).seconds
    result.samples += 1
    if track.mediaType == .audio, let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) {
      addLevels(blockBuffer, to: &result)
    }
  }
  return result
}

// adds a block of 32-bit float samples to the peak and the sum of squares
func addLevels(_ blockBuffer: CMBlockBuffer, to result: inout TrackScan) {
  var length = 0
  var bytes: UnsafeMutablePointer<Int8>? = nil
  CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length,
                              dataPointerOut: &bytes)
  guard let bytes else { return }
  let count = length / MemoryLayout<Float>.size
  bytes.withMemoryRebound(to: Float.self, capacity: count) { samples in
    for index in 0..<count {
      let level = abs(samples[index])
      if level > result.peak {
        result.peak = level
      }

      result.sumSquares += Double(level * level)
      result.values += 1
    }
  }
}

// the track's codec, and for audio its sample rate and channel count
func formatText(_ track: AVAssetTrack, _ formats: [CMFormatDescription]) -> String {
  guard let format = formats.first else { return "" }
  var text = fourCC(CMFormatDescriptionGetMediaSubType(format))
  if track.mediaType == .audio, let stream = CMAudioFormatDescriptionGetStreamBasicDescription(format)?.pointee {
    text += " \(Int(stream.mSampleRate))Hz ch=\(stream.mChannelsPerFrame)"
  }
  return text
}

Task {
  do {
    let tracks = try await asset.load(.tracks)
    var firstPTS: [AVMediaType: Double] = [:]
    var out = "  avaudio:"
    for track in tracks {
      let timeRange = try await track.load(.timeRange)
      let formats = try await track.load(.formatDescriptions)
      let result = try scan(track)
      firstPTS[track.mediaType] = result.firstPTS
      out += "\n   [\(track.mediaType.rawValue) \(formatText(track, formats))"
        + " timeRange.start=\(secs3(timeRange.start.seconds)) dur=\(secs3(timeRange.duration.seconds))"
        + " firstPTS=\(secs3(result.firstPTS)) endPTS=\(secs3(result.endPTS)) samples=\(result.samples)"
      if track.mediaType == .audio {
        let peakDB = 20 * log10(Double(max(result.peak, peakFloor)))
        let rmsDB = 10 * log10(max(result.sumSquares / Double(max(result.values, 1)), meanSquareFloor))
        out += String(format: " peak=%.1fdBFS rms=%.1fdBFS", peakDB, rmsDB)
      }

      out += "]"
    }

    if let videoStart = firstPTS[.video], let audioStart = firstPTS[.audio] {
      out += String(format: "\n   A/V start offset (audio firstPTS - video firstPTS) = %+.1f ms",
                    (audioStart - videoStart) * 1000)
    } else {
      out += "\n   no audio track"
    }

    print(out)
  } catch {
    print("  avaudio ERROR \(error)")
  }
  done.signal()
}
done.wait()
