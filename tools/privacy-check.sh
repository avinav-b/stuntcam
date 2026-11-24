#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# privacy-check.sh — fail if a tracked file carries something that shouldn't be public.
#   Scans every git-tracked file (working-tree contents) for: secrets and password-style assignments, email addresses
#   other than noreply and example ones, absolute home paths, agent-harness paths, local hostnames, private IPs,
#   symlinks and files over 5 MB.
#   It also greps for each line of a local, never-committed denylist (~/.config/stuntcam-export/deny.txt, or
#   $STUNTCAM_DENYLIST): fixed strings, case-insensitive, hits printed as file:line only so the strings never show up
#   in a log. Without the file that check is skipped with a note (CI has none).
#   tools/hooks/pre-push runs this before every push; CI runs it on every push and pull request.
#   Exit 0 = clean, 1 = something found (each hit printed), 2 = not run from a checkout.
set -eu
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 2
git rev-parse --show-toplevel > /dev/null 2>&1 || { echo "privacy-check: not a git checkout" >&2; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/privacy-check.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
FAILS=$TMP/fails
: > "$FAILS"
git ls-files -z > "$TMP/files"

# scan <name> <grep options...>: grep over the tracked files; hits matching the ERE in $ALLOWED are fine.
scan() {
  name=$1
  shift
  # grep exits 1 when nothing matches
  xargs -0 grep -Hn "$@" < "$TMP/files" > "$TMP/hits" 2> /dev/null || true
  if [ -n "${ALLOWED:-}" ]; then
    grep -Ev -- "$ALLOWED" "$TMP/hits" > "$TMP/hits2" || true
    mv "$TMP/hits2" "$TMP/hits"
  fi
  sed "s#^#$name: #" "$TMP/hits" >> "$FAILS"
  ALLOWED=
}

# Secrets: GitHub, AWS, npm, Slack, OpenAI/Stripe-style and Google keys, JWTs, private keys; then password-style
# assignments. scripts/package.sh's own secret scan names '_authToken'.
SECRETS_RX='gh[oprsu]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|npm_[A-Za-z0-9]{30,}'
SECRETS_RX="$SECRETS_RX"'|xox[abprs]-[A-Za-z0-9-]{10,}|sk-[A-Za-z0-9_-]{20,}|sk_live_[A-Za-z0-9]{10,}'
SECRETS_RX="$SECRETS_RX"'|AIza[0-9A-Za-z_-]{35}|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.'
SECRETS_RX="$SECRETS_RX"'|-----BEGIN [A-Z ]*PRIVATE KEY'
scan secrets -aoE "$SECRETS_RX"
ALLOWED='^scripts/package\.sh:[0-9]+:_authToken$|^tools/privacy-check\.sh:' scan secret-assignments -aoiE \
  '_authtoken|(password|passwd|secret|api[_-]?key)[[:space:]]*[:=]'

# Email addresses: only noreply and example ones. Retina image names (Icon@2x.png) aren't addresses.
ALLOWED=':[^:]*(noreply|@example\.(com|org|net)|@[0-9.]+x\.(png|jpe?g|pdf))$' \
  scan emails -oIE '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}'

# Absolute home paths (the tests use /Users/x and /Users/Shared), and agent-harness paths.
ALLOWED=':/Users/(x|Shared)$' scan home-paths-macos -aoE '/Users/[A-Za-z0-9._-]+'
scan home-paths-other -aoE '(^|[^A-Za-z0-9_./-])/home/[A-Za-z0-9._-]+|/var/folders/[A-Za-z0-9_+-]{2}/[A-Za-z0-9_+-]+'
ALLOWED='^tools/privacy-check\.sh:' scan harness-paths -aoiE '\.claude/projects|shell-snapshots|\.t3/worktrees|/scratchpad/'

# Local hostnames (not .xcode.env.local), tailnets, Mac default host names, and private IPs.
HOSTNAME_RX='[A-Za-z0-9-]+\.local([^A-Za-z0-9]|$)|[A-Za-z0-9-]+\.ts\.net'
HOSTNAME_RX="$HOSTNAME_RX"'|[A-Za-z0-9-]*Mac[Bb]ook-(Pro|Air)[A-Za-z0-9-]*|Mac-(mini|Studio)[A-Za-z0-9-]*'
ALLOWED=':[^:]*env\.local([^A-Za-z0-9]|$)|^tools/privacy-check\.sh:' scan hostnames -oIE "$HOSTNAME_RX"
PRIVATE_IP_RX='(^|[^0-9.])(10\.[0-9]{1,3}|192\.168|172\.(1[6-9]|2[0-9]|3[01])'
PRIVATE_IP_RX="$PRIVATE_IP_RX"'|100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])|169\.254)'
PRIVATE_IP_RX="$PRIVATE_IP_RX"'\.[0-9]{1,3}\.[0-9]{1,3}([^0-9.]|$)'
scan private-ips -oIE "$PRIVATE_IP_RX"

# Symlinks (could point outside the tree) and files over 5 MB.
tr '\0' '\n' < "$TMP/files" | while IFS= read -r f; do
  if [ -L "$f" ]; then echo "symlinks: $f"; fi
  if [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 5242880 ]; then echo "files-over-5MB: $f"; fi
done >> "$FAILS"

# The local denylist: fixed strings, case-insensitive, reported as file:line only.
DENYLIST=${STUNTCAM_DENYLIST:-$HOME/.config/stuntcam-export/deny.txt}
if [ -f "$DENYLIST" ]; then
  grep -v '^[[:space:]]*$' "$DENYLIST" > "$TMP/deny" || true
  if [ -s "$TMP/deny" ]; then
    xargs -0 grep -HnaiF -f "$TMP/deny" < "$TMP/files" 2> /dev/null | cut -d: -f1,2 | sed 's#^#local-denylist: #' >> "$FAILS" || true
  fi
else
  echo "privacy-check: no local denylist at $DENYLIST; that check is skipped" >&2
fi

if [ -s "$FAILS" ]; then
  cat "$FAILS"
  echo "privacy-check: FAILED ($(wc -l < "$FAILS" | tr -d ' ') hits)" >&2
  exit 1
fi
echo "privacy-check: ok ($(tr -cd '\0' < "$TMP/files" | wc -c | tr -d ' ') tracked files)"
