#!/usr/bin/env bash
# mkprompt.sh — assemble wrapper + selected feature prompts to the clipboard.
# Usage: ./mkprompt.sh 3 4
# Prevents accidentally pasting the whole prompt file (the main token risk).

set -euo pipefail

ROADMAP="docs/feature-prompts/feature-prompts-en.md"   # the numbered prompts 2..16
WRAPPER="docs/feature-prompts/WRAPPER.md"              # standard preamble + governing block

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <prompt-number> [<prompt-number> ...]" >&2
  exit 1
fi

for f in "$ROADMAP" "$WRAPPER"; do
  [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

# Pick a clipboard command for the platform.
clip() {
  if   command -v pbcopy   >/dev/null 2>&1; then pbcopy
  elif command -v wl-copy  >/dev/null 2>&1; then wl-copy
  elif command -v xclip    >/dev/null 2>&1; then xclip -selection clipboard
  elif command -v clip.exe >/dev/null 2>&1; then clip.exe
  else echo "no clipboard tool found (pbcopy/wl-copy/xclip/clip.exe)" >&2; exit 1
  fi
}

# Extract a single prompt block from the roadmap.
# Convention: each prompt starts with a heading line "## Prompt <n>" and runs
# until the next "## Prompt " heading or EOF.
extract() {
  local n="$1"
  awk -v n="$n" '
    $0 ~ "^## Prompt " n "([^0-9]|$)" { grab=1 }
    grab && $0 ~ "^## Prompt " && $0 !~ "^## Prompt " n "([^0-9]|$)" && seen { exit }
    grab { print; seen=1 }
  ' "$ROADMAP"
}

{
  cat "$WRAPPER"
  echo
  for n in "$@"; do
    block="$(extract "$n")"
    if [ -z "$block" ]; then
      echo "prompt $n not found in $ROADMAP" >&2
      exit 1
    fi
    echo "$block"
    echo
  done
} | clip

echo "copied wrapper + prompt(s): $* → clipboard"
