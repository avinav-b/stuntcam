# SPDX-License-Identifier: Apache-2.0
# Sourced by p12.sh (HOOK=src-hook.sh). Once the app runs, it switches the source live through the control file
# (image, video, then the scene) and takes a screenshot of each.
#   Needs fixtures/media/still.png (any still image; it is not committed).
source "$ROOT/scripts/env.sh"   # sim-read paths under $SIMCAM_HOME (no-op when p12.sh already sourced it)

# host_stamp: now in the engine log's clock (CFAbsoluteTime: seconds since 2001-01-01), with milliseconds.
host_stamp() { python3 -c 'import time;print("%.3f"%(time.time()-978307200))'; }

if grep -q "running=" $OUT/app.log && [[ -z ${SOURCES_DONE:-} ]]; then
  SOURCES_DONE=1   # lives on in p12.sh's shell between polls, so this runs once
  control_file=$OUT/control.txt
  sleep 1
  # screenshots are evidence only: one that fails changes no verdict (also below)
  xcrun simctl io $UDID screenshot $OUT/src-1-scene.png >/dev/null 2>&1

  echo "$(host_stamp) write image" >> $OUT/host.log
  echo "source=image:$SIMCAM_DEV/media/still.png" > $control_file
  sleep 1.2
  xcrun simctl io $UDID screenshot $OUT/src-2-image.png >/dev/null 2>&1

  echo "$(host_stamp) write video" >> $OUT/host.log
  echo "source=video:$SIMCAM_DEV/media/magenta.mov" > $control_file
  sleep 1.2
  xcrun simctl io $UDID screenshot $OUT/src-3-video-a.png >/dev/null 2>&1
  sleep 1
  xcrun simctl io $UDID screenshot $OUT/src-4-video-b.png >/dev/null 2>&1

  echo "$(host_stamp) write scene" >> $OUT/host.log
  echo "source=scene" > $control_file
  sleep 1.2
  xcrun simctl io $UDID screenshot $OUT/src-5-scene.png >/dev/null 2>&1
fi
