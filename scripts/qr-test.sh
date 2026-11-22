#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# qr-test.sh <rt> [app ...] — the `qr:` source end to end. For each text case it runs `stuntcam source qr:<text>`,
# then each app (plain, vcqr, expoqr, flutter-qr; default all four) through run.sh, and every scanner the app has must
# report exactly that text (scripts/qr-check.py): plain = AVCaptureMetadataOutput (meta) + Vision on video frames
# (vision), vcqr = VisionCamera's object output, expoqr = expo-camera's scanner, flutter-qr = mobile_scanner. Then a
# live switch: `stuntcam source qr:<new text>` while each switch app runs; its scanners must report the new text, and
# the latency (from the command's start) is printed.
#   SIMCAM_UDID_<rt>: a simulator armed with `stuntcam up` (CLI: $STUNTCAM, default cli/bin/stuntcam) with the apps
#   installed (scripts/build-fixtures.sh); Metro running for vcqr/expoqr (scripts/metro.sh start).
#   QRT_CASES   text cases (default: url unicode cjk latin1 wifi vcard long1200 max; see make_cases)
#   QRT_FULL    apps run in every QRT_POS x QRT_ORIENT (default: plain; empty: none); the others run back + portrait
#               for every case, plus the max case front + landscapeLeft
#   QRT_POS     camera positions (default: back front); QRT_ORIENT orientations (default: portrait landscapeLeft
#               landscapeRight upsideDown)
#   QRT_SWITCH  apps for the live-switch check (default: plain; empty: none), QRT_SWITCHES switches each (default 3)
# Writes $SIMCAM_RESULTS/qr-<rt>-<time>/summary.txt (one line per run and scanner: `<rt> <case> <app> <pos> <orient>
# <src> <OK|WRONG|NONE|NOFIRST|NOCHECK> …`; run.sh's directories are <app>-<rt>-boot-q<time>n<N>) and exits 1 unless
# every line is OK. Leaves the simulator on source scene and orientation auto.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"
if [[ ${1:-} == (-h|--help) ]]; then
  print_usage $0
  exit 0
fi
RUNTIME=${1:?usage: qr-test.sh <rt> [app ...]}
shift
need_udid "$0" "$RUNTIME" UDID long

typeset -a APPS
APPS=("$@")
(( $#APPS )) || APPS=(plain vcqr expoqr flutter-qr)   # ("${@:-a b}" is one word)
CLI=${STUNTCAM:-$ROOT/cli/bin/stuntcam}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$SIMCAM_RESULTS/qr-$RUNTIME-$STAMP
mkdir -p $OUT/cases
SUMMARY=$OUT/summary.txt
: > $SUMMARY

# The settings, split into words. QRT_FULL and QRT_SWITCH may be set empty: no app.
CASES=(${=QRT_CASES:-url unicode cjk latin1 wifi vcard long1200 max})
FULL=(${=QRT_FULL-plain})
POSITIONS=(${=QRT_POS:-back front})
ORIENTS=(${=QRT_ORIENT:-portrait landscapeLeft landscapeRight upsideDown})
SWITCH_APPS=(${=QRT_SWITCH-plain})
SWITCHES=${QRT_SWITCHES:-3}

CASE_RUN_S=8              # each case launch runs the app's qr mode this long
SWITCH_RUN_S=14           # ... and each live-switch launch this long
FIRST_REPORT_POLLS=600    # 0.1 s polls for a switch launch's first report: 60 s (an RN app's first launch waits for
                          # Metro's bundle; run.sh copies its log once a second)
SCANNERS_SETTLE_S=1.5     # then this long, so every scanner has reported the first text

# make_cases: one file per text case, exact bytes, no trailing newline
python3 - $OUT/cases <<'PY'
import os
import sys

URL_CHARS = 'abcdefghijklmnopqrstuvwxyz0123456789-._~/?=&ABCDEFGHIJKLMNOPQRSTUVWXYZ'


def near(n):
    """A printable ASCII, URL-like text of n bytes."""
    text = 'https://example.com/'
    i = 0
    while len(text) < n:
        text += URL_CHARS[(i * 7 + 3) % len(URL_CHARS)]
        i += 1
    return text


cases_dir = sys.argv[1]
cases = {
    'url': 'https://example.com/pay?id=42',
    'unicode': 'Grüße, 日本語, emoji 🎉✓ — «ok»',
    'cjk': '日本語のテキスト',   # byte mode has no charset marker: a decoder that guesses Shift-JIS misreads UTF-8
    'latin1': 'café',            # ... or one that guesses ISO 8859-1
    'wifi': 'WIFI:S:My Café\\;Net;T:WPA;P:p@ss\\:w"rd\'$`!*%20\\\\;;',
    'vcard': 'BEGIN:VCARD\nVERSION:3.0\nFN:Test Person\nTEL:+1 555 0100\nEND:VCARD',
    'long1200': near(1200),
    'max': near(2331),
}
for name, text in cases.items():
    with open(os.path.join(cases_dir, name + '.txt'), 'w', encoding='utf-8', newline='') as f:
        f.write(text)
PY

# srcs_of <app>: the app's scanners, comma-separated (nothing for an unknown app)
srcs_of() {
  case $1 in
    plain)
      print meta,vision
      ;;
    vcqr)
      print vc
      ;;
    expoqr)
      print expo
      ;;
    flutter-qr)
      print flutter
      ;;
  esac
}

for app in $APPS $SWITCH_APPS; do
  if [[ -z $(srcs_of $app) ]]; then
    echo "qr-test.sh: unknown app $app (plain, vcqr, expoqr, flutter-qr)" >&2
    exit 2
  fi
