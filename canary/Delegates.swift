// SPDX-License-Identifier: Apache-2.0
// The AVFoundation delegate callbacks. They run on the outputs' queues and only record what arrived, under lock; the
// stages check it.
import AVFoundation

// FNV-1a, 64-bit: the frame signature hash
let fnvOffsetBasis: UInt64 = 1469598103934665603
let fnvPrime: UInt64 = 1099511628211
// the signature samples at most this many rows of a frame, and one byte in every 4 of each sampled row
let signatureMaxRows = 256
let signatureByteStride = 4

// a frame's signature over its first plane (two frames with the same picture have the same one) and whether any sampled
// byte is nonzero
func frameSignature(_ pixelBuffer: CVPixelBuffer) -> (signature: UInt64, nonzero: Bool) {
  CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
  var signature = fnvOffsetBasis
  var nonzero = false
  let planar = CVPixelBufferIsPlanar(pixelBuffer)
  let base = planar ? CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0) : CVPixelBufferGetBaseAddress(pixelBuffer)
  let bytesPerRow = planar
    ? CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)
    : CVPixelBufferGetBytesPerRow(pixelBuffer)
  let height = planar ? CVPixelBufferGetHeightOfPlane(pixelBuffer, 0) : CVPixelBufferGetHeight(pixelBuffer)
  if let bytes = base?.assumingMemoryBound(to: UInt8.self) {
    for y in stride(from: 0, to: height, by: max(1, height / signatureMaxRows)) {
      for x in stride(from: 0, to: bytesPerRow, by: signatureByteStride) {
        let byte = bytes[y * bytesPerRow + x]
        if byte != 0 { nonzero = true }
        signature = (signature ^ UInt64(byte)) &* fnvPrime
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly)
  return (signature, nonzero)
}

extension Canary {
  // video frames (any VideoDataOutput) and audio buffers
  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    let arrival = hostNow()
    if output is AVCaptureAudioDataOutput {
      recordAudio(sampleBuffer)
      return
    }

    if gridRequested, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer),
       CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA {
      let lumas = lumaGrid(pixelBuffer)
      lock.withLock {
        grid = lumas
        gridRequested = false
      }
    }

    guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
      let empty = FrameRecord(pts: -1, arrival: arrival, width: 0, height: 0, pixelFormat: 0, signature: 0,
                              nonzero: false)
      lock.withLock { frames.append(empty) }
      return
    }

    let (signature, nonzero) = frameSignature(pixelBuffer)
    let frame = FrameRecord(pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer).seconds, arrival: arrival,
                            width: CVPixelBufferGetWidth(pixelBuffer), height: CVPixelBufferGetHeight(pixelBuffer),
                            pixelFormat: CVPixelBufferGetPixelFormatType(pixelBuffer), signature: signature,
                            nonzero: nonzero)
    lock.withLock { frames.append(frame) }
  }

  func recordAudio(_ sampleBuffer: CMSampleBuffer) {
    lock.withLock {
      audioBufferCount += 1
      audioPTS.append(CMSampleBufferGetPresentationTimeStamp(sampleBuffer).seconds)
      if let format = CMSampleBufferGetFormatDescription(sampleBuffer),
         let streamDescription = CMAudioFormatDescriptionGetStreamBasicDescription(format) {
        audioRate = streamDescription.pointee.mSampleRate
      }
    }
  }

  func captureOutput(_ output: AVCaptureOutput, didDrop sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    lock.withLock { dropped += 1 }
  }

  func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject],
                      from connection: AVCaptureConnection) {
    lock.withLock {
      for object in objects {
        qrObjects.append((object.type, (object as? AVMetadataMachineReadableCodeObject)?.stringValue, object.bounds))
      }
    }
  }

  func photoOutput(_ output: AVCapturePhotoOutput, willBeginCaptureFor settings: AVCaptureResolvedPhotoSettings) {
    lock.withLock {
      photoEvents.append("willBegin \(settings.photoDimensions.width)x\(settings.photoDimensions.height)")
    }
  }

  func photoOutput(_ output: AVCapturePhotoOutput, willCapturePhotoFor settings: AVCaptureResolvedPhotoSettings) {
    lock.withLock { photoEvents.append("willCapture") }
  }

  func photoOutput(_ output: AVCapturePhotoOutput, didCapturePhotoFor settings: AVCaptureResolvedPhotoSettings) {
    lock.withLock { photoEvents.append("didCapture") }
  }

  func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
    let data = photo.fileDataRepresentation()
    lock.withLock {
      photoEvents.append("processed")
      photos.append((data, error, photo.timestamp.seconds, photo.metadata))
    }
  }

  func photoOutput(_ output: AVCapturePhotoOutput, didFinishCaptureFor settings: AVCaptureResolvedPhotoSettings,
                   error: Error?) {
    lock.withLock {
      photoEvents.append("finished err=\(error.map { "\($0)" } ?? "nil")")
      photoDone = true
      stopsAtFinish = events.filter { $0.hasPrefix("didStop") }.count
      finishAt = Date()
    }
  }

  func fileOutput(_ output: AVCaptureFileOutput, didStartRecordingTo url: URL,
                  from connections: [AVCaptureConnection]) {
    recordEvent("movieStarted")
  }

  func fileOutput(_ output: AVCaptureFileOutput, didFinishRecordingTo url: URL,
                  from connections: [AVCaptureConnection], error: Error?) {
    var reportedError = error

    // AVFoundation can report a recording that finished normally as an error with this key set
    if let nsError = error as NSError?, (nsError.userInfo[AVErrorRecordingSuccessfullyFinishedKey] as? Bool) == true {
      reportedError = nil
    }

    lock.withLock { movieDone = (url, reportedError) }
  }
}
