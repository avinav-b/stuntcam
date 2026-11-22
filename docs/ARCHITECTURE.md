# How stuntcam works

stuntcam gives the iOS Simulator a camera by answering one layer below AVFoundation. Apps, and the libraries they use, only
ever talk to Apple's genuine AVFoundation objects; stuntcam supplies the hardware those objects expect underneath.
This page is the map for anyone changing the code or porting it to a new iOS. The full design, with the reasoning behind
each choice, is [design.md](design.md).

```
 Your app + any camera library (AVFoundation, VisionCamera, expo-camera, Flutter camera, mobile_scanner)    unchanged
   │ public AVFoundation API                       + a few client hooks (CoreMotion, photo data, rotation, Vision)
 Apple AVFoundation: sessions, devices, formats, outputs, preview layer, validation, errors, encoders     Apple's code
   │ Fig: capture sources + session vtable · CoreMedia notification centre                               private ABI
 libsimcam.dylib: the engine (one per process)
   │ sources from a camera profile · session slots · frame pump · photo queue · movie writer · audio · scene
 libsimcamloader.dylib: the loader (swappable without a reboot)
   │ restores Xcode's own inserted libraries · loads other tools' libraries (the chain) · loads the engine
 libsimcamstub.dylib: the stub (frozen, libSystem only), inserted into every simulator process
   │ app process? → read $SIMCAM_HOME/sims/<udid>/current → load the loader it names
 host: the stuntcam command and ~/Library/Application Support/stuntcam ($SIMCAM_HOME)
```

## The three libraries

**Stub** (`engine/stub/stub.c`, under 60 lines). The simulator's launchd inserts it into every process it starts, so it
has to be tiny and safe: libSystem only (anything heavier at boot once hung a simulator). In an app process it reads the
simulator's `current` file, `<proto> <absolute loader path>`, and loads that loader. Anything unexpected (no file, a
newer protocol, a relative path, `SIMCAM=0`) and it logs one line and loads nothing. It is frozen so that upgrades never
need a reboot.

