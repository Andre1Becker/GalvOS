#!/usr/bin/env bash
# setup.sh — one-time cold-start for the optimizer-refactor session workflow.
# Idempotent: safe to run more than once. Run from the repo root.
#
#   bash setup.sh
#
# Does NOT commit anything. Does NOT start Claude Code.
set -euo pipefail

# ── locate things ────────────────────────────────────────────────────────────
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "!! not inside a git repo — cd into the GalvOS repo first"; exit 1; }
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

SCRIPTS_DIR="scripts"
STATE_DIR="docs/optimizer-refactor"
HOOK_SRC="$SCRIPTS_DIR/pre-merge-commit"
HOOK_DST=".git/hooks/pre-merge-commit"
PIO="platformio.ini"
SNIPPET="$SCRIPTS_DIR/native-env-snippet.ini"

note() { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

# ── 1. scripts executable ────────────────────────────────────────────────────
step "make scripts executable"
for s in mkprompt.sh next-session.sh pre-merge-commit; do
  if [ -f "$SCRIPTS_DIR/$s" ]; then chmod +x "$SCRIPTS_DIR/$s"; note "chmod +x $SCRIPTS_DIR/$s";
  else note "MISSING $SCRIPTS_DIR/$s (copy it in, then re-run)"; fi
done

# ── 2. install the pre-merge hook ────────────────────────────────────────────
step "install pre-merge-commit hook"
if [ -f "$HOOK_SRC" ]; then
  cp "$HOOK_SRC" "$HOOK_DST"; chmod +x "$HOOK_DST"; note "installed -> $HOOK_DST"
  note "reminder: merge with --no-ff so the hook fires"
else
  note "MISSING $HOOK_SRC — hook not installed"
fi

# ── 3. state dir + local-only exclude ────────────────────────────────────────
step "state files (local, untracked)"
mkdir -p "$STATE_DIR"
for f in STATE.md CONTRACT.md DECISIONS.md; do
  if [ -f "$STATE_DIR/$f" ]; then note "exists $STATE_DIR/$f (kept)";
  elif [ -f "$f" ]; then cp "$f" "$STATE_DIR/"; note "copied $f -> $STATE_DIR/";
  else note "MISSING $f in repo root — put the template there and re-run"; fi
done
# exclude via .git/info/exclude (never pushed), only add the line once
EXCL=".git/info/exclude"
if ! grep -qxF "$STATE_DIR/" "$EXCL" 2>/dev/null; then
  echo "$STATE_DIR/" >> "$EXCL"; note "excluded $STATE_DIR/ in $EXCL"
else
  note "$STATE_DIR/ already excluded"
fi
# untrack if it was ever committed (keeps local copy)
if git ls-files --error-unmatch "$STATE_DIR" >/dev/null 2>&1; then
  git rm -r --cached "$STATE_DIR" >/dev/null; note "untracked previously-committed $STATE_DIR/"
fi

# ── 4. append native env to platformio.ini ───────────────────────────────────
step "native (host) test env"
if [ ! -f "$PIO" ]; then
  note "MISSING $PIO — are you in the repo root?"
elif grep -q '^\[env:native\]' "$PIO"; then
  note "[env:native] already present in $PIO (left as-is)"
elif [ -f "$SNIPPET" ]; then
  printf '\n' >> "$PIO"; cat "$SNIPPET" >> "$PIO"
  note "appended [env:native] from $SNIPPET"
else
  note "MISSING $SNIPPET — add [env:native] manually"
fi

# ── 5. PROMPT_FILE hint for mkprompt.sh ──────────────────────────────────────
step "mkprompt.sh PROMPT_FILE"
if [ -f "$SCRIPTS_DIR/mkprompt.sh" ]; then
  CUR="$(grep -m1 'PROMPT_FILE:=' "$SCRIPTS_DIR/mkprompt.sh" || true)"
  note "current default: ${CUR:-<none>}"
  note "override per shell if needed:  export PROMPT_FILE=~/galvos-refactor/optimizer-claude-code-prompts-en.md"
fi

# ── 6. sanity ────────────────────────────────────────────────────────────────
step "sanity check"
note "git status (should NOT list $STATE_DIR/):"
git status --short | sed 's/^/    /' || true

cat <<EOF

== done. next steps ==
  1) verify the native env compiles a stub:   pio run -e native   (may need Session A first)
  2) start the first session:
       ./scripts/next-session.sh contract
       ./scripts/mkprompt.sh contract
       claude --model opus      # then in-session:  /effort high
  3) review -> pio test -e native -> commit yourself -> git merge --no-ff opt/contract

Nothing here was committed. The state files are local-only and will not appear
in git status or online.
EOF
