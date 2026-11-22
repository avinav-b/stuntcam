// SPDX-License-Identifier: Apache-2.0
// Auto-running camera compatibility probe.
// Test selection: launch arg `-CAMTEST vc|expo|vcfull|vclidar|vcqr|expoqr|…` (NSUserDefaults, read via RN Settings).
// Every result line is prefixed `M1B|` and also mirrored to Documents/m1b.log
// so the host can read it from the app container without Metro.
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { NativeModules, Settings, StyleSheet, Text, View } from 'react-native';
import { File, FileMode, Paths } from 'expo-file-system';
import {
  Camera as VCamera,
  type CameraDevice,
  type CameraOutput,
  type CameraPhotoOutput,
  type CameraRef,
  type Constraint,
  type FlashMode,
  type MirrorMode,
  type PhotoFile,
  type Recorder,
  VisionCamera,
  isScannedCode,
  useCameraDevice,
  useCameraDevices,
  useObjectOutput,
  usePhotoOutput,
  useVideoOutput,
} from 'react-native-vision-camera';
import { CameraView, Camera as ExpoCamera } from 'expo-camera';

const lines: string[] = [];
const logFile = new File(Paths.document, 'm1b.log');
function L(msg: string) {
  const line = `M1B| ${new Date().toISOString().slice(11, 23)} ${msg}`;
  console.log(line);
  lines.push(line);
  try {
    logFile.writeSync(lines.join('\n') + '\n');
  } catch (e) {
    console.log('M1B| logfile write failed ' + String(e));
  }
}
const AUDIO = String(Settings.get('AUDIO') ?? '0') === '1';
const CYCLES = Number(Settings.get('CYCLES') ?? 20);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
function withTimeout<T>(p: Promise<T>, ms: number, what: string): Promise<T> {
  return Promise.race([
    p,
    new Promise<T>((_, rej) => setTimeout(() => rej(new Error(`TIMEOUT ${ms}ms in ${what}`)), ms)),
  ]);
}
const errStr = (e: any): string =>
  `${e?.name ?? 'Error'}: ${e?.message ?? String(e)}${e?.code ? ` code=${e.code}` : ''}${e?.cause ? ` cause=[${errStr(e.cause)}]` : ''}`;
async function step(name: string, fn: () => Promise<string | void>, ms = 15000) {
  try {
    const r = await withTimeout(Promise.resolve().then(fn), ms, name);
    L(`PASS ${name}${r ? ' ' + r : ''}`);
    return true;
  } catch (e) {
    L(`FAIL ${name} ${errStr(e)}`);
    return false;
  }
}
function fileInfo(pathOrUri: string) {
  const uri = pathOrUri.startsWith('file://') ? pathOrUri : 'file://' + pathOrUri;
  const f = new File(uri);
  return `exists=${f.exists} size=${f.exists ? f.size : -1} path=${uri.replace('file://', '')}`;
}

// ---------------------------------------------------------------- VisionCamera
function VcTest() {
  const device = useCameraDevice('back');
  const all = useCameraDevices();
  const [phase, setPhase] = useState<'wait' | 'photo' | 'video' | 'done'>('wait');
  const photoOutput = usePhotoOutput({});
  const videoOutput = useVideoOutput({ enableAudio: AUDIO });
  const camRef = useRef<CameraRef>(null);
  const started = useRef<(() => void) | null>(null);
  const startedP = useRef<Promise<void> | null>(null);
  const armStarted = () => {
    startedP.current = new Promise<void>((res) => (started.current = res));
  };
  const deviceRef = useRef(device);
  deviceRef.current = device;
  const allRef = useRef(all);
  allRef.current = all;

  useEffect(() => {
    (async () => {
      L('BEGIN vc');
      await step('vc.permission', async () => {
        let s: string = VisionCamera.cameraPermissionStatus;
        if (s !== 'authorized') {
          const ok = await VisionCamera.requestCameraPermission();
          s = VisionCamera.cameraPermissionStatus + ` (requested->${ok})`;
        }
        if (!s.startsWith('authorized')) throw new Error('status=' + s);
        return 'status=' + s;
      });
      if (AUDIO) await step('vc.micPermission', async () => {
        let s: string = VisionCamera.microphonePermissionStatus;
        if (s !== 'authorized') s += ` (requested->${await VisionCamera.requestMicrophonePermission()})`;
        return 'status=' + s;
      });
      for (let i = 0; i < 30 && !deviceRef.current; i++) await sleep(200);
      const d = deviceRef.current;
      const a = allRef.current;
      L(`INFO vc.allDevices count=${a.length} ${a.map((x) => `${x.localizedName}/${x.position}/${x.type}`).join(', ')}`);
      if (!d) {
        L('FAIL vc.device useCameraDevice(back) returned undefined');
        L('INFO UI shows NO CAMERA');
        L('END vc');
        setPhase('done');
        return;
      }
      L(`PASS vc.device id=${d.id} name=${d.localizedName} pos=${d.position} type=${d.type} virtual=${d.isVirtualDevice} hasTorch=${d.hasTorch} hasFlash=${d.hasFlash} zoom=${d.minZoom}..${d.maxZoom} focusMetering=${d.supportsFocusMetering} pixelFormats=${d.supportedPixelFormats.join('/')} fps=${JSON.stringify(d.supportedFPSRanges)}`);
      for (const t of ['photo', 'video', 'stream'] as const) {
        await step(`vc.resolutions.${t}`, async () => {
          const rs = d.getSupportedResolutions(t);
          const max = rs.reduce((x, y) => (y.width * y.height > x.width * x.height ? y : x), { width: 0, height: 0 });
          return `count=${rs.length} max=${max.width}x${max.height} all=${rs.map((r) => `${r.width}x${r.height}`).join(',')}`;
        });
      }
      await step('vc.supportsOutput', async () => `photo=${d.supportsOutput(photoOutput)} video=${d.supportsOutput(videoOutput)}`);
      armStarted();
      setPhase('photo');
    })();
  }, []);

  useEffect(() => {
    if (phase === 'photo') {
      (async () => {
        await step('vc.photo.onStarted', () => startedP.current!, 15000);
        await sleep(5000);
        L('SCREENSHOT vc-photo');
        await sleep(1500);
        await step('vc.photo.capture', async () => {
          const pf = await photoOutput.capturePhotoToFile({ flashMode: 'off', enableShutterSound: false }, {});
          return `w=${pf.width} h=${pf.height} orientation=${pf.orientation} mirrored=${pf.isMirrored} ${fileInfo(pf.filePath)}`;
        });
        const c = () => camRef.current?.controller;
        await step('vc.controller', async () => {
          if (!c()) throw new Error('controller undefined');
        });
        await step('vc.torch.on', async () => c()!.setTorchMode('on'));
        await step('vc.torch.off', async () => c()!.setTorchMode('off'));
        await step('vc.zoom.set2', async () => {
          await c()!.setZoom(Math.min(2, deviceRef.current!.maxZoom));
          return `zoom=${(c() as any).zoom}`;
        });
        await step('vc.focusTo', async () => {
          await camRef.current!.focusTo({ x: 150, y: 250 });
        });
        armStarted();
        setPhase('video');
      })();
    } else if (phase === 'video') {
      (async () => {
        await step('vc.video.onStarted', () => startedP.current!, 15000);
        await sleep(1500);
        let finished: (v: string) => void = () => {};
        const finishedP = new Promise<string>((r) => (finished = r));
        let recErr: Error | null = null;
        let recorder: any;
        const okStart = await step('vc.video.start', async () => {
          recorder = await videoOutput.createRecorder({});
          await recorder.startRecording(
            (path: string, reason: string) => finished(`${reason} ${fileInfo(path)}`),
            (e: Error) => {
              recErr = e;
              L(`FAIL vc.video.onRecordingError ${errStr(e)}`);
            },
          );
          return 'filePath=' + recorder.filePath;
        });
        if (okStart) {
          await sleep(3000);
          await step('vc.video.stop', async () => {
            await recorder.stopRecording();
            return `recordedDuration=${recorder.recordedDuration} size=${recorder.recordedFileSize}`;
          });
          await step('vc.video.finished', async () => {
            if (recErr) throw recErr;
            return await finishedP;
          });
        }
        L('END vc');
        setPhase('done');
      })();
    }
  }, [phase]);

  if (!device && phase === 'done') return <Text style={styles.t}>NO CAMERA (vc)</Text>;
  if (!device || phase === 'wait' || phase === 'done') return <Text style={styles.t}>vc: {phase} device={String(!!device)}</Text>;
  return (
    <VCamera
      key={phase}
      ref={camRef}
      style={StyleSheet.absoluteFill}
      device={device}
      isActive
      orientationSource={((Settings.get('VCORIENT') as string | undefined) ?? 'device') as any}
      outputs={phase === 'photo' ? [photoOutput] : [videoOutput]}
      onStarted={() => {
        L(`INFO vc.${phase}.onStarted fired`);
        started.current?.();
      }}
      onConfigured={() => L(`INFO vc.${phase}.onConfigured`)}
      onError={(e) => L(`FAIL vc.${phase}.onError ${errStr(e)}`)}
    />
  );
}

