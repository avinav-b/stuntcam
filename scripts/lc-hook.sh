# SPDX-License-Identifier: Apache-2.0
# sourced by p12.sh (HOOK=lc-hook.sh) every 0.2 s (lifecycle mode). Rounds: 1 Home-like (Preferences to front),
# 2 another camera app to front, 3 control-file interrupt/end, 4 runtime error
source $ROOT/scripts/env.sh   # sim-read paths under $SIMCAM_HOME (no-op when p12.sh already sourced it)
source $ROOT/scripts/common.sh   # inject_env
ctl=$OUT/control.txt
if grep -q "RECORDING round=1" $OUT/app.log && [[ -z ${LC1:-} ]]; then LC1=1; sleep 1.5
  echo "$(date +%T) host: Preferences to front" >> $OUT/host.log
  xcrun simctl launch $UDID com.apple.Preferences >/dev/null; sleep 4
  echo "$(date +%T) host: our app to front" >> $OUT/host.log; xcrun simctl launch $UDID $B >/dev/null; fi
if grep -q "RECORDING round=2" $OUT/app.log && [[ -z ${LC2:-} ]]; then LC2=1; sleep 1.5
  echo "$(date +%T) host: second camera app (VC, injected) to front" >> $OUT/host.log
  inject_env "$DY" "$SIMCAM_DEV/profiles/cfg-m10.plist" "$OUT/dylib-second.log"
  env $INJECT_ENV xcrun simctl launch $UDID dev.spike.m1bcam -CAMTEST vcorient -CAM back -RCT_jsLocation localhost:${EXPO_PORT:-8101} >/dev/null
  sleep 9
  echo "$(date +%T) host: our app to front" >> $OUT/host.log; xcrun simctl launch $UDID $B >/dev/null; sleep 1
  xcrun simctl terminate $UDID dev.spike.m1bcam; fi
if grep -q "RECORDING round=3" $OUT/app.log && [[ -z ${LC3:-} ]]; then LC3=1; sleep 1.5
  echo "event=interrupt:3@1" > $ctl; echo "$(date +%T) host: control interrupt:3" >> $OUT/host.log; sleep 3
  echo "event=end@2" > $ctl; echo "$(date +%T) host: control end" >> $OUT/host.log; fi
if grep -q "RECORDING round=4" $OUT/app.log && [[ -z ${LC4:-} ]]; then LC4=1; sleep 1.5
  echo "event=runtime-error:-11819@3" > $ctl; echo "$(date +%T) host: control runtime-error" >> $OUT/host.log; fi
