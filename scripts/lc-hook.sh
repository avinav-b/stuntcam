# SPDX-License-Identifier: Apache-2.0
# Sourced by p12.sh (HOOK=lc-hook.sh) every 0.2 s, in lifecycle mode. Each round runs once, when the app logs its
# "RECORDING round=<n>" line:
#   1  a Home-like interruption: Preferences comes to the front, then our app again
#   2  another camera app (the RN fixture's VisionCamera test, injected) comes to the front, then our app again
#   3  an interruption, then an end, through the control file
#   4  a runtime error, through the control file
# ROUND<n>_DONE marks a round as done; they live on in p12.sh's shell between polls.
source "$ROOT/scripts/env.sh"   # sim-read paths under $SIMCAM_HOME (no-op when p12.sh already sourced it)
source "$ROOT/scripts/common.sh"   # inject_env

control_file=$OUT/control.txt

if grep -q "RECORDING round=1" $OUT/app.log && [[ -z ${ROUND1_DONE:-} ]]; then
  ROUND1_DONE=1
  sleep 1.5
  echo "$(date +%T) host: Preferences to front" >> $OUT/host.log
  xcrun simctl launch $UDID com.apple.Preferences >/dev/null
  sleep 4
  echo "$(date +%T) host: our app to front" >> $OUT/host.log
  xcrun simctl launch $UDID $BUNDLE >/dev/null
fi

if grep -q "RECORDING round=2" $OUT/app.log && [[ -z ${ROUND2_DONE:-} ]]; then
  ROUND2_DONE=1
  sleep 1.5
  echo "$(date +%T) host: second camera app (VC, injected) to front" >> $OUT/host.log
  inject_env "$ENGINE_DYLIB" "$SIMCAM_DEV/profiles/cfg-m10.plist" "$OUT/dylib-second.log"
  env $INJECT_ENV xcrun simctl launch $UDID dev.spike.m1bcam -CAMTEST vcorient -CAM back \
    -RCT_jsLocation localhost:${EXPO_PORT:-8101} >/dev/null
  sleep 9
  echo "$(date +%T) host: our app to front" >> $OUT/host.log
  xcrun simctl launch $UDID $BUNDLE >/dev/null
  sleep 1
  xcrun simctl terminate $UDID dev.spike.m1bcam
fi

if grep -q "RECORDING round=3" $OUT/app.log && [[ -z ${ROUND3_DONE:-} ]]; then
  ROUND3_DONE=1
  sleep 1.5
  echo "event=interrupt:3@1" > $control_file
  echo "$(date +%T) host: control interrupt:3" >> $OUT/host.log
  sleep 3
  echo "event=end@2" > $control_file
  echo "$(date +%T) host: control end" >> $OUT/host.log
fi

if grep -q "RECORDING round=4" $OUT/app.log && [[ -z ${ROUND4_DONE:-} ]]; then
  ROUND4_DONE=1
  sleep 1.5
  echo "event=runtime-error:-11819@3" > $control_file
  echo "$(date +%T) host: control runtime-error" >> $OUT/host.log
fi