**Loader** (`engine/loader/loader.c`). Xcode Run and Xcode tests set `DYLD_INSERT_LIBRARIES` themselves (the debugger's
helpers, test-bundle injection, the view debugger); a boot environment that inserts the stub displaces them. The loader
puts them back (`restore-rules/<Xcode build>.json`, staged per content and named in the sim's `config.json`; built-in rules
in `loader.c` when none are staged; an "unverified" line for a build without rules), loads any libraries listed in the simulator's chain file
(another tool's loader), and then the engine next to it. It never loads the engine into test runners or app extensions.

**Engine** (`engine/src/simcam.m`, a single translation unit built from the `fig/`, `core/`, `providers/` and `client/`
fragments). It is a singleton: two loads in one process keep one engine.

## Arming a simulator

Two ways to get the stub into a simulator's processes:

- **Boot mode** passes it in the boot environment (`simctl boot` with `SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=…` and
  stuntcam's other variables). Every process of that boot inherits it, including Xcode Run and tests. It needs a (re)boot
  and lasts until the simulator shuts down.
- **launchd mode** sets the same variables in a running simulator's launchd (`launchctl setenv` inside it). There's no
  reboot, but launches that set their own `DYLD_INSERT_LIBRARIES` (Xcode Run) don't get it. Other tools' entries are kept.
  A simulator booted with the variable in its boot environment ignores a later `setenv` of it.

Either way, what an app gets is decided per launch by `current`: `stuntcam down` removes it (no reboot), and
`stuntcam swap` points it at another build.

## Fig primer

Fig is Apple's private capture layer underneath AVFoundation (CoreMedia/CMCapture). AVFoundation's capture objects
(AVFCapture) ask Fig for capture sources at discovery, and run a session through a Fig session object's vtable, with
Fig's notifications arriving through CoreMedia's notification centre. In the simulator the camera side of that is
stubbed out. stuntcam fills those stubs and leaves everything above them to Apple (design §1, §3).

**Who owns what.** Apple's code is every object the app touches, plus validation, KVO, notification fan-out, error
mapping, point conversions and the encoders. stuntcam's is what the simulator stubs out: Fig sources answering from a
profile, session slots for configure, start, stop, photo and movie, the producers, and a few client patches where the
simulator's AVFoundation compiles paths out.

**What the engine depends on**, by category (the full per-runtime list is `tools/inventory.py`'s output):

| Category | What | Seen to change |
|---|---|---|
| ObjC entry points | the video-source, audio-source and session factories, plus one ivar | 17.5: one method-signature difference |
| Called, not hooked | 27's source-manager method; Apple's attributes initializer | new in 27 |
| C functions | 9 in CoreMedia/CMCapture | 27 dropped the source-list function |
| Vtables | source (2 classes, 3 slots); session (8 slots replaced, plus base Invalidate/Finalize) | the session vtable grew 19 → 24 slots in 27; the replaced slots kept their positions |
| Contract | one local-queue message type; about 25 notification names and keys | 110 attribute constants removed in 27 |
| Data schemas | source attributes, format keys, barcode capability and payload | 27 typed the attributes; Apple's initializer absorbs it |
| Semantic switches | rotation encoding, mic, Vision revision, interruption map, … (about 7) | 17.5's rotation is an orientation value |

That small, inventoried surface is what a port touches (below): Apple's own initializers absorbed most of the 26 → 27
changes. The preflight gate checks its shape at the first camera call, so a runtime that moved something gets the stock
simulator; a semantic change can still slip past it (design §13), and the sentinel latches after two attributed crashes.

## Inside the engine

- **Lazy arming** (`core/lazy.inc`). Nothing happens at load except the CoreMotion decision: the stand-in, or, when a
  marker from an earlier launch of this build says so (a camera latch, a `motion` latch or a real preflight failure),
  stock CoreMotion behind one-shot traps that say so once. Traps on AVFoundation's
  public entry points arm the engine on the app's first camera call, once, even with several threads racing.
- **Preflight gate** (`core/preflight.inc`). Before arming, it checks that the runtime's private surface (vtable sizes,
  symbols, classes) matches what the engine expects. Anything off means the stock simulator: no camera, never a crash.
- **Fig layer** (`fig/`). AVFoundation asks Fig, Apple's private capture layer, for capture sources and runs sessions
  through a vtable. The engine answers with sources built from a camera profile (`engine/profiles/*.plist`: devices,
  formats, frame rates, photo sizes) and replaces the session slots for configure, start, stop, photo and movie. Its
  notifications go through CoreMedia's notification centre, which AVFoundation already listens to.
- **Providers** (`providers/`).
  - **Frame pump:** a serial queue that renders the scene at the active format's rate and feeds every sink: preview
    layers, video data outputs, movie writers.
  - **Photo queue:** mirrors and encodes JPEG or HEIC with EXIF, off the pump so previews don't stall. Stopping or
    interrupting a session waits for photos in flight.
  - **Movie writer:** `AVAssetWriter` in the simulator, sized by the active format.
  - **Audio:** a tone by default; the Mac's microphone in `mic` mode.
  - **Scene:** the synthetic picture with a QR code and an EAN-13 barcode, an image or video file, or a QR code of any
    text (`qr:`, made in the app with Core Image; `providers/qr.c`).
  - **Webcam:** the Mac camera, published by the helper app (`helper/`, "Stuntcam Camera") into a read-only
    shared-memory ring ([webcam-ring.md](webcam-ring.md)). The pump locks its ticks to the helper's frames, and an
    offline helper means the scene with a "webcam offline" band. Its frames go to each output in one GPU pass (Core
    Image on the simulator's Metal device: crop to aspect, scale, turn, mirror), webcam only; scene, image and video
    keep their CG path (`scripts/golden.sh` checks their frames stay bit-identical).
- **Client hooks** (`client/`). A few public-API fixes where the simulator's AVFoundation differs from a phone: CoreMotion
  for orientation, photo data and rotation, writer settings, and Vision's barcode request pinned to revision 1.
- **Control** (`core/control.inc`). Per-launch settings (`SIMCAM_SOURCE`, `SIMCAM_ORIENT`) and a control file it polls
  every 250 ms: source and orientation changes, interruptions, runtime errors, photo and movie errors for tests.
- **Safety** (`core/safety.inc`, then `crash_handler.inc`, `markers.inc`, `events_file.inc`, `sentinel_activate.inc`). A
  pause-aware watchdog times the session and photo operations the engine owns (movie operations aren't timed yet): a
  debugger pause or the Mac sleeping doesn't count. A signal handler attributes crashes to the engine. Both write the
  app's `state/<bundle>/events.log`. Two hangs of a feature in one launch latch it in that process: photos
  error-complete, sessions get at most one runtime error per 5 s. Two hangs or crashes within an app's last 20
  activations (and 24 hours) of a build keep its next launches stock, until they age out or a new build arrives. Crashes
  under a debugger never count. `stuntcam sentinel clear` resets an app.

## On the Mac

Everything a simulator process reads or writes lives under `$SIMCAM_HOME` (`~/Library/Application Support/stuntcam`),
because simulator processes can't read macOS privacy-protected folders:

| Path | Holds |
|---|---|
| `builds/<hash>/` | Staged engine + loader pairs and stubs, at content-hash paths, ad-hoc signed |
| `sims/<udid>/current` | Which loader this simulator's apps load |
| `sims/<udid>/chain.txt` | Extra libraries to load |
| `sims/<udid>/state/<bundle>/events.log` | The sentinel's memory per app |
| `sims/<udid>/arm.json` | How and when `stuntcam` armed it |
| `devices/` | A private simulator set for the self-test; Xcode and Simulator never list it |
| `verdicts/<key>.json` | Cached self-test verdicts per runtime, engine, stub, self-test app and profile |
| `logs/` | Stub, engine and service logs |

## Verdicts and the self-test

`stuntcam up` and `verify` run the canary (`canary/`) on a throwaway simulator in the private set. The canary
walks every capture path and prints `PASS`, `FAIL` or `INCONCLUSIVE`. "Late" is inconclusive and "never" is a fail.
Pass and fail are cached per key; inconclusive never is. A content fail is cached only after a second launch fails too; a pass after a fail is cached for an hour (W4 e40). A failed verdict stops `up` from arming.

## Porting to a new iOS

The private surface is small and inventoried: `tools/inventory.py` lists every class, symbol, vtable slot, notification
and constant the engine uses, per runtime. A port is usually a few lines: 26 → 27 took +34/−9, because Apple's own
initializers absorbed most changes. The steps:

1. Run the inventory against the new SDK and diff it.
2. Fix the preflight gate's expectations.
3. Check the new Xcode's inserted libraries against the loader's restore rules (`loader.c`) and record them in
   `restore-rules/`.
4. Run the canary, the fixture matrix and the mutation suite.

Every bug found so far, with what guards it, is in [ledger.md](ledger.md).
