#!/usr/bin/env bash
# next-session.sh — enforce a clean tree and create the session branch.
# Usage: ./next-session.sh 3          -> branch feat/3
#        ./next-session.sh 14a        -> branch feat/14a
#        ./next-session.sh 7 --sub 2  -> branch feat/7-s2 (warp sub-session 2)

set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <prompt-number> [--sub <k>]" >&2
  exit 1
fi

N="$1"; shift
SUFFIX=""
if [ "${1:-}" = "--sub" ] && [ -n "${2:-}" ]; then
  SUFFIX="-s$2"
fi
BRANCH="feat/${N}${SUFFIX}"

# Must be inside a git repo.
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "not a git repository" >&2; exit 1; }

# Tree must be clean (untracked state files under docs/feature-prompts/ are excluded
# via .git/info/exclude, so they don't count here).
if [ -n "$(git status --porcelain)" ]; then
  echo "working tree not clean — commit or stash first:" >&2
  git status --short >&2
  exit 1
fi

# Base off the integration branch (adjust if you don't use main).
BASE="main"
git checkout "$BASE" >/dev/null 2>&1
git pull --ff-only 2>/dev/null || true   # no-op on a local-only repo

if git show-ref --verify --quiet "refs/heads/$BRANCH"; then
  echo "branch $BRANCH already exists — checking it out" >&2
  git checkout "$BRANCH"
else
  git checkout -b "$BRANCH"
fi

echo "on branch: $BRANCH (base: $BASE @ $(git rev-parse --short HEAD))"

# --- Model + effort selection per prompt --------------------------------------
# Opus 4.8 for the hard prompts: analyze-first signal processing, optimizer
# hot-path (byte-identical guarantee), the weld algorithm, and the broad refactor.
# Sonnet 5 for the well-specified, mechanical prompts (spec carries the work).
# xhigh effort only where the reasoning depth actually pays off.
# Key on the full session id incl. sub-suffix (e.g. 11a, 12b), falling back to
# the base number. Override any time with --model / /model inside the session.

SID="${N}${SUFFIX#-}"      # e.g. "7" + "-s2" -> "7s2"; plain sub ids like 11a pass N="11a" directly
# If you invoke sub-sessions as e.g. `next-session.sh 11a`, N already carries the letter.

MODEL="claude-sonnet-5"
EFFORT="high"

case "$SID" in
  5)                      MODEL="claude-opus-4-8" ;;                 # weld algorithm + budget analysis
  11a|11b)                MODEL="claude-opus-4-8" ;;                 # optimizer core, byte-identical
  12a)                    MODEL="claude-opus-4-8"; EFFORT="xhigh" ;; # analyze-first: dithering model
  12b|13)                 MODEL="claude-opus-4-8"; EFFORT="xhigh" ;; # analyze-first: IIR / resonance
  16)                     MODEL="claude-opus-4-8" ;;                 # codebase-wide refactor
  7a|7b|7c|9a|9b|2|3|4|6|8|10|14a|14b|15)
                          MODEL="claude-sonnet-5" ;;                 # spec-driven / mechanical
esac

echo
echo "suggested model: $MODEL   (effort: $EFFORT)"
echo "run:"
echo "  claude --model $MODEL --effort $EFFORT"
echo
echo "(then paste the clipboard from mkprompt.sh $N)"
