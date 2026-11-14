# fixtures/media

- `qr.png`: the 1280x720 QR still the engine shows (`M5_IMAGE`, set by `scripts/boot.sh` / `scripts/run.sh`).
- `magenta.mov`: the 4 s video source from spike m12 (`SIMCAM_SOURCE=video:…`), made by `tools/mkvideo.swift`.

These were left out because they are over 2 MB: `frame.bin` (8 MB raw frame, made by `tools/mkqr.swift`) and m12's
`still.png` (3.6 MB).
