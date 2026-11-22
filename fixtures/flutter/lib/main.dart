// SPDX-License-Identifier: Apache-2.0
// Auto-run harness: camera (camera_avfoundation) + mobile_scanner. No taps.
// Env (via SIMCTL_CHILD_*): M7_TEST=all|camera|scanner|stream|audio|qr (default all), M7_AUDIO=1|0 for `audio`;
// for `qr` (scripts/qr-test.sh): M7_QR_POS=back|front, M7_QR_SECS=N (default 10).
import 'dart:async';
import 'dart:io';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:path_provider/path_provider.dart';

File? _log;
int _pass = 0, _fail = 0;
void log(String s) {
  final line = 'M7| $s';
  // ignore: avoid_print
  print(line);
  _log?.writeAsStringSync('$line\n', mode: FileMode.append, flush: true);
}

void pass(String step, [String d = '']) { _pass++; log('PASS $step $d'); }
void fail(String step, Object e) { _fail++; log('FAIL $step $e'); }

Future<T> step<T>(String name, Future<T> Function() f, {int secs = 15, String Function(T)? desc}) async {
  final sw = Stopwatch()..start();
  try {
    final r = await f().timeout(Duration(seconds: secs));
    pass(name, '${desc != null ? desc(r) : ''} (${sw.elapsedMilliseconds} ms)');
    return r;
  } catch (e) {
    fail(name, '$e (${sw.elapsedMilliseconds} ms)');
    rethrow;
  }
}

Future<void> sleep(int ms) => Future.delayed(Duration(milliseconds: ms));

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final docs = await getApplicationDocumentsDirectory();
  _log = File('${docs.path}/m7.log');
  runApp(const M7App());
}

class M7App extends StatefulWidget {
  const M7App({super.key});
  @override
  State<M7App> createState() => _M7AppState();
}

