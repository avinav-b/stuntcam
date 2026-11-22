# SPDX-License-Identifier: Apache-2.0
# env.sh — sourced by ./build.sh and scripts/*.sh (after ROOT is set). Every path a simulator process reads or writes
# lives under $SIMCAM_HOME, never in the clone.
#   Why: simulator processes can't see macOS privacy-protected (TCC) folders. From a clone under ~/Documents, ~/Desktop,
#   ~/Downloads or iCloud Drive, dyld and the apps get "no such file" for the stub, the engine, profiles, media and log
#   paths, so nothing loads and nothing is logged.
#   SIMCAM_HOME (default ~/Library/Application Support/stuntcam; readable and writable from sim apps):
#     builds/<hash>/   copies of the engine and stub made by ./build.sh (build/current_*.txt point here)
#     dev/<hash>/profiles/, dev/<hash>/media/   engine/profiles/ and fixtures/media/, staged by content (hash of the
#                      files): what $SIMCAM_DEV points at. Clones with different profiles get different dirs,
#                      so a sim one clone armed never reads another clone's profile (otherwise parallel runs
#                      see each other's flash).
#     dev/profiles/, dev/media/   the latest clone's copies, kept only for the Go CLI's fallback (artifacts.dataFiles);
#                      scripts never pass these paths to a simulator
#     logs/            boot logs: logs/bootlogs-<rt>-<HHMMSS>/ (stub.log, dylib-<pid>.log)
#     results/         per-run result dirs (app logs, dylib.log, control.txt, …); the clone's results is a symlink here
#     sims/<udid>/     chain.txt, current, config.json (boot.sh: the Xcode build whose restore rules the loader applies,
#                      and rules_dir, the restore-rules/<hash>/ it was armed with)
#     restore-rules/<hash>/   the clone's restore-rules/<xcode build>.json, staged by content like dev/<hash>/
#                      ($SIMCAM_RULES_DIR; "" if the clone has none). The hash is the CLI's too
#                      (artifacts.RulesHash): sha1 of one "<name> <sha1>" line per [0-9]*.json in C order, 12 hex
#                      digits. An armed sim's rules never change under it, and another clone's or a stale file never
#                      feeds the loader's "newest" best guess.

[[ -n ${_SIMCAM_ENV:-} ]] && return 0
_SIMCAM_ENV=1
# ${(%):-%x} is the file being sourced (this one); :A makes it absolute, :h:h is the clone's root.
: ${ROOT:=${${(%):-%x}:A:h:h}}

export SIMCAM_HOME=${SIMCAM_HOME:-$HOME/Library/Application Support/stuntcam}
SIMCAM_LOGS=$SIMCAM_HOME/logs
SIMCAM_RESULTS=$SIMCAM_HOME/results
mkdir -p $SIMCAM_HOME/dev/profiles $SIMCAM_HOME/dev/media $SIMCAM_LOGS $SIMCAM_RESULTS $SIMCAM_HOME/builds \
  $SIMCAM_HOME/sims

