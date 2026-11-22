# SPDX-License-Identifier: Apache-2.0
# common.sh — small helpers shared by build.sh and scripts/*.sh (zsh). Sourced after scripts/env.sh; it only defines
# functions, so sourcing it twice is harmless.

# print_usage <script>: prints the script's header comment as --help text. That is its lines from line 3 (after the
# shebang and the SPDX line) up to the first line that isn't a comment, printed as they are.
print_usage() { awk 'NR < 3 { next } !/^#/ { exit } { print }' "$1"; }

# need_udid <prog> <rt> <var> <long|short>: sets <var> to the value of SIMCAM_UDID_<rt>, the UDID of the iOS <rt>
# simulator to use. If that is unset or empty, it prints "<prog>: set SIMCAM_UDID_<rt>" on stderr ("long" adds
# " to the UDID of your iOS <rt> simulator") and exits 2.
need_udid() {
  local udid_var=SIMCAM_UDID_$2
  # (P) reads the variable a name refers to: ${(P)udid_var} is $SIMCAM_UDID_<rt>, ${(P)3} is <var>'s new value.
  typeset -g "$3=${(P)udid_var:-}"
  [[ -n ${(P)3} ]] && return 0
  if [[ $4 == long ]]; then
    echo "$1: set $udid_var to the UDID of your iOS $2 simulator" >&2
  else
    echo "$1: set $udid_var" >&2
  fi
  exit 2
}

# inject_env <engine dylib> <profile plist> <engine log>: sets INJECT_ENV to the SIMCTL_CHILD_* words (for
# `env ... xcrun simctl launch`) that inject the engine into one launch on a plainly booted sim: the dylib, the fake
# camera with the session hook and no log dictionary, the profile, and where the engine logs. Each setting also goes
# under its stuntcam 1.1.0 name (cli/internal/arm LegacyNames), for an older engine passed as <engine dylib>.
inject_env() {
  INJECT_ENV=(SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=$1 SIMCTL_CHILD_SIMCAM_FIG_SOURCES=1 SIMCTL_CHILD_M2_FAKE=1
    SIMCTL_CHILD_SIMCAM_FIG_SESSION=1 SIMCTL_CHILD_M2_SESSION_HOOK=1
    SIMCTL_CHILD_SIMCAM_OFF_KEY_LOG=1 SIMCTL_CHILD_M2_NOLOGDICT=1
    SIMCTL_CHILD_SIMCAM_PROFILE=$2 SIMCTL_CHILD_M2_PLIST=$2 SIMCTL_CHILD_SIMCAM_ENGINE_LOG=$3 SIMCTL_CHILD_M5_LOG=$3)
}

# wait_for_line <file> <pattern> <max polls> <pid>: polls every 0.1 s until <file> has a line matching <pattern> (grep's
# basic regex) or process <pid> (the backgrounded `simctl launch --console-pty`) has exited, at most <max polls> times.
wait_for_line() {
  local i
  for i in {1..$3}; do
    sleep 0.1
    grep -q "$2" $1 && break
    kill -0 $4 2>/dev/null || break
  done
}
