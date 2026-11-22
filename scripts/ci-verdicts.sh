#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# scripts/ci-verdicts.sh [package.sh args…] — after scripts/package.sh: run the packaged CLI's canary once on each
# general iOS simulator runtime installed here, and package again with the plain passes as shipped verdicts. A user's
# first `stuntcam up` on those runtime builds then skips the self-test. Publishes nothing.
#
# The release workflow's build job runs it right after package.sh, with the same arguments (--helper on tags).
#
# Runtimes: every available iOS runtime whose identifier is com.apple.CoreSimulator.SimRuntime.iOS-<major>-<minor>.
#   Device-specific ones (such as iOS-27-2-iPhone19-4) are skipped, because no device type here can run them.
#   `--runtime 27.2` alone could pick one, so each verify gets the full identifier.
#
# Runs: one `stuntcam verify --runtime <identifier>` per runtime, from /, never re-run. Only a plain pass on the first
#   attempt ships. package.sh --verdicts drops retried, relaunched and confirmed-then-passed passes, which are
#   short-lived by design.
#
# Fails: a cached fail is a confirmed one, from two content fails or two crashes in a row. It fails this script once
#   every runtime has run, before anything is packaged, so on a tag the release stops.
#   An inconclusive verdict, an unconfirmed fail or a setup error (no runtime list, exit 64) is only a warning. That
#   runtime ships no verdict.
#
# Time: each verify gets $CI_VERDICTS_RUN_S seconds (default 600). Then it gets SIGTERM, and it shuts its device down.
#   If it is still there 90 s later, it gets SIGKILL and its devices are shut down.
#   A verify starts only if it can end, stop included, within $CI_VERDICTS_BUDGET_S seconds of the start (default
#   1500). The build job has 45 minutes.
#   The read-back below gets its own 120 s watchdog. It takes well under a second.
#   Both times are script settings, not repository variables.
#
# CI_VERDICTS_FAILS: a repository variable for a release that the runner itself breaks. Unset it afterwards.
#     warn   a confirmed fail only warns and ships nothing
#     skip   no canary runs, and no verdicts ship
#   A new value reaches "Re-run failed jobs" only if GitHub re-reads variables then. If it doesn't, run the workflow
#   by hand on the tag with `stage` ticked, because a new run reads it.
#
# Packaging: with no plain pass, nothing ships (a warning), and the package stays as package.sh built it.
#   Otherwise package.sh runs again with --no-build --verdicts <a dir with exactly those passes>.
#   The engine, stub and profile are reused, and the canary is rebuilt. The new payload.tar must be byte-identical to
#   the verified one, or the package from before is restored. A failed run restores it too, with a warning. This step
#   never costs a release its package.
#   The CLI binary is rebuilt as well, because it embeds the manifest's hash. The verdict key doesn't cover the CLI, by
#   design.
#
# Read-back: the new package's CLI reads each shipped verdict back from a fresh SIMCAM_HOME ("shipped with this
#   package").
#
# SIMCAM_HOME must be a scratch directory whose verdicts/ holds nothing yet, so the verdicts shipped are this run's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"
OUT_DIR=$ROOT/build/package
CLI=$OUT_DIR/stuntcam/bin/stuntcam
PAYLOAD=stuntcam/share/stuntcam/payload.tar
KILL_GRACE_S=90   # a stopped verify gets this long between SIGTERM and SIGKILL

warn() { print -r -- "::warning::ci-verdicts: $*" }

die() {
  print -r -- "::error::ci-verdicts: $*"
  exit 1
}

case ${CI_VERDICTS_FAILS:-} in
  '')
    ;;
  skip)
    warn "CI_VERDICTS_FAILS=skip: no canary runs and no verdicts ship" \
      "(unset the repository variable after this release)"
    exit 0
    ;;
  warn)
    warn "CI_VERDICTS_FAILS=warn: a confirmed canary fail only warns" \
      "(unset the repository variable after this release)"
    ;;
  *)
    die "CI_VERDICTS_FAILS is '$CI_VERDICTS_FAILS': want warn, skip or unset"
    ;;
