// SPDX-License-Identifier: Apache-2.0
// The front camera on a portrait VideoDataOutput connection: turning mirroring on must flip the picture horizontally.
import AVFoundation

// the brightness grid is lumaGridSide x lumaGridSide cells, each one pixel sampled at the cell's centre
let lumaGridSide = 32
// mirrored vs unmirrored: the flipped grid must differ by less than this fraction of the unflipped difference
let mirrorFlipMaxRatio: Float = 0.5

// the front session's frame window, the pause after mirroring is turned on, and how long to wait for a grid
let frontWindow = FrameWindow(seconds: 0.4, minFrames: 6)
let mirrorSettleS = 0.25
let gridWaitS = 1.5
// the connection's rotation for a portrait buffer
let portraitRotationDegrees: CGFloat = 90

// the summed B+G+R value of each grid cell's centre pixel of a 32-bit BGRA frame, row by row
func lumaGrid(_ pixelBuffer: CVPixelBuffer) -> [Float] {
  CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
  defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
  let width = CVPixelBufferGetWidth(pixelBuffer)
  let height = CVPixelBufferGetHeight(pixelBuffer)
  let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
  guard let bytes = CVPixelBufferGetBaseAddress(pixelBuffer)?.assumingMemoryBound(to: UInt8.self) else { return [] }
  let side = lumaGridSide
  var lumas = [Float](repeating: 0, count: side * side)
  for cellRow in 0..<side {
    for cellColumn in 0..<side {
      let x = cellColumn * width / side + width / (2 * side)
      let y = cellRow * height / side + height / (2 * side)
      let offset = y * bytesPerRow + x * 4
      lumas[cellRow * side + cellColumn] = Float(bytes[offset]) + Float(bytes[offset + 1]) + Float(bytes[offset + 2])
    }
  }
  return lumas
}

// How far the mirrored grid is from the unmirrored one, as a sum of cell differences.
// `same` compares the grids as they are, and `flip` compares with the unmirrored grid flipped horizontally.
func mirrorDistances(_ unmirrored: [Float], _ mirrored: [Float]) -> (same: Float, flip: Float) {
  let side = lumaGridSide
  var same: Float = 0
  var flip: Float = 0
  for y in 0..<side {
    for x in 0..<side {
      same += abs(mirrored[y * side + x] - unmirrored[y * side + x])
      flip += abs(mirrored[y * side + x] - unmirrored[y * side + side - 1 - x])
    }
  }
  return (same, flip)
}

extension Canary {
  func frontMirror() {
    verdict.stage = "front-mirror"
    guard let front = startFrontSession() else { return }
    let (session, connection) = front
    firstFrame("front")
    checkFrameWindow("front", frontWindow)
    let last = snapshotFrames().last
    check(last.map { $0.height > $0.width } ?? false,
          "front portrait buffer \(last.map { "\($0.width)x\($0.height)" } ?? "-")")
    let unmirrored = takeGrid()
    session.beginConfiguration()
    connection.isVideoMirrored = true
    session.commitConfiguration()
    pause(mirrorSettleS)
    let mirrored = takeGrid()
    session.stopRunning()
    let cells = lumaGridSide * lumaGridSide
    guard let unmirroredGrid = unmirrored, let mirroredGrid = mirrored, unmirroredGrid.count == cells,
          mirroredGrid.count == cells else {
      check(false, "front grids captured")
      return
    }

    let distances = mirrorDistances(unmirroredGrid, mirroredGrid)
    check(distances.flip < distances.same * mirrorFlipMaxRatio,
          "mirrored frame = horizontal flip of unmirrored"
            + " (diff flip \(Int(distances.flip)) vs same \(Int(distances.same)))")
  }

  // a running session on the front camera: a 32-bit BGRA VideoDataOutput rotated to portrait, mirroring off. Nil (after
  // a FAIL) when there is no front camera, input or connection.
  func startFrontSession() -> (AVCaptureSession, AVCaptureConnection)? {
    guard let frontCamera = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) else {
      check(false, "front camera present")
      return nil
    }

    let session = AVCaptureSession()
    session.beginConfiguration()
    guard let input = try? AVCaptureDeviceInput(device: frontCamera), session.canAddInput(input) else {
      check(false, "front input")
      return nil
    }

    session.addInput(input)
    let videoOutput = AVCaptureVideoDataOutput()
    videoOutput.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    videoOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.front"))
    session.addOutput(videoOutput)
    guard let connection = videoOutput.connection(with: .video) else {
      check(false, "front vdo connection")
      return nil
    }

    if connection.isVideoRotationAngleSupported(portraitRotationDegrees) {
      connection.videoRotationAngle = portraitRotationDegrees
    }

    check(connection.isVideoMirroringSupported, "front connection supports mirroring")
    connection.automaticallyAdjustsVideoMirroring = false
    connection.isVideoMirrored = false
    session.commitConfiguration()
    session.startRunning()
    return (session, connection)
  }

  // the brightness grid of the next 32-bit BGRA frame (Delegates.swift), or nil if none came within gridWaitS
  func takeGrid() -> [Float]? {
    lock.withLock {
      grid = nil
      gridRequested = true
    }

    _ = waitFor(gridWaitS) { lock.withLock { grid != nil } }  // on a timeout grid stays nil
    return lock.withLock { grid }
  }
}