// ---------------------------------------------------------------- expo-camera
function ExpoTest() {
  const ref = useRef<CameraView>(null);
  const [mode, setMode] = useState<'picture' | 'video' | null>(null);
  const readyRes = useRef<(() => void) | null>(null);
  const readyP = useRef<Promise<void> | null>(null);
  const arm = () => (readyP.current = new Promise<void>((r) => (readyRes.current = r)));
  const scanned = useRef<string | null>(null);

  useEffect(() => {
    (async () => {
      L('BEGIN expo');
      await step('expo.permission', async () => {
        let p = await ExpoCamera.getCameraPermissionsAsync();
        if (!p.granted) p = await ExpoCamera.requestCameraPermissionsAsync();
        if (!p.granted) throw new Error('status=' + p.status);
        return 'status=' + p.status;
      });
      if (AUDIO) await step('expo.micPermission', async () => {
        let p = await ExpoCamera.getMicrophonePermissionsAsync();
        if (!p.granted) p = await ExpoCamera.requestMicrophonePermissionsAsync();
        return 'status=' + p.status;
      });
      await step('expo.isAvailable', async () => {
        const a = await (CameraView as any).isAvailableAsync?.();
        return 'isAvailableAsync=' + a;
      });
      arm();
      setMode('picture');
      await step('expo.onCameraReady', () => readyP.current!, 15000);
      await sleep(5000);
      L('SCREENSHOT expo-picture');
      await sleep(1500);
      await step('expo.lenses', async () => JSON.stringify(await ref.current!.getAvailableLensesAsync()));
      await step('expo.takePicture', async () => {
        const p = await ref.current!.takePictureAsync({ exif: true, quality: 0.8 });
        if (!p) throw new Error('takePictureAsync returned undefined');
        const ex: any = p.exif ?? {};
        return `w=${p.width} h=${p.height} exif.Orientation=${ex.Orientation} exif.keys=${Object.keys(ex).length} ${fileInfo(p.uri)}`;
      });
      await step('expo.takePicture.noexif', async () => {
        const p = await ref.current!.takePictureAsync({ exif: false, quality: 0.8 });
        if (!p) throw new Error('takePictureAsync returned undefined');
        return `w=${p.width} h=${p.height} ${fileInfo(p.uri)}`;
      });
      await step('expo.barcode.qr', async () => {
        for (let i = 0; i < 50 && !scanned.current; i++) await sleep(200);
        if (!scanned.current) throw new Error('no onBarcodeScanned event within 10 s');
        return scanned.current;
      }, 12000);
      arm();
      setMode('video');
      await step('expo.video.onCameraReady', () => readyP.current!, 15000);
      await sleep(1500);
      await step('expo.recordAsync', async () => {
        const recP = ref.current!.recordAsync({ maxDuration: 10 });
        await sleep(3000);
        ref.current!.stopRecording();
        const r = await recP;
        if (!r) throw new Error('recordAsync resolved undefined');
        return fileInfo(r.uri);
      }, 20000);
      L('END expo');
      setMode(null);
    })();
  }, []);

  if (!mode) return <Text style={styles.t}>expo: idle</Text>;
  return (
    <CameraView
      key={mode}
      ref={ref}
      style={StyleSheet.absoluteFill}
      facing="back"
      mode={mode}
      mute={!AUDIO}
      barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
      onBarcodeScanned={(e) => {
        if (!scanned.current) {
          scanned.current = `type=${e.type} data=${JSON.stringify(e.data)}`;
          L(`INFO expo.onBarcodeScanned ${scanned.current}`);
        }
      }}
      onCameraReady={() => {
        L(`INFO expo.${mode}.onCameraReady fired`);
        readyRes.current?.();
      }}
      onMountError={(e) => L(`FAIL expo.${mode}.onMountError ${e.message}`)}
    />
  );
}

// ---------------------------------------------------------------- open/close cycles (soak)
function CycleTest({ lib }: { lib: 'vc' | 'expo' }) {
  const device = useCameraDevice('back');
  const deviceRef = useRef(device);
  deviceRef.current = device;
  const photoOutput = usePhotoOutput({});
  const [n, setN] = useState(-1);
  const res = useRef<(() => void) | null>(null);
  useEffect(() => {
    (async () => {
      L(`BEGIN cycle-${lib} n=${CYCLES}`);
      if (lib === 'vc') { for (let i = 0; i < 30 && !deviceRef.current; i++) await sleep(200); if (!deviceRef.current) { L('FAIL cycle.device none'); L(`END cycle-${lib}`); return; } }
      else { const p = await ExpoCamera.requestCameraPermissionsAsync(); if (!p.granted) L('FAIL cycle.permission'); }
      await sleep(1000);
      L('MEMMARK start');
      await sleep(1500);
      let ok = 0;
      for (let i = 0; i < CYCLES; i++) {
        const p = new Promise<void>((r) => (res.current = r));
        setN(i);
        const good = await step(`cycle.${lib}.${i}.started`, () => p, 10000);
        if (good) ok++;
        await sleep(700);
        setN(-1);
        await sleep(400);
      }
      L(`INFO cycle ok=${ok}/${CYCLES}`);
      await sleep(1500);
      L('MEMMARK after');
      await sleep(1500);
      L(`END cycle-${lib}`);
    })();
  }, []);
  if (n < 0) return <Text style={styles.t}>cycle {lib}: closed</Text>;
  if (lib === 'vc')
    return <VCamera key={n} style={StyleSheet.absoluteFill} device={deviceRef.current!} isActive outputs={[photoOutput]}
      onStarted={() => res.current?.()} onError={(e) => L(`FAIL cycle.onError ${errStr(e)}`)} />;
  return <CameraView key={n} style={StyleSheet.absoluteFill} facing="back" mode="picture" mute
    onCameraReady={() => res.current?.()} onMountError={(e) => L(`FAIL cycle.onMountError ${e.message}`)} />;
}


