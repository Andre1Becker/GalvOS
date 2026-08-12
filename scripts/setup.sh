#!/usr/bin/env bash
# setup.sh — one-shot cold start for the GalvOS feature-prompt workflow.
# Idempotent: safe to run more than once. Run from the repo root.
#
# Does:
#   1. verify we're in the GalvOS repo root
#   2. create docs/feature-prompts/ and hide it via .git/info/exclude
#   3. place the state files there if missing (STATE / CONTRACT / DECISIONS /
#      SESSION-RUNBOOK / WRAPPER / feature-prompts-en) — never overwrites
#   4. make the workflow scripts executable
#   5. install the pre-merge-commit hook
#   6. append [env:native] to platformio.ini if not already present
#
# Assumes this script and its siblings live in scripts/ next to the repo, OR
# that you point SRC at wherever you unpacked the delivered files.

set -euo pipefail

# Where the delivered files sit. Default: the directory this script is in plus
# ../feature-prompts for the docs. Override with SRC=/path ./setup.sh
SRC="${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
DOCS_SRC="${DOCS_SRC:-$(cd "$SRC/../feature-prompts" 2>/dev/null && pwd || true)}"

say()  { printf '  %s\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*" >&2; }

# --- 1. repo root -------------------------------------------------------------
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  warn "not inside a git repository — cd to the GalvOS repo root first"; exit 1; }

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
if [ ! -f platformio.ini ]; then
  warn "no platformio.ini here — is $ROOT really the GalvOS root?"; exit 1
fi
ok "repo root: $ROOT"

# --- 2. state dir + git exclude ----------------------------------------------
STATE_DIR="docs/feature-prompts"
mkdir -p "$STATE_DIR"
ok "state dir: $STATE_DIR"

EXCL=".git/info/exclude"
mkdir -p "$(dirname "$EXCL")"
touch "$EXCL"
if ! grep -qxF "$STATE_DIR/" "$EXCL"; then
  printf '%s\n' "$STATE_DIR/" >> "$EXCL"
  ok "excluded $STATE_DIR/ (untracked, shared across branches)"
else
  say "$STATE_DIR/ already excluded"
fi

# --- 3. drop in state files (never overwrite) --------------------------------
STATE_FILES="STATE.md CONTRACT.md DECISIONS.md SESSION-RUNBOOK.md WRAPPER.md feature-prompts-en.md"
if [ -n "${DOCS_SRC:-}" ] && [ -d "$DOCS_SRC" ]; then
  for f in $STATE_FILES; do
    if [ -f "$STATE_DIR/$f" ]; then
      say "$f exists — kept"
    elif [ -f "$DOCS_SRC/$f" ]; then
      cp "$DOCS_SRC/$f" "$STATE_DIR/$f"
      ok "placed $f"
    else
      warn "$f not found in $DOCS_SRC — place it manually"
    fi
  done
else
  warn "state-file source dir not found (set DOCS_SRC=/path) — skipping file copy"
fi

# --- 4. scripts executable ----------------------------------------------------
SCRIPT_DIR="scripts"
mkdir -p "$SCRIPT_DIR"
for s in mkprompt.sh next-session.sh setup.sh; do
  if [ -f "$SRC/$s" ] && [ ! -f "$SCRIPT_DIR/$s" ]; then
    cp "$SRC/$s" "$SCRIPT_DIR/$s"
    ok "placed scripts/$s"
  fi
  [ -f "$SCRIPT_DIR/$s" ] && chmod +x "$SCRIPT_DIR/$s" && say "chmod +x scripts/$s"
done

# --- 5. pre-merge-commit hook -------------------------------------------------
HOOK_SRC=""
for cand in "$SRC/pre-merge-commit" "$SCRIPT_DIR/pre-merge-commit"; do
  [ -f "$cand" ] && HOOK_SRC="$cand" && break
done
HOOK_DST=".git/hooks/pre-merge-commit"
if [ -n "$HOOK_SRC" ]; then
  if [ -f "$HOOK_DST" ] && ! cmp -s "$HOOK_SRC" "$HOOK_DST"; then
    cp "$HOOK_DST" "$HOOK_DST.bak.$(date +%s)"
    warn "existing hook backed up to $HOOK_DST.bak.*"
  fi
  cp "$HOOK_SRC" "$HOOK_DST"
  chmod +x "$HOOK_DST"
  ok "installed pre-merge-commit hook"
  say "remember: merge with --no-ff so the hook fires"
else
  warn "pre-merge-commit source not found — hook not installed"
fi

# --- 6. append [env:native] to platformio.ini --------------------------------
SNIP=""
for cand in "$SRC/native-env-snippet.ini" "$SCRIPT_DIR/native-env-snippet.ini"; do
  [ -f "$cand" ] && SNIP="$cand" && break
done
if grep -q '^\[env:native\]' platformio.ini; then
  say "[env:native] already present in platformio.ini"
elif [ -n "$SNIP" ]; then
  # Strip the comment header of the snippet, keep only from [env:native] on.
  printf '\n' >> platformio.ini
  sed -n '/^\[env:native\]/,$p' "$SNIP" >> platformio.ini
  ok "appended [env:native] to platformio.ini"
  warn "review the build_src_filter paths — adjust to your actual sources/shims"
else
  warn "native-env-snippet.ini not found — add [env:native] manually"
fi

# --- done ---------------------------------------------------------------------
echo
ok "setup complete"
echo
say "next:"
say "  1. fill <HASH> / <x.y.z> in docs/feature-prompts/STATE.md"
say "  2. ./scripts/next-session.sh 2      # clean-tree check + branch + model hint"
say "  3. ./scripts/mkprompt.sh 2          # wrapper + prompt to clipboard"
