# SPDX-License-Identifier: Apache-2.0
# sourced by p12.sh (HOOK=src-hook.sh): switch sources live through the control file and screenshot each
#   needs fixtures/media/still.png (any still image; not committed)
source $ROOT/scripts/env.sh   # sim-read paths under $SIMCAM_HOME (no-op when p12.sh already sourced it)
if grep -q "running=" $OUT/app.log && [[ -z ${SRC1:-} ]]; then SRC1=1; ctl=$OUT/control.txt
  sleep 1; xcrun simctl io $UDID screenshot $OUT/src-1-scene.png >/dev/null 2>&1
  echo "$(python3 -c 'import time;print("%.3f"%(time.time()-978307200))') write image" >> $OUT/host.log; echo "source=image:$SIMCAM_DEV/media/still.png" > $ctl; sleep 1.2; xcrun simctl io $UDID screenshot $OUT/src-2-image.png >/dev/null 2>&1
  echo "$(python3 -c 'import time;print("%.3f"%(time.time()-978307200))') write video" >> $OUT/host.log; echo "source=video:$SIMCAM_DEV/media/magenta.mov" > $ctl; sleep 1.2; xcrun simctl io $UDID screenshot $OUT/src-3-video-a.png >/dev/null 2>&1; sleep 1; xcrun simctl io $UDID screenshot $OUT/src-4-video-b.png >/dev/null 2>&1
  echo "$(python3 -c 'import time;print("%.3f"%(time.time()-978307200))') write scene" >> $OUT/host.log; echo "source=scene" > $ctl; sleep 1.2; xcrun simctl io $UDID screenshot $OUT/src-5-scene.png >/dev/null 2>&1
fi
