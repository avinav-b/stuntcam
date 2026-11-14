// SPDX-License-Identifier: Apache-2.0
// features-probe (w4 e22): which camera features does AVFoundation say the camera supports, and what happens when an app
// turns them on. Measurement only: it logs, it doesn't check. Every line is
//   PROBE| <CFAbsoluteTime, 3 dp> <s since launch> <text>
// (CFAbsoluteTime is the engine log's clock, engine/src/simcam.m LOG).
// Launch arguments (README.md):
//   -mode read                    read-out only: every camera DiscoverySession returns, its formats, and the photo / movie /
//                                 video-data outputs' `is…Supported` & co. in two session configurations
//   -mode feature -feature <f>    the read-out of one camera, then turn feature <f> on the way Apple's AVCam does and take
//                                 5 photos (photo features) or one 3 s movie (movie features)
//   -camera <label>               the camera for -mode feature (default back-wide); labels as the read-out prints them
// Objective-C exceptions are caught (SCTry.m) and logged as EXC, so one feature doesn't stop the run.
// Read lines:  R <camera> <object> <key> <value>       (object: device, class, photo, photo-run, movie, input, vdo, multicam)
//              FMT <camera> <i> <format> | <keys that read YES>      FMTCOUNT <camera> <key> <n>/<total>
// Feature lines: SET <what> ok|EXC …, CB <i> <callback> …, PHOTO <i> …, MOVIE …, HANG …, RESULT feature=<f> …, END
import UIKit
import AVFoundation
import ImageIO

let T0 = ProcessInfo.processInfo.systemUptime
let logLock = NSLock()
setvbuf(stdout, nil, _IOLBF, 0)
func log(_ s: String) {
  logLock.lock(); defer { logLock.unlock() }
  print(String(format: "PROBE| %.3f %7.3f ", CFAbsoluteTimeGetCurrent(), ProcessInfo.processInfo.systemUptime - T0) + s); fflush(stdout)
}
func arg(_ n: String) -> String? { let a = CommandLine.arguments; if let i = a.firstIndex(of: n), i + 1 < a.count { return a[i + 1] }; return nil }
let mode = arg("-mode") ?? "read"
let featureArg = arg("-feature") ?? "baseline"
let cameraArg = arg("-camera") ?? "back-wide"
let noCatch = arg("-nocatch") == "1"   // capturePhoto without SCTry: an exception it raises is uncaught, as in an app
let movieSeconds = Double(arg("-seconds") ?? "") ?? 3   // w4 e23b: movie length for movie features (e22: 3 s)

// ---------- formatting ----------
func fourcc(_ v: UInt32) -> String {
  let b = [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]
  if b.allSatisfy({ $0 >= 32 && $0 < 127 }) { return String(b.map { Character(UnicodeScalar(UInt8($0))) }) }
  return "\(v)"
}
let colorSpaceNames = [0: "sRGB", 1: "P3_D65", 2: "HLG_BT2020", 3: "appleLog", 4: "appleLog2"]
func fmtV(_ v: Any?, key: String = "") -> String {
  guard let v = v else { return "nil" }
  if let n = v as? NSNumber {
    if CFGetTypeID(n) == CFBooleanGetTypeID() { return n.boolValue ? "YES" : "NO" }
    if key.lowercased().contains("pixelformat") { return fourcc(n.uint32Value) }
    if key.lowercased().contains("colorspace") { return colorSpaceNames[n.intValue] ?? n.stringValue }
    if key == "maxPhotoQualityPrioritization" { return [1: "speed", 2: "balanced", 3: "quality"][n.intValue] ?? n.stringValue }
    return n.stringValue
  }
  if let a = v as? [Any] { return "[" + a.map { fmtV($0, key: key) }.joined(separator: ",") + "]" }
  if let s = v as? String { return s.replacingOccurrences(of: " ", with: "_") }
  if let f = v as? AVCaptureDevice.Format { return fdesc(f) }
  if let nv = v as? NSValue, key == "maxPhotoDimensions" { var d = CMVideoDimensions(width: 0, height: 0); nv.getValue(&d, size: MemoryLayout<CMVideoDimensions>.size); return "\(d.width)x\(d.height)" }
  if let r = v as? AVFrameRateRange { return "\(r.minFrameRate)-\(r.maxFrameRate)" }
  return "\(v)".replacingOccurrences(of: "\n", with: " ")
}
func fdesc(_ f: AVCaptureDevice.Format) -> String {
  let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
  let pf = fourcc(CMFormatDescriptionGetMediaSubType(f.formatDescription))
  let fps = f.videoSupportedFrameRateRanges.map { "\($0.minFrameRate)-\($0.maxFrameRate)" }.joined(separator: ",")
  return "\(d.width)x\(d.height)/\(pf)/fps[\(fps)]"
}
/// KVC read of a getter that may not exist on this OS: nil = absent (doesn't respond), else the value or EXC(...)
func kv(_ o: NSObject, _ k: String) -> String? {
  guard o.responds(to: Selector(k)) else { return nil }
  var v: Any?
  if let e = SCTry({ v = o.value(forKey: k) }) { return "EXC(\(e.replacingOccurrences(of: " ", with: "_")))" }
  return fmtV(v, key: k)
}
func readKeys(_ cam: String, _ obj: String, _ o: NSObject, _ keys: [String]) -> [String: String] {
  var r: [String: String] = [:]
  for k in keys { let v = kv(o, k) ?? "absent"; r[k] = v; log("R \(cam) \(obj) \(k) \(v)") }
  return r
}
let photoKeys = ["isLivePhotoCaptureSupported", "isLivePhotoCaptureEnabled", "isLivePhotoAutoTrimmingEnabled", "availableLivePhotoVideoCodecTypes",
  "isDepthDataDeliverySupported", "isPortraitEffectsMatteDeliverySupported", "availableSemanticSegmentationMatteTypes",
  "isAppleProRAWSupported", "availableRawPhotoPixelFormatTypes", "availableRawPhotoFileTypes",
  "isResponsiveCaptureSupported", "isZeroShutterLagSupported", "isZeroShutterLagEnabled", "isFastCapturePrioritizationSupported",
  "isAutoDeferredPhotoDeliverySupported", "isConstantColorSupported", "maxPhotoQualityPrioritization",
  "availablePhotoCodecTypes", "availablePhotoPixelFormatTypes", "availablePhotoFileTypes", "supportedFlashModes",
  "isCameraCalibrationDataDeliverySupported", "isDualCameraDualPhotoDeliverySupported", "isVirtualDeviceConstituentPhotoDeliverySupported",
  "isContentAwareDistortionCorrectionSupported", "isShutterSoundSuppressionSupported", "isStillImageStabilizationSupported",
  "isLensStabilizationDuringBracketedCaptureSupported", "maxBracketedCapturePhotoCount", "isCameraSensorOrientationCompensationSupported",
  "isHighResolutionCaptureEnabled", "maxPhotoDimensions"]
let formatKeys = ["isVideoHDRSupported", "isMultiCamSupported", "isCenterStageSupported", "isPortraitEffectSupported", "isStudioLightSupported",
  "reactionEffectsSupported", "isSpatialVideoCaptureSupported", "isCinematicVideoCaptureSupported", "isHighPhotoQualitySupported",
  "isHighestPhotoQualitySupported", "isBackgroundReplacementSupported", "isAutoVideoFrameRateSupported", "isGlobalToneMappingSupported",
  "isVideoBinned", "isSmartStyleRenderingSupported", "isCameraLensSmudgeDetectionSupported", "isEdgeLightSupported", "isPortraitEffectsMatteStillImageDeliverySupported"]
