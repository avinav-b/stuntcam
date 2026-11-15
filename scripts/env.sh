# SPDX-License-Identifier: Apache-2.0
# env.sh — sourced by ./build.sh and scripts/*.sh (after ROOT is set). Every path a simulator process reads or writes
# lives under $SIMCAM_HOME, never in the clone.
#   Why: simulator processes can't see macOS privacy-protected (TCC) folders. From a clone under ~/Documents, ~/Desktop,
#   ~/Downloads or iCloud Drive, dyld and the apps get "no such file" for the stub, the engine, profiles, media and log
#   paths, so nothing loads and nothing is logged.
#   SIMCAM_HOME (default ~/Library/Application Support/stuntcam; readable and writable from sim apps):
#     builds/<hash>/   copies of the engine and stub made by ./build.sh (build/current_*.txt point here)
#     dev/<hash>/profiles/, dev/<hash>/media/   engine/profiles/ and fixtures/media/, staged by content (hash of the
#                      files): what $SIMCAM_DEV points at. Clones with different profiles get different dirs, so a sim one
#                      clone armed never reads another clone's profile (otherwise parallel runs see each other's flash).
#     dev/profiles/, dev/media/   the latest clone's copies, kept only for the Go CLI's fallback (artifacts.dataFiles);
#                      scripts never pass these paths to a simulator
#     logs/            boot logs: logs/bootlogs-<rt>-<HHMMSS>/ (stub.log, dylib-<pid>.log)
#     results/         per-run result dirs (app logs, dylib.log, control.txt, …); the clone's results is a symlink here
#     sims/<udid>/     chain.txt, current, config.json (boot.sh: the Xcode build whose restore rules the loader applies,
#                      and rules_dir, the restore-rules/<hash>/ it was armed with)
#     restore-rules/<hash>/   the clone's restore-rules/<xcode build>.json, staged by content like dev/<hash>/ ($SIMCAM_RULES_DIR;
#                      "" if the clone has none). The hash is the CLI's too (artifacts.RulesHash): sha1 of one "<name> <sha1>"
#                      line per [0-9]*.json in C order, 12 hex digits. An armed sim's rules never change under it, and
#                      another clone's or a stale file never feeds the loader's "newest" best guess.
[[ -n ${_SIMCAM_ENV:-} ]] && return 0; _SIMCAM_ENV=1
: ${ROOT:=${${(%):-%x}:A:h:h}}
export SIMCAM_HOME=${SIMCAM_HOME:-$HOME/Library/Application Support/stuntcam}
SIMCAM_LOGS=$SIMCAM_HOME/logs; SIMCAM_RESULTS=$SIMCAM_HOME/results
mkdir -p $SIMCAM_HOME/dev/profiles $SIMCAM_HOME/dev/media $SIMCAM_LOGS $SIMCAM_RESULTS $SIMCAM_HOME/builds $SIMCAM_HOME/sims
() {   # content hash of the clone's profiles + media (relative names + bytes), then stage them once under dev/<hash>/
  local f h t LC_ALL=C   # C collation: the same content always globs in the same order, so it gets the same hash
  h=$(for f in $ROOT/engine/profiles/*(.N) $ROOT/fixtures/media/*(.N); do
        [[ ${f:t} == README.md ]] && continue; print -r -- "${f#$ROOT/} $(shasum < $f | cut -c1-40)"
      done | shasum | cut -c1-12)
  SIMCAM_DEV=$SIMCAM_HOME/dev/$h
  [[ -f $SIMCAM_DEV/.complete ]] && return 0
  # staged in a temp dir and renamed, so a parallel script never sees half a copy; .complete only after every copy worked
  # (most scripts don't run under set -e, and a half copy marked complete would stay under this hash for good)
  t=$(mktemp -d "$SIMCAM_HOME/dev/.stage-XXXXXX") || { print -u2 "env.sh: mktemp failed in $SIMCAM_HOME/dev"; return 1 }
  local ok=1   # if-statements only below: a false && list must not trip set -e in callers like build.sh
  if ! mkdir -p $t/profiles $t/media; then ok=0; fi
  for f in $ROOT/engine/profiles/*(.N) $ROOT/fixtures/media/*(.N); do   # README.md stays out, as in the hash
    if (( ok )) && [[ ${f:t} != README.md ]]; then cp -p $f $t/${${f:h}:t}/ || ok=0; fi   # into profiles/ or media/
  done
  if (( ok )); then : > $t/.complete || ok=0; fi
  if (( ! ok )); then rm -rf $t; print -u2 "env.sh: staging $SIMCAM_DEV failed"; return 1; fi
  if [[ -d $SIMCAM_DEV ]]; then rm -rf $t   # another script staged the same content meanwhile: use theirs
  else mv $t $SIMCAM_DEV 2>/dev/null || rm -rf $t
    [[ -d $SIMCAM_DEV/${t:t} ]] && rm -rf $SIMCAM_DEV/${t:t}   # lost a rename race (BSD mv moves into an existing dir)
  fi
  return 0
}
# the CLI's fallback copies (never passed to a simulator by the scripts)
rsync -a --delete $ROOT/engine/profiles/ $SIMCAM_HOME/dev/profiles/
rsync -a --delete --exclude README.md $ROOT/fixtures/media/ $SIMCAM_HOME/dev/media/
() {   # restore rules: $SIMCAM_HOME/restore-rules/<content hash>/, as dev/<hash>/ above (temp dir, .complete, rename)
  local f h t ok=1 LC_ALL=C; local -a fs; fs=($ROOT/restore-rules/[0-9]*.json(.N))
  SIMCAM_RULES_DIR=; (( $#fs )) || return 0
  h=$(for f in $fs; do print -r -- "${f:t} $(shasum < $f | cut -c1-40)"; done | shasum | cut -c1-12)
  SIMCAM_RULES_DIR=$SIMCAM_HOME/restore-rules/$h
  [[ -f $SIMCAM_RULES_DIR/.complete ]] && return 0
  mkdir -p $SIMCAM_HOME/restore-rules
  t=$(mktemp -d "$SIMCAM_HOME/restore-rules/.stage-XXXXXX") || { print -u2 "env.sh: mktemp failed in $SIMCAM_HOME/restore-rules"; SIMCAM_RULES_DIR=; return 1 }
  chmod 755 $t
  for f in $fs; do if (( ok )); then cp -p $f $t/ || ok=0; fi; done
  if (( ok )); then : > $t/.complete || ok=0; fi
  if (( ! ok )); then rm -rf $t; print -u2 "env.sh: staging $SIMCAM_RULES_DIR failed"; SIMCAM_RULES_DIR=; return 1; fi
  if [[ -d $SIMCAM_RULES_DIR ]]; then rm -rf $t
  else mv $t $SIMCAM_RULES_DIR 2>/dev/null || rm -rf $t
    [[ -d $SIMCAM_RULES_DIR/${t:t} ]] && rm -rf $SIMCAM_RULES_DIR/${t:t}
  fi
  return 0
}
# convenience: <clone>/results -> $SIMCAM_RESULTS (git-ignored)
if [[ -L $ROOT/results || ! -e $ROOT/results ]]; then
  [[ $(readlink $ROOT/results 2>/dev/null) == $SIMCAM_RESULTS ]] || ln -sfn $SIMCAM_RESULTS $ROOT/results
else
  echo "env.sh: $ROOT/results is a directory; results go to $SIMCAM_RESULTS (move the old dir away to get the symlink)" >&2
fi
# simcam_guard <path|colon-list> ...: warn about paths a simulator process can't reach
simcam_guard() {
  local a p
  for a in "$@"; do for p in ${(s.:.)a}; do
    case ${p:A} in
      $HOME/Documents|$HOME/Documents/*|$HOME/Desktop|$HOME/Desktop/*|$HOME/Downloads|$HOME/Downloads/*|"$HOME/Library/Mobile Documents"*|$HOME/Library/CloudStorage/*)
        echo "warning: $p is in a privacy-protected folder; simulator processes can't read or write it. Keep it under \$SIMCAM_HOME ($SIMCAM_HOME)" >&2;;
    esac
  done; done
}
