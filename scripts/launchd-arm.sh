#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# launchd-arm.sh [--off] <udid> — arm (or disarm) an already-booted sim in launchd mode (design §4.2), no reboot
#   Arm: every variable boot.sh passes at boot (scripts/arm-env.sh) is set in this sim's launchd with
#   `xcrun simctl spawn <udid> launchctl setenv`, so every process launched from now on inherits it. Apps that are already
#   running keep their old env until relaunched. Only the sim's own launchd_sim is touched, never the host's.
#   DYLD_INSERT_LIBRARIES: (1) the current value is read with `launchctl getenv` (stdout and stderr apart), (2) our stub is
#   appended and the list deduped (same path, a builds/<same hash>/ copy, or any libsimcam* basename: an older stuntcam entry is
#   replaced), so a loader that is already there (e.g. another tool's) keeps its place. (3) Each write is read back
#   and retried up to 3×.
#   --off: removes only our entries (libsimcam* basenames) from DYLD_INSERT_LIBRARIES, keeping the rest in order (unset when
#   nothing is left), and unsets the other variables the last arm set ($SIMCAM_HOME/sims/<udid>/launchd-vars.txt).
#   RT (for build/bootlogdir-<rt>.txt, which run.sh, p12.sh and lib.sh read): $RT, else from the sim's runtime (26.3 -> 263,
#   27.0 -> 27, 17.5 -> 175). Extra settings: export SIMCTL_CHILD_<VAR> (e.g. SIMCTL_CHILD_SIMCAM_LAZY=0), as for boot.sh.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/arm-env.sh
OFF=0; [[ ${1:-} == --off ]] && { OFF=1; shift; }
UDID=${1:-}; [[ -n $UDID ]] || { echo "usage: $0 [--off] <udid>" >&2; exit 2; }
xcrun simctl list devices | grep $UDID | grep -q Booted || { echo "$0: $UDID is not a booted simulator" >&2; exit 2; }
STATE=$SIMCAM_HOME/sims/$UDID; mkdir -p $STATE; ERR=$STATE/launchctl.err

# lget <var>: the sim launchd's value on stdout (empty = unset); exit 1 if launchctl itself failed
lget() {
  local v; v=$(xcrun simctl spawn $UDID launchctl getenv $1 2>$ERR) || { echo "$0: launchctl getenv $1 failed: $(cat $ERR)" >&2; return 1; }
  [[ -s $ERR ]] && echo "$0: launchctl getenv $1: $(cat $ERR)" >&2
  print -r -- "$v"
}
# lset <var> <value>: write, read back, retry up to 3×
lset() {
  local i got
  for i in 1 2 3; do
    xcrun simctl spawn $UDID launchctl setenv $1 "$2" 2>$ERR || echo "$0: setenv $1 (try $i): $(cat $ERR)" >&2
    got=$(lget $1) && [[ $got == "$2" ]] && return 0
    if [[ -z $got ]]; then echo "$0: $1 read back as '' (try $i); it may already be in the sim's boot environment (set at boot: simctl boot passes SIMCTL_CHILD_$1 on, and launchd then ignores setenv of it; w3 e13, w4 e46)" >&2
    else echo "$0: $1 read back as '$got' (try $i)" >&2; fi; sleep 0.5
  done
  return 1
}
# lunset <var>: unset, read back, retry up to 3×
lunset() {
  local i got
  for i in 1 2 3; do
    xcrun simctl spawn $UDID launchctl unsetenv $1 2>$ERR || echo "$0: unsetenv $1 (try $i): $(cat $ERR)" >&2
    got=$(lget $1) && [[ -z $got ]] && return 0
    echo "$0: $1 still '$got' after unsetenv (try $i)" >&2; sleep 0.5
  done
  return 1
}
# without_ours <list>: the colon list minus our entries and duplicates, order kept
without_ours() {
  local e; local -a out; out=()
  for e in ${(s.:.)1}; do
    [[ -z $e || ${e:t} == libsimcam* ]] && continue
    [[ -n ${STUB:-} && ( $e == $STUB || ( ${e:h:h:t} == builds && ${e:h:t} == ${STUB:h:t} ) ) ]] && continue   # a stuntcam builds/<hash>/ copy of the same stub
    (( ${out[(Ie)$e]} )) || out+=($e)
  done
  print -r -- ${(j.:.)out}
}