let deviceKeys = ["isVirtualDevice", "hasFlash", "hasTorch", "isVideoHDREnabled", "automaticallyAdjustsVideoHDREnabled", "activeColorSpace",
  "activeDepthDataFormat", "isLowLightBoostSupported", "isGeometricDistortionCorrectionSupported", "isCenterStageActive", "isPortraitEffectActive",
  "isStudioLightActive", "canPerformReactionEffects", "isBackgroundReplacementActive", "isAutoVideoFrameRateEnabled", "isSmartStyleRenderingEnabled",
  "minAvailableVideoZoomFactor", "maxAvailableVideoZoomFactor", "virtualDeviceSwitchOverVideoZoomFactors", "activeFormat"]
let classKeys = ["isCenterStageEnabled", "centerStageControlMode", "isPortraitEffectEnabled", "isStudioLightEnabled", "reactionEffectsEnabled",
  "reactionEffectGesturesEnabled", "isBackgroundReplacementEnabled", "preferredMicrophoneMode", "activeMicrophoneMode"]
let movieKeys = ["availableVideoCodecTypes", "isSpatialVideoCaptureSupported", "isSpatialVideoCaptureEnabled", "isCinematicVideoCaptureSupported",
  "isPrimaryConstituentDeviceSwitchingBehaviorForRecordingEnabled"]
let inputKeys = ["isCinematicVideoCaptureSupported", "isCinematicVideoCaptureEnabled", "isWindNoiseRemovalSupported", "isMultichannelAudioModeSupported"]
let vdoKeys = ["availableVideoCVPixelFormatTypes", "availableVideoCodecTypes", "deliversPreviewSizedOutputBuffers", "preparesCellularRadioForNetworkConnection"]

// ---------- cameras ----------
let allTypes: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInDualCamera,
  .builtInDualWideCamera, .builtInTripleCamera, .builtInTrueDepthCamera, .builtInLiDARDepthCamera]
func shortType(_ t: AVCaptureDevice.DeviceType) -> String {
  switch t {
  case .builtInWideAngleCamera: return "wide"; case .builtInUltraWideCamera: return "ultrawide"; case .builtInTelephotoCamera: return "tele"
  case .builtInDualCamera: return "dual"; case .builtInDualWideCamera: return "dualwide"; case .builtInTripleCamera: return "triple"
  case .builtInTrueDepthCamera: return "truedepth"; case .builtInLiDARDepthCamera: return "lidar"; default: return t.rawValue
  }
}
func label(_ d: AVCaptureDevice) -> String { (d.position == .back ? "back" : d.position == .front ? "front" : "unspec") + "-" + shortType(d.deviceType) }
func cameras() -> [AVCaptureDevice] { AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).devices }

func sleepS(_ s: Double) { Thread.sleep(forTimeInterval: s) }

final class FrameCounter: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  let lock = NSLock(); var n = 0; var drops = 0; var lastDims = ""
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    lock.lock(); n += 1
    if let pb = CMSampleBufferGetImageBuffer(sb) { lastDims = "\(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb))/\(fourcc(CVPixelBufferGetPixelFormatType(pb)))" }
    lock.unlock()
  }
  func captureOutput(_ o: AVCaptureOutput, didDrop sb: CMSampleBuffer, from c: AVCaptureConnection) { lock.lock(); drops += 1; lock.unlock() }
  func take() -> (Int, Int, String) { lock.lock(); defer { n = 0; drops = 0; lock.unlock() }; return (n, drops, lastDims) }
}
let frameQ = DispatchQueue(label: "probe.frames")

// ---------- read-out ----------
func readFormats(_ cam: String, _ dev: AVCaptureDevice) {
  var counts: [String: Int] = [:]
  let extra = ["colorSpace:HLG_BT2020", "colorSpace:appleLog", "colorSpace:appleLog2", "depthDataFormats>0", "maxFps>30", "maxFps>60"]
  for (i, f) in dev.formats.enumerated() {
    var yes: [String] = []
    for k in formatKeys { if kv(f, k) == "YES" { yes.append(k) } }
    let cs = f.supportedColorSpaces.map { $0.rawValue }
    if cs.contains(2) { yes.append("colorSpace:HLG_BT2020") }; if cs.contains(3) { yes.append("colorSpace:appleLog") }; if cs.contains(4) { yes.append("colorSpace:appleLog2") }
    if !f.supportedDepthDataFormats.isEmpty { yes.append("depthDataFormats>0") }
    let mx = f.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? 0
    if mx > 30 { yes.append("maxFps>30") }; if mx > 60 { yes.append("maxFps>60") }
    for y in yes { counts[y, default: 0] += 1 }
    let present = formatKeys.filter { f.responds(to: Selector($0)) }.count
    log("FMT \(cam) \(i) \(fdesc(f)) cs=\(cs.map { colorSpaceNames[$0] ?? "\($0)" }.joined(separator: ",")) maxPhoto=\(f.supportedMaxPhotoDimensions.map { "\($0.width)x\($0.height)" }.joined(separator: ",")) keysPresent=\(present)/\(formatKeys.count) | \(yes.joined(separator: ","))")
  }
  for k in formatKeys + extra {
    let present = dev.formats.first.map { $0.responds(to: Selector(k)) } ?? false
    log("FMTCOUNT \(cam) \(k) \(counts[k] ?? 0)/\(dev.formats.count)\(k.contains(":") || k.contains(">") || present ? "" : " absent")")
  }
}
func readCamera(_ dev: AVCaptureDevice) {
  let cam = label(dev)
  log("CAMERA \(cam) name=\(dev.localizedName.replacingOccurrences(of: " ", with: "_")) type=\(dev.deviceType.rawValue) id=\(dev.uniqueID) formats=\(dev.formats.count) constituents=\(dev.constituentDevices.map { shortType($0.deviceType) }.joined(separator: ","))")
  _ = readKeys(cam, "device", dev, deviceKeys)
  readFormats(cam, dev)
  // configuration 1: photo preset + photo output + video data output (AVCam's photo mode)
  let s = AVCaptureSession(); let fc = FrameCounter()
  s.beginConfiguration()
  if s.canSetSessionPreset(.photo) { s.sessionPreset = .photo }
  var inp: AVCaptureDeviceInput?
  do { inp = try AVCaptureDeviceInput(device: dev) } catch { log("R \(cam) session1 input-error \(error)") }
  if let inp, s.canAddInput(inp) { s.addInput(inp) } else { log("R \(cam) session1 canAddInput NO") }
  let photo = AVCapturePhotoOutput(); if s.canAddOutput(photo) { s.addOutput(photo) } else { log("R \(cam) session1 canAddOutput(photo) NO") }
  let vdo = AVCaptureVideoDataOutput(); vdo.setSampleBufferDelegate(fc, queue: frameQ); if s.canAddOutput(vdo) { s.addOutput(vdo) } else { log("R \(cam) session1 canAddOutput(vdo) NO") }
  s.commitConfiguration()
  log("R \(cam) session1 activeFormat \(fdesc(dev.activeFormat)) preset=\(s.sessionPreset.rawValue)")
  let before = readKeys(cam, "photo", photo, photoKeys)
  _ = readKeys(cam, "vdo", vdo, vdoKeys)
  if let inp { _ = readKeys(cam, "input", inp, inputKeys) }
  s.startRunning(); sleepS(1.0)
  let (n, _, dims) = fc.take()
  log("R \(cam) session1 running=\(s.isRunning ? 1 : 0) frames1s=\(n) \(dims)")
  for k in photoKeys { let v = kv(photo, k) ?? "absent"; if v != before[k] { log("R \(cam) photo-run \(k) \(v)") } }
  s.stopRunning()
  // configuration 2: movie file output + video data output
  let s2 = AVCaptureSession()
  s2.beginConfiguration()
  if s2.canSetSessionPreset(.high) { s2.sessionPreset = .high }
  if let i2 = try? AVCaptureDeviceInput(device: dev), s2.canAddInput(i2) { s2.addInput(i2) } else { log("R \(cam) session2 canAddInput NO") }
  let movie = AVCaptureMovieFileOutput(); if s2.canAddOutput(movie) { s2.addOutput(movie) } else { log("R \(cam) session2 canAddOutput(movie) NO") }
  let vdo2 = AVCaptureVideoDataOutput(); log("R \(cam) session2 canAddOutput(vdo) \(s2.canAddOutput(vdo2) ? "YES" : "NO")"); if s2.canAddOutput(vdo2) { s2.addOutput(vdo2) }
  s2.commitConfiguration()
  log("R \(cam) session2 activeFormat \(fdesc(dev.activeFormat)) preset=\(s2.sessionPreset.rawValue)")
  _ = readKeys(cam, "movie", movie, movieKeys)
}
func readAll() {
  log("pid=\(getpid()) mode=\(mode) feature=\(featureArg) camera=\(cameraArg) os=\(UIDevice.current.systemVersion) model=\(UIDevice.current.model)")
  log("AUTH video=\(AVCaptureDevice.authorizationStatus(for: .video).rawValue)")
  _ = readKeys("-", "class", unsafeBitCast(AVCaptureDevice.self as AnyObject, to: NSObject.self), classKeys)
  log("R - multicam AVCaptureMultiCamSession.isMultiCamSupported \(AVCaptureMultiCamSession.isMultiCamSupported ? "YES" : "NO")")
  if #available(iOS 17.0, *) { log("R - multicam supportedMultiCamDeviceSets \(AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).supportedMultiCamDeviceSets.map { $0.map(label).sorted().joined(separator: "+") })") }
  log("R - default video device \(AVCaptureDevice.default(for: .video).map(label) ?? "nil")")
  let cams = cameras()
  log("CAMERAS \(cams.count) \(cams.map(label).joined(separator: " "))")
  if cams.isEmpty {
    // no camera: what a bare output says
    _ = readKeys("none", "photo", AVCapturePhotoOutput(), photoKeys)
    _ = readKeys("none", "movie", AVCaptureMovieFileOutput(), movieKeys)
    _ = readKeys("none", "vdo", AVCaptureVideoDataOutput(), vdoKeys)
  }
  for d in cams where mode == "read" || label(d) == cameraArg { readCamera(d) }
}

