# SPDX-License-Identifier: Apache-2.0
# arm-env.sh — sourced by boot.sh and launchd-arm.sh (after env.sh). The one list of variables that arms a sim, so the two
# modes stay in step:
#   boot mode     passes each as SIMCTL_CHILD_<VAR> to `simctl boot` (the boot env every process inherits)
#   launchd mode  sets each with `launchctl setenv` inside the booted sim (every process launched afterwards inherits it)
# simcam_arm_vars <rt> <udid> sets:
#   BL     a new boot log dir ($SIMCAM_LOGS/bootlogs-<rt>-<HHMMSS>/), recorded in build/bootlogdir-<rt>.txt for run.sh & co.
#   CHAIN  the chain file (SIMCAM_CHAIN_FILE, default $SIMCAM_HOME/sims/<udid>/chain.txt), created empty if missing
#   STUB   the stub (build/current_stub.txt); SIMDIR $SIMCAM_HOME/sims/<udid>, whose `current` names the loader
#   ARMV   (VAR=value ...), DYLD_INSERT_LIBRARIES=$STUB among them
#   and writes SIMDIR/config.json: the Xcode in use (xcodebuild -version, which follows DEVELOPER_DIR), as `stuntcam up` does;
#   the loader applies $SIMCAM_RULES_DIR/<that build>.json (design §4.1 "Which Xcode's libs")
# Extra settings: SIMCTL_CHILD_<VAR>=<value> exported by the caller are added to ARMV as <VAR>=<value>
# (e.g. SIMCTL_CHILD_SIMCAM_LAZY=0 arms at load). In boot mode simctl also passes them on by itself; the value is the same.
# simcam_record_xcode <sim dir> [source]: config.json with the Xcode build (the format of cli/internal/arm.Config): rules_dir
# is $SIMCAM_RULES_DIR (env.sh's per-content copy), boot_signals the displaced-insert signal variables among the
# SIMCTL_CHILD_ extras (they reach every process, so the loader doesn't count them). An Xcode that can't be identified
# keeps the Xcode of an existing config.json (and writes this arm's rules dir and signals). DEVELOPER_DIR=…/Xcode.app
# counts as …/Xcode.app/Contents/Developer. simcam_set_rules_dir <sim dir>: point an existing config.json at
# $SIMCAM_RULES_DIR, keeping the rest (swap.sh).
simcam_jstr() {   # a JSON string; other control characters as \u00XX, so Go's ReadConfig accepts it too (review r2 nit)
  local s=$1 c o=; s=${s//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\n'/\\n}; s=${s//$'\t'/\\t}; s=${s//$'\r'/\\r}
  if [[ $s == *[[:cntrl:]]* ]]; then for c in ${(s::)s}; do
      if [[ $c == [[:cntrl:]] ]]; then o+=$(printf '\\u%04x' "'$c"); else o+=$c; fi; done; s=$o; fi
  print -rn -- "\"$s\"" }
# simcam_cfgline <config.json> <key>: that key's whole value as written (both writers put one key per line), "" if absent
simcam_cfgline() { local l; for l in "${(@f)$(<$1)}"; do [[ $l == ' "'$2'": '* ]] && { l=${l#' "'$2'": '}; print -rn -- ${l%,}; return 0 }; done; return 1 }
simcam_record_xcode() {
  local v xv xb xd kv sig=() sj=
  v=$(xcodebuild -version 2>/dev/null); xv=${${(M)${(f)v}:#Xcode *}#Xcode }; xb=${${(M)${(f)v}:#Build version *}#Build version }
  local jv jb jd ob=
  [[ -z $xb && -f $1/config.json ]] && ob=$(simcam_cfgline $1/config.json xcode_build) && [[ $ob != '""' ]] || ob=
  xd=${DEVELOPER_DIR:-$(env -u DEVELOPER_DIR xcode-select -p 2>/dev/null)}
  while [[ $xd == ?*/ ]]; do xd=${xd%/}; done; [[ $xd == *.app ]] && xd=$xd/Contents/Developer
  if [[ -n $ob ]]; then   # keep only the Xcode recorded earlier; the rules dir and boot signals are this arm's (review r2 L-c)
    echo "arm-env.sh: could not identify Xcode (xcodebuild -version); keeping the Xcode recorded in $1/config.json" >&2
    jv=$(simcam_cfgline $1/config.json xcode_version) || jv='""'; jb=$ob; jd=$(simcam_cfgline $1/config.json developer_dir) || jd='""'
  else
    [[ -z $xb ]] && echo "arm-env.sh: could not identify Xcode (xcodebuild -version); apps will say restore rules unverified" >&2
    jv=$(simcam_jstr "$xv"); jb=$(simcam_jstr "$xb"); jd=$(simcam_jstr "$xd")
  fi
  for kv in ${(f)"$(env | grep '^SIMCTL_CHILD_')"}; do kv=${kv#SIMCTL_CHILD_}
    case ${kv%%=*} in IDB_*|ASAN_OPTIONS|TSAN_OPTIONS|MALLOC_PROTECT_BEFORE|MALLOC_FILL_SPACE|MALLOC_STRICT_SIZE|MALLOC_ALLOW_READS|MALLOC_VECTOR_SIZE|MALLOC_PERMIT_INSANE_REQUESTS)
      sig+=("$(simcam_jstr $kv)");; esac
  done
  (( $#sig )) && sj=$'\n "boot_signals": ['${(j:, :)sig}'],'
  print -r -- "{
 \"proto\": 1,
 \"xcode_version\": $jv,
 \"xcode_build\": $jb,
 \"developer_dir\": $jd,
 \"rules_dir\": $(simcam_jstr "${SIMCAM_RULES_DIR:-}"),$sj
 \"source\": $(simcam_jstr "${2:-scripts}"),
 \"at\": $(date +%s)
}" > "$1/config.json.tmp" && mv "$1/config.json.tmp" "$1/config.json"
}
simcam_set_rules_dir() {
  local l out=() f=$1/config.json
  for l in "${(@f)$(<$f)}"; do [[ $l == ' "rules_dir": '* ]] && l=' "rules_dir": '$(simcam_jstr "${SIMCAM_RULES_DIR:-}")','; out+=("$l"); done
  print -rl -- $out > $f.tmp && mv $f.tmp $f
}
simcam_arm_vars() {
  local rt=$1 udid=$2 kv LDR PL IMG STATE
  BL=$SIMCAM_LOGS/bootlogs-$rt-$(date +%H%M%S); mkdir -p $BL; mkdir -p $ROOT/build; echo $BL > $ROOT/build/bootlogdir-$rt.txt
  # chain file (m11 1e): libraries listed here load in every app launch, no reboot needed; one path per line
  CHAIN=${SIMCAM_CHAIN_FILE:-$SIMCAM_HOME/sims/$udid/chain.txt}; mkdir -p "${CHAIN:h}"; touch "$CHAIN"
  # swappable loader (design §4.1, U14): the boot env names only the stub and the sim dir; the stub reads SIMDIR/current
  # ("<proto> <loader path>") at every launch, so scripts/swap.sh can change the build without a reboot
  SIMDIR=$SIMCAM_HOME/sims/$udid; mkdir -p "$SIMDIR"
  STUB=$(cat $ROOT/build/current_stub.txt); LDR=$(cat $ROOT/build/current_loader.txt); print -r -- "1 $LDR" > "$SIMDIR/current.tmp" && mv "$SIMDIR/current.tmp" "$SIMDIR/current"; PL=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}; IMG=$SIMCAM_DEV/media/qr.png
  simcam_record_xcode "$SIMDIR"
  # sentinel/watchdog events (engine core/safety.inc): <state>/<bundle id>/events.log
  STATE=${SIMCAM_STATE_DIR:-$SIMCAM_HOME/sims/$udid/state}; mkdir -p "$STATE"
  simcam_guard $CHAIN $STUB $LDR $PL $IMG $BL $STATE
  ARMV=("SIMCAM_STATE_DIR=$STATE" "SIMCAM_CHAIN=$CHAIN" "DYLD_INSERT_LIBRARIES=$STUB" "SIMCAM_SIM_DIR=$SIMDIR" "SIMCAM_STUB_LOG=$BL/stub.log"
        M2_FAKE=1 M2_SESSION_HOOK=1 M2_NOLOGDICT=1 "M2_PLIST=$PL" "M5_IMAGE=$IMG" "M5_LOG=$BL/dylib-%p.log")
  # DYLIB=<engine> pins an engine for this boot (e.g. a sanitizer build); otherwise the loader uses the engine next to it
  [[ -n ${DYLIB:-} ]] && ARMV+=("SIMCAM_DYLIB=$DYLIB")
  for kv in ${(f)"$(env | grep '^SIMCTL_CHILD_')"}; do
    [[ -z $kv ]] && continue
    case ${kv#SIMCTL_CHILD_} in   # these would replace the stub or its sim dir (review r1): ignore with a warning
      DYLD_INSERT_LIBRARIES=*|SIMCAM_SIM_DIR=*) echo "arm-env.sh: ignoring ${kv%%=*} (stuntcam sets it)" >&2;;
      *) ARMV+=("${kv#SIMCTL_CHILD_}");;
    esac
  done
}