fail=0
OLD=$(lget DYLD_INSERT_LIBRARIES) || exit 1
echo "DYLD_INSERT_LIBRARIES before: ${OLD:-(unset)}"
if (( OFF )); then
  NEW=$(without_ours "$OLD")
  if [[ -n $NEW ]]; then lset DYLD_INSERT_LIBRARIES "$NEW" || fail=1; else lunset DYLD_INSERT_LIBRARIES || fail=1; fi
  if [[ -f $STATE/launchd-vars.txt ]]; then
    for v in ${(f)"$(<$STATE/launchd-vars.txt)"}; do [[ -n $v && $v != DYLD_INSERT_LIBRARIES ]] && { lunset $v || fail=1; }; done
    rm -f $STATE/launchd-vars.txt
  fi
  echo "DYLD_INSERT_LIBRARIES after:  $(lget DYLD_INSERT_LIBRARIES || true)"
  (( fail )) && { echo "$0: disarm FAILED for $UDID" >&2; exit 1; }
  echo "disarmed $UDID (launchd)"; exit 0
fi

if [[ -z ${RT:-} ]]; then
  RT=$(xcrun simctl list devices -j | python3 -I -c 'import json,sys,re
u=sys.argv[1]
for k,ds in json.load(sys.stdin)["devices"].items():
  for d in ds:
    if d["udid"]==u:
      m=re.search(r"iOS-(\d+)-(\d+)",k); print(m.group(1)+(m.group(2) if m.group(2)!="0" else ""))' $UDID)
fi
[[ -n $RT ]] || { echo "$0: can't tell the runtime of $UDID; set RT" >&2; exit 2; }
PREV_CURRENT=$(cat "$SIMCAM_HOME/sims/$UDID/current" 2>/dev/null)   # restored if arming fails
simcam_arm_vars $RT $UDID
NEW=$(without_ours "$OLD"); NEW=${NEW:+$NEW:}$STUB
# remember every variable we ever set (re-arming must not forget earlier ones for --off)
touch $STATE/launchd-vars.txt
# DYLD_INSERT_LIBRARIES last: apps launched meanwhile must not get the stub without its settings (review r1)
typeset -a ORDERED; ORDERED=(); for kv in $ARMV; do [[ ${kv%%=*} == DYLD_INSERT_LIBRARIES ]] || ORDERED+=($kv); done; ORDERED+=("DYLD_INSERT_LIBRARIES=$NEW")
typeset -a SETNOW; SETNOW=()
for kv in $ORDERED; do
  k=${kv%%=*}; v=${kv#*=}
  grep -qx $k $STATE/launchd-vars.txt || echo $k >> $STATE/launchd-vars.txt
  if ! lset $k "$v"; then
    echo "$0: could not set $k; rolling back" >&2; fail=1
    # put DYLD_INSERT_LIBRARIES back as found minus stuntcam's own entries (other tools' loaders kept), unset what this run set
    # (minus stuntcam's own entries, so a failed re-arm leaves the sim cleanly disarmed rather than half on an old stub)
    REST=$(without_ours "$OLD")
    if [[ -n $REST ]]; then lset DYLD_INSERT_LIBRARIES "$REST" || echo "$0: could not restore DYLD_INSERT_LIBRARIES=$REST" >&2
    else lunset DYLD_INSERT_LIBRARIES; fi
    [[ -n ${PREV_CURRENT:-} ]] && print -r -- "$PREV_CURRENT" > "$SIMCAM_HOME/sims/$UDID/current.tmp" && mv "$SIMCAM_HOME/sims/$UDID/current.tmp" "$SIMCAM_HOME/sims/$UDID/current"
    for s in $SETNOW; do [[ $s != DYLD_INSERT_LIBRARIES ]] && lunset $s; done
    break
  fi
  SETNOW+=($k)
done
echo "DYLD_INSERT_LIBRARIES after:  $(lget DYLD_INSERT_LIBRARIES || true)"
(( fail )) && { echo "$0: arm FAILED for $UDID" >&2; exit 1; }
echo "armed $UDID (launchd, rt $RT): ${#ARMV} variables; boot logs $BL"