// ---------- photo features ----------
final class PhotoDelegate: NSObject, AVCapturePhotoCaptureDelegate {
  var i = 0; var done = DispatchSemaphore(value: 0); var cbs: [String] = []; var errors: [String] = []; var photos = 0; var liveMovies = 0
  let lock = NSLock()
  func cb(_ s: String, _ e: Error?) {
    lock.lock(); cbs.append(s); if let e { errors.append("\(s):\((e as NSError).domain):\((e as NSError).code)") }; lock.unlock()
    log("CB \(i) \(s)" + (e.map { " error=\(($0 as NSError).domain) \(($0 as NSError).code) \"\($0.localizedDescription)\" \(($0 as NSError).userInfo)" } ?? ""))
  }
  func photoOutput(_ o: AVCapturePhotoOutput, willBeginCaptureFor r: AVCaptureResolvedPhotoSettings) {
    var x = "photo=\(r.photoDimensions.width)x\(r.photoDimensions.height) raw=\(r.rawPhotoDimensions.width)x\(r.rawPhotoDimensions.height) live=\(r.livePhotoMovieDimensions.width)x\(r.livePhotoMovieDimensions.height) matte=\(r.portraitEffectsMatteDimensions.width)x\(r.portraitEffectsMatteDimensions.height) expected=\(r.expectedPhotoCount) flash=\(r.isFlashEnabled ? 1 : 0)"
    if #available(iOS 17.0, *) { x += " deferredProxy=\(r.deferredPhotoProxyDimensions.width)x\(r.deferredPhotoProxyDimensions.height) fastPrio=\(r.isFastCapturePrioritizationEnabled ? 1 : 0)" }
    cb("willBeginCapture " + x, nil)
  }
  func photoOutput(_ o: AVCapturePhotoOutput, willCapturePhotoFor r: AVCaptureResolvedPhotoSettings) { cb("willCapturePhoto", nil) }
  func photoOutput(_ o: AVCapturePhotoOutput, didCapturePhotoFor r: AVCaptureResolvedPhotoSettings) { cb("didCapturePhoto", nil) }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) {
    lock.lock(); photos += 1; lock.unlock()
    var x = "raw=\(p.isRawPhoto ? 1 : 0)"
    if let d = p.fileDataRepresentation() {
      x += " bytes=\(d.count)"
      if let src = CGImageSourceCreateWithData(d as CFData, nil), let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] {
        x += " type=\(CGImageSourceGetType(src).map { $0 as String } ?? "?") \(props[kCGImagePropertyPixelWidth] ?? "?")x\(props[kCGImagePropertyPixelHeight] ?? "?") orient=\(props[kCGImagePropertyOrientation] ?? "?") images=\(CGImageSourceGetCount(src)) exifFlash=\((props[kCGImagePropertyExifDictionary] as? [CFString: Any])?[kCGImagePropertyExifFlash] ?? "-") exifEV=\((props[kCGImagePropertyExifDictionary] as? [CFString: Any])?[kCGImagePropertyExifExposureBiasValue] ?? "-")"
        // auxiliary data inside the file
        for (n, t) in [("depth", kCGImageAuxiliaryDataTypeDepth), ("disparity", kCGImageAuxiliaryDataTypeDisparity), ("matte", kCGImageAuxiliaryDataTypePortraitEffectsMatte)] {
          if CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, 0, t) != nil { x += " fileAux:\(n)" }
        }
      }
    } else { x += " fileData=nil" }
    if let dd = p.depthData { x += " depthData=\(CVPixelBufferGetWidth(dd.depthDataMap))x\(CVPixelBufferGetHeight(dd.depthDataMap))/\(fourcc(dd.depthDataType))" } else { x += " depthData=nil" }
    x += " matte=\(p.portraitEffectsMatte != nil ? 1 : 0)"
    for t in o.enabledSemanticSegmentationMatteTypes { x += " seg:\(t.rawValue)=\(p.semanticSegmentationMatte(for: t) != nil ? 1 : 0)" }
    x += " calib=\(p.cameraCalibrationData != nil ? 1 : 0) pixelBuffer=\(p.pixelBuffer != nil ? 1 : 0) preview=\(p.previewPixelBuffer != nil ? 1 : 0)"
    if #available(iOS 18.0, *) { x += " constantColorConfidence=\(p.constantColorConfidenceMap != nil ? 1 : 0) constantColorFallback=\(p.isConstantColorFallbackPhoto ? 1 : 0)" }
    cb("didFinishProcessingPhoto " + x, error)
  }
  @available(iOS 17.0, *)
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishCapturingDeferredPhotoProxy p: AVCaptureDeferredPhotoProxy?, error: Error?) {
    lock.lock(); photos += 1; lock.unlock()
    cb("didFinishCapturingDeferredPhotoProxy proxy=\(p != nil ? 1 : 0) bytes=\(p?.fileDataRepresentation()?.count ?? -1)", error)
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishRecordingLivePhotoMovieForEventualFileAt u: URL, resolvedSettings r: AVCaptureResolvedPhotoSettings) {
    cb("didFinishRecordingLivePhotoMovie \(u.lastPathComponent)", nil)
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingLivePhotoToMovieFileAt u: URL, duration: CMTime, photoDisplayTime: CMTime, resolvedSettings r: AVCaptureResolvedPhotoSettings, error: Error?) {
    lock.lock(); liveMovies += 1; lock.unlock()
    let sz = (try? FileManager.default.attributesOfItem(atPath: u.path)[.size] as? Int) ?? -1
    cb("didFinishProcessingLivePhotoToMovieFile \(u.lastPathComponent) bytes=\(sz ?? -1) duration=\(String(format: "%.2f", duration.seconds)) displayTime=\(String(format: "%.2f", photoDisplayTime.seconds)) \(movieInfo(u))", error)
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishCaptureFor r: AVCaptureResolvedPhotoSettings, error: Error?) {
    cb("didFinishCapture", error); done.signal()
  }
}
func movieInfo(_ u: URL) -> String {
  guard FileManager.default.fileExists(atPath: u.path) else { return "file=missing" }
  let a = AVURLAsset(url: u)
  var x = "dur=\(String(format: "%.2f", a.duration.seconds))"
  for t in a.tracks {
    x += " track:\(t.mediaType.rawValue)"
    if t.mediaType == .video {
      x += ":\(Int(t.naturalSize.width))x\(Int(t.naturalSize.height))@\(String(format: "%.1f", t.nominalFrameRate)) frames~\(Int((Double(t.nominalFrameRate) * t.timeRange.duration.seconds).rounded()))"
      // w4 e23b: the samples actually in the file (no decode), and samples / track duration
      if let r = try? AVAssetReader(asset: a) { let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil); o.alwaysCopiesSampleData = false
        if r.canAdd(o) { r.add(o) }; var n = 0
        if r.startReading() { while let sb = o.copyNextSampleBuffer() { n += CMSampleBufferGetNumSamples(sb) } }
        x += " samples=\(n) fps=\(String(format: "%.2f", Double(n) / max(t.timeRange.duration.seconds, 0.001)))" }
      if let fd = t.formatDescriptions.first {
        let f = fd as! CMFormatDescription
        x += ":\(fourcc(CMFormatDescriptionGetMediaSubType(f)))"
        for (n, k) in [("tf", kCVImageBufferTransferFunctionKey), ("prim", kCVImageBufferColorPrimariesKey)] {
          if let v = CMFormatDescriptionGetExtension(f, extensionKey: k) { x += " \(n)=\(v)" }
        }
        if let ext = CMFormatDescriptionGetExtensions(f) as? [String: Any], let atoms = ext["SampleDescriptionExtensionAtoms"] as? [String: Any] { x += " atoms=\(atoms.keys.sorted().joined(separator: ","))" }
      }
    }
  }
  return x
}

