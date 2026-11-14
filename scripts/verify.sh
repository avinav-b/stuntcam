#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# verify.sh [--runtime <iOS version|runtime id>] [--fresh] [--keep]  — canary verdict on a private device set (design §5.3, U1)
#   The canary runs on a throwaway sim in stuntcam's own device set ($SIMCAM_HOME/devices, `simctl --set`), so Xcode and
#   Simulator never list it. One device per runtime build, created on first use. Steps: boot armed (boot env, as boot.sh) ->
#   install canary + grants -> launch (60 s for its RESULT line; the canary's own budget is 45 s) -> shut down.
#   Verdict key = runtime build + engine build (the loader/engine hash dir) + stub build + canary executable hash + profile
#   hash (the preflight fingerprint joins it once tools/inventory has a runtime half). Verdicts: pass | fail(<checks>) |
#   inconclusive(<reason>), the canary's own PASS/FAIL/INCONCLUSIVE; one INCONCLUSIVE is retried, a second is
#   inconclusive(...) (design §5.2: "couldn't run", never cached). A canary that exits without a verdict (a crash) is run
#   once more; two in a row are fail(crash). A content fail (core=FAIL, not a crash) is confirmed by one more launch: a
#   second content fail is the verdict; a pass after it is the verdict, cached for an hour only (failed_first, as the Go CLI's
#   Result.FailedFirst); a confirming launch without a verdict leaves the fail uncached (w4 e40).
#   pass and fail are cached in $SIMCAM_HOME/verdicts/<key>.json (written
#   atomically; an unreadable file is a miss); inconclusive never is, and neither is a run with DYLIB=<engine> or extra
#   SIMCTL_CHILD_* settings (they change the result but not the key; SIMCAM_CHAIN_FILE and SIMCAM_STATE_DIR are ignored).
#   One verify per runtime build at a time ($SIMCAM_HOME/verify-<build>.lock, after the cache read; the Go CLI must use
#   the same lock, key and file format). --fresh ignores the cache; --keep leaves the device booted. Ctrl-C/HUP shut the
#   device down. The device's sentinel state is cleared per run.
#   Output: one line "verdict <state> key=<key> …" plus timings; exit 0 pass, 1 fail, 2 inconclusive.
#   Needs ./build.sh and the canary app (scripts/build-fixtures.sh canary).
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/arm-env.sh
WANT=; FRESH=0; KEEP=0
while (( $# )); do case $1 in --runtime) WANT=$2; shift 2;; --fresh) FRESH=1; shift;; --keep) KEEP=1; shift;; *) echo "usage: $0 [--runtime V] [--fresh] [--keep]" >&2; exit 64;; esac; done
SET=$SIMCAM_HOME/devices; mkdir -p "$SET"; VD=$SIMCAM_HOME/verdicts; mkdir -p "$VD"
APP=$ROOT/canary/build/m10canary.app; B=dev.simcam.spike.m10canary
[[ -d $APP ]] || { echo "$0: no canary app at $APP (scripts/build-fixtures.sh canary)" >&2; exit 64; }
sc() { xcrun simctl --set "$SET" "$@"; }
T0=$SECONDS; RUNID=$(date +%Y%m%d-%H%M%S)
# settings that would make this canary something other than the stock check on a clean device: never inherited
unset SIMCAM_CHAIN_FILE SIMCAM_STATE_DIR
LOCK=; U=; LP=; BP=
cleanup() {   # every exit path: children, the device (unless --keep), our lock
  [[ -n $LP ]] && kill $LP 2>/dev/null; [[ -n $BP ]] && kill $BP 2>/dev/null
  [[ -n $U ]] && (( ! KEEP )) && sc shutdown $U >/dev/null 2>&1
  [[ -n $LOCK && $(cat "$LOCK/pid" 2>/dev/null) == $$ ]] && rm -rf "$LOCK"
}
trap cleanup EXIT
trap 'echo "verdict inconclusive(interrupted)" ; exit 130' INT TERM HUP

# runtime: newest available iOS, or the one asked for (version "26.3" or a runtime identifier)
RTI=$(xcrun simctl list runtimes -j | python3 -I -c '
import json, sys
want = sys.argv[1]
rs = [r for r in json.load(sys.stdin)["runtimes"] if r["isAvailable"] and r["platform"] == "iOS"]
if want: rs = [r for r in rs if r["identifier"] == want or r["version"] == want or r["version"].startswith(want + ".")]
if not rs: sys.exit(1)
r = sorted(rs, key=lambda r: [int(p) for p in r["version"].split(".")])[-1]
print(r["identifier"], r["buildversion"], r["version"])' "$WANT") || { echo "verdict inconclusive(no iOS runtime matching '${WANT:-any}')"; exit 2; }
read RTID RTBUILD RTVER <<<"$RTI"   # not `read <<<"$(…)" || …`: read succeeds on the empty line, so a missing runtime went unnoticed

[[ -f $ROOT/build/current_loader.txt && -f $ROOT/build/current_stub.txt ]] || { echo "$0: no build (./build.sh)" >&2; exit 64; }
BUILD=$(basename "$(dirname "$(cat $ROOT/build/current_loader.txt)")")
STUBB=$(basename "$(dirname "$(cat $ROOT/build/current_stub.txt)")")
CH=$(shasum "$APP/m10canary" | cut -c1-8)   # the canary's checks are part of the verdict: a rebuilt canary is a new key
PROFILE=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}; PH=$(shasum "$PROFILE" | cut -c1-12)
KEY=$RTBUILD-$BUILD-$STUBB-c$CH-$PH; VF=$VD/$KEY.json
# settings that change the result but aren't in the key: never read or write the cache with them
NOCACHE=; [[ -n ${DYLIB:-} ]] && NOCACHE="DYLIB=$DYLIB"
XC=$(env | grep '^SIMCTL_CHILD_' | cut -d= -f1 | tr '\n' ' '); [[ -n $XC ]] && NOCACHE="${NOCACHE:+$NOCACHE, }$XC"
[[ -n $NOCACHE ]] && echo "not using the verdict cache: $NOCACHE" >&2
if (( ! FRESH )) && [[ -z $NOCACHE && -f $VF ]]; then
  # an unreadable file or an unknown verdict is a cache miss
  # a retried pass (the canary's frame-gap retry, w4 e38) is used for an hour only, as the Go CLI's RetriedPassTTL
  CV=$(python3 -I -c 'import json,sys,time; d=json.load(open(sys.argv[1])); v=d["verdict"]; rt=d.get("retried") or d.get("relaunched") or d.get("failed_first") or (" retried:[" in d.get("result", ""))
print("expired" if v == "pass" and rt and time.time() - d.get("at", 0) > 3600 else v if v == "pass" or v.startswith("fail(") else "")' $VF 2>/dev/null)
  if [[ $CV == expired ]]; then echo "the cached pass needed a canary retry and is over an hour old: running the canary again" >&2
  elif [[ -n $CV ]]; then echo "verdict $CV key=$KEY (cached $VF)"; [[ $CV == pass ]] && exit 0; exit 1   # a cached fail exits 1, as a fresh one does
  else echo "ignoring an unreadable cache file $VF" >&2; fi
fi

# one verify per device (one device per runtime build): a second would share it and shut it down under the first
LOCK=$SIMCAM_HOME/verify-$RTBUILD.lock
# a lock held by the Go CLI's detached shutdown child (a "shutdown" marker: udid, unix time) is waited for, up to 80 s (its
# ShutdownWait); a marker over 80 s old is a dead child's even if its pid was reused (cli/internal/verify/shutdown.go, w4 e38)
mk_age() { local t; t=$(awk '{print $2}' "$LOCK/shutdown" 2>/dev/null); [[ -n $t ]] && print $(( EPOCHSECONDS - t )); }
zmodload zsh/datetime
TW=$EPOCHSECONDS; SAID=0
while ! mkdir "$LOCK" 2>/dev/null; do
  OP=$(cat "$LOCK/pid" 2>/dev/null); [[ -z $OP ]] && { sleep 1; OP=$(cat "$LOCK/pid" 2>/dev/null); }   # its owner may not have written it yet
  [[ -d $LOCK ]] || continue   # released meanwhile (the Go shutdown child's handoff): try mkdir again (review r2)
  MA=$(mk_age)
  if [[ -n $OP && -n $MA ]] && (( MA <= 80 )) && kill -0 $OP 2>/dev/null && (( EPOCHSECONDS - TW < 80 )); then
    (( SAID )) || { SAID=1; echo "waiting for the previous verify's device $(awk '{print $1}' "$LOCK/shutdown" 2>/dev/null) to finish shutting down" >&2; }
    sleep 0.2; continue
  fi
  STALE=0; [[ -n $MA ]] && (( MA > 80 )) && STALE=1   # a marker older than the child can live: a dead child's lock
  if (( ! STALE )) && { [[ -z $OP ]] || kill -0 $OP 2>/dev/null; }; then LOCK=; echo "verdict inconclusive(another verify is using this runtime's device${OP:+, pid $OP})"; exit 2; fi
  rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null || { LOCK=; echo "verdict inconclusive(lock taken over by another verify)"; exit 2; }   # a stale lock (dead owner)
  break
done
echo $$ > "$LOCK/pid"

# the device: one per runtime build, in the private set
NAME=stuntcam-canary-$RTBUILD
U=$(sc list devices -j | python3 -I -c 'import json,sys; n=sys.argv[1]; print(next((d["udid"] for ds in json.load(sys.stdin)["devices"].values() for d in ds if d["name"] == n), ""))' $NAME)
CREATED=0
if [[ -z $U ]]; then
  # newest "iPhone <n> Pro" this runtime supports (the newest overall, iPhone 18 Pro, can't be created on 26.3 or 17.5)
  DT=$(xcrun simctl list runtimes -j | python3 -I -c 'import json,sys,re; r=next(r for r in json.load(sys.stdin)["runtimes"] if r["identifier"] == sys.argv[1]); d=[x for x in r.get("supportedDeviceTypes", []) if re.fullmatch(r"iPhone \d+ Pro", x["name"])]; print(max(d, key=lambda x: int(x["name"].split()[1]))["identifier"])' $RTID) || { echo "verdict inconclusive(no iPhone Pro device type for $RTID)"; exit 2; }
  echo "creating $NAME ($DT, $RTID) in the private set $SET (first use of this runtime: a minute or two)"
  U=$(sc create $NAME $DT $RTID) || { echo "verdict inconclusive(create failed)"; exit 2; }; CREATED=1
fi
TC=$SECONDS

# boot armed: same variables as boot.sh, under the private set (simcam_arm_vars writes SIMCAM_HOME/sims/<udid>/current)
boot_install() {   # -> 0, or prints an inconclusive verdict and exits
  sc shutdown $U >/dev/null 2>&1
  # a throwaway device: no sentinel history from earlier verifies (two attributed crashes would latch the canary and turn
  # the next verdict into a fail whose checks hide the cause)
  rm -rf "$SIMCAM_HOME/sims/$U/state"
  simcam_arm_vars canary $U
  env "${(@)ARMV/#/SIMCTL_CHILD_}" xcrun simctl --set "$SET" boot $U || { echo "verdict inconclusive(boot failed) key=$KEY"; exit 2; }
  xcrun simctl --set "$SET" bootstatus $U -b >/dev/null 2>&1 &   # macOS has no timeout(1): bound bootstatus ourselves
  BP=$!; for i in {1..120}; do kill -0 $BP 2>/dev/null || break; sleep 1; done
  if kill -0 $BP 2>/dev/null; then kill $BP 2>/dev/null; BP=; echo "verdict inconclusive(boot did not finish in 120 s) key=$KEY"; exit 2; fi
  wait $BP || { BP=; echo "verdict inconclusive(bootstatus failed) key=$KEY"; exit 2; }
  BP=
  # w2 e9: on a loaded Mac backboardd can start after SpringBoard and restart it ~5 s after bootstatus returns; an app installed
  # in that gap is refused ("Busy", "failed preflight checks") for the rest of the boot. Install only once SpringBoard has kept
  # the same pid for 2 s of wall time (each poll can itself take ~1 s on a loaded Mac, w2 e10a), at most 30 s.
  zmodload zsh/datetime
  local pid last= since=0 stable=0 seq= tg=$SECONDS
  while (( SECONDS - tg < 30 )); do
    pid=$(sc spawn $U launchctl list 2>/dev/null | awk '$3 == "com.apple.SpringBoard" {print $1}')
    [[ ${pid:-none} == ${seq##* } ]] || seq+=" ${pid:-none}"   # distinct pids seen, in order ("-" or none: not running)
    if [[ -n $pid && $pid != - && $pid == $last ]]; then (( EPOCHREALTIME - since >= 2 )) && { stable=1; break; }
    else since=$EPOCHREALTIME; fi
    last=$pid; sleep 0.3
  done
  (( stable )) || echo "SpringBoard not stable after 30 s (pid ${pid:-none}); installing anyway" >&2
  [[ $seq == " $pid" ]] || echo "SpringBoard was not running or changed pid after bootstatus (pids:$seq); waited $((SECONDS - tg)) s before install" >&2
  sc install $U "$APP" || { echo "verdict inconclusive(install failed) key=$KEY"; exit 2; }
  sc privacy $U grant camera $B >/dev/null 2>&1; sc privacy $U grant microphone $B >/dev/null 2>&1
}
boot_install
TB=$SECONDS

run_canary() {   # -> sets R to the canary's RESULT line (or a synthetic one)
  local log=$SIMCAM_LOGS/verify-$KEY-$RUNID-$1.log i bg=0   # one log per call: a later verify must not overwrite the evidence
  local try alive=0; REFUSED=0
  # SpringBoard can refuse a launch just after boot + install (FBSOpenApplicationServiceErrorDomain "Busy", "Application
  # failed preflight checks"): the canary never ran, and an immediate retry is refused too. Launch again after 2 s, 3 tries.
  for try in 1 2 3; do
    xcrun simctl --set "$SET" launch --console-pty $U $B > $log 2>&1 &   # not via sc(): $! must be simctl itself
    LP=$!
    for i in {1..600}; do sleep 0.1   # 60 s: the canary's budget is 45 s (CANARY_BUDGET) plus launch
      grep -q "RESULT" $log && break
      kill -0 $LP 2>/dev/null || break
      if (( ! bg )) && grep -q "BG-REQUEST" $log; then bg=1; sc launch $U com.apple.Preferences >/dev/null 2>&1; sleep 1.2; sc launch $U $B >/dev/null 2>&1; fi
    done
    grep -qE 'Busy|failed preflight' $log && ! grep -q "CANARY" $log || break
    (( try == 3 )) && REFUSED=1
    mv $log $log.refused-$try; echo "canary launch refused ($(grep -hE 'reason:|failed to launch' $log.refused-$try | tail -1 | sed 's/^[[:space:]]*//' | cut -c1-120)); launching again in 2 s" >&2; sleep 2
  done
  kill -0 $LP 2>/dev/null && alive=1
  sleep 0.3; kill $LP 2>/dev/null; LP=; sc terminate $U $B 2>/dev/null; sc terminate $U com.apple.Preferences 2>/dev/null
  R=$(grep -o "RESULT.*" $log 2>/dev/null | head -1 | tr -d '\r')   # the console pty ends lines with CR
  local last="$(grep -hE 'CANARY|reason:|failed to launch' $log $log.refused-*(N) 2>/dev/null | tail -1 | sed 's/^[[:space:]]*//' | cut -c1-120)"
  if [[ -z $R ]]; then   # no RESULT line: refused or still running is "couldn't run"; a canary that ran and exited without one crashed
    if (( REFUSED )); then R="RESULT core=INCONCLUSIVE (every launch refused; last: $last)"
    elif (( alive )) || ! grep -q "CANARY" $log 2>/dev/null; then R="RESULT core=INCONCLUSIVE (no verdict in 60 s; last: $last)"
    else R="RESULT core=FAIL ext=FAIL core:[canary exited without a verdict (crash); last: $last]"; fi
  fi
  return 0
}
run_canary 1; R1=$R
# every launch of this boot refused: reboot and reinstall once (the guard above makes this rare)
if (( REFUSED )); then echo "every canary launch was refused this boot; rebooting once" >&2; boot_install; run_canary 1r; R1=$R; fi
# one more run after an INCONCLUSIVE, or after a crash: a single kill (CoreSimulator restart, jetsam on a loaded Mac) must
# not become a cached fail; two crashes in a row are a fail(crash)
RL=   # the first launch's RESULT when a whole relaunch decided the verdict (relaunched)
if [[ $R == *core=INCONCLUSIVE* || $R == *"canary exited without a verdict"* ]]; then
  RL=$R1; run_canary 2
  # fail(crash) needs two crashes in a row: one crash after an inconclusive first run is still "couldn't tell"
  if [[ $R == *"canary exited without a verdict"* && $R1 != *"canary exited without a verdict"* ]]; then
    R="RESULT core=INCONCLUSIVE (the canary crashed once, after an inconclusive first run; not cached)"
  fi
fi
# a content fail is cached for good, so it is confirmed by one more launch first (w4 e40; the Go CLI's confirmFail)
FF=; NOSTORE=0; CONF=
if [[ $R == *core=FAIL* && $R != *"canary exited without a verdict"* ]]; then
  RF=$R; run_canary c
  if [[ $R == *core=FAIL* && $R != *"canary exited without a verdict"* ]]; then CONF=" (confirmed by a second launch)"
  elif [[ $R == *core=PASS* ]]; then FF=$RF; CONF=" (the first launch FAILED: $(print -r -- $RF | cut -c1-160); $([[ -n $NOCACHE ]] && echo "not cached: $NOCACHE" || echo "cached for an hour only"))"
  else NOSTORE=1; CONF=" (not cached: the launch run to confirm this fail gave no verdict: $(print -r -- $R | cut -c1-120))"; R=$RF; fi
fi
TR=$SECONDS   # canary= covers every launch: the first, a relaunch and a confirming one (w4 e44, review)
(( KEEP )) || sc shutdown $U >/dev/null 2>&1
TD=$SECONDS

case $R in
  *core=PASS*) V=pass; RC=0;;
  *core=FAIL*) V="fail($(print -r -- $R | grep -oE 'core:\[.*' | sed -E -e 's/\] (timing|ext):\[.*//' -e t -e 's/\]$//' | cut -c7-300))"; RC=1;;
  *) V="inconclusive($(print -r -- $R | cut -c8-200))"; RC=2;;
esac
NEW=; (( CREATED )) && NEW=" (new device)"
TIMES="create=$((TC - T0))s$NEW boot=$((TB - TC))s canary=$((TR - TB))s total=$((TD - T0))s"
if (( RC < 2 && ! NOSTORE )) && [[ -z $NOCACHE ]]; then
  python3 -I -c 'import json,sys,time,os,re; k,v,r,rt,b,sb,ch,ph,u,t=sys.argv[1:11]; f=sys.argv[11]; r1=sys.argv[12]; ff=sys.argv[13]
m=re.search(r" retried:\[(.*?)\](?= core:\[| timing:\[| ext:\[|$)", r)
d={"proto":1,"key":k,"verdict":v,"result":r,"runtime_build":rt,"engine_build":b,"stub_build":sb,"canary_sha":ch,"profile_sha":ph,"device":u,"times":t,"at":int(time.time())}
if m: d["retried"]=m.group(1)
if r1: d["relaunched"]=r1[:300]   # a pass after a whole relaunch is short-lived too (the Go CLI'"'"'s Result.Relaunched)
if ff: d["failed_first"]=ff[:300]   # a pass after an unconfirmed content fail: short-lived (Result.FailedFirst)
json.dump(d, open(f + ".tmp","w"), indent=1)
os.replace(f + ".tmp", f)' \
    "$KEY" "$V" "$R" "$RTBUILD" "$BUILD" "$STUBB" "$CH" "$PH" "$U" "$TIMES" "$VF" "$RL" "$FF"
fi
FIRST=; [[ -n $RL ]] && FIRST=" (first run: $(print -r -- $RL | cut -c1-120))"
echo "verdict $V key=$KEY runtime=$RTVER ($RTBUILD) device=$U $TIMES$FIRST$CONF"
exit $RC
