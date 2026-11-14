---
name: Bug report
about: No camera, a crash, a hang or wrong output under stuntcam
title: ""
labels: bug
---

<!-- SPDX-License-Identifier: Apache-2.0 -->
<!--
Before filing: docs/TROUBLESHOOTING.md covers the common cases (booted-unarmed, launchd mode with ⌘R, latched apps,
Detox). What's verified and what to expect: docs/SUPPORT.md.

Reports without the doctor and status output below can't be triaged. Remove anything private (paths, bundle ids) if
you need to, but keep the rest of the output as printed.
-->

### What happened

<!-- What you did, what you expected, and what happened instead. Exact error lines help. -->

**Expected:**

**Actual:**

**Steps to reproduce:**

1.
2.

### Your setup

- stuntcam version (`stuntcam version`) and commit (`git rev-parse --short HEAD` in your checkout):
- Xcode (`xcodebuild -version`, both lines):
- Simulator runtime and device (e.g. iOS 27.0 (24A434), iPhone 17 Pro):
- Mac (Apple silicon model) and macOS version:
- How the simulator was armed: <!-- boot mode (`stuntcam up` booted it, or `up --reboot`) / launchd mode (`up` on a running simulator, `--mode launchd`) / `stuntcam exec` / other -->
- How the app was launched: <!-- Xcode ⌘R / ⌘U / `xcodebuild test` / `expo run:ios` / `flutter run` / `simctl launch` / Detox / other -->
- What your app uses for the camera: <!-- AVFoundation directly / VisionCamera (version) / expo-camera (version) / Flutter `camera` (version) / `mobile_scanner` (version) / other -->

### `stuntcam doctor --json`

<!-- Run with the simulator still booted. -->

```
paste here
```

### `stuntcam status --json`

```json
paste here
```

### `stuntcam sentinel ls` (if `status` lists your app as latched)

```
paste here
```

### Anything else

<!-- Logs (`stuntcam up --log` writes them under ~/Library/Application Support/stuntcam/logs/<udid>/), crash reports, a
minimal project that reproduces it. -->
