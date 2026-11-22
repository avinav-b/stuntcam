# SPDX-License-Identifier: Apache-2.0
# arm-env.sh — sourced by boot.sh, launchd-arm.sh, swap.sh and verify.sh (after env.sh). It holds the one list of
# variables that arms a sim, so the two modes stay in step:
#   boot mode     passes each as SIMCTL_CHILD_<VAR> to `simctl boot` (the boot env every process inherits)
#   launchd mode  sets each with `launchctl setenv` inside the booted sim (every process
#                 launched afterwards inherits it)
# simcam_arm_vars <rt> <udid> sets:
#   BOOT_LOG_DIR  a new boot log dir ($SIMCAM_LOGS/bootlogs-<rt>-<HHMMSS>/), recorded in build/bootlogdir-<rt>.txt
#                 for run.sh and the other scripts
#   CHAIN         the chain file (SIMCAM_CHAIN_FILE, default $SIMCAM_HOME/sims/<udid>/chain.txt), created empty if
#                 missing
#   STUB          the stub (build/current_stub.txt)
#   SIM_DIR       $SIMCAM_HOME/sims/<udid>, whose `current` file names the loader
#   ARM_VARS      (VAR=value ...), DYLD_INSERT_LIBRARIES=$STUB among them
#   It also writes SIM_DIR/config.json with the Xcode in use (xcodebuild -version, which follows DEVELOPER_DIR), as
#   `stuntcam up` does. The loader applies the restore rules of that Xcode build, $SIMCAM_RULES_DIR/<build>.json.
# Extra settings: SIMCTL_CHILD_<VAR>=<value> exported by the caller are added to ARM_VARS as <VAR>=<value>
# (e.g. SIMCTL_CHILD_SIMCAM_LAZY=0 arms at load). In boot mode simctl also passes them on by itself; the
# value is the same.

# config.json, in the layout both of its writers use (this file and the CLI's arm.MarshalConfig):
#   {
#    "proto": 1,
#    "xcode_version": "26.1",
#    ...
#    "at": 1763700000
#   }
# Each key is on its own line: one space, the quoted key, ": ", the JSON value, and a comma on every line but the
# last. Only the CLI's boot_signals array spans several lines, and it is never read here. So the helpers below read
# and replace a key by the start of its line, with no JSON parser: boot.sh and swap.sh need nothing but zsh.

# simcam_json_string <text>: prints <text> as a JSON string. Each character is copied, or replaced by its escape.
# Control characters must be escaped, or Go's ReadConfig rejects the file.
simcam_json_string() {
  local char json=
  for char in ${(s::)1}; do   # (s::) splits the text into its characters
    case $char in
      \\|\")       json+=\\$char ;;
      $'\n')       json+='\n' ;;
      $'\t')       json+='\t' ;;
      $'\r')       json+='\r' ;;
      [[:cntrl:]]) json+=$(printf '\\u%04x' "'$char") ;;   # \u and the code point, e.g. \u001b
      *)           json+=$char ;;
    esac
  done
  print -rn -- "\"$json\""
}

