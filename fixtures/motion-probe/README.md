# fixtures/motion-probe

A measurement app: two `CMMotionManager`s record the accelerometer at 100 Hz, with or without the camera running, in
either start order. It reports rate, gaps and timestamp offsets per manager.

```sh
SIMCAM_UDID_263=<udid> fixtures/motion-probe/build.sh
fixtures/motion-probe/drive.sh <rt> <tag> boot -camera 1 -order motionFirst
fixtures/motion-probe/matrix.sh <rt> <tag>                # every combination
python3 -I fixtures/motion-probe/summarize.py "$SIMCAM_HOME/results" <tag>
```