var ifsEnabled = Set<String>()   // w4 e23b: what `ifsupported` turned on
struct Feature {
  let name: String; let kind: String   // photo | movie
  var configure: (AVCaptureSession, AVCaptureDevice, AVCaptureDeviceInput, AVCapturePhotoOutput?, AVCaptureMovieFileOutput?) -> Void = { _, _, _, _, _ in }
  var settings: (AVCapturePhotoOutput, Int) -> AVCapturePhotoSettings = { _, _ in AVCapturePhotoSettings() }
  var afterStart: (AVCaptureDevice) -> Void = { _ in }
}
func set(_ what: String, _ b: () -> Void) { let e = SCTry(b); log("SET \(what) " + (e.map { "EXC \($0)" } ?? "ok")) }
func tmpURL(_ ext: String, _ i: Int) -> URL { URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("e22-\(featureArg)-\(i)-\(UUID().uuidString.prefix(6)).\(ext)") }
func pickFormat(_ dev: AVCaptureDevice, _ ok: (AVCaptureDevice.Format) -> Bool) -> AVCaptureDevice.Format? {
  dev.formats.filter(ok).max { a, b in
    let da = CMVideoFormatDescriptionGetDimensions(a.formatDescription), db = CMVideoFormatDescriptionGetDimensions(b.formatDescription)
    return Int(da.width) * Int(da.height) < Int(db.width) * Int(db.height)
  }
}
func useFormat(_ dev: AVCaptureDevice, _ what: String, _ ok: (AVCaptureDevice.Format) -> Bool) -> Bool {
  guard let f = pickFormat(dev, ok) else { log("SET activeFormat(\(what)) none: no format reads YES"); return false }
  set("activeFormat=\(fdesc(f)) (\(what))") { do { try dev.lockForConfiguration(); dev.activeFormat = f; dev.unlockForConfiguration() } catch { log("lockForConfiguration error \(error)") } }
  return true
}
func boolKey(_ o: NSObject, _ k: String) -> Bool { kv(o, k) == "YES" }

