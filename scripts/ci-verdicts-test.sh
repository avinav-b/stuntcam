#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# scripts/ci-verdicts-test.sh — every branch of scripts/ci-verdicts.sh against stubs (a fake packaged CLI, package.sh,
# xcrun and sw_vers in a temp tree): no simulator, no build, no Xcode. Runs anywhere with bash, zsh and node
# (ZSH=/path/to/zsh to pick one). Exit 0 when every case passes.
set -u
# the release step exports these (CI_VERDICTS_FAILS from a repository variable): each case sets its own
unset CI_VERDICTS_FAILS CI_VERDICTS_RUN_S CI_VERDICTS_BUDGET_S
REPO="$(cd "$(dirname "$0")/.." && pwd -P)"
ZSH=${ZSH:-zsh}
pass=0
fail=0

# setup <mode> ...: a temp tree $T with the script, stub tools and a fake package. One mode per runtime, in order
# (iOS-27-1, iOS-27-2, …): pass|retried|nocache|fail|unconf|inconc|setup|hang|latepass|latefail. A device-specific
# runtime (iOS-27-2-iPhone19-4) is always listed too; the script must skip it.
setup() {
  T=$(mktemp -d)
  mkdir -p $T/scripts $T/build/package/stuntcam/bin $T/build/package/stuntcam/share/stuntcam $T/stub $T/home
  cp $REPO/scripts/ci-verdicts.sh $T/scripts/
  echo payload > $T/build/package/stuntcam/share/stuntcam/payload.tar
  echo orig > $T/build/package/marker

  stub_runtimes "$@"
  stub_xcrun
  stub_cli
  stub_package
  chmod +x $T/stub/* $T/build/package/stuntcam/bin/stuntcam $T/scripts/package.sh
}

# stub_runtimes <mode> ...: each runtime's mode file, and the runtimes `xcrun simctl list runtimes -j` lists
stub_runtimes() {
  local i=0
  local rts=""
  local runtime mode
  for mode in "$@"; do
    i=$((i+1))
    echo $mode > $T/stub/mode-iOS-27-$i
    runtime="{\"isAvailable\":true,\"identifier\":\"com.apple.CoreSimulator.SimRuntime.iOS-27-$i\","
    runtime+="\"version\":\"27.$i\",\"buildversion\":\"B$i\"},"
    rts="$rts$runtime"
  done
  rts="$rts{\"isAvailable\":true,\"identifier\":\"com.apple.CoreSimulator.SimRuntime.iOS-27-2-iPhone19-4\","
  rts+="\"version\":\"27.2\",\"buildversion\":\"DS\"}"
  echo "{\"runtimes\":[$rts]}" > $T/stub/runtimes.json
}

# stub_xcrun: an xcrun that only lists the runtimes (STUB_XCRUN_FAIL=1 makes that fail), and a sw_vers
stub_xcrun() {
  cat > $T/stub/xcrun <<'X'
#!/bin/bash
[[ ${STUB_XCRUN_FAIL:-} == 1 && $* == *"list runtimes"* ]] && exit 1
[[ $* == *"list runtimes -j"* ]] && cat "$STUBDIR/runtimes.json"
exit 0
X
  printf '#!/bin/bash\necho 27.0.1\n' > $T/stub/sw_vers
}

# stub_cli: the packaged CLI. `verify --json` answers as its runtime's mode says; plain `verify` is the read-back.
stub_cli() {
  cat > $T/build/package/stuntcam/bin/stuntcam <<'C'
#!/bin/bash
id=${3##*.}
key="K-$id"
if [[ " $* " != *" --json "* ]]; then
  if [[ -f $STUBDIR/shipped/$key.json && -z ${STUB_READBACK_MISS:-} ]]; then
    echo "verdict pass  key=$key  iOS x (y)  shipped with this package (measured z)"
  else
    echo "verdict pass key=$key device D"
  fi
  exit 0
fi
m=$(cat $STUBDIR/mode-$id)
mkdir -p $SIMCAM_HOME/verdicts
cached=$SIMCAM_HOME/verdicts/$key.json
case $m in
  pass)
    echo "{\"key\":\"$key\",\"verdict\":\"pass\",\"times\":\"t\"}"
    echo '{}' > $cached
    exit 0
    ;;
  retried)
    echo "{\"key\":\"$key\",\"verdict\":\"pass\",\"retried\":\"photo-jpeg 120 ms -> retry ok\"}"
    echo '{}' > $cached
    exit 0
    ;;
  nocache)
    echo "{\"key\":\"$key\",\"verdict\":\"pass\"}"
    exit 0
    ;;
  fail)
    echo "{\"key\":\"$key\",\"verdict\":\"fail(qr: x|y\u001fz)\"}"
    echo '{}' > $cached
    exit 1
    ;;
  unconf)
    echo "{\"key\":\"$key\",\"verdict\":\"fail(qr)\"}"
    exit 1
    ;;
  inconc)
    echo "{\"key\":\"$key\",\"verdict\":\"inconclusive(device: no Pro)\"}"
    exit 2
    ;;
  setup)
    echo "stuntcam: verify setup: no canary" >&2
    exit 64
    ;;
  hang)
    trap 'exit 130' TERM
    sleep 30 &
    wait
    exit 0
    ;;
  latepass)
    echo '{"verdict": "pass"}' > $cached
    trap 'exit 130' TERM
    sleep 30 &
    wait
    ;;
  latefail)
    echo '{"verdict": "fail(x)"}' > $cached
    trap 'exit 130' TERM
    sleep 30 &
    wait
    ;;
esac
C
}

# stub_package: package.sh. It records its arguments and the verdicts it was given; STUB_PKG_FAIL and
# STUB_PAYLOAD_CHANGE break it.
stub_package() {
  cat > $T/scripts/package.sh <<'P'
#!/bin/bash
[[ ${STUB_PKG_FAIL:-} == 1 ]] && { rm -rf build/package/stuntcam; exit 1; }
[[ ${STUB_PAYLOAD_CHANGE:-} == 1 ]] && echo changed >> build/package/stuntcam/share/stuntcam/payload.tar
echo new > build/package/marker
mkdir -p "$STUBDIR/shipped"
for f in "${@: -1}"/*.json; do
  b=$(basename $f)
  [[ $b == K-* ]] && cp $f "$STUBDIR/shipped/$b"
done
echo "pkg args: $*"
echo "pkg got: $(cd "${@: -1}" && ls | tr '\n' ' ')"
P
}

# check <name> <want rc> <want marker> <text> ...: the last run's exit status, the package marker ("orig": the package
# from before, "new": repackaged) and texts its log must hold; prints PASS or FAIL (with the log) and removes $T
check() {
  local name=$1
  local want_rc=$2
  local want_marker=$3
  shift 3
  local ok=1
  # no marker: empty
  local marker=$(cat $T/build/package/marker 2>/dev/null)
  if [[ $rc != $want_rc ]]; then
    ok=0
    echo "  rc $rc want $want_rc"
  fi
  if [[ $marker != $want_marker ]]; then
    ok=0
    echo "  marker $marker want $want_marker"
  fi
  for g in "$@"; do
    if ! grep -qF -- "$g" $T/log; then
      ok=0
      echo "  missing: $g"
    fi
  done
  if ((ok)); then
    pass=$((pass+1))
    echo "PASS $name"
  else
    fail=$((fail+1))
    echo "FAIL $name"
    sed 's/^/    | /' $T/log
  fi
  rm -rf $T
}

# go [VAR=value ...]: runs the script in $T with the stubs first on PATH and the settings given; sets rc
go() {
  (cd $T && PATH=$T/stub:$PATH STUBDIR=$T/stub SIMCAM_HOME=$T/home "$@" $ZSH scripts/ci-verdicts.sh --helper "X Y.app" \
    > $T/log 2>&1)
  rc=$?
}

setup pass pass
go env
check "two plain passes ship" 0 new "packaging again with 2 verdict(s)" \
  "pkg args: --helper X Y.app --no-build --verdicts" "iOS-27-1: read back as shipped" "iOS-27-2: read back as shipped"

setup pass fail
go env
check "confirmed fail stops" 1 orig "::error::ci-verdicts: the canary failed (cached, confirmed) on iOS 27.2 (B2)" \
  "not packaging"

setup pass fail
go env CI_VERDICTS_FAILS=warn
check "fail + warn ships the pass" 0 new "CI_VERDICTS_FAILS=warn: a confirmed" \
  "not shipped (CI_VERDICTS_FAILS=warn)" "packaging again with 1 verdict(s): K-iOS-27-1"

setup pass
go env CI_VERDICTS_FAILS=skip
check "skip" 0 orig "CI_VERDICTS_FAILS=skip"

setup pass
go env CI_VERDICTS_FAILS=bogus
check "bad override" 1 orig "want warn, skip or unset"

setup retried unconf inconc setup nocache
go env
check "non-shippable outcomes" 0 orig "short-lived: not shipped" "didn't confirm (not cached)" \
  "no verdict to ship (exit 2): inconclusive" "no verdict to ship (exit 64)" "the pass wasn't cached" \
  "no plain canary pass"

setup hang pass
go env CI_VERDICTS_RUN_S=2
check "watchdog stops a hang" 0 new "stopped after 2 s" "packaging again with 1 verdict(s): K-iOS-27-2"

setup latepass pass
go env CI_VERDICTS_RUN_S=2
check "a pass cached before the stop doesn't ship" 0 new "stopped after 2 s: no verdict shipped" \
  "pkg got: K-iOS-27-2.json "

setup latefail pass
go env CI_VERDICTS_RUN_S=2
check "a fail cached before the stop stops" 1 orig \
  "the canary failed (cached, confirmed) on iOS 27.1 (B1): a fail cached before the stop (K-iOS-27-1.json)"

setup pass
go env STUB_READBACK_MISS=1
check "read-back miss warns" 0 new "didn't read its shipped verdict back"

setup pass pass
go env CI_VERDICTS_RUN_S=600 CI_VERDICTS_BUDGET_S=100
check "budget" 0 orig "could end past the 100 s budget" "no plain canary pass"

setup pass
go env CI_VERDICTS_BUDGET_S=25m
check "bad budget number" 0 new "isn't a whole number of seconds: using 1500"

setup pass
go env STUB_PKG_FAIL=1
check "repack fails -> restore" 0 orig "package.sh --verdicts failed: restoring"

setup pass
go env STUB_PAYLOAD_CHANGE=1
check "payload changed -> restore" 0 orig "payload.tar differs from the verified one: restoring"

setup pass
go env STUB_XCRUN_FAIL=1
check "runtime list fails" 0 orig "can't list the simulator runtimes"

setup pass
mkdir -p $T/home/verdicts
echo x > $T/home/verdicts/old.json
go env
check "dirty verdicts dir" 1 orig "already holds verdicts"

setup pass
rm $T/stub/mode-iOS-27-1
echo '{"runtimes":[]}' > $T/stub/runtimes.json
go env
check "no runtimes" 0 orig "no general iOS simulator runtime"

echo "== $pass passed, $fail failed"
(( fail == 0 ))