class _M7AppState extends State<M7App> {
  Widget body = const Center(child: Text('m7 starting', style: TextStyle(color: Colors.white)));
  String status = '';
  void show(Widget w, String s) => setState(() { body = w; status = s; });

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => run());
  }

  Future<void> shot(String name, [int waitMs = 1500]) async {
    await sleep(waitMs);
    log('SCREENSHOT $name');
    await sleep(1500); // host takes it
  }

  Future<void> run() async {
    final docs = await getApplicationDocumentsDirectory();
    final tf = File('${docs.path}/m7test.txt');
    final test = Platform.environment['M7_TEST'] ?? (tf.existsSync() ? tf.readAsStringSync().trim() : 'all');
    log('START test=$test pid=$pid env=${Platform.environment.keys.where((k) => k.startsWith('M')).toList()}');
    try {
      if (test == 'all' || test == 'camera' || test.startsWith('stream') || test.startsWith('audio')) await cameraTests(test);
    } catch (e) { log('camera tests aborted: $e'); }
    try {
      if (test == 'all' || test == 'scanner') await scannerTest();
    } catch (e) { log('scanner test aborted: $e'); }
    try {
      if (test == 'qr') await qrTest();
    } catch (e) { log('qr test aborted: $e'); }
    log('SUMMARY pass=$_pass fail=$_fail');
    show(Center(child: Text('END pass=$_pass fail=$_fail', style: const TextStyle(color: Colors.white))), 'end');
    log('END');
  }

  Future<String> rec(CameraController c, int ms) async {
    await c.startVideoRecording();
    await sleep(ms);
    final f = await c.stopVideoRecording();
    final len = await File(f.path).length();
    log('path=${f.path} bytes=$len');
    return '${f.path} $len B';
  }

  Future<void> cameraTests(String test) async {
    final cams = await step('availableCameras', () => availableCameras(),
        desc: (l) => l.map((c) => '[${c.name} ${c.lensDirection.name} sensorOrientation=${c.sensorOrientation} lens=${c.lensType.name}]').join(' '));
    if (cams.isEmpty) { fail('availableCameras', 'empty'); return; }
    final back = cams.firstWhere((c) => c.lensDirection == CameraLensDirection.back, orElse: () => cams.first);

    if (test.startsWith('audio')) {
      final audio = !test.contains('noaudio') && Platform.environment['M7_AUDIO'] != '0';
      final c = CameraController(back, ResolutionPreset.high, enableAudio: audio);
      await step('init.high.audio=$audio', () => c.initialize());
      show(CameraPreview(c), 'audio');
      try { await step('video.3s.audio=$audio', () => rec(c, 3000), secs: 20, desc: (s) => s); } catch (_) {}
      await c.dispose();
      return;
    }

    // ---- ResolutionPreset.max, audio on
    final c = CameraController(back, test == 'stream-yuv' ? ResolutionPreset.high : ResolutionPreset.max, enableAudio: test != 'stream-yuv',
        imageFormatGroup: test == 'stream-yuv' ? ImageFormatGroup.yuv420 : null);
    await step('init.max', () => c.initialize(), desc: (_) => 'previewSize=${c.value.previewSize} aspect=${c.value.aspectRatio.toStringAsFixed(3)} '
        'flash=${c.value.flashMode.name} exposurePointSupported=${c.value.exposurePointSupported} focusPointSupported=${c.value.focusPointSupported} '
        'deviceOrientation=${c.value.deviceOrientation}');
    show(CameraPreview(c), 'preview max');
    await shot('preview-max');

    if (!test.startsWith('stream')) {
      try {
        await step('takePicture.max', () async {
          final f = await c.takePicture();
          final len = await File(f.path).length();
          log('path=${f.path} bytes=$len');
          return '${f.path} $len B';
        }, desc: (s) => s);
      } catch (_) {}
      try {
        await step('video.3s.audio=true', () => rec(c, 3000), secs: 20, desc: (s) => s);
      } catch (_) {}
    }

    try {
      await step('imageStream.3s', () async {
        int n = 0; CameraImage? first; final sw = Stopwatch();
        await c.startImageStream((img) { if (n == 0) { first = img; sw.start(); } n++; });
        await sleep(3000);
        await c.stopImageStream();
        if (n == 0) throw 'no frames';
        final f = first!;
        final fps = (n - 1) / (sw.elapsedMilliseconds / 1000);
        return 'frames=$n fps=${fps.toStringAsFixed(1)} fmt=${f.format.group.name}(raw ${f.format.raw}) ${f.width}x${f.height} planes=${f.planes.length} '
            'bpr=${f.planes.map((p) => p.bytesPerRow).toList()} sensorExposureTime=${f.sensorExposureTime} sensitivity=${f.sensorSensitivity} aperture=${f.lensAperture} '
            'sensorOrientation=${c.description.sensorOrientation}';
      }, desc: (s) => s);
    } catch (_) {}
    if (test.startsWith('stream')) { await c.dispose(); return; }

    try {
      await step('flash.torch', () async { await c.setFlashMode(FlashMode.torch); await sleep(500); final m = c.value.flashMode; await c.setFlashMode(FlashMode.off); return m; },
          desc: (m) => 'mode=${m.name}');
    } catch (_) {}
    try {
      await step('flash.always.photo', () async {
        await c.setFlashMode(FlashMode.always);
        final f = await c.takePicture(); log('path=${f.path} (flash always)');
        await c.setFlashMode(FlashMode.off); return f.path;
      }, desc: (s) => s);
    } catch (_) {}
    try {
      await step('zoom', () async {
        final mx = await c.getMaxZoomLevel(); final mn = await c.getMinZoomLevel();
        await c.setZoomLevel(mx >= 2 ? 2.0 : mx); return 'min=$mn max=$mx set=${mx >= 2 ? 2.0 : mx}';
      }, desc: (s) => s);
    } catch (_) {}
    try { await step('focusPoint', () => c.setFocusPoint(const Offset(0.3, 0.6))); } catch (_) {}
    try { await step('exposurePoint', () => c.setExposurePoint(const Offset(0.3, 0.6))); } catch (_) {}
    try {
      await step('exposureOffset', () async {
        final mn = await c.getMinExposureOffset(); final mx = await c.getMaxExposureOffset(); final st = await c.getExposureOffsetStepSize();
        final r = await c.setExposureOffset(mx > 0 ? 1.0.clamp(mn, mx) : 0); return 'min=$mn max=$mx step=$st set->$r';
      }, desc: (s) => s);
    } catch (_) {}
    try {
      await step('lockCaptureOrientation', () async {
        await c.lockCaptureOrientation(DeviceOrientation.portraitUp);
        final l = c.value.lockedCaptureOrientation;
        final f = await c.takePicture();
        log('path=${f.path} (locked portraitUp)');
        await c.unlockCaptureOrientation();
        return 'locked=$l unlocked=${c.value.lockedCaptureOrientation}';
      }, desc: (s) => s);
    } catch (_) {}
    try { await c.setZoomLevel(1.0); } catch (_) {}
    await shot('preview-after-controls', 500);
    await c.dispose();

    // ---- ResolutionPreset.high, audio off
    final h = CameraController(back, ResolutionPreset.high, enableAudio: false);
    try {
      await step('init.high', () => h.initialize(), desc: (_) => 'previewSize=${h.value.previewSize}');
      show(CameraPreview(h), 'preview high');
      await shot('preview-high');
      await step('takePicture.high', () async { final f = await h.takePicture(); log('path=${f.path}'); return f.path; }, desc: (s) => s);
      await step('video.3s.audio=false', () => rec(h, 3000), secs: 20, desc: (s) => s);
    } catch (_) {}
    await h.dispose();

    final front = cams.where((c) => c.lensDirection == CameraLensDirection.front);
    if (front.isNotEmpty) {
      final fc = CameraController(front.first, ResolutionPreset.high, enableAudio: false);
      try {
        await step('init.front', () => fc.initialize(), desc: (_) => 'previewSize=${fc.value.previewSize}');
        show(CameraPreview(fc), 'front');
        await shot('preview-front');
        await step('takePicture.front', () async { final f = await fc.takePicture(); log('path=${f.path}'); return f.path; }, desc: (s) => s);
      } catch (_) {}
      await fc.dispose();
    }
  }

  Future<void> scannerTest() async {
    // QR + EAN-13 from the default scene
    final ctl = MobileScannerController(formats: const [BarcodeFormat.qrCode, BarcodeFormat.ean13]);
    final seen = <String, String>{};
    final got = Completer<String>();
    int errs = 0;
    final sub = ctl.barcodes.listen(onError: (e) { if (errs++ < 3) log('scanner stream error $e'); }, (cap) {
      for (final b in cap.barcodes) {
        if (!seen.containsKey(b.format.name)) { seen[b.format.name] = '${b.rawValue}'; log('INFO scanner saw format=${b.format.name} raw="${b.rawValue}" corners=${b.corners.length} size=${cap.size}'); }
      }
      if (seen.containsKey('qrCode') && seen.containsKey('ean13') && !got.isCompleted) got.complete(seen.toString());
    });
    ctl.addListener(() { if (ctl.value.error != null) log('scanner error ${ctl.value.error}'); });
    show(MobileScanner(controller: ctl, errorBuilder: (ctx, e) => Center(child: Text('scanner error: $e', style: const TextStyle(color: Colors.white)))), 'scanner');
    try {
      await step('scanner.qr+ean13', () => got.future, secs: 20, desc: (s) => s);
    } catch (_) { log('INFO scanner saw $seen'); }
    await shot('scanner', 200);
    await sub.cancel();
    await ctl.dispose();
  }

  // The QR payloads mobile_scanner reports: `QRSEEN src=flutter t=<ms since start> at=<Unix ms>
  // pct=<Uri.encodeComponent>` each time
  // the payload differs from the last one (scripts/qr-test.sh compares them).
  Future<void> qrTest() async {
    final front = Platform.environment['M7_QR_POS'] == 'front';
    final secs = int.tryParse(Platform.environment['M7_QR_SECS'] ?? '') ?? 10;
    log('BEGIN qr pos=${front ? 'front' : 'back'} secs=$secs');
    final ctl = MobileScannerController(formats: const [BarcodeFormat.qrCode],
        facing: front ? CameraFacing.front : CameraFacing.back);
    final sw = Stopwatch()..start();
    String? last;
    var sized = false;
    final sub = ctl.barcodes.listen(onError: (e) => log('qr stream error $e'), (cap) {
      if (!sized) { sized = true; log('INFO qr analysed size=${cap.size}'); }
      for (final b in cap.barcodes) {
        final v = b.rawValue;
        if (b.format != BarcodeFormat.qrCode || v == null || v == last) continue;
        last = v;
        log('QRSEEN src=flutter t=${sw.elapsedMilliseconds} at=${DateTime.now().millisecondsSinceEpoch} pct=${Uri.encodeComponent(v)}');
      }
    });
    ctl.addListener(() { if (ctl.value.error != null) log('qr scanner error ${ctl.value.error}'); });
    show(MobileScanner(controller: ctl), 'qr');
    await sleep(secs * 1000);
    await sub.cancel();
    await ctl.dispose();
  }

  @override
  Widget build(BuildContext context) => MaterialApp(
        home: Scaffold(
          backgroundColor: Colors.black,
          body: SafeArea(child: Stack(children: [Positioned.fill(child: body),
            Positioned(left: 8, bottom: 8, child: Text(status, style: const TextStyle(color: Colors.white)))])),
        ),
      );
}
