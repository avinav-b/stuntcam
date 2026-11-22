#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
#
# helper/dev.sh start [helper options] | stop [<cam>] | status — run the camera helper the way the CLI will.
#
#   start   launches build/current_helper.txt's bundle ($SIMCAM_HOME/helper/<sha>/Stuntcam Camera.app, from
#           helper/build.sh) with `open -n -g <absolute path> --args --home "$SIMCAM_HOME" <options>`: LaunchServices
#           makes it its own responsible process for camera permission. Never exec the binary directly: the launching
#           process would then be charged for the camera. Options are the helper's (helper/Options.swift): --synthetic,
#           --camera <uniqueID>, --size WxH, --fps N, --stamp, --close-after S, --quit-after S. Waits up to 5 s for a
#           new pid file ($SIMCAM_HOME/webcam/<cam>.pid) and prints it.
#   stop    SIGTERM to the helper named by each live pid file (or by <cam>.pid), only if that pid runs a helper from
#           $SIMCAM_HOME/helper/; waits up to 5 s for it to exit (it clears its ring and unlinks it first).
#   status  every pid file, and whether its pid is alive.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
source "$ROOT/scripts/env.sh"
WEBCAM_DIR=$SIMCAM_HOME/webcam

# alive <pid>: whether the process exists. A process of another user counts too: kill -0 says "not permitted".
alive() { kill -0 $1 2>/dev/null || [[ $(kill -0 $1 2>&1) == *"not permitted"* ]]; }

# pidof_file <pid file>: the pid in its pid= line (empty if there is none).
pidof_file() { sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p' $1 2>/dev/null | head -1; }

# is_helper <pid>: whether that pid runs a helper binary from $SIMCAM_HOME/helper/.
is_helper() {
  [[ $(ps -o comm= -p $1 2>/dev/null) == \
    "$SIMCAM_HOME/helper/"*"/Stuntcam Camera.app/Contents/MacOS/stuntcam-camera" ]]
}

case ${1:-} in
  start)
    shift
    APP=$(cat $ROOT/build/current_helper.txt 2>/dev/null) || { echo "dev.sh: run helper/build.sh first" >&2; exit 2; }
    [[ $APP == /* && -d $APP ]] || { echo "dev.sh: no helper at $APP" >&2; exit 2; }
    mkdir -p $WEBCAM_DIR
    START_S=$(date +%s)
    open -n -g "$APP" --args --home "$SIMCAM_HOME" "$@" || exit 1
    for i in {1..50}; do   # up to 5 s for a pid file written since the launch, of a live process
      for pid_file in $WEBCAM_DIR/*.pid(N); do
        pid=$(pidof_file $pid_file)
        [[ -n $pid ]] || continue
        if (( $(stat -f %m $pid_file) >= START_S )) && alive $pid; then
          echo "pid file $pid_file"
          cat $pid_file
          exit 0
        fi
      done
      sleep 0.1
    done
    echo "dev.sh: no new pid file after 5 s (see $SIMCAM_HOME/logs/webcam-*.log)" >&2
    exit 1
    ;;

  stop)
    shift
    typeset -a pid_files
    if (( $# )); then
      pid_files=($WEBCAM_DIR/$1.pid(N))
    else
      pid_files=($WEBCAM_DIR/*.pid(N))
    fi
    for pid_file in $pid_files; do
      pid=$(pidof_file $pid_file)
      [[ -n $pid ]] && alive $pid || continue
      if ! is_helper $pid; then
        echo "dev.sh: pid $pid in $pid_file is not a helper; not touching it" >&2
        continue
      fi
      kill -TERM $pid
      for i in {1..50}; do   # up to 5 s for it to exit
        alive $pid || break
        sleep 0.1
      done
      if alive $pid; then
        echo "dev.sh: helper $pid still running after 5 s" >&2
      else
        echo "stopped helper $pid (${pid_file:t})"
      fi
    done
    ;;

  status)
    for pid_file in $WEBCAM_DIR/*.pid(N); do
      pid=$(pidof_file $pid_file)
      state=$(alive $pid && echo alive || echo dead)
      echo "${pid_file:t}: pid $pid $state $(grep -E '^(auth|state|camera)=' $pid_file | tr '\n' ' ')"
    done
    ;;

  *)
    awk 'NR >= 3 && !/^#/ { exit } NR >= 3' $0   # the header comment, from line 3 to its end
    exit 2
    ;;
esac
