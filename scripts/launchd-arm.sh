#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# launchd-arm.sh [--off] <udid> — arms (or disarms) an already-booted sim in launchd mode, without a reboot.
#   Arm: every variable boot.sh passes at boot (scripts/arm-env.sh) is set in this sim's launchd with
#   `xcrun simctl spawn <udid> launchctl setenv`, so every process launched from now on inherits it. Apps that are
#   already running keep their old env until relaunched. Only the sim's own launchd_sim is touched, never the host's.
#   DYLD_INSERT_LIBRARIES: (1) the current value is read with `launchctl getenv` (stdout and stderr apart), (2) our stub
#   is appended and the list deduped (same path, a builds/<same hash>/ copy, or any libsimcam* basename: an older
#   stuntcam entry is replaced), so a loader that is already there (e.g. another tool's) keeps its place. (3) Each write
#   is read back and retried up to 3×.
#   --off: removes only our entries (libsimcam* basenames) from DYLD_INSERT_LIBRARIES, keeping the rest in order (unset
#   when nothing is left), and unsets the other variables the last arm set ($SIMCAM_HOME/sims/<udid>/launchd-vars.txt).
#   RT (for build/bootlogdir-<rt>.txt, which run.sh, p12.sh and lib.sh read): $RT, else from the sim's runtime
#   (26.3 -> 263, 27.0 -> 27, 17.5 -> 175). Extra settings: export SIMCTL_CHILD_<VAR>
#   (e.g. SIMCTL_CHILD_SIMCAM_LAZY=0), as for boot.sh.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/arm-env.sh"

OFF=0
if [[ ${1:-} == --off ]]; then
  OFF=1
  shift
fi
UDID=${1:-}
[[ -n $UDID ]] || { echo "usage: $0 [--off] <udid>" >&2; exit 2; }
xcrun simctl list devices | grep $UDID | grep -q Booted || { echo "$0: $UDID is not a booted simulator" >&2; exit 2; }

# The same path simcam_arm_vars sets SIM_DIR to; --off needs it without arming.
SIM_DIR=$SIMCAM_HOME/sims/$UDID
mkdir -p $SIM_DIR
LAUNCHCTL_ERR=$SIM_DIR/launchctl.err

# launchd_get <var>: prints the sim launchd's value (empty = unset). Returns 1 if launchctl itself failed.
launchd_get() {
  local value
  if ! value=$(xcrun simctl spawn $UDID launchctl getenv $1 2>$LAUNCHCTL_ERR); then
    echo "$0: launchctl getenv $1 failed: $(cat $LAUNCHCTL_ERR)" >&2
    return 1
  fi
  [[ -s $LAUNCHCTL_ERR ]] && echo "$0: launchctl getenv $1: $(cat $LAUNCHCTL_ERR)" >&2
  print -r -- "$value"
}

# launchd_set <var> <value>: writes the value, reads it back, and retries up to 3 times.
launchd_set() {
  local i got
  for i in 1 2 3; do
    xcrun simctl spawn $UDID launchctl setenv $1 "$2" 2>$LAUNCHCTL_ERR ||
      echo "$0: setenv $1 (try $i): $(cat $LAUNCHCTL_ERR)" >&2
    got=$(launchd_get $1) && [[ $got == "$2" ]] && return 0
    if [[ -z $got ]]; then
      echo "$0: $1 read back as '' (try $i); it may already be in the sim's boot environment (set at boot:" \
        "simctl boot passes SIMCTL_CHILD_$1 on, and launchd then ignores setenv of it)" >&2
    else
      echo "$0: $1 read back as '$got' (try $i)" >&2
    fi
    sleep 0.5
  done
  return 1
}

# launchd_unset <var>: unsets the variable, reads it back, and retries up to 3 times.
launchd_unset() {
  local i got
  for i in 1 2 3; do
    xcrun simctl spawn $UDID launchctl unsetenv $1 2>$LAUNCHCTL_ERR ||
      echo "$0: unsetenv $1 (try $i): $(cat $LAUNCHCTL_ERR)" >&2
    got=$(launchd_get $1) && [[ -z $got ]] && return 0
    echo "$0: $1 still '$got' after unsetenv (try $i)" >&2
    sleep 0.5
  done
  return 1
}

# is_ours <entry>: true when a DYLD_INSERT_LIBRARIES entry is stuntcam's own: any libsimcam* library, the current
# stub ($STUB), or a copy of the stub in another stuntcam build directory (.../builds/<hash>/<file>, with the same
# <hash> directory name as the stub's).
is_ours() {
  local entry=$1
  local file_name=${entry:t}   # :t keeps the last path component
  [[ $file_name == libsimcam* ]] && return 0
  [[ -n ${STUB:-} ]] || return 1
  [[ $entry == $STUB ]] && return 0

  local hash_dir=${entry:h:t}       # :h drops the last component: the name of the directory holding the entry
  local parent_dir=${entry:h:h:t}   # the name of the directory above that ("builds" for a stuntcam build copy)
  local stub_hash_dir=${STUB:h:t}   # the name of the directory holding the stub
  [[ $parent_dir == builds && $hash_dir == $stub_hash_dir ]]
}

# without_ours <list>: prints the colon list without our entries and duplicates, in its order.
without_ours() {
  local entry
  local -a entries kept
  entries=(${(s.:.)1})   # the list split at each colon (empty pieces are dropped; the -z test below is a guard)
  kept=()
  for entry in $entries; do
    if [[ -z $entry ]] || is_ours $entry; then
      continue
    fi
    # ${kept[(Ie)$entry]} is the index of the last element equal to $entry, 0 when there is none.
    if (( ${kept[(Ie)$entry]} == 0 )); then
      kept+=($entry)
    fi
  done
  print -r -- ${(j.:.)kept}   # joined with colons
}