// ---------------------------------------------------------------- orientation (preview screenshot + photo + movie), barcodes
const CAM = ((Settings.get('CAM') as string | undefined) ?? 'back') as 'back' | 'front';
function VcOrient() {
  const device = useCameraDevice(CAM);
  const photoOutput = usePhotoOutput({});
  const videoOutput = useVideoOutput({ enableAudio: false });
  const started = useRef<(() => void) | null>(null);
  // made once: `useRef(new Promise(…))` would run the executor at every render and leave `started` holding the resolver
  // of a promise nobody awaits (the component renders twice when the device list arrives after the first render)
  const startedP = useRef<Promise<void> | null>(null);
  if (startedP.current == null) startedP.current = new Promise<void>((r) => (started.current = r));
  const deviceRef = useRef(device); deviceRef.current = device;
  useEffect(() => {
    (async () => {
      L(`BEGIN vcorient cam=${CAM}`);
      if (VisionCamera.cameraPermissionStatus !== 'authorized') await VisionCamera.requestCameraPermission();
      await step('vco.onStarted', () => startedP.current!, 15000);
      await sleep(2500);
      L('SCREENSHOT preview'); await sleep(1500);
      await step('vco.photo', async () => {
        const pf = await photoOutput.capturePhotoToFile({ flashMode: 'off', enableShutterSound: false }, {});
        return `orientation=${pf.orientation} mirrored=${pf.isMirrored} ${fileInfo(pf.filePath)}`;
      });
      await step('vco.video', async () => {
        const rec: any = await videoOutput.createRecorder({});
        let fin: (v: string) => void = () => {}; const finP = new Promise<string>((r) => (fin = r));
        await rec.startRecording((path: string, reason: string) => fin(`${reason} ${fileInfo(path)}`), (e: Error) => L(`FAIL vco.video.onRecordingError ${errStr(e)}`));
        await sleep(2000); await rec.stopRecording();
        return await finP;
      }, 20000);
      L('END vcorient');
    })();
  }, []);
  if (!device) return <Text style={styles.t}>no device</Text>;
  return <VCamera style={StyleSheet.absoluteFill} device={device} isActive orientationSource={((Settings.get('VCORIENT') as string | undefined) ?? 'device') as any}
    outputs={[photoOutput, videoOutput]} onStarted={() => started.current?.()} onError={(e) => L(`FAIL vco.onError ${errStr(e)}`)} />;
}
function ExpoOrient() {
  const ref = useRef<CameraView>(null);
  const [mode, setMode] = useState<'picture' | 'video' | null>(null);
  const readyRes = useRef<(() => void) | null>(null);
  const readyP = useRef<Promise<void> | null>(null);
  const arm = () => (readyP.current = new Promise<void>((r) => (readyRes.current = r)));
  const codes = useRef<Record<string, string>>({});
  const responsive = String(Settings.get('RESPONSIVE') ?? '0') === '1';
  useEffect(() => {
    (async () => {
      L(`BEGIN expoorient cam=${CAM} responsive=${responsive}`);
      const p = await ExpoCamera.getCameraPermissionsAsync(); if (!p.granted) await ExpoCamera.requestCameraPermissionsAsync();
      arm(); setMode('picture');
      await step('eo.onCameraReady', () => readyP.current!, 15000);
      await sleep(2500);
      L('SCREENSHOT preview'); await sleep(1500);
      await step('eo.photo', async () => {
        const ph = await ref.current!.takePictureAsync({ exif: true, quality: 0.9 });
        return `w=${ph!.width} h=${ph!.height} exif.Orientation=${(ph!.exif as any)?.Orientation} ${fileInfo(ph!.uri)}`;
      });
      await step('eo.barcodes', async () => {
        for (let i = 0; i < 40 && Object.keys(codes.current).length < 2; i++) await sleep(200);
        const k = Object.keys(codes.current);
        if (!k.includes('qr') || !k.some((t) => t.startsWith('ean13'))) throw new Error('got ' + JSON.stringify(codes.current));
        return JSON.stringify(codes.current);
      }, 12000);
      arm(); setMode('video');
      await step('eo.video.onCameraReady', () => readyP.current!, 15000);
      await sleep(1000);
      await step('eo.video', async () => {
        const recP = ref.current!.recordAsync({ maxDuration: 10 });
        await sleep(2500); ref.current!.stopRecording();
        const r = await recP; return fileInfo(r!.uri);
      }, 20000);
      L('END expoorient');
      setMode(null);
    })();
  }, []);
  if (!mode) return <Text style={styles.t}>expo: idle</Text>;
  return (
    <CameraView key={mode} ref={ref} style={StyleSheet.absoluteFill} facing={CAM} mode={mode} mute responsiveOrientationWhenOrientationLocked={responsive}
      barcodeScannerSettings={{ barcodeTypes: ['qr', 'ean13'] }}
      onBarcodeScanned={(e) => { if (!codes.current[e.type]) { codes.current[e.type] = String(e.data); L(`INFO eo.onBarcodeScanned type=${e.type} data=${e.data}`); } }}
      onCameraReady={() => readyRes.current?.()} onMountError={(e) => L(`FAIL eo.onMountError ${e.message}`)} />
  );
}

// ---------------------------------------------------------------- vcfull: exhaustive, generic VisionCamera checks
// Written from fixtures/expo/README.md ("vcfull") and VisionCamera's own TypeScript types. Every check is a step()
// named vcf.*; the full list with its denominator is in the README. The pre-registered failures are VCF_EXPECTED and the
// pre-registered n/a passes VCF_EXPECTED_NA: anything else is reported as unexpected.
type Sz = { width: number; height: number };
const VCF_EXPECTED = new Set([
  'vcf.video.4k60.fps', // the simulator's software encoder manages 16-32 fps at 4K60 (v1.0 cap)
  'vcf.format.video.1280x720', // engine gap: no 720p format in the profiles
  'vcf.zoom.ramp', // engine gap: zoom ramps never reach the engine
  'vcf.front.mirror', // engine gap: mirrored front photos keep EXIF Orientation 6
]);
const VCF_EXPECTED_NA = new Set([
  'vcf.format.video.1280x720.fps30', 'vcf.format.video.1280x720.fps60', // follow from the missing 720p format
  'vcf.focus.locked', // engine gap: focus locking not offered
]);
const VCF_TOTAL = (audio: boolean) => (audio ? 109 : 102); // the README's denominator
const sameSize = (a?: Sz, b?: Sz) =>
  !!a && !!b && ((a.width === b.width && a.height === b.height) || (a.width === b.height && a.height === b.width));
const szStr = (s?: Sz) => (s ? `${s.width}x${s.height}` : 'none');
const area = (s: Sz) => s.width * s.height;
const near = (a: number, b: number, eps = 0.01) => Math.abs(a - b) <= eps;
// RN's performance.now() is ms since boot on the host clock, the clock VisionCamera timestamps photos with (in seconds)
const hostNow = () => (globalThis as any).performance.now() / 1000;
const toUri = (p: string) => (p.startsWith('file://') ? p : 'file://' + p);
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
async function waitFor(cond: () => boolean, ms: number, what: string) {
  for (const t0 = Date.now(); !cond(); await sleep(50)) if (Date.now() - t0 > ms) throw new Error(`TIMEOUT ${ms}ms waiting for ${what}`);
}

