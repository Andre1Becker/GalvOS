#!/usr/bin/env bash
# next-session.sh — enforce a clean tree, create the session branch, and print
# the model + effort recommended for this prompt (and the ready-to-run command).
#
# Usage:
#   ./next-session.sh 15            -> branch opt/15,  model+effort for prompt 15
#   ./next-session.sh 02-05         -> branch opt/02-05 (grouped session)
#   ./next-session.sh contract      -> branch opt/contract (Session A)
#
# Model selection rationale (Claude Code v2.1.x):
#   --model is a real startup flag; /effort is an IN-SESSION slash command.
#   So we launch with --model and print the /effort line to run first.
#   Aliases: opus (-> Opus 5), sonnet (-> Sonnet 5, the default).
#   Effort:  high (default) | xhigh (hardest architectural/agentic work).
set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 <prompt-number | group | contract>"; exit 1; }
KEY="$1"
BR="opt/$KEY"

first_num() {
  case "$1" in contract) echo "contract"; return ;; esac
  echo "$1" | sed 's/[^0-9].*//' | sed 's/^0*//'
}

pick() {
  local n; n="$(first_num "$1")"
  case "$n" in
    contract)     MODEL=opus;   EFFORT=high  ; WHY="defines the invariants; foundation for every wave" ;;
    16|17|18)     MODEL=opus;   EFFORT=xhigh ; WHY="structural refactor / bisection across many call sites" ;;
    12|13|14)     MODEL=opus;   EFFORT=xhigh ; WHY="analyze-first: judgement call, gates the SegmentPlan design" ;;
    2|3|4|5|6|7)  MODEL=opus;   EFFORT=high  ; WHY="P0 correctness — physics/budget critical but localized" ;;
    1)            MODEL=opus;   EFFORT=high  ; WHY="telemetry base that later waves depend on" ;;
    8|9|10|11)    MODEL=sonnet; EFFORT=high  ; WHY="P1 fix — mechanical, well-scoped" ;;
    19|20|21|22)  MODEL=sonnet; EFFORT=high  ; WHY="additive feature behind a gate" ;;
    23)           MODEL=sonnet; EFFORT=high  ; WHY="comment/doc cleanup, no behavior change" ;;
    *)            MODEL=sonnet; EFFORT=high  ; WHY="default: scoped change" ;;
  esac
}
pick "$KEY"

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "not a git repo"; exit 1; }

if [ -n "$(git status --porcelain)" ]; then
  echo "!! working tree dirty — a previous session is unmerged. Resolve first:"
  git status --short
  exit 1
fi

CUR="$(git symbolic-ref --short HEAD 2>/dev/null || echo main)"
case "$CUR" in opt/*) git checkout main 2>/dev/null || git checkout master ;; esac

if git rev-parse --verify "$BR" >/dev/null 2>&1; then
  echo "branch $BR exists — checking it out"; git checkout "$BR"
else
  git checkout -b "$BR"
fi

echo "── on $BR ─────────────────────────────────────────"
echo "model : $MODEL"
echo "effort: $EFFORT   ($WHY)"
echo
echo "1) ./mkprompt.sh $KEY            # wrapper + prompt(s) to clipboard"
echo "2) claude --model $MODEL         # start session"
echo "3) first line in the session:  /effort $EFFORT"
echo "4) paste clipboard, run"
echo "5) review: git diff   ->   pio test -e native"
echo "6) commit yourself, then: git checkout main && git merge --no-ff $BR"
