#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# verify.sh [--runtime <iOS version|runtime id>] [--fresh] [--keep]  — canary verdict on a private device set
#
#   The canary runs on a throwaway sim in stuntcam's own device set ($SIMCAM_HOME/devices, used with `simctl --set`).
#   Xcode and Simulator never list it. There is one device per runtime build, created on first use.
#   The steps: boot the device armed (with the boot environment, as boot.sh does), install the canary and grant its
#   permissions, launch it, and shut the device down. The launch gets 60 s for the RESULT line. The canary's own budget
#   is 45 s.
#
#   The verdict key joins five hashes: the runtime build, the engine build (the loader and engine hash dir), the stub
#   build, the canary executable and the profile. The preflight fingerprint will join it once tools/inventory has a
#   runtime half.
#
#   The verdict is pass, fail(<checks>) or inconclusive(<reason>), from the canary's own PASS, FAIL or INCONCLUSIVE.
#   - An INCONCLUSIVE is retried once. A second one is inconclusive(...). It means "couldn't run" and is never cached.
#   - A canary that exits without a verdict has crashed. It is run once more, and two crashes in a row are fail(crash).
#   - A content fail (core=FAIL, not a crash) is confirmed by one more launch. A second content fail is the verdict.
#     A pass after it is the verdict too, but it is cached for an hour only (failed_first, as the Go CLI's
#     Result.FailedFirst). A confirming launch with no verdict leaves the fail uncached.
#
#   pass and fail are cached in $SIMCAM_HOME/verdicts/<key>.json. The file is written atomically, and an unreadable
#   file counts as a miss. inconclusive is never cached.
#   A run with DYLIB=<engine> or extra SIMCTL_CHILD_* settings isn't cached either, because those change the result but
#   not the key. SIMCAM_CHAIN_FILE and SIMCAM_STATE_DIR are ignored.
#
#   Only one verify runs per runtime build at a time. The lock is $SIMCAM_HOME/verify-<build>.lock, taken after the
#   cache read. The Go CLI must use the same lock, key and file format.
#   --fresh ignores the cache, and --keep leaves the device booted. Ctrl-C or a hangup shuts the device down. The
#   device's sentinel state is cleared for each run.
#
#   Output: one line "verdict <state> key=<key> …" with timings. Exit 0 for pass, 1 for fail, 2 for inconclusive.
#   Needs ./build.sh and the canary app (scripts/build-fixtures.sh canary).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/arm-env.sh"

# How long to wait for a lock held by the Go CLI's detached shutdown child. It is the CLI's ShutdownWait: ShutdownLife
# (75 s) plus 5 s of slack, in cli/internal/verify/shutdown.go. A shutdown marker older than this belongs to a dead
# child.
SHUTDOWN_WAIT_S=80
# How long a retried pass is cached, as the Go CLI's RetriedPassTTL. A pass is retried when it needed the canary's
# frame-gap retry, a relaunch or a confirming launch.
RETRIED_PASS_TTL_S=3600
BOOTSTATUS_WAIT_S=120     # bootstatus gets this long (macOS has no timeout(1))
SPRINGBOARD_WAIT_S=30     # SpringBoard gets this long to keep one pid before the install goes ahead anyway
SPRINGBOARD_STABLE_S=2    # ... and must keep it this long (wall time)
CANARY_WAIT_S=60          # the canary's RESULT line: its own budget (45 s, CANARY_BUDGET) plus the launch
LAUNCH_TRIES=3            # launches refused as "Busy" are tried again this many times in all,
LAUNCH_RETRY_GAP_S=2      # ... this long apart

WANT=
FRESH=0
KEEP=0
while (( $# )); do
  case $1 in
    --runtime)
      WANT=$2
      shift 2
      ;;
    --fresh)
      FRESH=1
      shift
      ;;
    --keep)
      KEEP=1
      shift
      ;;
    *)
      echo "usage: $0 [--runtime V] [--fresh] [--keep]" >&2
      exit 64
      ;;
  esac
done

DEVICE_SET=$SIMCAM_HOME/devices
mkdir -p "$DEVICE_SET"

VERDICT_DIR=$SIMCAM_HOME/verdicts
mkdir -p "$VERDICT_DIR"

# m10: the milestone that added the canary; its product and bundle id are data and keep the name
APP=$ROOT/canary/build/m10canary.app
CANARY_BUNDLE=dev.simcam.spike.m10canary
if [[ ! -d $APP ]]; then
  echo "$0: no canary app at $APP (scripts/build-fixtures.sh canary)" >&2
  exit 64