# simcam_config_value <config.json> <key>: prints the key's JSON value as written, without its line's comma.
# Returns 1 if the file has no such key.
simcam_config_value() {
  local file=$1 key_start=" \"$2\": " line
  while IFS= read -r line || [[ -n $line ]]; do   # (the || reads a last line that has no newline)
    if [[ $line == "$key_start"* ]]; then
      line=${line#"$key_start"}
      print -rn -- ${line%,}
      return 0
    fi
  done < $file
  return 1
}

# simcam_write_config <config.json> <key> <JSON value> [<key> <JSON value> ...]: writes the file with those keys in
# that order, in the layout above. The file is replaced in one step (a temporary file, then mv), so a launch never
# reads half of it.
simcam_write_config() {
  local file=$1 key value comma=,
  shift
  {
    print -r -- '{'
    while (( $# )); do
      key=$1
      value=$2
      shift 2
      (( $# )) || comma=   # no comma after the last key
      print -r -- " \"$key\": $value$comma"
    done
    print -r -- '}'
  } > $file.tmp && mv $file.tmp $file
}

# simcam_boot_signals_json: prints the JSON array of the displaced-insert signal variables among the caller's
# SIMCTL_CHILD_ settings, each as "<VAR>=<value>", or nothing if there are none. They reach every process of the
# boot, so the loader doesn't take them for a displaced per-launch insert.
simcam_boot_signals_json() {
  local kv
  local -a signal_settings
  while IFS= read -r kv; do
    kv=${kv#SIMCTL_CHILD_}
    case ${kv%%=*} in
      # the displaced-insert signal variables (one pattern, continued on the next line)
      IDB_*|ASAN_OPTIONS|TSAN_OPTIONS|MALLOC_PROTECT_BEFORE|MALLOC_FILL_SPACE|MALLOC_STRICT_SIZE|MALLOC_ALLOW_READS|\
MALLOC_VECTOR_SIZE|MALLOC_PERMIT_INSANE_REQUESTS)
        signal_settings+=("$(simcam_json_string $kv)")
        ;;
    esac
  done < <(env | grep '^SIMCTL_CHILD_')
  if (( $#signal_settings )); then
    print -rn -- "[${(j:, :)signal_settings}]"   # (j:, :) joins them with ", "
  fi
}

# simcam_record_xcode <sim dir> [source]: writes config.json with the Xcode in use, in the format of
# cli/internal/arm.Config.
#   - rules_dir is $SIMCAM_RULES_DIR, env.sh's per-content copy.
#   - boot_signals is simcam_boot_signals_json, present only if there are any.
#   - If Xcode can't be identified, it keeps the Xcode of an existing config.json. It still writes this arm's rules
#     dir and signals.
simcam_record_xcode() {
  local config=$1/config.json line
  local -a versions builds   # what follows "Xcode " and "Build version " in `xcodebuild -version`
  while IFS= read -r line || [[ -n $line ]]; do
    case $line in
      'Xcode '*)         versions+=("${line#Xcode }") ;;
      'Build version '*) builds+=("${line#Build version }") ;;
    esac
  done < <(xcodebuild -version 2>/dev/null)   # no Xcode: no lines, handled below
  # xcodebuild prints one line of each. Were there several, they would be joined with spaces.
  local xcode_version="$versions" xcode_build="$builds"

  # The Xcode build an earlier config.json recorded, used when this one is unknown.
  local recorded_build=
  if [[ -z $xcode_build && -f $config ]]; then
    recorded_build=$(simcam_config_value $config xcode_build) || recorded_build=
    [[ $recorded_build == '""' ]] && recorded_build=
  fi

  # The Xcode developer dir in use: DEVELOPER_DIR, or else xcode-select's choice. Trailing slashes are dropped, and
  # DEVELOPER_DIR=…/Xcode.app counts as …/Xcode.app/Contents/Developer.
  local developer_dir=${DEVELOPER_DIR:-$(env -u DEVELOPER_DIR xcode-select -p 2>/dev/null)}
  while [[ $developer_dir == ?*/ ]]; do
    developer_dir=${developer_dir%/}
  done
  [[ $developer_dir == *.app ]] && developer_dir=$developer_dir/Contents/Developer

  # The three Xcode values, as JSON.
  local version_json build_json developer_dir_json
  if [[ -n $recorded_build ]]; then
    echo "arm-env.sh: could not identify Xcode (xcodebuild -version); keeping the Xcode recorded in $1/config.json" >&2
    version_json=$(simcam_config_value $config xcode_version) || version_json='""'
    build_json=$recorded_build
    developer_dir_json=$(simcam_config_value $config developer_dir) || developer_dir_json='""'
  else
    [[ -z $xcode_build ]] &&
      echo "arm-env.sh: could not identify Xcode (xcodebuild -version); apps will say restore rules unverified" >&2
    version_json=$(simcam_json_string "$xcode_version")
    build_json=$(simcam_json_string "$xcode_build")
    developer_dir_json=$(simcam_json_string "$developer_dir")
  fi

  local -a fields
  fields=(proto 1 xcode_version "$version_json" xcode_build "$build_json" developer_dir "$developer_dir_json"
    rules_dir "$(simcam_json_string "${SIMCAM_RULES_DIR:-}")")
  local signals_json=$(simcam_boot_signals_json)
  [[ -n $signals_json ]] && fields+=(boot_signals "$signals_json")
  fields+=(source "$(simcam_json_string "${2:-scripts}")" at "$(date +%s)")
  simcam_write_config $config "${fields[@]}"
}

# simcam_set_rules_dir <sim dir>: points an existing config.json at $SIMCAM_RULES_DIR and keeps the rest (swap.sh).
simcam_set_rules_dir() {
  local file=$1/config.json key_start=' "rules_dir": ' line
  local -a lines
  while IFS= read -r line || [[ -n $line ]]; do   # (the || reads a last line that has no newline)
    [[ $line == "$key_start"* ]] && line="$key_start$(simcam_json_string "${SIMCAM_RULES_DIR:-}"),"
    lines+=("$line")
  done < $file
  print -rl -- $lines > $file.tmp && mv $file.tmp $file
}

# simcam_arm_vars <rt> <udid>: sets BOOT_LOG_DIR, CHAIN, STUB, SIM_DIR and ARM_VARS (see the header).
simcam_arm_vars() {
  local rt=$1 udid=$2 kv loader profile fixture_image state_dir
  BOOT_LOG_DIR=$SIMCAM_LOGS/bootlogs-$rt-$(date +%H%M%S)
  mkdir -p $BOOT_LOG_DIR
  mkdir -p $ROOT/build
  echo $BOOT_LOG_DIR > $ROOT/build/bootlogdir-$rt.txt

  # The chain file: the libraries listed here (one path per line) load in every app launch, with no reboot needed.
  CHAIN=${SIMCAM_CHAIN_FILE:-$SIMCAM_HOME/sims/$udid/chain.txt}
  mkdir -p "${CHAIN:h}"
  touch "$CHAIN"

  # The loader is swappable: the boot env names only the stub and the sim dir. The stub reads SIM_DIR/current
  # ("<proto> <loader path>") at every launch, so scripts/swap.sh can change the build without a reboot.
  SIM_DIR=$SIMCAM_HOME/sims/$udid
  mkdir -p "$SIM_DIR"
  STUB=$(cat $ROOT/build/current_stub.txt)
  loader=$(cat $ROOT/build/current_loader.txt)
  print -r -- "1 $loader" > "$SIM_DIR/current.tmp" && mv "$SIM_DIR/current.tmp" "$SIM_DIR/current"

  profile=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}
  fixture_image=$SIMCAM_DEV/media/qr.png
  simcam_record_xcode "$SIM_DIR"

  # Sentinel and watchdog events (engine core/safety.inc; the path is in core/markers.inc) go to
  # <state>/<bundle id>/events.log.
  state_dir=${SIMCAM_STATE_DIR:-$SIMCAM_HOME/sims/$udid/state}
  mkdir -p "$state_dir"
  simcam_guard $CHAIN $STUB $loader $profile $fixture_image $BOOT_LOG_DIR $state_dir

  # The six engine settings also go under their stuntcam 1.1.0 names, as the CLI sets them (cli/internal/arm
  # LegacyNames): an older engine (swap.sh or golden.sh with another loader, DYLIB=<older engine>) reads only those.
  ARM_VARS=("SIMCAM_STATE_DIR=$state_dir" "SIMCAM_CHAIN=$CHAIN" "DYLD_INSERT_LIBRARIES=$STUB"
    "SIMCAM_SIM_DIR=$SIM_DIR" "SIMCAM_STUB_LOG=$BOOT_LOG_DIR/stub.log"
    SIMCAM_FIG_SOURCES=1 M2_FAKE=1 SIMCAM_FIG_SESSION=1 M2_SESSION_HOOK=1 SIMCAM_OFF_KEY_LOG=1 M2_NOLOGDICT=1
    "SIMCAM_PROFILE=$profile" "M2_PLIST=$profile" "SIMCAM_FIXTURE_IMAGE=$fixture_image" "M5_IMAGE=$fixture_image"
    "SIMCAM_ENGINE_LOG=$BOOT_LOG_DIR/dylib-%p.log" "M5_LOG=$BOOT_LOG_DIR/dylib-%p.log")

  # DYLIB=<engine> pins an engine for this boot (e.g. a sanitizer build). Otherwise the loader uses the engine next
  # to it.
  [[ -n ${DYLIB:-} ]] && ARM_VARS+=("SIMCAM_DYLIB=$DYLIB")

  while IFS= read -r kv; do
    [[ -z $kv ]] && continue
    case ${kv#SIMCTL_CHILD_} in   # These would replace the stub or its sim dir: they are ignored with a warning.
      DYLD_INSERT_LIBRARIES=*|SIMCAM_SIM_DIR=*)
        echo "arm-env.sh: ignoring ${kv%%=*} (stuntcam sets it)" >&2
        ;;
      *)
        ARM_VARS+=("${kv#SIMCTL_CHILD_}")
        ;;
    esac
  done < <(env | grep '^SIMCTL_CHILD_')
}