done

# launch <app> <pos> <tag> [secs]: one run.sh launch of the app's qr mode; its log is $(log_dir <app> <tag>)/app.log
launch() {
  local app=$1
  local pos=$2
  local tag=$3
  local secs=${4:-10}
  case $app in
    plain)
      # its output stays in its result dir; the verdict is read from there (each case below)
      XENV="M5APP_MODE=qr QR_POS=$pos QR_SECS=$secs" $ROOT/scripts/run.sh plain $RUNTIME boot $tag >/dev/null 2>&1
      ;;
    vcqr|expoqr)
      QRPOS=$pos QRSECS=$secs $ROOT/scripts/run.sh $app $RUNTIME boot $tag >/dev/null 2>&1
      ;;
    flutter-qr)
      XENV="M7_QR_POS=$pos M7_QR_SECS=$secs" $ROOT/scripts/run.sh flutter-qr $RUNTIME boot $tag >/dev/null 2>&1
      ;;
  esac
}

# log_dir <app> <tag>: run.sh's result dir for that launch
log_dir() { print $SIMCAM_RESULTS/$1-$RUNTIME-boot-$2; }

# qr_results <app.log> <expected text file> <app> [--switch <unix ms> <first text file>]: qr-check.py's result line
# for each of the app's scanners (nothing if it printed none: no app.log, or it crashed)
qr_results() {
  local log=$1
  local expected=$2
  local app=$3
  shift 3
  python3 $ROOT/scripts/qr-check.py $log $expected $(srcs_of $app) "$@" 2>&1 | grep -E '^[a-z]+ (OK|WRONG|NONE|NOFIRST)'
}

# summarize <case> <app> <pos> <orient> <qr_results output>: one summary line per scanner (NOCHECK for each when
# qr-check printed nothing: no app.log, or it crashed)
summarize() {
  local line src
  if [[ -z $5 ]]; then
    for src in ${(s:,:)$(srcs_of $2)}; do   # the app's scanners, split at the commas
      print -r -- "$RUNTIME $1 $2 $3 $4 $src NOCHECK" | tee -a $SUMMARY
    done
  else
    for line in ${(f)5}; do   # one line per scanner
      print -r -- "$RUNTIME $1 $2 $3 $4 $line" | tee -a $SUMMARY
    done
  fi
}

# check <case> <app> <pos> <orient> <tag>
check() {
  local log
  log="$(log_dir $2 $5)/app.log"   # (the results dir has a space: never an unquoted $(…) argument)
  summarize $1 $2 $3 $4 "$(qr_results $log $OUT/cases/$1.txt $2)"
}

# Each text case: every app, in each position and orientation it runs in.
n=0
for case_name in $CASES; do
  if ! $CLI source --udid $UDID "qr:$(<$OUT/cases/$case_name.txt)" >/dev/null; then
    echo "$RUNTIME $case_name - - - source FAILED" | tee -a $SUMMARY
    continue
  fi

  for app in $APPS; do
    typeset -a combos   # "<pos> <orient>" pairs
    combos=()
    if (( ${FULL[(Ie)$app]} )); then   # the app is in FULL: every position and orientation
      for pos in $POSITIONS; do
        for orient in $ORIENTS; do
          combos+=("$pos $orient")
        done
      done
    else
      combos=("back portrait")
      if [[ $case_name == max ]]; then
        combos+=("front landscapeLeft")
      fi
    fi

    for combo in $combos; do
      pos=${combo% *}
      orient=${combo#* }
      n=$((n + 1))
      tag=q${STAMP}n$n
      $CLI orient --udid $UDID $orient >/dev/null
      launch $app $pos $tag $CASE_RUN_S
      check $case_name $app $pos $orient $tag
    done
  done
done
$CLI orient --udid $UDID portrait >/dev/null

# Live switch: the app runs on one text; after its scanners report it, a new one is set.
for app in $SWITCH_APPS; do
  for (( i = 1; i <= SWITCHES; i++ )); do
    n=$((n + 1))
    tag=q${STAMP}n$n
    first=$OUT/cases/url.txt
    new=$OUT/cases/switch$i.txt
    printf 'switched %s %s' $i $RANDOM > $new
    $CLI source --udid $UDID "qr:$(<$first)" >/dev/null
    launch $app back $tag $SWITCH_RUN_S &
    launch_pid=$!

    # The first report. The directory name is fresh, so no earlier run's log is read, and qr-check fails a scanner
    # that hadn't reported the first text before the switch.
    dir=$(log_dir $app $tag)
    for poll in {1..$FIRST_REPORT_POLLS}; do
      sleep 0.1
      # app.log may not exist yet
      grep -q QRSEEN $dir/app.log 2>/dev/null && break
    done
    sleep $SCANNERS_SETTLE_S

    switched_at_ms=$(python3 -c 'import time; print(int(time.time() * 1000))')
    $CLI source --udid $UDID "qr:$(<$new)" >/dev/null
    wait $launch_pid
    summarize switch$i $app back portrait "$(qr_results $dir/app.log $new $app --switch $switched_at_ms $first)"
  done
done
$CLI source --udid $UDID scene >/dev/null
$CLI orient --udid $UDID auto >/dev/null

good=$(awk '$7 == "OK"' $SUMMARY | wc -l | tr -d ' ')
bad=$(awk '$7 != "OK"' $SUMMARY | wc -l | tr -d ' ')
echo "qr-test $RUNTIME: $good OK, $bad not OK; $SUMMARY ($(uptime | sed 's/.*load/load/'))"
(( bad == 0 && good > 0 ))