# Hashes the clone's profiles and media (relative names and bytes), then stages them once under dev/<hash>/.
() {
  # LC_ALL=C: the same content always globs in the same order, so it gets the same hash.
  local file hash stage_dir LC_ALL=C
  hash=$(for file in $ROOT/engine/profiles/*(.N) $ROOT/fixtures/media/*(.N); do
        [[ ${file:t} == README.md ]] && continue
        print -r -- "${file#$ROOT/} $(shasum < $file | cut -c1-40)"
      done | shasum | cut -c1-12)
  SIMCAM_DEV=$SIMCAM_HOME/dev/$hash
  [[ -f $SIMCAM_DEV/.complete ]] && return 0

  # The copy is staged in a temp dir and renamed, so a parallel script never sees half a copy. .complete is written
  # only after every copy worked: most scripts don't run under set -e, and a half copy marked complete would stay
  # under this hash for good.
  if ! stage_dir=$(mktemp -d "$SIMCAM_HOME/dev/.stage-XXXXXX"); then
    print -u2 "env.sh: mktemp failed in $SIMCAM_HOME/dev"
    return 1
  fi
  local ok=1   # Only if-statements below: a false && list must not trip set -e in callers like build.sh.
  if ! mkdir -p $stage_dir/profiles $stage_dir/media; then
    ok=0
  fi

  for file in $ROOT/engine/profiles/*(.N) $ROOT/fixtures/media/*(.N); do   # README.md stays out, as in the hash
    if (( ok )) && [[ ${file:t} != README.md ]]; then
      cp -p $file $stage_dir/${file:h:t}/ || ok=0   # into profiles/ or media/ (:h:t: its directory's name)
    fi
  done

  if (( ok )); then
    : > $stage_dir/.complete || ok=0
  fi
  if (( ! ok )); then
    rm -rf $stage_dir
    print -u2 "env.sh: staging $SIMCAM_DEV failed"
    return 1
  fi

  if [[ -d $SIMCAM_DEV ]]; then
    rm -rf $stage_dir   # Another script staged the same content meanwhile: use theirs.
  else
    # another env.sh run won the rename: use its copy
    mv $stage_dir $SIMCAM_DEV 2>/dev/null || rm -rf $stage_dir
    # If we lost a rename race, BSD mv moved our dir into the existing one.
    [[ -d $SIMCAM_DEV/${stage_dir:t} ]] && rm -rf $SIMCAM_DEV/${stage_dir:t}
  fi
  return 0
}

# The CLI's fallback copies. The scripts never pass them to a simulator.
rsync -a --delete $ROOT/engine/profiles/ $SIMCAM_HOME/dev/profiles/
rsync -a --delete --exclude README.md $ROOT/fixtures/media/ $SIMCAM_HOME/dev/media/

# Stages the restore rules under $SIMCAM_HOME/restore-rules/<content hash>/, the same way as dev/<hash>/ above
# (a temp dir, .complete, then a rename).
() {
  local file hash stage_dir ok=1 LC_ALL=C
  local -a rule_files
  rule_files=($ROOT/restore-rules/[0-9]*.json(.N))
  SIMCAM_RULES_DIR=
  (( $#rule_files )) || return 0
  hash=$(for file in $rule_files; do
        print -r -- "${file:t} $(shasum < $file | cut -c1-40)"
      done | shasum | cut -c1-12)
  SIMCAM_RULES_DIR=$SIMCAM_HOME/restore-rules/$hash
  [[ -f $SIMCAM_RULES_DIR/.complete ]] && return 0

  mkdir -p $SIMCAM_HOME/restore-rules
  if ! stage_dir=$(mktemp -d "$SIMCAM_HOME/restore-rules/.stage-XXXXXX"); then
    print -u2 "env.sh: mktemp failed in $SIMCAM_HOME/restore-rules"
    SIMCAM_RULES_DIR=
    return 1
  fi

  chmod 755 $stage_dir
  for file in $rule_files; do
    if (( ok )); then
      cp -p $file $stage_dir/ || ok=0
    fi
  done
  if (( ok )); then
    : > $stage_dir/.complete || ok=0
  fi
  if (( ! ok )); then
    rm -rf $stage_dir
    print -u2 "env.sh: staging $SIMCAM_RULES_DIR failed"
    SIMCAM_RULES_DIR=
    return 1
  fi

  if [[ -d $SIMCAM_RULES_DIR ]]; then
    rm -rf $stage_dir
  else
    # another env.sh run won the rename: use its copy
    mv $stage_dir $SIMCAM_RULES_DIR 2>/dev/null || rm -rf $stage_dir
    [[ -d $SIMCAM_RULES_DIR/${stage_dir:t} ]] && rm -rf $SIMCAM_RULES_DIR/${stage_dir:t}
  fi
  return 0
}

# A convenience link: <clone>/results -> $SIMCAM_RESULTS (git-ignored).
if [[ -L $ROOT/results || ! -e $ROOT/results ]]; then
  # not a link yet: make it
  [[ $(readlink $ROOT/results 2>/dev/null) == $SIMCAM_RESULTS ]] || ln -sfn $SIMCAM_RESULTS $ROOT/results
else
  echo "env.sh: $ROOT/results is a directory; results go to $SIMCAM_RESULTS" \
    "(move the old dir away to get the symlink)" >&2
fi

# simcam_guard <path|colon-list> ...: warns about paths a simulator process can't reach.
simcam_guard() {
  local arg entry
  for arg in "$@"; do
    for entry in ${(s.:.)arg}; do   # each entry of a colon list
      case ${entry:A} in   # :A is the absolute path, symlinks resolved
        $HOME/Documents|$HOME/Documents/*|$HOME/Desktop|$HOME/Desktop/*|$HOME/Downloads|$HOME/Downloads/*|\
"$HOME/Library/Mobile Documents"*|$HOME/Library/CloudStorage/*)
          echo "warning: $entry is in a privacy-protected folder; simulator processes can't read or write it." \
            "Keep it under \$SIMCAM_HOME ($SIMCAM_HOME)" >&2
          ;;
      esac
    done
  done
}