fi

sc() { xcrun simctl --set "$DEVICE_SET" "$@"; }

START_S=$SECONDS
RUNID=$(date +%Y%m%d-%H%M%S)

# settings that would make this canary something other than the stock check on a clean device: never inherited
unset SIMCAM_CHAIN_FILE SIMCAM_STATE_DIR

LOCK=
DEVICE_UDID=
LAUNCH_PID=
BOOTSTATUS_PID=

cleanup() {   # every exit path: children, the device (unless --keep), our lock
  # a child that has already exited: nothing to kill
  [[ -n $LAUNCH_PID ]] && kill $LAUNCH_PID 2>/dev/null
  [[ -n $BOOTSTATUS_PID ]] && kill $BOOTSTATUS_PID 2>/dev/null
  [[ -n $DEVICE_UDID ]] && (( ! KEEP )) && sc shutdown $DEVICE_UDID >/dev/null 2>&1
  [[ -n $LOCK && $(cat "$LOCK/pid" 2>/dev/null) == $$ ]] && rm -rf "$LOCK"
}
trap cleanup EXIT
trap 'echo "verdict inconclusive(interrupted)" ; exit 130' INT TERM HUP

# ---------------------------------------------------------------- runtime, key, cache
# runtime: newest available iOS, or the one asked for (version "26.3" or a runtime identifier)
RUNTIME_INFO=$(xcrun simctl list runtimes -j | python3 -I -c '
import json
import sys

want = sys.argv[1]
runtimes = [runtime for runtime in json.load(sys.stdin)["runtimes"]
            if runtime["isAvailable"] and runtime["platform"] == "iOS"]
if want:
    runtimes = [runtime for runtime in runtimes
                if runtime["identifier"] == want or runtime["version"] == want
                or runtime["version"].startswith(want + ".")]
if not runtimes:
    sys.exit(1)
newest = sorted(runtimes, key=lambda runtime: [int(part) for part in runtime["version"].split(".")])[-1]
print(newest["identifier"], newest["buildversion"], newest["version"])' "$WANT")
if (( $? )); then
  echo "verdict inconclusive(no iOS runtime matching '${WANT:-any}')"
  exit 2
fi
# not `read <<<"$(…)" || …`: read succeeds on the empty line, so a missing runtime would go unnoticed
read RTID RTBUILD RTVER <<<"$RUNTIME_INFO"

if [[ ! -f $ROOT/build/current_loader.txt || ! -f $ROOT/build/current_stub.txt ]]; then
  echo "$0: no build (./build.sh)" >&2
  exit 64
fi

BUILD=$(basename "$(dirname "$(cat $ROOT/build/current_loader.txt)")")
STUB_BUILD=$(basename "$(dirname "$(cat $ROOT/build/current_stub.txt)")")

# the canary's checks are part of the verdict: a rebuilt canary is a new key
CANARY_SHA=$(shasum "$APP/m10canary" | cut -c1-8)

PROFILE=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}
PROFILE_SHA=$(shasum "$PROFILE" | cut -c1-12)
KEY=$RTBUILD-$BUILD-$STUB_BUILD-c$CANARY_SHA-$PROFILE_SHA
VERDICT_FILE=$VERDICT_DIR/$KEY.json

# settings that change the result but aren't in the key: never read or write the cache with them
NOCACHE=
[[ -n ${DYLIB:-} ]] && NOCACHE="DYLIB=$DYLIB"
CHILD_VARS=$(env | grep '^SIMCTL_CHILD_' | cut -d= -f1 | tr '\n' ' ')
[[ -n $CHILD_VARS ]] && NOCACHE="${NOCACHE:+$NOCACHE, }$CHILD_VARS"
[[ -n $NOCACHE ]] && echo "not using the verdict cache: $NOCACHE" >&2

