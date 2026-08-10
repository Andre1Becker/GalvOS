#!/usr/bin/env bash
# mkprompt.sh — assemble the session wrapper + selected prompt block(s) and copy
# to clipboard. Prevents accidentally pasting the whole prompt file (your main
# token risk).
#
# Usage:
#   ./mkprompt.sh 2 3 4 5        # session C: prompts 2-5
#   ./mkprompt.sh 15             # session J: single prompt
#   ./mkprompt.sh contract       # session A: the contract-test session
#
# Point PROMPT_FILE at your local copy (kept OUTSIDE the repo).
set -euo pipefail

PROMPT_FILE="${PROMPT_FILE:-$HOME/galvos-refactor/optimizer-claude-code-prompts-en.md}"

# clipboard command autodetect: macOS / Wayland / X11 / WSL
clip() {
  if command -v pbcopy >/dev/null;      then pbcopy
  elif command -v wl-copy >/dev/null;   then wl-copy
  elif command -v xclip >/dev/null;     then xclip -selection clipboard
  elif command -v clip.exe >/dev/null;  then clip.exe
  else cat; echo "  (no clipboard tool found — output printed above)"; fi
}

wrapper() {
  cat <<'EOF'
Follow the standard preamble from the roadmap.
Read docs/optimizer-refactor/STATE.md, CONTRACT.md, DECISIONS.md before doing
anything. Then execute the prompt(s) below. Nothing else.

EOF
}

# extract a "## N. Title" section up to the next "## " or "# " heading
extract() {
  local n="$1"
  awk -v n="$n" '
    $0 ~ "^## "n"\\. " { grab=1 }
    grab && NR>1 && ($0 ~ "^## " && $0 !~ "^## "n"\\. ") { exit }
    grab && /^# / && $0 !~ "^## " { exit }
    grab { print }
  ' "$PROMPT_FILE"
}

{
  wrapper
  if [ "${1:-}" = "contract" ]; then
    echo "Write the Contract tests described in docs/optimizer-refactor/CONTRACT.md."
    echo "Create test/optimizer/ with a native (host) build. Add the [env:native]"
    echo "block if missing. Commit the tests RED: several MUST fail against HEAD —"
    echo "that failure is the specification. Do not fix production code in this"
    echo "session. Verify with: pio test -e native (failures expected)."
  else
    for n in "$@"; do
      extract "$n"
      echo
    done
  fi
} | clip

echo "Copied wrapper + prompt(s) [$*] to clipboard."