// --- byte helpers (photo files are read whole; movie files only box by box)
const u16 = (b: Uint8Array, o: number, le = false) => (le ? b[o] | (b[o + 1] << 8) : (b[o] << 8) | b[o + 1]);
const u32 = (b: Uint8Array, o: number, le = false) =>
  (le ? b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24) : (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const u64 = (b: Uint8Array, o: number) => u32(b, o) * 2 ** 32 + u32(b, o + 4);
const fourcc = (b: Uint8Array, o: number) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
function findBytes(b: Uint8Array, pat: number[], from = 0, to = b.length) {
  const p0 = pat[0];
  outer: for (let i = from; i <= to - pat.length; i++) {
    if (b[i] !== p0) continue;
    for (let j = 1; j < pat.length; j++) if (b[i + j] !== pat[j]) continue outer;
    return i;
  }
  return -1;
}
type Box = { type: string; s: number; e: number }; // s = payload start, e = end
function boxes(b: Uint8Array, start: number, end: number): Box[] {
  const out: Box[] = [];
  for (let o = start; o + 8 <= end; ) {
    let len = u32(b, o), hl = 8;
    if (len === 1) { len = u64(b, o + 8); hl = 16; } else if (len === 0) len = end - o;
    if (len < hl) break;
    out.push({ type: fourcc(b, o + 4), s: o + hl, e: Math.min(o + len, end) });
    o += len;
  }
  return out;
}
const child = (b: Uint8Array, x: Box | undefined, type: string) => (x ? boxes(b, x.s, x.e).find((y) => y.type === type) : undefined);
function photoKind(b: Uint8Array): 'jpeg' | 'heic' | 'unknown' {
  if (b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return 'jpeg';
  if (b.length > 12 && fourcc(b, 4) === 'ftyp' && ['heic', 'heix', 'mif1', 'heim', 'heis', 'hevc', 'hevx'].includes(fourcc(b, 8))) return 'heic';
  return 'unknown';
}
function jpegSize(b: Uint8Array): Sz | undefined { // first top-level SOFn (APP1 with its EXIF thumbnail is skipped by length)
  for (let o = 2; o + 9 < b.length; ) {
    if (b[o] !== 0xff) return undefined;
    const m = b[o + 1];
    if (m === 0xff) { o++; continue; }
    if (m === 0xd8 || m === 0x01 || (m >= 0xd0 && m <= 0xd7)) { o += 2; continue; }
    if (m >= 0xc0 && m <= 0xcf && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) return { height: u16(b, o + 5), width: u16(b, o + 7) };
    if (m === 0xda) return undefined;
    o += 2 + u16(b, o + 2);
  }
  return undefined;
}
function heicSize(b: Uint8Array): Sz | undefined { // largest 'ispe' in the top-level meta box (the grid/primary item)
  const meta = boxes(b, 0, b.length).find((x) => x.type === 'meta');
  if (!meta) return undefined;
  let best: Sz | undefined;
  const ISPE = [0x69, 0x73, 0x70, 0x65];
  for (let i = findBytes(b, ISPE, meta.s, meta.e); i >= 0; i = findBytes(b, ISPE, i + 1, meta.e)) {
    const s = { width: u32(b, i + 8), height: u32(b, i + 12) };
    if (!best || area(s) > area(best)) best = s;
  }
  return best;
}
function exifOrientation(b: Uint8Array): number | undefined { // "Exif\0\0" + TIFF header, IFD0 tag 0x0112 (JPEG APP1 or HEIC Exif item)
  const EXIF = [0x45, 0x78, 0x69, 0x66, 0, 0];
  for (let i = findBytes(b, EXIF); i >= 0; i = findBytes(b, EXIF, i + 1)) {
    const t = i + 6;
    const le = b[t] === 0x49 && b[t + 1] === 0x49 && b[t + 2] === 0x2a && b[t + 3] === 0;
    const be = b[t] === 0x4d && b[t + 1] === 0x4d && b[t + 2] === 0 && b[t + 3] === 0x2a;
    if (!le && !be) continue;
    const ifd = t + u32(b, t + 4, le);
    for (let k = 0, n = u16(b, ifd, le); k < n; k++) {
      const e = ifd + 2 + k * 12;
      if (u16(b, e, le) === 0x0112) return u16(b, e + 8, le);
    }
    return undefined;
  }
  return undefined;
}
type MovTrack = { handler: string; timescale: number; duration: number; frames: number; coded?: Sz; codec?: string };
type MovInfo = { duration: number; tracks: MovTrack[] };
function movInfo(path: string): MovInfo { // reads only the moov box (QuickTime/MP4 from AVAssetWriter)
  const h = new File(toUri(path)).open(FileMode.ReadOnly);
  let m: Uint8Array | undefined;
  try {
    const size = h.size ?? 0;
    for (let off = 0; off + 8 <= size; ) {
      h.offset = off;
      const hd = h.readBytesSync(16);
      let len = u32(hd, 0), hl = 8;
      if (len === 1) { len = u64(hd, 8); hl = 16; } else if (len === 0) len = size - off;
      if (len < hl) break;
      if (fourcc(hd, 4) === 'moov') { h.offset = off + hl; m = h.readBytesSync(len - hl); break; }
      off += len;
    }
  } finally {
    h.close();
  }
  if (!m) throw new Error('no moov box');
  const b = m;
  const top = boxes(b, 0, b.length);
  const ts = (x: Box) => (b[x.s] === 1 ? { scale: u32(b, x.s + 20), dur: u64(b, x.s + 24) } : { scale: u32(b, x.s + 12), dur: u32(b, x.s + 16) });
  const mvhd = top.find((x) => x.type === 'mvhd');
  if (!mvhd) throw new Error('no mvhd');
  const mv = ts(mvhd);
  const tracks: MovTrack[] = [];
  for (const trak of top.filter((x) => x.type === 'trak')) {
    const mdia = child(b, trak, 'mdia');
    const hdlr = child(b, mdia, 'hdlr');
    const mdhd = child(b, mdia, 'mdhd');
    const stbl = child(b, child(b, mdia, 'minf'), 'stbl');
    const stsd = child(b, stbl, 'stsd');
    const stts = child(b, stbl, 'stts');
    if (!hdlr || !mdhd) continue;
    const handler = fourcc(b, hdlr.s + 8);
    const md = ts(mdhd);
    let frames = 0;
    if (stts) for (let i = 0, n = u32(b, stts.s + 4); i < n; i++) frames += u32(b, stts.s + 8 + i * 8);
    const t: MovTrack = { handler, timescale: md.scale, duration: md.scale ? md.dur / md.scale : 0, frames };
    if (stsd && handler === 'vide') {
      const e = stsd.s + 8; // first sample entry: size, type, 6 reserved, data ref idx, 16 bytes, width, height
      t.codec = fourcc(b, e + 4);
      t.coded = { width: u16(b, e + 32), height: u16(b, e + 34) };
    }
    tracks.push(t);
  }
  return { duration: mv.scale ? mv.dur / mv.scale : 0, tracks };
}

type VcfStage = { key: string; device: CameraDevice; outputs: CameraOutput[]; constraints?: Constraint[]; mirrorMode?: MirrorMode };
type VcfVideo = { id: string; size: Sz; fps: number; audio: boolean; torch?: boolean };
const VCF_VIDEOS: VcfVideo[] = [
  { id: '1080p30', size: { width: 1920, height: 1080 }, fps: 30, audio: false, torch: true },
  ...(AUDIO ? [{ id: '1080p30a', size: { width: 1920, height: 1080 }, fps: 30, audio: true }] : []),
  { id: '1080p60', size: { width: 1920, height: 1080 }, fps: 60, audio: false },
  { id: '4k30', size: { width: 3840, height: 2160 }, fps: 30, audio: false },
  { id: '4k60', size: { width: 3840, height: 2160 }, fps: 60, audio: false },
];
const portrait = (s: Sz): Sz => ({ width: Math.min(s.width, s.height), height: Math.max(s.width, s.height) }); // VisionCamera's CommonResolutions convention

function VcFull() {
  const all = useCameraDevices();
  const allRef = useRef(all);
  allRef.current = all;
  const front = useCameraDevice('front');
  const frontRef = useRef(front);
  frontRef.current = front;
  const [stage, setStage] = useState<VcfStage | null>(null);
  const [status, setStatus] = useState('starting');
  const camRef = useRef<CameraRef>(null);
  const startedRes = useRef<(() => void) | null>(null);
  const startedAt = useRef(0); // host seconds of the current stage's onStarted
  const errors = useRef<string[]>([]);

  async function mount(s: VcfStage): Promise<boolean> {
    const p = new Promise<void>((r) => (startedRes.current = r));
    setStatus(s.key);
    setStage(s);
    try {
      await withTimeout(p, 15000, `${s.key}.onStarted`);
      await waitFor(() => !!camRef.current?.controller, 5000, 'controller');
      L(`INFO vcf.stage ${s.key} started device=${s.device.id}`);
      return true;
    } catch (e) {
      L(`INFO vcf.stage ${s.key} did not start: ${errStr(e)}`);
      return false;
    }
  }

  useEffect(() => {
    (async () => {
      L(`BEGIN vcfull audio=${AUDIO}`);
      try {
      const paths = new Set<string>();
      const previews: string[] = [];
      await step('vcf.permission', async () => {
        let s: string = VisionCamera.cameraPermissionStatus;
        if (s !== 'authorized') s += ` (requested->${await VisionCamera.requestCameraPermission()})`;
        if (VisionCamera.cameraPermissionStatus !== 'authorized') throw new Error('status=' + s);
        return 'status=' + s;
      });
      if (AUDIO) await step('vcf.micPermission', async () => {
        let s: string = VisionCamera.microphonePermissionStatus;
        if (s !== 'authorized') s += ` (requested->${await VisionCamera.requestMicrophonePermission()})`;
        if (VisionCamera.microphonePermissionStatus !== 'authorized') throw new Error('status=' + s);
        return 'status=' + s;
      });
      for (let i = 0; i < 40 && !allRef.current.some((x) => x.position === 'back'); i++) await sleep(200);
      L(`INFO vcf.allDevices count=${allRef.current.length} ${allRef.current.map((x) => `${x.localizedName}/${x.position}/${x.type}/${x.physicalDevices.length}`).join(', ')}`);

      // ---- device
      const backs = allRef.current.filter((x) => x.position === 'back');
      const d = backs.reduce<CameraDevice | undefined>((a, x) => (!a || x.physicalDevices.length > a.physicalDevices.length ? x : a), undefined);
      await step('vcf.device.back', async () => {
        if (!d) throw new Error(`no back camera among ${allRef.current.length} devices`);
        return `id=${d.id} name=${d.localizedName} type=${d.type} virtual=${d.isVirtualDevice} constituents=${d.physicalDevices.length}`;
      });
      await step('vcf.device.triple', async () => {
        if (!d) throw new Error('no back camera');
        const offered = backs.some((x) => x.type === 'triple');
        if (offered && d.type !== 'triple') throw new Error(`profile offers a triple camera but the most-constituent back camera is ${d.type}`);
        return offered ? 'type=triple' : `no triple offered; type=${d.type}`;
      });
      await step('vcf.device.constituents', async () => {
        if (!d) throw new Error('no back camera');
        const types = d.physicalDevices.map((x) => x.type);
        if (!d.isVirtualDevice || types.length < 2) throw new Error(`not a virtual device: virtual=${d.isVirtualDevice} constituents=[${types}]`);
        if (d.type === 'triple') for (const t of ['ultra-wide-angle', 'wide-angle', 'telephoto']) if (!types.includes(t as any)) throw new Error(`triple without ${t}: [${types}]`);
        return `types=[${types.join(',')}]`;
      });
      await step('vcf.device.zoomSwitchFactors', async () => {
        if (!d) throw new Error('no back camera');
        if (!d.zoomLensSwitchFactors.length) throw new Error('zoomLensSwitchFactors=[]');
        return `factors=[${d.zoomLensSwitchFactors.join(',')}] constituents=${d.physicalDevices.length}`;
      });
      await step('vcf.device.zoomRange', async () => {
        if (!d) throw new Error('no back camera');
        if (!(d.minZoom < d.maxZoom)) throw new Error(`minZoom=${d.minZoom} maxZoom=${d.maxZoom}`);
        return `zoom=${d.minZoom}..${d.maxZoom}`;
      });

      // ---- formats
      const videoRes = d ? d.getSupportedResolutions('video') : [];
      const photoRes = d ? d.getSupportedResolutions('photo') : [];
      L(`INFO vcf.format.lists video=${videoRes.map(szStr).join(',')} photo=${photoRes.map(szStr).join(',')} fps=${JSON.stringify(d?.supportedFPSRanges ?? [])}`);
      for (const r of [{ width: 3840, height: 2160 }, { width: 1920, height: 1080 }, { width: 1280, height: 720 }]) {
        await step(`vcf.format.video.${szStr(r)}`, async () => {
          if (!videoRes.some((x) => sameSize(x, r))) throw new Error(`not in ${videoRes.length} video resolutions`);
          return 'listed';
        });
        for (const fps of [30, 60]) {
          await step(`vcf.format.video.${szStr(r)}.fps${fps}`, async () => {
            if (!d) throw new Error('no back camera');
            if (!videoRes.some((x) => sameSize(x, r))) return `n/a: ${szStr(r)} not offered (vcf.format.video.${szStr(r)})`;
            if (!d.supportsFPS(fps)) return `n/a: device does not offer ${fps} fps`;
            const out = VisionCamera.createVideoOutput({ targetResolution: portrait(r) });
            const cfg = await VisionCamera.resolveConstraints(d, [{ output: out, mirrorMode: 'auto' }], [{ resolutionBias: out }, { fps }]);
            if (cfg.selectedFPS !== fps) throw new Error(`resolveConstraints(resolution first, fps ${fps}) selected fps=${cfg.selectedFPS}`);
            return `selectedFPS=${cfg.selectedFPS}`;
          });
        }
      }
      const maxPhoto = photoRes.reduce<Sz | undefined>((a, x) => (!a || area(x) > area(a) ? x : a), undefined);
      await step('vcf.format.photo.max', async () => {
        if (!maxPhoto) throw new Error('no photo resolutions');
        const max43 = photoRes.filter((x) => Math.abs(Math.max(x.width, x.height) / Math.min(x.width, x.height) - 4 / 3) < 0.01)
          .reduce<Sz | undefined>((a, x) => (!a || area(x) > area(a) ? x : a), undefined);
        if (!sameSize(max43, { width: 4032, height: 3024 })) throw new Error(`largest 4:3 photo resolution ${szStr(max43)}, expected 4032x3024 (max ${szStr(maxPhoto)})`);
        return `max=${szStr(maxPhoto)} max4:3=${szStr(max43)}`;
      });

      // ---- depth off
      await step('vcf.depth.unsupported', async () => {
        if (!d) throw new Error('no back camera');
        if (d.mediaTypes.includes('depth')) throw new Error(`mediaTypes=[${d.mediaTypes}]`);
        return `mediaTypes=[${d.mediaTypes}]`;
      });
      const target = maxPhoto ?? { width: 4032, height: 3024 };
      const photoA = VisionCamera.createPhotoOutput({
        targetResolution: portrait(target), containerFormat: 'jpeg', quality: 0.9, qualityPrioritization: 'balanced',
        previewImageTargetSize: { width: 120, height: 160 },
      });
      const errs0 = errors.current.length;
      const started = d ? await mount({ key: 'photo', device: d, outputs: [photoA] }) : false;
      const t0 = startedAt.current;
      await step('vcf.camera.started', async () => {
        if (!started) throw new Error('back camera did not start (onStarted within 15 s)');
        return `device=${d!.id}`;
      });
      await sleep(1000);
      await step('vcf.depth.noError', async () => {
        const e = errors.current.slice(errs0);
        if (e.length) throw new Error(`onError x${e.length}: ${e[0]}`);
        return 'no onError with default options';
      });

      // ---- controls
      const cam = () => { if (!camRef.current?.controller) throw new Error('no controller'); return camRef.current; };
      const c = () => cam().controller!;
      await step('vcf.zoom.set2', async () => {
        await c().setZoom(2);
        if (!near(c().zoom, 2)) throw new Error(`zoom read back ${c().zoom}`);
        return `zoom=${c().zoom} displayable=${c().displayableZoomFactor}`;
      }, 3000);
      await step('vcf.zoom.ramp', async () => { // pinch-equivalent: startZoomAnimation resolves when the ramp ends
        const t = Date.now();
        await withTimeout(cam().startZoomAnimation(4, 4), 4000, 'ramp 2->4');
        if (!near(c().zoom, 4)) throw new Error(`zoom after ramp up read back ${c().zoom}`);
        const up = Date.now() - t;
        await withTimeout(cam().startZoomAnimation(2, 4), 4000, 'ramp 4->2');
        if (!near(c().zoom, 2)) throw new Error(`zoom after ramp back read back ${c().zoom}`);
        return `2->4 in ${up}ms, back to ${c().zoom} in ${Date.now() - t - up}ms`;
      }, 9000);
      try { await withTimeout(cam().cancelZoomAnimation(), 2000, 'cancelZoomAnimation'); await withTimeout(c().setZoom(2), 2000, 'setZoom'); }
      catch (e) { L(`INFO vcf.zoom.restore ${errStr(e)}`); }
      const pt = { x: 200, y: 400 };
      await step('vcf.focus.point', async () => {
        const t = Date.now();
        await cam().focusTo(pt);
        return `${Date.now() - t}ms focusMode=${c().focusMode}`;
      }, 2000);
      await step('vcf.focus.locked', async () => {
        if (!d!.supportsFocusLocking) return 'n/a: supportsFocusLocking=false';
        const t = Date.now();
        await cam().focusTo(pt, { adaptiveness: 'locked', autoResetAfter: null });
        return `${Date.now() - t}ms focusMode=${c().focusMode}`;
      }, 2000);
      try { await withTimeout(cam().resetFocus(), 2000, 'resetFocus'); } catch (e) { L(`INFO vcf.focus.reset ${errStr(e)}`); }
      await step('vcf.exposure.bias', async () => {
        if (!d!.supportsExposureBias) throw new Error('supportsExposureBias=false');
        const t = Date.now();
        await c().setExposureBias(1);
        if (!near(c().exposureBias, 1)) throw new Error(`exposureBias read back ${c().exposureBias}`);
        return `${Date.now() - t}ms bias=${c().exposureBias} range=${d!.minExposureBias}..${d!.maxExposureBias}`;
      }, 1000);
      try { await withTimeout(c().setExposureBias(0), 2000, 'reset bias'); } catch (e) { L(`INFO vcf.exposure.reset ${errStr(e)}`); }
      await step('vcf.torch.on', async () => {
        await c().setTorchMode('on');
        if (c().torchMode !== 'on') throw new Error(`torchMode read back ${c().torchMode}`);
        return `torchMode=${c().torchMode}`;
      }, 3000);
      await step('vcf.torch.off', async () => {
        await c().setTorchMode('off');
        if (c().torchMode !== 'off') throw new Error(`torchMode read back ${c().torchMode}`);
        return `torchMode=${c().torchMode}`;
      }, 3000);

      // ---- photos: 3 spaced (flash off/on/auto), a 3-photo burst, then 2 more with the other quality settings
      type Shot = { id: string; fmt: 'jpeg' | 'heic'; p: Promise<PhotoFile>; host: number; wall: number };
      // toFile=false: capturePhoto() + saveToTemporaryFileAsync(). Used for the burst: capturePhotoToFile() builds its
      // AVCapturePhotoSettings inside a Swift Task, so un-awaited calls can reach -capturePhotoWithSettings: out of uid
      // order and AVFoundation throws "Settings may not be re-used" (a crash; VisionCamera 5.2.4, README). capturePhoto()
      // builds and submits them synchronously, in call order.
      const shoot = (out: CameraPhotoOutput, id: string, fmt: Shot['fmt'], flashMode: FlashMode, toFile = true): Shot => {
        const host = hostNow(), wall = Date.now();
        const settings = { flashMode, enableShutterSound: false };
        const cbs = { onPreviewImageAvailable: (img: { width: number; height: number }) => previews.push(`${id}:${img.width}x${img.height}`) };
        const p: Promise<PhotoFile> = toFile
          ? out.capturePhotoToFile(settings, cbs)
          : out.capturePhoto(settings, cbs).then(async (ph) => ({
              filePath: await ph.saveToTemporaryFileAsync(), width: ph.width, height: ph.height, orientation: ph.orientation,
              isMirrored: ph.isMirrored, timestamp: ph.timestamp, isRawPhoto: ph.isRawPhoto, containerFormat: ph.containerFormat,
            }));
        p.catch(() => {});
        return { id, fmt, p, host, wall };
      };
      const check = async (s: Shot) => {
        const n = `vcf.photo.${s.id}`;
        let pf: PhotoFile | undefined;
        let bytes: Uint8Array | undefined;
        await step(`${n}.delivered`, async () => {
          pf = await withTimeout(s.p, 10000, 'capturePhotoToFile');
          return `w=${pf.width} h=${pf.height} fmt=${pf.containerFormat} orientation=${pf.orientation} mirrored=${pf.isMirrored} ${fileInfo(pf.filePath)}`;
        }, 12000);
        const need = () => { if (!pf) throw new Error('photo not delivered'); return pf; };
        await step(`${n}.bytes`, async () => {
          const p = need();
          bytes = await new File(toUri(p.filePath)).bytes();
          const k = photoKind(bytes);
          if (k !== s.fmt) throw new Error(`expected ${s.fmt} bytes, got ${k} (head ${hex(bytes.slice(0, 12))}) containerFormat=${p.containerFormat} size=${bytes.length}`);
          return `${k} size=${bytes.length} containerFormat=${p.containerFormat}`;
        });
        await step(`${n}.dims`, async () => {
          const p = need();
          const enc = bytes ? (s.fmt === 'heic' ? heicSize(bytes) : jpegSize(bytes)) : undefined;
          if (!sameSize(p, target)) throw new Error(`photo ${p.width}x${p.height}, max photo resolution ${szStr(target)}`);
          if (!sameSize(enc, target)) throw new Error(`encoded ${szStr(enc)}, max photo resolution ${szStr(target)}`);
          return `${p.width}x${p.height} encoded=${szStr(enc)}`;
        });
        await step(`${n}.exif`, async () => {
          need();
          const o = bytes ? exifOrientation(bytes) : undefined;
          if (o !== 6) throw new Error(`EXIF Orientation ${o}, want 6 (back camera, portrait)`);   // as a device writes it
          return `Orientation=${o}`;
        });
        await step(`${n}.timestamp`, async () => {
          const p = need();
          const dt = p.timestamp - s.host;
          if (!(Math.abs(dt) <= 2)) throw new Error(`timestamp ${p.timestamp} is ${dt.toFixed(3)} s from the shutter (host clock ${s.host.toFixed(3)})`);
          return `dt=${dt.toFixed(3)}s at=${new Date(s.wall + dt * 1000).toISOString()} shutter=${new Date(s.wall).toISOString()}`;
        });
        await step(`${n}.path`, async () => {
          const p = need();
          if (paths.has(p.filePath)) throw new Error('duplicate path ' + p.filePath);
          paths.add(p.filePath);
          return 'unique';
        });
      };
      const wait = 5.5 - (hostNow() - t0);
      if (wait > 0) await sleep(wait * 1000);
      L('SCREENSHOT vcf-photo');
      const first = shoot(photoA, 'off', 'jpeg', 'off');
      await check(first);
      for (const f of ['on', 'auto'] as const) {
        await sleep(1500);
        await check(shoot(photoA, f, 'jpeg', f));
      }
      await sleep(1500);
      const burst = [1, 2, 3].map((i) => shoot(photoA, `burst${i}`, 'jpeg', 'off', false)); // fired back-to-back, no awaits
      for (const s of burst) await check(s);
      const speed = d?.supportsSpeedQualityPrioritization ? 'speed' : 'balanced';
      const photoB = VisionCamera.createPhotoOutput({ targetResolution: portrait(target), containerFormat: 'jpeg', quality: 0.5, qualityPrioritization: speed });
      if (d) await mount({ key: 'photo-speed', device: d, outputs: [photoB] });
      await sleep(1500);
      L(`INFO vcf.photo.speed qualityPrioritization=${speed} quality=0.5 jpeg`);
      await check(shoot(photoB, 'speed', 'jpeg', 'off'));
      const photoC = VisionCamera.createPhotoOutput({ targetResolution: portrait(target), containerFormat: 'heic', quality: 1, qualityPrioritization: 'quality' });
      if (d) await mount({ key: 'photo-quality', device: d, outputs: [photoC] });
      await sleep(1500);
      L('INFO vcf.photo.quality qualityPrioritization=quality quality=1 heic');
      await check(shoot(photoC, 'quality', 'heic', 'off'));
      L(`INFO vcf.photo.previewImage supportsPreviewImage=${d?.supportsPreviewImage} delivered=${previews.length} (requested on the off/on/auto/burst photos) ${previews.join(',')}`);

      // ---- video, 3 s each
      for (const v of VCF_VIDEOS) {
        const n = `vcf.video.${v.id}`;
        const out = VisionCamera.createVideoOutput({ targetResolution: portrait(v.size), enableAudio: v.audio });
        if (d) await mount({ key: `video-${v.id}`, device: d, outputs: [out], constraints: [{ resolutionBias: out }, { fps: v.fps }] });
        await sleep(1500);
        let fin: (r: { path: string; reason: string }) => void = () => {};
        const finP = new Promise<{ path: string; reason: string }>((r) => (fin = r));
        let recErr: Error | undefined;
        let rec: Recorder | undefined;
        try {
          rec = await withTimeout(out.createRecorder({}), 5000, 'createRecorder');
          await withTimeout(rec.startRecording((path, reason) => fin({ path, reason }), (e) => { recErr = e; L(`INFO ${n}.onRecordingError ${errStr(e)}`); }), 5000, 'startRecording');
        } catch (e) {
          recErr = e as Error;
        }
        const t = Date.now();
        if (v.torch) {
          await sleep(1000);
          await step('vcf.video.torch.on', async () => {
            await c().setTorchMode('on');
            if (c().torchMode !== 'on') throw new Error(`torchMode read back ${c().torchMode}`);
            return `recording=${rec?.isRecording} torchMode=${c().torchMode}`;
          }, 1500);
          await sleep(Math.max(0, 2000 - (Date.now() - t)));
          await step('vcf.video.torch.off', async () => {
            await c().setTorchMode('off');
            if (c().torchMode !== 'off') throw new Error(`torchMode read back ${c().torchMode}`);
            return `recording=${rec?.isRecording} torchMode=${c().torchMode}`;
          }, 1500);
        }
        await sleep(Math.max(0, 3000 - (Date.now() - t)));
        try {
          if (rec && !recErr) await withTimeout(rec.stopRecording(), 10000, 'stopRecording');
        } catch (e) {
          recErr = recErr ?? (e as Error);
        }
        let path: string | undefined;
        let mi: MovInfo | undefined;
        await step(`${n}.finished`, async () => {
          if (recErr) throw recErr;
          const r = await withTimeout(finP, 20000, 'onRecordingFinished');
          path = r.path;
          return `reason=${r.reason} recordedDuration=${rec?.recordedDuration} ${fileInfo(r.path)}`;
        }, 25000);
        await step(`${n}.file`, async () => {
          if (!path) throw new Error('no recording');
          const f = new File(toUri(path));
          if (!f.exists || !f.size) throw new Error(`exists=${f.exists} size=${f.size}`);
          mi = movInfo(path);
          return `size=${f.size} tracks=${mi.tracks.map((x) => `${x.handler}/${x.codec ?? ''}`).join(',')}`;
        });
        const vt = () => { const x = mi?.tracks.find((y) => y.handler === 'vide'); if (!x) throw new Error('no video track'); return x; };
        await step(`${n}.duration`, async () => {
          if (!mi) throw new Error('no movie info');
          if (Math.abs(mi.duration - 3) > 0.4) throw new Error(`duration ${mi.duration.toFixed(3)} s, expected 3 ± 0.4`);
          return `duration=${mi.duration.toFixed(3)}s`;
        });
        await step(`${n}.dims`, async () => {
          const x = vt();
          if (!sameSize(x.coded, v.size)) throw new Error(`coded ${szStr(x.coded)}, format ${szStr(v.size)}`);
          return `coded=${szStr(x.coded)} codec=${x.codec}`;
        });
        await step(`${n}.fps`, async () => {
          const x = vt();
          const fps = x.duration ? x.frames / x.duration : 0;
          const known = VCF_EXPECTED.has(`${n}.fps`) ? ' (expected: the simulator software encoder)' : '';
          if (fps < 0.9 * v.fps) throw new Error(`fps=${fps.toFixed(1)} < ${(0.9 * v.fps).toFixed(1)} (90% of ${v.fps}) frames=${x.frames} in ${x.duration.toFixed(3)}s${known}`);
          return `fps=${fps.toFixed(1)} frames=${x.frames} in ${x.duration.toFixed(3)}s target=${v.fps}`;
        });
        if (v.audio) await step(`${n}.audio`, async () => {
          if (!mi) throw new Error('no movie info');
          const a = mi.tracks.find((y) => y.handler === 'soun');
          if (!a) throw new Error(`no audio track: ${mi.tracks.map((y) => y.handler).join(',')}`);
          return `audio track ${a.duration.toFixed(3)}s timescale=${a.timescale}`;
        });
      }

      // ---- front camera: mirroring on and off
      const fd = frontRef.current;
      const frontMax = fd?.getSupportedResolutions('photo').reduce<Sz | undefined>((a, x) => (!a || area(x) > area(a) ? x : a), undefined);
      L(`INFO vcf.front device=${fd ? `${fd.id}/${fd.type}` : 'none'} maxPhoto=${szStr(frontMax)}`);
      const frontShots: Record<string, { isMirrored?: boolean; exif?: number }> = {};
      for (const [mid, mm] of [['mirrorOn', 'on'], ['mirrorOff', 'off']] as const) {
        const out = VisionCamera.createPhotoOutput({ targetResolution: portrait(frontMax ?? target), containerFormat: 'jpeg', quality: 0.9, qualityPrioritization: 'balanced' });
        if (fd) await mount({ key: `front-${mid}`, device: fd, outputs: [out], mirrorMode: mm });
        await sleep(1500);
        let pf: PhotoFile | undefined;
        await step(`vcf.front.${mid}.delivered`, async () => {
          if (!fd) throw new Error('no front camera');
          pf = await out.capturePhotoToFile({ flashMode: 'off', enableShutterSound: false }, {});
          const exif = exifOrientation(await new File(toUri(pf.filePath)).bytes());
          frontShots[mid] = { isMirrored: pf.isMirrored, exif };
          return `mirrorMode=${mm} isMirrored=${pf.isMirrored} orientation=${pf.orientation} exif.Orientation=${exif} ${fileInfo(pf.filePath)}`;
        }, 10000);
        await step(`vcf.front.${mid}.dims`, async () => {
          if (!pf) throw new Error('photo not delivered');
          if (!sameSize(pf, frontMax)) throw new Error(`photo ${pf.width}x${pf.height}, front max ${szStr(frontMax)}`);
          return `${pf.width}x${pf.height}`;
        });
      }
      await step('vcf.front.mirror', async () => {   // mirroring on vs off must differ, as on a device
        const a = frontShots.mirrorOn, b = frontShots.mirrorOff;
        if (!a || !b) throw new Error('front photos missing');
        if (a.isMirrored === b.isMirrored && a.exif === b.exif) throw new Error(`same isMirrored=${a.isMirrored} and EXIF ${a.exif} with mirroring on and off`);
        return `on: isMirrored=${a.isMirrored} exif=${a.exif}; off: isMirrored=${b.isMirrored} exif=${b.exif}`;
      });
      setStage(null);
      await sleep(500);
      await step('vcf.errors.none', async () => {
        if (errors.current.length) throw new Error(`onError x${errors.current.length}: ${errors.current.join(' | ')}`);
        return 'no onError in any stage';
      });
      } catch (e) {
        // a call outside step() threw: say so, and still count and summarise what ran
        L(`FAIL vcf.aborted ${errStr(e)}`);
        setStage(null);
      }

      let pass = 0, fail = 0, known = 0;
      const unexpected: string[] = [], na: string[] = [], naUnexpected: string[] = [];
      for (const l of lines) {
        const m = /^M1B\| \S+ (PASS|FAIL) (\S+)( n\/a:)?/.exec(l);
        if (!m) continue;
        if (m[1] === 'PASS') {
          pass++;
          if (m[3]) { na.push(m[2]); if (!VCF_EXPECTED_NA.has(m[2])) naUnexpected.push(m[2]); }
        } else { fail++; if (VCF_EXPECTED.has(m[2])) known++; else unexpected.push(m[2]); }
      }
      const total = pass + fail, want = VCF_TOTAL(AUDIO);
      if (total !== want) unexpected.push(`vcf.count(total=${total},expected=${want})`);
      unexpected.push(...naUnexpected.map((n) => `${n}(n/a)`));
      L(`INFO vcf.summary pass=${pass} fail=${fail} known=${known} unexpected=${unexpected.length} total=${total} expected_total=${want} na=${na.length} [${unexpected.join(',')}]`);
      L('END vcfull');
      setStatus('done');
    })();
  }, []);

  if (!stage) return <Text style={styles.t}>vcfull: {status}</Text>;
  return (
    <VCamera
      key={stage.key}
      ref={camRef}
      style={StyleSheet.absoluteFill}
      device={stage.device}
      isActive
      outputs={stage.outputs}
      constraints={stage.constraints}
      mirrorMode={stage.mirrorMode}
      onStarted={() => {
        startedAt.current = hostNow();
        startedRes.current?.();
      }}
      onSessionConfigSelected={(cfg) => L(`INFO vcf.${stage.key}.config selectedFPS=${cfg.selectedFPS} hdr=${cfg.isPhotoHDREnabled} pixelFormat=${cfg.nativePixelFormat} binned=${cfg.isBinned}`)}
      onError={(e) => {
        errors.current.push(`${stage.key}: ${errStr(e)}`);
        L(`INFO vcf.onError ${stage.key} ${errStr(e)}`);
      }}
    />
  );
}

// ---------------------------------------------------------------- VisionCamera: the back LiDAR depth device
// `-CAMTEST vclidar`: pick the LiDAR device as VisionCamera 5 exposes it (by `type === 'lidar-depth'`, and what
// `useCameraDevice('back', { physicalDevices: ['lidar-depth'] })` returns), mount <Camera> with a photo output, take a
// photo (default; then `enableDepthData: true`, which an app should only ask for when `supportsDepthDataDelivery`), then
// remount with a depth frame output added. `-LIDARDEV wide` runs the same steps on the back wide camera (control).
function VcLidar() {
  const all = useCameraDevices();
  const hookPick = useCameraDevice('back', { physicalDevices: ['lidar-depth'] });
  const want = ((Settings.get('LIDARDEV') as string | undefined) ?? 'lidar') === 'wide' ? 'wide-angle' : 'lidar-depth';
  const device = all.find((d) => d.position === 'back' && d.type === want);
  const [phase, setPhase] = useState<'wait' | 'photo' | 'depth' | 'done'>('wait');
  const photoOutput = usePhotoOutput({});
  const depthOutput = React.useMemo(
    () => VisionCamera.createDepthFrameOutput({ targetResolution: { width: 640, height: 480 }, enablePhysicalBufferRotation: false, dropFramesWhileBusy: true, allowDeferredStart: false, enableFiltering: false } as any),
    [],
  );
  const camRef = useRef<CameraRef>(null);
  const started = useRef<(() => void) | null>(null);
  const startedP = useRef<Promise<void> | null>(null);
  const armStarted = () => (startedP.current = new Promise<void>((res) => (started.current = res)));
  const devRef = useRef(device); devRef.current = device;
  const allRef = useRef(all); allRef.current = all;
  const pickRef = useRef(hookPick); pickRef.current = hookPick;
  const desc = (d: CameraDevice) =>
    `id=${d.id} name='${d.localizedName}' type=${d.type} virtual=${d.isVirtualDevice} physical=[${d.physicalDevices.map((p) => p.type).join(',')}] mediaTypes=[${d.mediaTypes.join(',')}] zoom=${d.minZoom}..${d.maxZoom} switch=[${d.zoomLensSwitchFactors.join(',')}] torch=${d.hasTorch} flash=${d.hasFlash}`;

  useEffect(() => {
    (async () => {
      L(`BEGIN vclidar want=${want}`);
      await step('vcl.permission', async () => {
        let s: string = VisionCamera.cameraPermissionStatus;
        if (s !== 'authorized') s += ` (requested->${await VisionCamera.requestCameraPermission()})`;
        return 'status=' + s;
      });
      for (let i = 0; i < 30 && allRef.current.length === 0; i++) await sleep(200);
      L(`INFO vcl.allDevices count=${allRef.current.length} ${allRef.current.map((x) => `${x.localizedName}/${x.position}/${x.type}`).join(', ')}`);
      L(`INFO vcl.hookPick useCameraDevice('back',{physicalDevices:['lidar-depth']}) -> ${pickRef.current ? desc(pickRef.current) : 'undefined'}`);
      const d = devRef.current;
      if (!d) { L(`FAIL vcl.device no back device with type ${want}`); L('END vclidar'); setPhase('done'); return; }
      L(`PASS vcl.device ${desc(d)}`);
      for (const t of ['photo', 'video', 'stream', 'depth-photo', 'depth-stream'] as const) {
        await step(`vcl.resolutions.${t}`, async () => {
          const rs = d.getSupportedResolutions(t);
          return `count=${rs.length} ${rs.map((r) => `${r.width}x${r.height}`).join(',')}`;
        });
      }
      await step('vcl.supportsOutput', async () => `photo=${d.supportsOutput(photoOutput)} depth=${d.supportsOutput(depthOutput)}`);
      armStarted();
      setPhase('photo');
    })();
  }, []);

  useEffect(() => {
    if (phase === 'photo') {
      (async () => {
        await step('vcl.photo.onStarted', () => startedP.current!, 15000);
        await sleep(3000);
        L('SCREENSHOT vcl-photo');
        await sleep(1500);
        L(`INFO vcl.photoOutput supportsDepthDataDelivery=${(photoOutput as any).supportsDepthDataDelivery}`);
        await step('vcl.photo.default', async () => {
          const pf = await photoOutput.capturePhotoToFile({ flashMode: 'off', enableShutterSound: false }, {});
          return `w=${pf.width} h=${pf.height} orientation=${pf.orientation} ${fileInfo(pf.filePath)}`;
        });
        // what an app that doesn't check supportsDepthDataDelivery gets (VisionCamera's own check, or AVFoundation's)
        await step('vcl.photo.enableDepthData', async () => {
          const ph = await photoOutput.capturePhoto({ flashMode: 'off', enableShutterSound: false, enableDepthData: true } as any, {});
          const depth = (ph as any).depth;
          return `w=${ph.width} h=${ph.height} depth=${depth ? `${depth.width}x${depth.height}` : 'none'}`;
        });
        armStarted();
        setPhase('depth');
      })();
    } else if (phase === 'depth') {
      (async () => {
        await step('vcl.depth.onStarted', () => startedP.current!, 10000);
        await sleep(3000);
        L('SCREENSHOT vcl-depth');
        await sleep(1000);
        await step('vcl.depth.photo', async () => {
          const pf = await photoOutput.capturePhotoToFile({ flashMode: 'off', enableShutterSound: false }, {});
          return `w=${pf.width} h=${pf.height} ${fileInfo(pf.filePath)}`;
        });
        L('END vclidar');
        setPhase('done');
      })();
    }
  }, [phase]);

  if (!device || phase === 'wait' || phase === 'done') return <Text style={styles.t}>vclidar: {phase} device={String(!!device)}</Text>;
  return (
    <VCamera
      key={phase}
      ref={camRef}
      style={StyleSheet.absoluteFill}
      device={device}
      isActive
      outputs={phase === 'photo' ? [photoOutput] : [photoOutput, depthOutput]}
      onStarted={() => { L(`INFO vcl.${phase}.onStarted fired`); started.current?.(); }}
      onConfigured={() => L(`INFO vcl.${phase}.onConfigured`)}
      onError={(e) => L(`FAIL vcl.${phase}.onError ${errStr(e)}`)}
    />
  );
}

// ---------------------------------------------------------------- vcqr / expoqr: the QR payloads a scanner reports
// `-CAMTEST vcqr` (VisionCamera's object output) or `expoqr` (expo-camera's onBarcodeScanned), `-QRPOS back|front`
// (default back), `-QRSECS N` (default 10). Logs `QRSEEN src=<vc|expo> t=<ms since start> at=<Unix ms>
// pct=<encodeURIComponent>`
// each time the payload differs from the last one, then `END qr`. scripts/qr-test.sh compares the payloads.
const QR_TYPES = ['qr'] as const;
function QrWatch({ lib }: { lib: 'vc' | 'expo' }) {
  const pos = (Settings.get('QRPOS') as string | undefined) === 'front' ? 'front' : 'back';
  const secs = Number(Settings.get('QRSECS') ?? 10);
  const t0 = useRef(Date.now());
  const last = useRef<string | null>(null);
  const [on, setOn] = useState(false);
  const seen = useCallback((v: string | undefined | null) => {
    if (v == null || v === last.current) return;
    last.current = v;
    let pct: string;
    try {
      pct = encodeURIComponent(v);
    } catch (e) {
      pct = 'UNENCODABLE:' + JSON.stringify(v);
    }
    L(`QRSEEN src=${lib} t=${Date.now() - t0.current} at=${Date.now()} pct=${pct}`);
  }, []);
  useEffect(() => {
    (async () => {
      L(`BEGIN qr lib=${lib} pos=${pos} secs=${secs}`);
      if (lib === 'vc') {
        if (VisionCamera.cameraPermissionStatus !== 'authorized') await VisionCamera.requestCameraPermission();
        L(`INFO qr.permission ${VisionCamera.cameraPermissionStatus}`);
      } else {
        const p = await ExpoCamera.requestCameraPermissionsAsync();
        L(`INFO qr.permission ${p.status}`);
      }
      t0.current = Date.now();
      setOn(true);
      await sleep(secs * 1000);
      setOn(false);
      L(`END qr seen=${last.current !== null}`);
    })();
  }, []);
  if (!on) return <Text style={styles.t}>qr: idle</Text>;
  if (lib === 'vc') return <VcQr pos={pos} onValue={seen} />;
  return (
    <CameraView style={StyleSheet.absoluteFill} facing={pos} barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
      onBarcodeScanned={(e) => seen(e.data)} onMountError={(e) => L(`FAIL qr.onMountError ${e.message}`)} />
  );
}
function VcQr({ pos, onValue }: { pos: 'back' | 'front'; onValue: (v: string | undefined) => void }) {
  const device = useCameraDevice(pos);
  const onObjectsScanned = useCallback((objects: any[]) => {
    for (const o of objects) if (isScannedCode(o) && o.type === 'qr') onValue(o.value);
  }, [onValue]);
  const objectOutput = useObjectOutput({ types: QR_TYPES as any, onObjectsScanned });
  if (!device) return <Text style={styles.t}>qr: no {pos} device</Text>;
  return <VCamera style={StyleSheet.absoluteFill} device={device} isActive outputs={[objectOutput]}
    onError={(e) => L(`FAIL qr.onError ${errStr(e)}`)} />;
}

export default function App() {
  const test = (Settings.get('CAMTEST') as string | undefined) ?? 'none';
  useEffect(() => {
    L(`START test=${test} audio=${AUDIO} vcorient=${Settings.get('VCORIENT') ?? 'device'} nativeModules=${Object.keys(NativeModules).length}`);
  }, []);
  return (
    <View style={styles.c}>
      <Text style={styles.t}>m1b test={test}</Text>
      {test === 'vcorient' ? <VcOrient /> : test === 'expoorient' ? <ExpoOrient /> : test === 'vc' ? <VcTest /> : test === 'vcfull' ? <VcFull /> : test === 'vclidar' ? <VcLidar /> : test === 'expo' ? <ExpoTest /> : test === 'vccycle' ? <CycleTest lib="vc" /> : test === 'expocycle' ? <CycleTest lib="expo" /> : test === 'vcqr' ? <QrWatch lib="vc" /> : test === 'expoqr' ? <QrWatch lib="expo" /> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  c: { flex: 1, backgroundColor: '#222', paddingTop: 60 },
  t: { color: '#0f0', fontSize: 14, padding: 8 },
});