if (( ! FRESH )) && [[ -z $NOCACHE && -f $VERDICT_FILE ]]; then
  # the cached verdict, "expired" for a retried pass over RETRIED_PASS_TTL_S old, or "" for an unreadable file or an
  # unknown verdict (a cache miss: Python's error is that answer, not one to show)
  CACHED=$(python3 -I -c '
import json
import sys
import time

cache = json.load(open(sys.argv[1]))
verdict = cache["verdict"]
retried = (cache.get("retried") or cache.get("relaunched") or cache.get("failed_first")
           or (" retried:[" in cache.get("result", "")))
expired = verdict == "pass" and retried and time.time() - cache.get("at", 0) > int(sys.argv[2])
print("expired" if expired else verdict if verdict == "pass" or verdict.startswith("fail(") else "")' \
    $VERDICT_FILE $RETRIED_PASS_TTL_S 2>/dev/null)
  if [[ $CACHED == expired ]]; then
    # "an hour" is RETRIED_PASS_TTL_S (3600 s): change the text with it
    echo "the cached pass needed a canary retry and is over an hour old: running the canary again" >&2
  elif [[ -n $CACHED ]]; then
    echo "verdict $CACHED key=$KEY (cached $VERDICT_FILE)"
    [[ $CACHED == pass ]] && exit 0
    exit 1   # a cached fail exits 1, as a fresh one does
  else
    echo "ignoring an unreadable cache file $VERDICT_FILE" >&2
  fi
fi

# ---------------------------------------------------------------- the per-runtime lock
# one verify per device (one device per runtime build): a second would share it and shut it down under the first
LOCK=$SIMCAM_HOME/verify-$RTBUILD.lock
# The Go CLI's detached shutdown child holds the lock with a "shutdown" marker file: "<udid> <unix time>".
# Such a lock is waited for, up to SHUTDOWN_WAIT_S. A marker older than that belongs to a dead child, even if its pid
# was reused.

# marker_age: the shutdown marker's age in seconds (nothing if there is no marker)
marker_age() {
  local marker_time
  marker_time=$(awk '{print $2}' "$LOCK/shutdown" 2>/dev/null)   # no marker: empty
  [[ -n $marker_time ]] && print $(( EPOCHSECONDS - marker_time ))
}

zmodload zsh/datetime
WAIT_START=$EPOCHSECONDS
SAID_WAITING=0

# a failed mkdir is the lock being held: the loop's answer, not an error to show
while ! mkdir "$LOCK" 2>/dev/null; do
  OWNER_PID=$(cat "$LOCK/pid" 2>/dev/null)
  if [[ -z $OWNER_PID ]]; then   # its owner may not have written it yet
    sleep 1
    OWNER_PID=$(cat "$LOCK/pid" 2>/dev/null)
  fi
  [[ -d $LOCK ]] || continue   # released meanwhile (the Go shutdown child's handoff): try mkdir again

  MARKER_AGE=$(marker_age)
  # a live shutdown child within its life, and this verify has not waited longer than that: wait
  if [[ -n $OWNER_PID && -n $MARKER_AGE ]] && (( MARKER_AGE <= SHUTDOWN_WAIT_S )) && kill -0 $OWNER_PID 2>/dev/null &&
    (( EPOCHSECONDS - WAIT_START < SHUTDOWN_WAIT_S )); then
    if (( ! SAID_WAITING )); then
      SAID_WAITING=1
      echo "waiting for the previous verify's device $(awk '{print $1}' "$LOCK/shutdown" 2>/dev/null)" \
        "to finish shutting down" >&2
    fi
    sleep 0.2
    continue
  fi

  STALE=0
  [[ -n $MARKER_AGE ]] && (( MARKER_AGE > SHUTDOWN_WAIT_S )) && STALE=1   # older than the child can live: dead
  if (( ! STALE )) && { [[ -z $OWNER_PID ]] || kill -0 $OWNER_PID 2>/dev/null; }; then
    LOCK=
    echo "verdict inconclusive(another verify is using this runtime's device${OWNER_PID:+, pid $OWNER_PID})"
    exit 2
  fi

  # a stale lock (dead owner): take it over
  rm -rf "$LOCK"
  if ! mkdir "$LOCK" 2>/dev/null; then
    LOCK=
    echo "verdict inconclusive(lock taken over by another verify)"
    exit 2
  fi
  break
done

echo $$ > "$LOCK/pid"

# ---------------------------------------------------------------- the device: one per runtime build, in the private set
NAME=stuntcam-canary-$RTBUILD
DEVICE_UDID=$(sc list devices -j | python3 -I -c '
import json
import sys

name = sys.argv[1]
devices = json.load(sys.stdin)["devices"]
print(next((device["udid"] for runtime_devices in devices.values() for device in runtime_devices
            if device["name"] == name), ""))' $NAME)

CREATED=0
if [[ -z $DEVICE_UDID ]]; then
  # newest "iPhone <n> Pro" this runtime supports (the newest overall, iPhone 18 Pro, can't be created on 26.3 or 17.5)
  DEVICE_TYPE=$(xcrun simctl list runtimes -j | python3 -I -c '
import json
import re
import sys

runtime = next(runtime for runtime in json.load(sys.stdin)["runtimes"] if runtime["identifier"] == sys.argv[1])
pro_types = [device_type for device_type in runtime.get("supportedDeviceTypes", [])
             if re.fullmatch(r"iPhone \d+ Pro", device_type["name"])]
print(max(pro_types, key=lambda device_type: int(device_type["name"].split()[1]))["identifier"])' $RTID)
  if (( $? )); then
    echo "verdict inconclusive(no iPhone Pro device type for $RTID)"
    exit 2
  fi
  echo "creating $NAME ($DEVICE_TYPE, $RTID) in the private set $DEVICE_SET" \
    "(first use of this runtime: a minute or two)"
  if ! DEVICE_UDID=$(sc create $NAME $DEVICE_TYPE $RTID); then
    echo "verdict inconclusive(create failed)"
    exit 2
  fi
  CREATED=1
fi

T_CREATED=$SECONDS

# wait_springboard: waits until SpringBoard has kept the same pid for SPRINGBOARD_STABLE_S, for at most
# SPRINGBOARD_WAIT_S. Both are wall time, because one poll can take about 1 s on a loaded Mac.
# It warns if SpringBoard never settled, or if its pid changed.
wait_springboard() {
  zmodload zsh/datetime
  local pid last= since=0 stable=0 seq= tg=$SECONDS
  while (( SECONDS - tg < SPRINGBOARD_WAIT_S )); do
    # a sim that does not answer yet gives no pid this round
    pid=$(sc spawn $DEVICE_UDID launchctl list 2>/dev/null | awk '$3 == "com.apple.SpringBoard" {print $1}')
    [[ ${pid:-none} == ${seq##* } ]] || seq+=" ${pid:-none}"   # distinct pids seen, in order ("-" or none: not running)
    if [[ -n $pid && $pid != - && $pid == $last ]]; then
      if (( EPOCHREALTIME - since >= SPRINGBOARD_STABLE_S )); then
        stable=1
        break
      fi
    else
      since=$EPOCHREALTIME
    fi
    last=$pid
    sleep 0.3
  done

  (( stable )) || echo "SpringBoard not stable after $SPRINGBOARD_WAIT_S s (pid ${pid:-none}); installing anyway" >&2
  [[ $seq == " $pid" ]] ||
    echo "SpringBoard was not running or changed pid after bootstatus (pids:$seq);" \
      "waited $((SECONDS - tg)) s before install" >&2
}

# boot_install: boots the device armed and installs the canary with its grants.
# Arming uses the same variables as boot.sh, under the private set. simcam_arm_vars writes
# SIMCAM_HOME/sims/<udid>/current.
# On failure it prints an inconclusive verdict and exits.
boot_install() {
  # fine if it is already shut down
  sc shutdown $DEVICE_UDID >/dev/null 2>&1

  # a throwaway device: no sentinel history from earlier verifies (two attributed crashes would latch the canary and
  # turn the next verdict into a fail whose checks hide the cause)
  rm -rf "$SIMCAM_HOME/sims/$DEVICE_UDID/state"
  simcam_arm_vars canary $DEVICE_UDID
  # ${(@)ARM_VARS/#/SIMCTL_CHILD_}: each VAR=value of ARM_VARS with SIMCTL_CHILD_ in front, one word each
  if ! env "${(@)ARM_VARS/#/SIMCTL_CHILD_}" xcrun simctl --set "$DEVICE_SET" boot $DEVICE_UDID; then
    echo "verdict inconclusive(boot failed) key=$KEY"
    exit 2
  fi

  # macOS has no timeout(1): bound it ourselves.
  xcrun simctl --set "$DEVICE_SET" bootstatus $DEVICE_UDID -b >/dev/null 2>&1 &
  BOOTSTATUS_PID=$!
  for i in {1..$BOOTSTATUS_WAIT_S}; do
    kill -0 $BOOTSTATUS_PID 2>/dev/null || break
    sleep 1
  done
  if kill -0 $BOOTSTATUS_PID 2>/dev/null; then
    kill $BOOTSTATUS_PID 2>/dev/null
    BOOTSTATUS_PID=
    echo "verdict inconclusive(boot did not finish in $BOOTSTATUS_WAIT_S s) key=$KEY"
    exit 2
  fi
  if ! wait $BOOTSTATUS_PID; then
    BOOTSTATUS_PID=
    echo "verdict inconclusive(bootstatus failed) key=$KEY"
    exit 2
  fi
  BOOTSTATUS_PID=

  # On a loaded Mac, backboardd can start after SpringBoard and restart it about 5 s after bootstatus returns.
  # An app installed in that gap is refused for the rest of the boot ("Busy", "failed preflight checks").
  wait_springboard

  if ! sc install $DEVICE_UDID "$APP"; then
    echo "verdict inconclusive(install failed) key=$KEY"
    exit 2
  fi

  # already granted: nothing to report
  sc privacy $DEVICE_UDID grant camera $CANARY_BUNDLE >/dev/null 2>&1
  sc privacy $DEVICE_UDID grant microphone $CANARY_BUNDLE >/dev/null 2>&1
}
boot_install
T_BOOTED=$SECONDS

# ---------------------------------------------------------------- the canary
# launch_refused <log>: true if SpringBoard refused the launch ("Busy", "failed preflight checks") and
# the canary never started
launch_refused() { grep -qE 'Busy|failed preflight' $1 && ! grep -q "CANARY" $1; }

# run_canary <tag>: one canary run, of up to LAUNCH_TRIES launches.
# Sets RESULT to the canary's RESULT line, or to a stand-in line when it printed none.
# Sets REFUSED=1 if every launch was refused.
run_canary() {
  # One log per call: a later verify must not overwrite it.
  local log=$SIMCAM_LOGS/verify-$KEY-$RUNID-$1.log i backgrounded=0
  local try alive=0
  REFUSED=0

  # SpringBoard can refuse a launch just after boot + install (FBSOpenApplicationServiceErrorDomain "Busy",
  # "Application failed preflight checks"): the canary never ran, and an immediate retry is refused too.
  # Launch again after LAUNCH_RETRY_GAP_S.
  for try in {1..$LAUNCH_TRIES}; do
    # not via sc(): $! must be simctl itself
    xcrun simctl --set "$DEVICE_SET" launch --console-pty $DEVICE_UDID $CANARY_BUNDLE > $log 2>&1 &
    LAUNCH_PID=$!
    for i in {1..$(( CANARY_WAIT_S * 10 ))}; do   # 0.1 s polls
      sleep 0.1
      grep -q "RESULT" $log && break
      kill -0 $LAUNCH_PID 2>/dev/null || break
      if (( ! backgrounded )) && grep -q "BG-REQUEST" $log; then
        # the canary asks to be sent to the background (Settings) and back
        backgrounded=1
        sc launch $DEVICE_UDID com.apple.Preferences >/dev/null 2>&1
        sleep 1.2
        sc launch $DEVICE_UDID $CANARY_BUNDLE >/dev/null 2>&1
      fi
    done
    launch_refused $log || break
    (( try == LAUNCH_TRIES )) && REFUSED=1
    mv $log $log.refused-$try
    local reason=$(grep -hE 'reason:|failed to launch' $log.refused-$try | tail -1 | sed 's/^[[:space:]]*//' |
      cut -c1-120)
    echo "canary launch refused ($reason); launching again in $LAUNCH_RETRY_GAP_S s" >&2
    sleep $LAUNCH_RETRY_GAP_S
  done

  kill -0 $LAUNCH_PID 2>/dev/null && alive=1
  sleep 0.3
  # the launch or the app may have ended already
  kill $LAUNCH_PID 2>/dev/null
  LAUNCH_PID=
  sc terminate $DEVICE_UDID $CANARY_BUNDLE 2>/dev/null
  sc terminate $DEVICE_UDID com.apple.Preferences 2>/dev/null

  RESULT=$(grep -o "RESULT.*" $log 2>/dev/null | head -1 | tr -d '\r')   # the console pty ends lines with CR
  local last="$(grep -hE 'CANARY|reason:|failed to launch' $log $log.refused-*(N) 2>/dev/null | tail -1 |
    sed 's/^[[:space:]]*//' | cut -c1-120)"

  # no RESULT line: refused or still running is "couldn't run"; a canary that ran and exited without one crashed
  if [[ -z $RESULT ]]; then
    if (( REFUSED )); then
      RESULT="RESULT core=INCONCLUSIVE (every launch refused; last: $last)"
    elif (( alive )) || ! grep -q "CANARY" $log 2>/dev/null; then
      RESULT="RESULT core=INCONCLUSIVE (no verdict in $CANARY_WAIT_S s; last: $last)"   # the Go CLI's same text
    else
      RESULT="RESULT core=FAIL ext=FAIL core:[canary exited without a verdict (crash); last: $last]"
    fi
  fi
  return 0
}

# crashed <RESULT line>: true if it is the synthetic line of a canary that exited without a verdict
crashed() { [[ $1 == *"canary exited without a verdict"* ]]; }

run_canary 1
RESULT1=$RESULT

# every launch of this boot refused: reboot and reinstall once (the guard above makes this rare)
if (( REFUSED )); then
  echo "every canary launch was refused this boot; rebooting once" >&2
  boot_install
  run_canary 1r
  RESULT1=$RESULT
fi

# One more run after an INCONCLUSIVE or a crash. A single kill, such as a CoreSimulator restart or jetsam on a loaded
# Mac, must not become a cached fail. Two crashes in a row are a fail(crash).
RELAUNCHED=   # the first launch's RESULT when a whole relaunch decided the verdict (relaunched)
if [[ $RESULT == *core=INCONCLUSIVE* ]] || crashed "$RESULT"; then
  RELAUNCHED=$RESULT1
  run_canary 2
  # fail(crash) needs two crashes in a row: one crash after an inconclusive first run is still "couldn't tell"
  if crashed "$RESULT" && ! crashed "$RESULT1"; then
    RESULT="RESULT core=INCONCLUSIVE (the canary crashed once, after an inconclusive first run; not cached)"
  fi
fi

# A content fail is cached for good, so one more launch confirms it first (the Go CLI's confirmFail).
#   FIRST_RESULT   the first launch's RESULT line: the fail being confirmed
#   FAILED_FIRST   the same line, set only when the confirming launch passed (the verdict JSON's failed_first)
#   UNCONFIRMED    1 when the confirming launch gave no verdict; then nothing is cached
#   CONFIRM_NOTE   what the verdict line says about the confirmation
FAILED_FIRST=
UNCONFIRMED=0
CONFIRM_NOTE=
if [[ $RESULT == *core=FAIL* ]] && ! crashed "$RESULT"; then
  FIRST_RESULT=$RESULT
  run_canary c
  if [[ $RESULT == *core=FAIL* ]] && ! crashed "$RESULT"; then
    CONFIRM_NOTE=" (confirmed by a second launch)"
  elif [[ $RESULT == *core=PASS* ]]; then
    FAILED_FIRST=$FIRST_RESULT
    # "an hour" is RETRIED_PASS_TTL_S (3600 s): change the text with it
    CACHE_NOTE=$([[ -n $NOCACHE ]] && echo "not cached: $NOCACHE" || echo "cached for an hour only")
    CONFIRM_NOTE=" (the first launch FAILED: $(print -r -- $FIRST_RESULT | cut -c1-160); $CACHE_NOTE)"
  else
    UNCONFIRMED=1
    CONFIRM_NOTE=" (not cached: the launch run to confirm this fail gave no verdict:"
    CONFIRM_NOTE+=" $(print -r -- $RESULT | cut -c1-120))"
    RESULT=$FIRST_RESULT
  fi
fi

T_CANARY=$SECONDS   # canary= covers every launch: the first, a relaunch and a confirming one
(( KEEP )) || sc shutdown $DEVICE_UDID >/dev/null 2>&1
T_DONE=$SECONDS

# ---------------------------------------------------------------- the verdict
# fail_checks <RESULT line>: the failing checks for fail(…), at most 294 characters (the Go CLI's failChecksLen).
# It is the core:[…] list without its brackets. Example:
#   in:  RESULT core=FAIL ext=PASS t=12.3s core:[photo: dims 0x0; frames: none] timing:[fps: 9 < 24]
#   out: photo: dims 0x0; frames: none
fail_checks() {
  # The list ends where the next list starts ("] timing:[" or "] ext:[").
  local drop_next_list='s/\] (timing|ext):\[.*//'
  # When no list follows, the core list ends the line: drop its own closing "]".
  local drop_closing_bracket='s/\]$//'
  # sed's "t" skips to the end of the script when the substitution before it matched, so only one of the two applies.
  local skip_if_cut='t'
  # Characters 7-300: skip "core:[" (6 characters) and keep at most 294.
  local checks_columns='7-300'

  print -r -- $1 |
    grep -oE 'core:\[.*' |
    sed -E -e "$drop_next_list" -e "$skip_if_cut" -e "$drop_closing_bracket" |
    cut -c"$checks_columns"
}

# inconclusive_reason <RESULT line>: the text for inconclusive(…), the line after "RESULT ", at most 193 characters
# (the Go CLI's inconclusiveLen). Example:
#   in:  RESULT core=INCONCLUSIVE ext=INCONCLUSIVE watchdog stage=photo after 45s
#   out: core=INCONCLUSIVE ext=INCONCLUSIVE watchdog stage=photo after 45s
inconclusive_reason() {
  # Characters 8-200: skip "RESULT " (7 characters) and keep at most 193.
  local reason_columns='8-200'

  print -r -- $1 | cut -c"$reason_columns"
}

case $RESULT in
  *core=PASS*)
    VERDICT=pass
    EXIT_STATUS=0
    ;;
  *core=FAIL*)
    VERDICT="fail($(fail_checks "$RESULT"))"
    EXIT_STATUS=1
    ;;
  *)
    VERDICT="inconclusive($(inconclusive_reason "$RESULT"))"
    EXIT_STATUS=2
    ;;
esac

NEW=
(( CREATED )) && NEW=" (new device)"
TIMES="create=$((T_CREATED - START_S))s$NEW boot=$((T_BOOTED - T_CREATED))s canary=$((T_CANARY - T_BOOTED))s"
TIMES+=" total=$((T_DONE - START_S))s"

if (( EXIT_STATUS < 2 && ! UNCONFIRMED )) && [[ -z $NOCACHE ]]; then
  # The cache file, in the format of cli/internal/verify's Result. Python reads the program from the here-doc, and each
  # input is a named --name=value argument, split by hand at its first "=" so that any value is taken as it is.
  python3 -I - \
    --key="$KEY" \
    --verdict="$VERDICT" \
    --result="$RESULT" \
    --runtime-build="$RTBUILD" \
    --engine-build="$BUILD" \
    --stub-build="$STUB_BUILD" \
    --canary-sha="$CANARY_SHA" \
    --profile-sha="$PROFILE_SHA" \
    --device="$DEVICE_UDID" \
    --times="$TIMES" \
    --cache-file="$VERDICT_FILE" \
    --relaunched="$RELAUNCHED" \
    --failed-first="$FAILED_FIRST" <<'PY'
import json
import os
import re
import sys
import time
from types import SimpleNamespace

MAX_NOTE_CHARS = 300   # of the relaunched and failed_first notes

# Each argument is --name=value. Not argparse: Python 3.9's drops a value that is exactly "--".
args = SimpleNamespace(**{name[2:].replace("-", "_"): value
                          for name, value in (argument.split("=", 1) for argument in sys.argv[1:])})

retried = re.search(r" retried:\[(.*?)\](?= core:\[| timing:\[| ext:\[|$)", args.result)
cache = {"proto": 1, "key": args.key, "verdict": args.verdict, "result": args.result,
         "runtime_build": args.runtime_build, "engine_build": args.engine_build, "stub_build": args.stub_build,
         "canary_sha": args.canary_sha, "profile_sha": args.profile_sha, "device": args.device, "times": args.times,
         "at": int(time.time())}
if retried:
    cache["retried"] = retried.group(1)

if args.relaunched:
    # A pass after a whole relaunch is short-lived too (the Go CLI's Result.Relaunched).
    cache["relaunched"] = args.relaunched[:MAX_NOTE_CHARS]

if args.failed_first:
    # A pass after an unconfirmed content fail is short-lived (Result.FailedFirst).
    cache["failed_first"] = args.failed_first[:MAX_NOTE_CHARS]

# Written to a temp file and renamed, so a reader never sees half a file.
json.dump(cache, open(args.cache_file + ".tmp", "w"), indent=1)
os.replace(args.cache_file + ".tmp", args.cache_file)
PY
fi

FIRST=
[[ -n $RELAUNCHED ]] && FIRST=" (first run: $(print -r -- $RELAUNCHED | cut -c1-120))"
echo "verdict $VERDICT key=$KEY runtime=$RTVER ($RTBUILD) device=$DEVICE_UDID $TIMES$FIRST$CONFIRM_NOTE"
exit $EXIT_STATUS