let features: [Feature] = {
  var a: [Feature] = []
  a.append(Feature(name: "baseline", kind: "photo"))
  // Live Photo as AVCam: isLivePhotoCaptureEnabled = isLivePhotoCaptureSupported (here: true, guarded); per shot livePhotoMovieFileURL if enabled
  var lp = Feature(name: "livephoto", kind: "photo")
  lp.configure = { _, _, _, p, _ in let p = p!; set("photo.isLivePhotoCaptureEnabled=true (supported=\(p.isLivePhotoCaptureSupported ? "YES" : "NO"))") { p.isLivePhotoCaptureEnabled = true }; log("SET-RESULT photo.isLivePhotoCaptureEnabled=\(p.isLivePhotoCaptureEnabled ? "YES" : "NO")") }
  lp.settings = { p, i in
    let s = AVCapturePhotoSettings(format: p.availablePhotoCodecTypes.contains(.hevc) ? [AVVideoCodecKey: AVVideoCodecType.hevc] : nil)
    if p.isLivePhotoCaptureEnabled {
      s.livePhotoMovieFileURL = tmpURL("mov", i)
      if p.availableLivePhotoVideoCodecTypes.contains(.hevc) { s.livePhotoVideoCodecType = .hevc }
      log("SETTINGS \(i) livePhotoMovieFileURL set codec=\(s.livePhotoVideoCodecType.rawValue)")
    } else { log("SETTINGS \(i) livePhotoMovieFileURL not set (isLivePhotoCaptureEnabled NO)") }
    return s
  }
  a.append(lp)
  // depth as AVCam: output.isDepthDataDeliveryEnabled = supported; settings.isDepthDataDeliveryEnabled = output's value
  var dp = Feature(name: "depth", kind: "photo")
  dp.configure = { _, _, _, p, _ in let p = p!; set("photo.isDepthDataDeliveryEnabled=true (supported=\(p.isDepthDataDeliverySupported ? "YES" : "NO"))") { p.isDepthDataDeliveryEnabled = true }; log("SET-RESULT photo.isDepthDataDeliveryEnabled=\(p.isDepthDataDeliveryEnabled ? "YES" : "NO")") }
  dp.settings = { p, i in let s = AVCapturePhotoSettings(); s.isDepthDataDeliveryEnabled = p.isDepthDataDeliveryEnabled; return s }
  a.append(dp)
  // portrait effects matte + semantic segmentation (AVCam: needs depth on)
  var pm = Feature(name: "portraitmatte", kind: "photo")
  pm.configure = { _, _, _, p, _ in let p = p!
    set("photo.isDepthDataDeliveryEnabled=true") { p.isDepthDataDeliveryEnabled = true }
    set("photo.isPortraitEffectsMatteDeliveryEnabled=true (supported=\(p.isPortraitEffectsMatteDeliverySupported ? "YES" : "NO"))") { p.isPortraitEffectsMatteDeliveryEnabled = true } }
  pm.settings = { p, i in let s = AVCapturePhotoSettings(); s.isDepthDataDeliveryEnabled = p.isDepthDataDeliveryEnabled; s.isPortraitEffectsMatteDeliveryEnabled = p.isPortraitEffectsMatteDeliveryEnabled; return s }
  a.append(pm)
  var sg = Feature(name: "semantic", kind: "photo")
  sg.configure = { _, _, _, p, _ in let p = p!
    set("photo.isDepthDataDeliveryEnabled=true") { p.isDepthDataDeliveryEnabled = true }
    set("photo.enabledSemanticSegmentationMatteTypes=available \(p.availableSemanticSegmentationMatteTypes.map { $0.rawValue })") { p.enabledSemanticSegmentationMatteTypes = p.availableSemanticSegmentationMatteTypes } }
  sg.settings = { p, i in let s = AVCapturePhotoSettings(); s.isDepthDataDeliveryEnabled = p.isDepthDataDeliveryEnabled; s.enabledSemanticSegmentationMatteTypes = p.enabledSemanticSegmentationMatteTypes; return s }
  a.append(sg)
  // Apple ProRAW and Bayer RAW
  var pr = Feature(name: "proraw", kind: "photo")
  pr.configure = { _, _, _, p, _ in let p = p!; set("photo.isAppleProRAWEnabled=true (supported=\(p.isAppleProRAWSupported ? "YES" : "NO"))") { p.isAppleProRAWEnabled = true }
    log("SET-RESULT raw formats now \(p.availableRawPhotoPixelFormatTypes.map { fourcc($0) })") }
  pr.settings = { p, i in
    if let r = p.availableRawPhotoPixelFormatTypes.first(where: { AVCapturePhotoOutput.isAppleProRAWPixelFormat($0) }) { log("SETTINGS \(i) rawPixelFormatType=\(fourcc(r)) (ProRAW) + HEVC"); return AVCapturePhotoSettings(rawPixelFormatType: r, processedFormat: [AVVideoCodecKey: AVVideoCodecType.hevc]) }
    log("SETTINGS \(i) no ProRAW pixel format available: plain settings"); return AVCapturePhotoSettings()
  }
  a.append(pr)
  var rw = Feature(name: "raw", kind: "photo")
  rw.settings = { p, i in
    if let r = p.availableRawPhotoPixelFormatTypes.first(where: { AVCapturePhotoOutput.isBayerRAWPixelFormat($0) }) ?? p.availableRawPhotoPixelFormatTypes.first { log("SETTINGS \(i) rawPixelFormatType=\(fourcc(r))"); return AVCapturePhotoSettings(rawPixelFormatType: r) }
    log("SETTINGS \(i) no RAW pixel format available: plain settings"); return AVCapturePhotoSettings()
  }
  a.append(rw)
  // responsive capture / zero shutter lag / fast capture prioritization / auto-deferred (iOS 17)
  var zs = Feature(name: "zsl", kind: "photo")
  zs.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 17.0, *) { set("photo.isZeroShutterLagEnabled=true (supported=\(p.isZeroShutterLagSupported ? "YES" : "NO"))") { p.isZeroShutterLagEnabled = true } } }
  a.append(zs)
  var rs = Feature(name: "responsive", kind: "photo")
  rs.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 17.0, *) {
    set("photo.isZeroShutterLagEnabled=true") { p.isZeroShutterLagEnabled = true }
    set("photo.isResponsiveCaptureEnabled=true (supported=\(p.isResponsiveCaptureSupported ? "YES" : "NO"))") { p.isResponsiveCaptureEnabled = true } } }
  a.append(rs)
  var fp = Feature(name: "fastprio", kind: "photo")
  fp.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 17.0, *) {
    set("photo.isZeroShutterLagEnabled=true") { p.isZeroShutterLagEnabled = true }
    set("photo.isResponsiveCaptureEnabled=true") { p.isResponsiveCaptureEnabled = true }
    set("photo.isFastCapturePrioritizationEnabled=true (supported=\(p.isFastCapturePrioritizationSupported ? "YES" : "NO"))") { p.isFastCapturePrioritizationEnabled = true } } }
  a.append(fp)
  var df = Feature(name: "deferred", kind: "photo")
  df.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 17.0, *) {
    set("photo.maxPhotoQualityPrioritization=.quality") { p.maxPhotoQualityPrioritization = .quality }
    set("photo.isAutoDeferredPhotoDeliveryEnabled=true (supported=\(p.isAutoDeferredPhotoDeliverySupported ? "YES" : "NO"))") { p.isAutoDeferredPhotoDeliveryEnabled = true } } }
  df.settings = { p, i in let s = AVCapturePhotoSettings(); s.photoQualityPrioritization = .quality; return s }
  a.append(df)
  var qp = Feature(name: "quality", kind: "photo")
  qp.configure = { _, _, _, p, _ in let p = p!; set("photo.maxPhotoQualityPrioritization=.quality") { p.maxPhotoQualityPrioritization = .quality } }
  qp.settings = { p, i in let s = AVCapturePhotoSettings(); s.photoQualityPrioritization = .quality; return s }
  a.append(qp)
  var cc = Feature(name: "constantcolor", kind: "photo")
  cc.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 18.0, *) { set("photo.isConstantColorEnabled=true (supported=\(p.isConstantColorSupported ? "YES" : "NO"))") { p.isConstantColorEnabled = true } } }
  cc.settings = { p, i in let s = AVCapturePhotoSettings(); if #available(iOS 18.0, *) { s.flashMode = .auto; s.isConstantColorEnabled = p.isConstantColorEnabled; s.isConstantColorFallbackPhotoDeliveryEnabled = p.isConstantColorEnabled }; return s }
  a.append(cc)
  var vc = Feature(name: "constituents", kind: "photo")
  vc.configure = { _, _, _, p, _ in let p = p!; set("photo.isVirtualDeviceConstituentPhotoDeliveryEnabled=true (supported=\(p.isVirtualDeviceConstituentPhotoDeliverySupported ? "YES" : "NO"))") { p.isVirtualDeviceConstituentPhotoDeliveryEnabled = true } }
  vc.settings = { p, i in let s = AVCapturePhotoSettings(); if p.isVirtualDeviceConstituentPhotoDeliveryEnabled { s.virtualDeviceConstituentPhotoDeliveryEnabledDevices = (AVCaptureDevice.default(.builtInTripleCamera, for: .video, position: .back) ?? AVCaptureDevice.default(.builtInDualWideCamera, for: .video, position: .back))?.constituentDevices ?? [] }; return s }
  a.append(vc)
  var cal = Feature(name: "calibration", kind: "photo")
  cal.configure = { _, _, _, p, _ in let p = p!; set("photo.isDepthDataDeliveryEnabled=true") { p.isDepthDataDeliveryEnabled = true } }
  cal.settings = { p, i in let s = AVCapturePhotoSettings(); s.isDepthDataDeliveryEnabled = p.isDepthDataDeliveryEnabled; s.isCameraCalibrationDataDeliveryEnabled = p.isCameraCalibrationDataDeliverySupported; return s }
  a.append(cal)
  var cad = Feature(name: "contentaware", kind: "photo")
  cad.configure = { _, _, _, p, _ in let p = p!; set("photo.isContentAwareDistortionCorrectionEnabled=true (supported=\(p.isContentAwareDistortionCorrectionSupported ? "YES" : "NO"))") { p.isContentAwareDistortionCorrectionEnabled = true } }
  a.append(cad)
  var ss = Feature(name: "shuttersound", kind: "photo")
  ss.configure = { _, _, _, p, _ in let p = p!; if #available(iOS 18.0, *) { log("SET photo.isShutterSoundSuppressionSupported=\(p.isShutterSoundSuppressionSupported ? "YES" : "NO") (settings-level only)") } }
  ss.settings = { p, i in let s = AVCapturePhotoSettings(); if #available(iOS 18.0, *) { s.isShutterSoundSuppressionEnabled = true }; return s }
  a.append(ss)
  var fl = Feature(name: "flash", kind: "photo")
  fl.settings = { p, i in let s = AVCapturePhotoSettings(); if p.supportedFlashModes.contains(.on) { s.flashMode = .on } else { log("SETTINGS \(i) flash .on not in supportedFlashModes") }; return s }
  a.append(fl)

  // Apple's AVCam sample, verbatim (avcam-dl PhotoCapture.swift updateConfiguration + createPhotoSettings), with its UI defaults
  // isLivePhotoEnabled = true and qualityPrioritization = .quality: livePhotoMovieFileURL is set from the toggle, not from support
  var av = Feature(name: "avcam", kind: "photo")
  av.configure = { _, d, _, p, _ in let p = p!
    set("AVCam updateConfiguration: maxPhotoDimensions=\(d.activeFormat.supportedMaxPhotoDimensions.last.map { "\($0.width)x\($0.height)" } ?? "0x0")") { p.maxPhotoDimensions = d.activeFormat.supportedMaxPhotoDimensions.last ?? CMVideoDimensions(width: 0, height: 0) }
    set("AVCam isLivePhotoCaptureEnabled=isLivePhotoCaptureSupported(\(p.isLivePhotoCaptureSupported ? "YES" : "NO"))") { p.isLivePhotoCaptureEnabled = p.isLivePhotoCaptureSupported }
    set("AVCam maxPhotoQualityPrioritization=.quality") { p.maxPhotoQualityPrioritization = .quality }
    if #available(iOS 17.0, *) {
      set("AVCam isResponsiveCaptureEnabled=isResponsiveCaptureSupported(\(p.isResponsiveCaptureSupported ? "YES" : "NO"))") { p.isResponsiveCaptureEnabled = p.isResponsiveCaptureSupported }
      set("AVCam isFastCapturePrioritizationEnabled=isFastCapturePrioritizationSupported(\(p.isFastCapturePrioritizationSupported ? "YES" : "NO"))") { p.isFastCapturePrioritizationEnabled = p.isFastCapturePrioritizationSupported }
      set("AVCam isAutoDeferredPhotoDeliveryEnabled=isAutoDeferredPhotoDeliverySupported(\(p.isAutoDeferredPhotoDeliverySupported ? "YES" : "NO"))") { p.isAutoDeferredPhotoDeliveryEnabled = p.isAutoDeferredPhotoDeliverySupported } } }
  av.settings = { p, i in
    var s = AVCapturePhotoSettings()
    if p.availablePhotoCodecTypes.contains(.hevc) { s = AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.hevc]) }
    if let pv = s.availablePreviewPhotoPixelFormatTypes.first { s.previewPhotoFormat = [kCVPixelBufferPixelFormatTypeKey as String: pv] }
    s.maxPhotoDimensions = p.maxPhotoDimensions
    s.livePhotoMovieFileURL = tmpURL("mov", i)   // features.isLivePhotoEnabled (UI default true)
    s.photoQualityPrioritization = .quality
    log("SETTINGS \(i) AVCam: hevc, maxPhotoDimensions=\(s.maxPhotoDimensions.width)x\(s.maxPhotoDimensions.height), livePhotoMovieFileURL set (isLivePhotoCaptureEnabled=\(p.isLivePhotoCaptureEnabled ? "YES" : "NO")), quality")
    return s
  }
  a.append(av)
  // bracketed capture (maxBracketedCapturePhotoCount reads > 0): 3 auto-exposure brackets -2/0/+2 EV
  var br = Feature(name: "bracket", kind: "photo")
  br.settings = { p, i in
    let b = [-2, 0, 2].map { AVCaptureAutoExposureBracketedStillImageSettings.autoExposureSettings(exposureTargetBias: Float($0)) }
    log("SETTINGS \(i) AVCapturePhotoBracketSettings 3x auto-exposure (max \(p.maxBracketedCapturePhotoCount))")
    return AVCapturePhotoBracketSettings(rawPixelFormatType: 0, processedFormat: [AVVideoCodecKey: AVVideoCodecType.jpeg], bracketedSettings: b)
  }
  a.append(br)
  // w4 e23b: AVCam-style "enable if supported" for the three features design §6.5 wants unsupported: flash .auto / .on if
  // offered, a 3-photo bracket if maxBracketedCapturePhotoCount >= 3, MultiCam if back-wide + front-wide is a supported set.
  // Logs IFSUPPORTED lines; RESULT carries enabled=<list> (none = nothing turned on)
  var ifs = Feature(name: "ifsupported", kind: "photo")
  ifs.configure = { _, d, _, _, _ in
    let sets = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).supportedMultiCamDeviceSets
    let pair = sets.contains { Set($0.map(label)) == ["back-wide", "front-wide"] }
    log("IFSUPPORTED multicam \(pair && AVCaptureMultiCamSession.isMultiCamSupported ? "ENABLE" : "skip") (sets=\(sets.count), class=\(AVCaptureMultiCamSession.isMultiCamSupported ? "YES" : "NO"))")
    if pair && AVCaptureMultiCamSession.isMultiCamSupported { ifsEnabled.insert("multicam") }
    log("IFSUPPORTED torch hasTorch=\(d.hasTorch ? "YES" : "NO") flash hasFlash=\(d.hasFlash ? "YES" : "NO") isFlashAvailable=\(d.isFlashAvailable ? "YES" : "NO")") }
  ifs.settings = { p, i in
    let modes = p.supportedFlashModes
    if p.maxBracketedCapturePhotoCount >= 3 {
      ifsEnabled.insert("bracket"); log("IFSUPPORTED \(i) bracket ENABLE (max \(p.maxBracketedCapturePhotoCount))")
      let b = [-2, 0, 2].map { AVCaptureAutoExposureBracketedStillImageSettings.autoExposureSettings(exposureTargetBias: Float($0)) }
      return AVCapturePhotoBracketSettings(rawPixelFormatType: 0, processedFormat: [AVVideoCodecKey: AVVideoCodecType.jpeg], bracketedSettings: b) }
    let s = AVCapturePhotoSettings()
    if modes.contains(.auto) { s.flashMode = .auto; ifsEnabled.insert("flash") } else if modes.contains(.on) { s.flashMode = .on; ifsEnabled.insert("flash") }
    log("IFSUPPORTED \(i) bracket skip (max \(p.maxBracketedCapturePhotoCount)) flash \(s.flashMode == .off ? "skip" : "ENABLE") (supportedFlashModes=\(modes.map { $0.rawValue }))")
    return s }
  a.append(ifs)
  var mx = Feature(name: "maxres", kind: "photo")
  mx.configure = { _, d, _, p, _ in let p = p!; let m = d.activeFormat.supportedMaxPhotoDimensions.max { $0.width * $0.height < $1.width * $1.height }; set("photo.maxPhotoDimensions=\(m.map { "\($0.width)x\($0.height)" } ?? "-")") { if let m { p.maxPhotoDimensions = m } } }
  mx.settings = { p, i in let s = AVCapturePhotoSettings(); s.maxPhotoDimensions = p.maxPhotoDimensions; return s }
  a.append(mx)

  // ---- movie features ----
  a.append(Feature(name: "moviebaseline", kind: "movie"))
  var hdr = Feature(name: "videohdr", kind: "movie")
  hdr.configure = { _, d, _, _, _ in
    if useFormat(d, "isVideoHDRSupported", { $0.isVideoHDRSupported }) {
      set("device.automaticallyAdjustsVideoHDREnabled=false; isVideoHDREnabled=true") { do { try d.lockForConfiguration(); d.automaticallyAdjustsVideoHDREnabled = false; d.isVideoHDREnabled = true; d.unlockForConfiguration() } catch { log("lock error \(error)") } }
      log("SET-RESULT device.isVideoHDREnabled=\(d.isVideoHDREnabled ? "YES" : "NO")") } }
  a.append(hdr)
  for (nm, cs) in [("hlg", AVCaptureColorSpace.HLG_BT2020), ("applelog", AVCaptureColorSpace(rawValue: 3)!)] {
    var f = Feature(name: nm, kind: "movie")
    f.configure = { s, d, _, _, _ in
      s.automaticallyConfiguresCaptureDeviceForWideColor = false
      if useFormat(d, "supportedColorSpaces contains \(colorSpaceNames[cs.rawValue]!)", { $0.supportedColorSpaces.contains(cs) }) {
        set("device.activeColorSpace=\(colorSpaceNames[cs.rawValue]!)") { do { try d.lockForConfiguration(); d.activeColorSpace = cs; d.unlockForConfiguration() } catch { log("lock error \(error)") } }
        log("SET-RESULT device.activeColorSpace=\(colorSpaceNames[d.activeColorSpace.rawValue] ?? "?")") } }
    a.append(f)
  }
  var fps = Feature(name: "fps", kind: "movie")
  fps.configure = { _, d, _, _, _ in
    let mx = d.formats.flatMap { $0.videoSupportedFrameRateRanges.map { $0.maxFrameRate } }.max() ?? 30
    if useFormat(d, "maxFrameRate=\(mx)", { $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate == mx } }) {
      set("device.activeVideoMin/MaxFrameDuration=1/\(Int(mx))") { do { try d.lockForConfiguration(); d.activeVideoMinFrameDuration = CMTime(value: 1, timescale: CMTimeScale(mx)); d.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: CMTimeScale(mx)); d.unlockForConfiguration() } catch { log("lock error \(error)") } }
      log("SET-RESULT activeVideoMinFrameDuration=\(d.activeVideoMinFrameDuration.seconds)") } }
  a.append(fps)
  // w4 e23b: 1080p at its highest frame rate (e22 measured only 4K60)
  var f10 = Feature(name: "fps1080", kind: "movie")
  f10.configure = { _, d, _, _, _ in
    let ok: (AVCaptureDevice.Format) -> Bool = { CMVideoFormatDescriptionGetDimensions($0.formatDescription).height == 1080 && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 60 } }
    if let fm = d.formats.first(where: ok) {
      set("activeFormat=\(fdesc(fm)); activeVideoMin/MaxFrameDuration=1/60") { do { try d.lockForConfiguration(); d.activeFormat = fm; d.activeVideoMinFrameDuration = CMTime(value: 1, timescale: 60); d.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: 60); d.unlockForConfiguration() } catch { log("lock error \(error)") } }
      log("SET-RESULT activeVideoMinFrameDuration=\(d.activeVideoMinFrameDuration.seconds)") } else { log("SET activeFormat(1080p60) none") } }
  a.append(f10)
  var sp = Feature(name: "spatial", kind: "movie")
  sp.configure = { _, d, _, _, m in
    if #available(iOS 18.0, *) {
      if useFormat(d, "isSpatialVideoCaptureSupported", { $0.isSpatialVideoCaptureSupported }) { }
      set("movie.isSpatialVideoCaptureEnabled=true (supported=\(m!.isSpatialVideoCaptureSupported ? "YES" : "NO"))") { m!.isSpatialVideoCaptureEnabled = true } } }
  a.append(sp)
  var cin = Feature(name: "cinematic", kind: "movie")
  cin.configure = { _, d, inp, _, _ in
    _ = useFormat(d, "isCinematicVideoCaptureSupported", { boolKey($0, "isCinematicVideoCaptureSupported") })
    set("input.cinematicVideoCaptureEnabled=true (supported=\(kv(inp, "isCinematicVideoCaptureSupported") ?? "absent"))") { inp.setValue(true, forKey: "cinematicVideoCaptureEnabled") } }
  a.append(cin)
  var cs = Feature(name: "centerstage", kind: "movie")
  cs.configure = { _, d, _, _, _ in
    _ = useFormat(d, "isCenterStageSupported", { $0.isCenterStageSupported })
    set("AVCaptureDevice.centerStageControlMode=.cooperative; isCenterStageEnabled=true") { AVCaptureDevice.centerStageControlMode = .cooperative; AVCaptureDevice.isCenterStageEnabled = true } }
  cs.afterStart = { d in log("SET-RESULT device.isCenterStageActive=\(d.isCenterStageActive ? "YES" : "NO") AVCaptureDevice.isCenterStageEnabled=\(AVCaptureDevice.isCenterStageEnabled ? "YES" : "NO")") }
  a.append(cs)
  var rx = Feature(name: "reactions", kind: "movie")
  rx.configure = { _, d, _, _, _ in _ = useFormat(d, "reactionEffectsSupported", { $0.reactionEffectsSupported }) }
  rx.afterStart = { d in
    log("SET-RESULT canPerformReactionEffects=\(d.canPerformReactionEffects ? "YES" : "NO") AVCaptureDevice.reactionEffectsEnabled=\(AVCaptureDevice.reactionEffectsEnabled ? "YES" : "NO") available=\(d.availableReactionTypes.map { $0.rawValue })")
    set("device.performEffect(for: .thumbsUp)") { d.performEffect(for: .thumbsUp) } }
  a.append(rx)
  var pe = Feature(name: "portraiteffect", kind: "movie")   // user-controlled (Control Center): nothing for the app to set
  pe.configure = { _, d, _, _, _ in _ = useFormat(d, "isPortraitEffectSupported", { $0.isPortraitEffectSupported }) }
  pe.afterStart = { d in log("SET-RESULT isPortraitEffectActive=\(d.isPortraitEffectActive ? "YES" : "NO") AVCaptureDevice.isPortraitEffectEnabled=\(AVCaptureDevice.isPortraitEffectEnabled ? "YES" : "NO") (user-controlled)") }
  a.append(pe)
  var sl = Feature(name: "studiolight", kind: "movie")
  sl.configure = { _, d, _, _, _ in _ = useFormat(d, "isStudioLightSupported", { $0.isStudioLightSupported }) }
  sl.afterStart = { d in log("SET-RESULT isStudioLightActive=\(d.isStudioLightActive ? "YES" : "NO") AVCaptureDevice.isStudioLightEnabled=\(AVCaptureDevice.isStudioLightEnabled ? "YES" : "NO") (user-controlled)") }
  a.append(sl)
  return a
}()

