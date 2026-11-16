#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
#
# helper/dev.sh start [helper options] | stop [<cam>] | status — run the camera helper the way the CLI will.
#
#   start   launches build/current_helper.txt's bundle ($SIMCAM_HOME/helper/<sha>/Stuntcam Camera.app, from
#           helper/build.sh) with `open -n -g <absolute path> --args --home "$SIMCAM_HOME" <options>`: LaunchServices
#           makes it its own responsible process for camera permission. Never exec the binary directly: the launching
#           process would then be charged for the camera. Options are the helper's (helper/main.swift): --synthetic,
#           --camera <uniqueID>, --size WxH, --fps N, --stamp, --close-after S, --quit-after S. Waits up to 5 s for a
#           new pid file ($SIMCAM_HOME/webcam/<cam>.pid) and prints it.
#   stop    SIGTERM to the helper named by each live pid file (or by <cam>.pid), only if that pid runs a helper from
#           $SIMCAM_HOME/helper/; waits up to 5 s for it to exit (it clears its ring and unlinks it first).
#   status  every pid file, and whether its pid is alive.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"; source $ROOT/scripts/env.sh
W=$SIMCAM_HOME/webcam
alive() { kill -0 $1 2>/dev/null || [[ $(kill -0 $1 2>&1) == *"not permitted"* ]]; }
pidof_file() { sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p' $1 2>/dev/null | head -1; }
is_helper() { [[ $(ps -o comm= -p $1 2>/dev/null) == "$SIMCAM_HOME/helper/"*"/Stuntcam Camera.app/Contents/MacOS/stuntcam-camera" ]]; }
case ${1:-} in
  start)
    shift
    APP=$(cat $ROOT/build/current_helper.txt 2>/dev/null) || { echo "dev.sh: run helper/build.sh first" >&2; exit 2; }
    [[ $APP == /* && -d $APP ]] || { echo "dev.sh: no helper at $APP" >&2; exit 2; }
    mkdir -p $W; T0=$(date +%s)
    open -n -g "$APP" --args --home "$SIMCAM_HOME" "$@" || exit 1
    for i in {1..50}; do
      for f in $W/*.pid(N); do
        p=$(pidof_file $f); [[ -n $p ]] || continue
        if (( $(stat -f %m $f) >= T0 )) && alive $p; then echo "pid file $f"; cat $f; exit 0; fi
      done
      sleep 0.1
    done
    echo "dev.sh: no new pid file after 5 s (see $SIMCAM_HOME/logs/webcam-*.log)" >&2; exit 1;;
  stop)
    shift; typeset -a fs; if (( $# )); then fs=($W/$1.pid(N)); else fs=($W/*.pid(N)); fi
    for f in $fs; do
      p=$(pidof_file $f); [[ -n $p ]] && alive $p || continue
      if ! is_helper $p; then echo "dev.sh: pid $p in $f is not a helper; not touching it" >&2; continue; fi
      kill -TERM $p; for i in {1..50}; do alive $p || break; sleep 0.1; done
      if alive $p; then echo "dev.sh: helper $p still running after 5 s" >&2; else echo "stopped helper $p (${f:t})"; fi
    done;;
  status)
    for f in $W/*.pid(N); do p=$(pidof_file $f); echo "${f:t}: pid $p $(alive $p && echo alive || echo dead) $(grep -E '^(auth|state|camera)=' $f | tr '\n' ' ')"; done;;
  *) sed -n '3,16p' $0; exit 2;;
esac