fail=0
INSERTS_BEFORE=$(launchd_get DYLD_INSERT_LIBRARIES) || exit 1
echo "DYLD_INSERT_LIBRARIES before: ${INSERTS_BEFORE:-(unset)}"

# --off: our entries out of DYLD_INSERT_LIBRARIES, then every other variable an arm set.
if (( OFF )); then
  INSERTS_AFTER=$(without_ours "$INSERTS_BEFORE")
  if [[ -n $INSERTS_AFTER ]]; then
    launchd_set DYLD_INSERT_LIBRARIES "$INSERTS_AFTER" || fail=1
  else
    launchd_unset DYLD_INSERT_LIBRARIES || fail=1
  fi

  if [[ -f $SIM_DIR/launchd-vars.txt ]]; then
    # each line of the file, a variable name ((f) splits at newlines)
    for name in ${(f)"$(<$SIM_DIR/launchd-vars.txt)"}; do
      if [[ -n $name && $name != DYLD_INSERT_LIBRARIES ]]; then
        launchd_unset $name || fail=1
      fi
    done
    rm -f $SIM_DIR/launchd-vars.txt
  fi

  # a failed read prints an empty value
  echo "DYLD_INSERT_LIBRARIES after:  $(launchd_get DYLD_INSERT_LIBRARIES || true)"
  (( fail )) && { echo "$0: disarm FAILED for $UDID" >&2; exit 1; }
  echo "disarmed $UDID (launchd)"
  exit 0
fi

# Arm: the runtime, then this sim's variables with our stub added to DYLD_INSERT_LIBRARIES.
if [[ -z ${RT:-} ]]; then
  RT=$(xcrun simctl list devices -j | python3 -I -c '
import json
import re
import sys

udid = sys.argv[1]
for runtime, devices in json.load(sys.stdin)["devices"].items():
    for device in devices:
        if device["udid"] == udid:
            # "...SimRuntime.iOS-26-3" -> "263", "...iOS-27-0" -> "27"
            version = re.search(r"iOS-(\d+)-(\d+)", runtime)
            major, minor = version.group(1), version.group(2)
            print(major + (minor if minor != "0" else ""))' $UDID)
fi
[[ -n $RT ]] || { echo "$0: can't tell the runtime of $UDID; set RT" >&2; exit 2; }
PREV_CURRENT=$(cat "$SIMCAM_HOME/sims/$UDID/current" 2>/dev/null)   # restored if arming fails
simcam_arm_vars $RT $UDID
INSERTS_AFTER=$(without_ours "$INSERTS_BEFORE")
INSERTS_AFTER=${INSERTS_AFTER:+$INSERTS_AFTER:}$STUB   # the kept entries, a colon if there are any, then our stub

# Remembers every variable we ever set: re-arming must not forget earlier ones for --off.
touch $SIM_DIR/launchd-vars.txt

# DYLD_INSERT_LIBRARIES goes last: apps launched meanwhile must not get the stub without its settings.
typeset -a ORDERED
ORDERED=()
for kv in $ARM_VARS; do
  [[ ${kv%%=*} == DYLD_INSERT_LIBRARIES ]] || ORDERED+=($kv)
done
ORDERED+=("DYLD_INSERT_LIBRARIES=$INSERTS_AFTER")

typeset -a SET_THIS_RUN
SET_THIS_RUN=()
for kv in $ORDERED; do
  name=${kv%%=*}
  value=${kv#*=}
  grep -qx $name $SIM_DIR/launchd-vars.txt || echo $name >> $SIM_DIR/launchd-vars.txt
  if ! launchd_set $name "$value"; then
    echo "$0: could not set $name; rolling back" >&2
    fail=1

    # The rollback sets DYLD_INSERT_LIBRARIES as found, minus stuntcam's own entries. Other tools' loaders are kept,
    # and a failed re-arm leaves the sim cleanly disarmed, not half on an old stub. It also restores `current` and
    # unsets what this run set.
    INSERTS_RESTORED=$(without_ours "$INSERTS_BEFORE")
    if [[ -n $INSERTS_RESTORED ]]; then
      launchd_set DYLD_INSERT_LIBRARIES "$INSERTS_RESTORED" ||
        echo "$0: could not restore DYLD_INSERT_LIBRARIES=$INSERTS_RESTORED" >&2
    else
      launchd_unset DYLD_INSERT_LIBRARIES
    fi
    if [[ -n ${PREV_CURRENT:-} ]] && print -r -- "$PREV_CURRENT" > "$SIMCAM_HOME/sims/$UDID/current.tmp"; then
      mv "$SIMCAM_HOME/sims/$UDID/current.tmp" "$SIMCAM_HOME/sims/$UDID/current"
    fi
    for set_name in $SET_THIS_RUN; do
      [[ $set_name != DYLD_INSERT_LIBRARIES ]] && launchd_unset $set_name
    done
    break
  fi
  SET_THIS_RUN+=($name)
done

# a failed read prints an empty value
echo "DYLD_INSERT_LIBRARIES after:  $(launchd_get DYLD_INSERT_LIBRARIES || true)"
(( fail )) && { echo "$0: arm FAILED for $UDID" >&2; exit 1; }
echo "armed $UDID (launchd, rt $RT): ${#ARM_VARS} variables; boot logs $BOOT_LOG_DIR"