func runFeature() {
  readAll()
  log("FEATURE \(featureArg) camera=\(cameraArg)")
  if featureArg == "multicam" { runMultiCam(); return }
  guard let f = features.first(where: { $0.name == featureArg }) else { log("RESULT feature=\(featureArg) unknown"); return }
  guard let dev = cameras().first(where: { label($0) == cameraArg }) else { log("RESULT feature=\(featureArg) camera \(cameraArg) not found"); return }
  let s = AVCaptureSession(); let fc = FrameCounter()
  var photo: AVCapturePhotoOutput?; var movie: AVCaptureMovieFileOutput?
  var cfgExc: String?
  var inp: AVCaptureDeviceInput!
  cfgExc = SCTry {
    s.beginConfiguration()
    if f.kind == "photo" { if s.canSetSessionPreset(.photo) { s.sessionPreset = .photo } } else if s.canSetSessionPreset(.high) { s.sessionPreset = .high }
    inp = try? AVCaptureDeviceInput(device: dev)
    if let inp, s.canAddInput(inp) { s.addInput(inp) } else { log("canAddInput NO") }
    if f.kind == "photo" { let p = AVCapturePhotoOutput(); if s.canAddOutput(p) { s.addOutput(p); photo = p } }
    else { let m = AVCaptureMovieFileOutput(); if s.canAddOutput(m) { s.addOutput(m); movie = m } }
    let v = AVCaptureVideoDataOutput(); v.setSampleBufferDelegate(fc, queue: frameQ); if s.canAddOutput(v) { s.addOutput(v) } else { log("canAddOutput(vdo) NO") }
    if let inp { f.configure(s, dev, inp, photo, movie) }
    s.commitConfiguration()
  }
  if let cfgExc { log("SET configuration EXC \(cfgExc)") }
  log("CONFIGURED activeFormat=\(fdesc(dev.activeFormat)) preset=\(s.sessionPreset.rawValue)")
  if let photo { for k in photoKeys where k.hasSuffix("Enabled") || k.hasPrefix("isLivePhoto") || k.hasPrefix("isDepth") { log("R \(cameraArg) photo-after \(k) \(kv(photo, k) ?? "absent")") } }
  var errNote = ""
  NotificationCenter.default.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: s, queue: nil) { n in
    let e = n.userInfo?[AVCaptureSessionErrorKey] as? NSError; errNote += " runtimeError=\(e?.code ?? 0)"; log("NOTE runtimeError \(e.map { "\($0.domain) \($0.code) \($0.localizedDescription)" } ?? "?")") }
  let startExc = SCTry { s.startRunning() }
  if let startExc { log("SET startRunning EXC \(startExc)") }
  sleepS(1.5)
  var (n, _, dims) = fc.take(); log("RUNNING running=\(s.isRunning ? 1 : 0) frames1.5s=\(n) \(dims)")
  f.afterStart(dev)
  if f.kind == "photo", let photo {
    let d = PhotoDelegate(); var ok = 0, hangs = 0, exc = 0
    for i in 1...5 {
      d.i = i; d.done = DispatchSemaphore(value: 0)
      var st: AVCapturePhotoSettings?
      if let e = SCTry({ st = f.settings(photo, i) }) { log("EXC settings \(i) \(e)"); exc += 1; continue }
      log("STEP capturePhoto \(i)")
      if noCatch { photo.capturePhoto(with: st!, delegate: d) }
      else if let e = SCTry({ photo.capturePhoto(with: st!, delegate: d) }) { log("EXC capturePhoto \(i) \(e)"); exc += 1; continue }
      if d.done.wait(timeout: .now() + 10) == .timedOut { log("HANG \(i) no didFinishCapture within 10 s"); hangs += 1 } else { ok += 1 }
      sleepS(0.3)
    }
    sleepS(1.0)   // late callbacks (live photo movie)
    (n, _, _) = fc.take()
    d.lock.lock(); let errs = d.errors; let photos = d.photos; let lm = d.liveMovies; d.lock.unlock()
    log("RESULT feature=\(featureArg) camera=\(cameraArg)\(featureArg == "ifsupported" ? " enabled=\(ifsEnabled.isEmpty ? "none" : ifsEnabled.sorted().joined(separator: ","))" : "") captures=\(ok)/5 hangs=\(hangs) exceptions=\(exc) photos=\(photos) liveMovies=\(lm) errors=\(errs.isEmpty ? "none" : errs.joined(separator: ","))\(errNote) running=\(s.isRunning ? 1 : 0) framesAfter=\(n)")
  } else if let movie {
    final class RecDelegate: NSObject, AVCaptureFileOutputRecordingDelegate {
      let done = DispatchSemaphore(value: 0); var err: String?
      func fileOutput(_ o: AVCaptureFileOutput, didStartRecordingTo u: URL, from c: [AVCaptureConnection]) { log("CB didStartRecording \(u.lastPathComponent)") }
      func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) {
        if let e = error as NSError? { err = "\(e.domain):\(e.code)"; log("CB didFinishRecording error=\(e.domain) \(e.code) \"\(e.localizedDescription)\" \(e.userInfo)") } else { log("CB didFinishRecording ok") }
        log("MOVIE \(movieInfo(u)) bytes=\(((try? FileManager.default.attributesOfItem(atPath: u.path))?[.size] as? Int) ?? -1)"); done.signal() }
    }
    let d = RecDelegate(); let u = tmpURL("mov", 0); var result = ""
    log("STEP startRecording")
    if let e = SCTry({ movie.startRecording(to: u, recordingDelegate: d) }) { log("EXC startRecording \(e)"); result = "exception" }
    else {
      sleepS(movieSeconds); (n, _, dims) = fc.take(); log("RECORDING seconds=\(movieSeconds) frames=\(n) \(dims) isRecording=\(movie.isRecording ? 1 : 0)")
      log("STEP stopRecording")
      if let e = SCTry({ movie.stopRecording() }) { log("EXC stopRecording \(e)"); result = "exception" }
      if d.done.wait(timeout: .now() + 10) == .timedOut { log("HANG no didFinishRecording within 10 s"); result = "hang" } else { result = d.err.map { "error \($0)" } ?? "ok" }
    }
    log("RESULT feature=\(featureArg) camera=\(cameraArg) movie=\(result)\(errNote) running=\(s.isRunning ? 1 : 0) vdoFrames=\(n)")
  } else { log("RESULT feature=\(featureArg) camera=\(cameraArg) no output added") }
  s.stopRunning()
}
func runMultiCam() {
  log("SET AVCaptureMultiCamSession.isMultiCamSupported=\(AVCaptureMultiCamSession.isMultiCamSupported ? "YES" : "NO")")
  guard AVCaptureMultiCamSession.isMultiCamSupported else { log("RESULT feature=multicam not attempted (isMultiCamSupported NO)"); return }
  let s = AVCaptureMultiCamSession(); var counters: [FrameCounter] = []
  for n in [AVCaptureSession.runtimeErrorNotification, AVCaptureSession.wasInterruptedNotification, AVCaptureSession.didStartRunningNotification, AVCaptureSession.didStopRunningNotification] {
    NotificationCenter.default.addObserver(forName: n, object: s, queue: nil) { note in
      let e = note.userInfo?[AVCaptureSessionErrorKey] as? NSError
      log("NOTE \(n.rawValue)" + (e.map { " error=\($0.domain) \($0.code) \"\($0.localizedDescription)\" \($0.userInfo)" } ?? "") + ((note.userInfo?[AVCaptureSessionInterruptionReasonKey]).map { " reason=\($0)" } ?? "")) } }
  let e = SCTry {
    s.beginConfiguration()
    for pos in [AVCaptureDevice.Position.back, .front] {
      guard let d = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: pos), let i = try? AVCaptureDeviceInput(device: d) else { continue }
      if let f = d.formats.first(where: { $0.isMultiCamSupported }) { try? d.lockForConfiguration(); d.activeFormat = f; d.unlockForConfiguration(); log("multicam \(label(d)) activeFormat=\(fdesc(f)) (isMultiCamSupported)") }
      else { log("multicam \(label(d)) no format with isMultiCamSupported; activeFormat stays \(fdesc(d.activeFormat))") }
      guard s.canAddInput(i) else { log("multicam canAddInput NO \(label(d))"); continue }
      s.addInputWithNoConnections(i)
      let v = AVCaptureVideoDataOutput(); let fc = FrameCounter(); v.setSampleBufferDelegate(fc, queue: frameQ); counters.append(fc)
      s.addOutputWithNoConnections(v)
      let c = AVCaptureConnection(inputPorts: i.ports(for: .video, sourceDeviceType: d.deviceType, sourceDevicePosition: pos), output: v)
      if s.canAddConnection(c) { s.addConnection(c) } else { log("multicam canAddConnection NO \(label(d))") }
    }
    s.commitConfiguration()
  }
  if let e { log("SET multicam configuration EXC \(e)") }
  log("multicam configured inputs=\(s.inputs.count) outputs=\(s.outputs.count) connections=\(s.connections.count) hardwareCost=\(s.hardwareCost) systemPressureCost=\(s.systemPressureCost)")
  log("STEP startRunning")
  let se = SCTry { s.startRunning() }; if let se { log("EXC startRunning \(se)") }
  log("multicam after startRunning running=\(s.isRunning ? 1 : 0) interrupted=\(s.isInterrupted ? 1 : 0)")
  sleepS(3)
  log("RESULT feature=multicam running=\(s.isRunning ? 1 : 0) hwCost=\(s.hardwareCost) frames3s=\(counters.map { "\($0.take().0)" }.joined(separator: ","))")
  s.stopRunning()
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    let w = UIWindow(windowScene: ws); w.rootViewController = UIViewController(); w.rootViewController!.view.backgroundColor = .darkGray; w.makeKeyAndVisible()
    window = w
    DispatchQueue(label: "probe.session").async {
      if mode == "feature" { runFeature() } else { readAll() }
      log("END")
    }
  }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: "Default", sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
  }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