esac

# whole_seconds <value> <name> <default>: prints <value> if it is a whole number of seconds (1 or more); otherwise warns
# and prints <default>.
whole_seconds() {
  local value=$1
  local name=$2
  local default=$3

  # <1-> is zsh's numeric-range pattern: digits only, with a value of 1 or more.
  if [[ $value == <1-> ]]; then
    print $value
  else
    warn "$name='$value' isn't a whole number of seconds: using $default" >&2
    print $default
  fi
}

RUN_S=$(whole_seconds "${CI_VERDICTS_RUN_S:-600}" CI_VERDICTS_RUN_S 600)
BUDGET_S=$(whole_seconds "${CI_VERDICTS_BUDGET_S:-1500}" CI_VERDICTS_BUDGET_S 1500)
[[ -n ${SIMCAM_HOME:-} ]] || die "SIMCAM_HOME isn't set (a scratch directory)"
[[ -x $CLI ]] || die "no $CLI: run scripts/package.sh first"
VERDICT_DIR=$SIMCAM_HOME/verdicts
existing=( $VERDICT_DIR/*(N) )   # (N): no match gives an empty list, not an error
(( $#existing == 0 )) || die "$VERDICT_DIR already holds verdicts: use a scratch SIMCAM_HOME"
command -v node >/dev/null || die "node not found"

# general_ios_runtimes: reads `simctl list runtimes -j` on stdin and prints one line per available general iOS
# runtime, each identifier once. Example line:
#   com.apple.CoreSimulator.SimRuntime.iOS-27-2 27.2 24C5089c
# Device-specific runtimes (com.apple.CoreSimulator.SimRuntime.iOS-27-2-iPhone19-4) don't match the pattern.
# The program's text keeps its layout, so a JSON error's stack trace names the same line and column.
general_ios_runtimes() {
  node -e '
  let s = ""; process.stdin.on("data", d => s += d).on("end", () => {
    const seen = new Set();
    for (const r of JSON.parse(s).runtimes)
      if (r.isAvailable && /^com\.apple\.CoreSimulator\.SimRuntime\.iOS-[0-9]+-[0-9]+$/.test(r.identifier) &&
          !seen.has(r.identifier)) {
        seen.add(r.identifier);
        console.log(`${r.identifier} ${r.version} ${r.buildversion}`);
      }
  });'
}

runtime_list=$(xcrun simctl list runtimes -j | general_ios_runtimes) || {
  warn "can't list the simulator runtimes: shipping no verdicts"
  exit 0
}
RUNTIMES=( ${(f)runtime_list} )   # one element per line
if (( $#RUNTIMES == 0 )); then
  warn "no general iOS simulator runtime installed: shipping no verdicts"
  exit 0
fi

# result_fields: reads a `verify --json` result on stdin and prints six fields on one line, separated by \x1f:
# key, verdict, times, retried, relaunched, failed_first. A missing field or unreadable JSON gives empty fields.
# Example (with \x1f shown as |):
#   in:  {"key":"K1","verdict":"pass","times":"create=0s boot=20s canary=31s total=51s"}
#   out: K1|pass|create=0s boot=20s canary=31s total=51s|||
# \x1f and newlines inside a value become spaces, so each field stays in its place.
# The separator is \x1f, not "|", because a QR text may hold "|". A non-whitespace IFS also keeps empty fields.
result_fields() {
  node -e '
  let s = ""; process.stdin.on("data", d => s += d).on("end", () => {
    let r = {}; try { r = JSON.parse(s); } catch {}
    const f = x => String(x || "").replace(/[\x1f\n]/g, " ");
    console.log([r.key, r.verdict, r.times, r.retried, r.relaunched, r.failed_first].map(f).join("\x1f"));
  });'
}

typeset -F SECONDS   # -F: a floating-point SECONDS, so it counts seconds with a fraction

# bounded <out file> <cmd…>: runs cmd from /, with its stdout in the file, for at most RUN_S seconds.
# Returns cmd's exit status.
# A cmd still running then gets SIGTERM, and SIGKILL KILL_GRACE_S seconds later. After a SIGKILL the devices of
# $SIMCAM_HOME are shut down.
# Also sets the global BOUNDED_STOPPED: 1 if cmd was stopped, 0 if it ended by itself.
bounded() {
  local out_file=$1
  local started=$SECONDS
  local term_sent verify_pid
  local exit_code=0
  shift
  (cd / && exec "$@") > $out_file &
  verify_pid=$!
  BOUNDED_STOPPED=0

  while kill -0 $verify_pid 2>/dev/null && (( SECONDS - started < RUN_S )); do
    sleep 1
  done

  if kill -0 $verify_pid 2>/dev/null; then
    # it may exit meanwhile
    kill -TERM $verify_pid 2>/dev/null || true
    BOUNDED_STOPPED=1
    term_sent=$SECONDS
    while kill -0 $verify_pid 2>/dev/null && (( SECONDS - term_sent < KILL_GRACE_S )); do
      sleep 1
    done
    if kill -0 $verify_pid 2>/dev/null; then
      # it may exit meanwhile, and there may be no device left to shut down
      kill -KILL $verify_pid 2>/dev/null || true
      xcrun simctl --set "$SIMCAM_HOME/devices" shutdown all >/dev/null 2>&1 || true
    fi
  fi

  wait $verify_pid || exit_code=$?
  return $exit_code
}

TMP_DIR=$(mktemp -d)
typeset -A SHIP   # identifier -> key of a plain pass
FAILED=()
for line in $RUNTIMES; do
  # line is "<identifier> <version> <build>", e.g. "com.apple.CoreSimulator.SimRuntime.iOS-27-2 27.2 24C5089c"
  id=${line%% *}                   # up to the first space
  version_and_build=${line#* }     # after the first space
  version=${version_and_build% *}  # up to the last space
  build=${line##* }                # after the last space
  what="iOS $version ($build)"
  if (( SECONDS + RUN_S + KILL_GRACE_S > BUDGET_S )); then
    warn "$what: not run, it could end past the ${BUDGET_S} s budget: no verdict"
    continue
  fi

  print "== $what: stuntcam verify --runtime $id"
  verify_started=$SECONDS
  before=( $VERDICT_DIR/*.json(N) )
  if bounded $TMP_DIR/out $CLI verify --runtime $id --json; then
    rc=0
  else
    rc=$?
  fi

  out=$(<$TMP_DIR/out)
  IFS=$'\x1f' read -r key verdict times retried relaunched failed_first <<< "$(print -r -- "$out" | result_fields)"
  print "   rc $rc, $(printf %.0f $(( SECONDS - verify_started )))s: ${verdict:-no verdict} ${times:+($times)}"
  if [[ -n $retried$relaunched$failed_first ]]; then
    print "   retried: ${retried:--}; relaunched: ${relaunched:--}; failed first: ${failed_first:--}"
  fi

  # A stopped verify prints no JSON. A fail it cached before the stop still counts.
  if (( BOUNDED_STOPPED )); then
    warn "$what: stopped after ${RUN_S} s: no verdict shipped"
    new=( $VERDICT_DIR/*.json(N) )
    new=( ${new:|before} )   # ${a:|b}: the elements of a that aren't in b, i.e. the files this verify added
    for f in $new; do
      if grep -q '"verdict": *"fail(' $f; then
        FAILED+=("$what: a fail cached before the stop (${f:t})")   # :t is the file name without its directory
      fi
    done
    continue
  fi

  case $rc:$verdict in
    0:pass)
      if [[ -n $retried$relaunched$failed_first ]]; then
        warn "$what: a pass that needed a retry, relaunch or confirming launch is short-lived: not shipped"
      elif [[ -z $key || ! -f $VERDICT_DIR/$key.json ]]; then
        warn "$what: the pass wasn't cached (see the log above): not shipped"
      else
        SHIP[$id]=$key
      fi
      ;;
    1:fail\(*)
      if [[ -n $key && -f $VERDICT_DIR/$key.json ]]; then
        FAILED+=("$what: $verdict")
      else
        warn "$what: a fail the confirming launch didn't confirm (not cached): not shipped: $verdict"
      fi
      ;;
    *)
      warn "$what: no verdict to ship (exit $rc): ${verdict:-see the log above}"
      ;;
  esac
done

if (( $#FAILED )) && [[ ${CI_VERDICTS_FAILS:-} == warn ]]; then
  for f in $FAILED; do
    warn "the canary failed (cached, confirmed) on $f: not shipped (CI_VERDICTS_FAILS=warn)"
  done
  FAILED=()
fi
for f in $FAILED; do
  print -r -- "::error::ci-verdicts: the canary failed (cached, confirmed) on $f"
done
if (( $#FAILED )); then
  die "the canary failed on ${#FAILED} runtime(s): not packaging. Re-check on a Mac (stuntcam" \
    "verify --fresh). If the runner is the cause, set the repository variable CI_VERDICTS_FAILS=warn (or skip) for" \
    "this release"
fi
if (( ${#SHIP} == 0 )); then
  warn "no plain canary pass on this runner: shipping no verdicts"
  exit 0
fi

shipped_keys=( ${(@v)SHIP} )   # (@v): the values of SHIP, one key per shipped runtime
key_list=${(j:, :)shipped_keys}   # (j:, :): the keys joined with ", "
print "== packaging again with ${#SHIP} verdict(s): $key_list"
BACKUP_DIR=$(mktemp -d)   # package.sh gets exactly the passes chosen above, never the rest of $VERDICT_DIR
mkdir $BACKUP_DIR/verdicts
for key in "${shipped_keys[@]}"; do
  cp $VERDICT_DIR/$key.json $BACKUP_DIR/verdicts/
done
cp -Rp $OUT_DIR $BACKUP_DIR/package || {
  warn "can't back the package up: shipping no verdicts"
  exit 0
}

# restore <why>: puts the package from before back and exits 0
restore() {
  warn "$1: restoring the package without verdicts"
  rm -rf $OUT_DIR
  mv $BACKUP_DIR/package $OUT_DIR
  exit 0
}

VERDICTS_FROM="release-build canary pass, first attempt ($(sw_vers -productVersion), $(uname -m))" \
  scripts/package.sh "$@" --no-build --verdicts $BACKUP_DIR/verdicts || restore "package.sh --verdicts failed"
cmp -s $BACKUP_DIR/package/$PAYLOAD $OUT_DIR/$PAYLOAD || restore "the new payload.tar differs from the verified one"
rm -rf $BACKUP_DIR

# The new package's CLI finds each verdict from a fresh home (a miss would run the canary, bounded: never wanted here).
READBACK_DIR=$(mktemp -d)
for id key in "${(@kv)SHIP}"; do   # (@kv): each identifier, then its key
  # Its exit status doesn't matter: the printed line says whether the verdict was found.
  RUN_S=120 SIMCAM_HOME=$READBACK_DIR/home bounded $TMP_DIR/out $CLI verify --runtime $id || true
  out=$(<$TMP_DIR/out)
  if [[ $out == *"key=$key "*"shipped with this package"* ]]; then
    print "   $id: read back as shipped"
  else
    warn "$id: the packaged CLI didn't read its shipped verdict back: ${out[1,300]}"
  fi
done
xcrun simctl --set $READBACK_DIR/home/devices shutdown all >/dev/null 2>&1 || true   # only if a miss ran the canary
xcrun simctl --set $READBACK_DIR/home/devices delete all >/dev/null 2>&1 || true
rm -rf $READBACK_DIR $TMP_DIR
