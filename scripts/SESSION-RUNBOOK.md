# Session Runbook — Optimizer Refactor

How to run the 23 prompts as separate token-cheap Claude Code sessions on a
locally-held repo.

## One-time setup

The state files stay LOCAL and are never committed or pushed. They live in the
working tree; git ignores them via the private, un-pushed exclude file.

1. Create the memory dir, drop the three files in, and exclude them locally:
   ```
   mkdir -p docs/optimizer-refactor
   cp STATE.md CONTRACT.md DECISIONS.md docs/optimizer-refactor/
   echo 'docs/optimizer-refactor/' >> .git/info/exclude
   ```
   No `git add`. `.git/info/exclude` is per-clone and never leaves your machine,
   so neither the state files nor the ignore rule ever appear online.

2. If these files were ever committed before, untrack them (keeps the local
   copy) so the exclude can take effect:
   ```
   git rm -r --cached docs/optimizer-refactor/ 2>/dev/null || true
   ```

3. Keep the prompt file (optimizer-claude-code-prompts-en.md) OUTSIDE the repo
   or in the same local-only dir — your choice. Sessions only need the single
   prompt pasted in, not the whole file.

### Consequence: work in ONE local clone

Because the state files are untracked, they do NOT move with branch switches and
are NOT duplicated per branch — there is exactly one physical STATE.md in the
tree, shared by every session and every branch. This is desirable: parallel
waves (E-H, N-Q) update one shared log instead of causing STATE.md merge
conflicts. The tradeoff is that the state is tied to this clone only; do not
run these sessions from a second checkout expecting the state to follow.

## Per-session wrapper (paste this, then the one prompt beneath it)

```
Follow the standard preamble from the roadmap.
Read docs/optimizer-refactor/STATE.md, CONTRACT.md, DECISIONS.md before doing
anything. Then execute the prompt below. Nothing else.

<paste exactly one numbered prompt here — or a grouped session's prompts>
```

Do not paste other waves. That is the whole point: each session sees ~1 prompt
plus ~2 pages of state, never the other 22 prompts or old diffs.

## Session grouping (18 sessions, not 23)

| Session | Prompts | Parallel? | Depends on |
|---|---|---|---|
| A | Contract tests | no | — (must be first, committed RED) |
| B | 1              | no | A |
| C | 2,3,4,5        | no | B |
| D | 6,7            | no | C |
| E | 8              | yes (group) | B |
| F | 9              | yes | B |
| G | 10             | yes | B |
| H | 11             | yes | B |
| I | 12,13,14       | no | B |
| J | 15             | no | I |
| K | 16             | no | J, I |
| L | 17             | no | K |
| M | 18             | no | K |
| N | 19             | yes (group) | K |
| O | 20             | yes | B |
| P | 21             | yes | C,D,O |
| Q | 22             | yes | B |
| R | 23             | no | everything |

E/F/G/H can run at the same time in separate terminals + branches.
N/O/P/Q likewise. Merge always in wave order regardless of when they ran.

## The loop you run per session

1. Start Claude Code in the repo. Paste wrapper + one prompt.
2. It branches (opt/<n>), reads state, works, emits a diff, self-checks with
   `git apply --check`, and updates the local (untracked) STATE.md /
   DECISIONS.md under docs/optimizer-refactor/.
3. YOU review the diff.
4. YOU merge in wave order and commit with your token. (Claude never commits.)
5. Close the session. Context is discarded; state lives in the repo.

## Two-stage handling for analyze-first prompts

Prompts 12,13,14,19,21,22 are marked "analyze first". To avoid paying for a
full code-reading pass twice if you dislike the proposal:

- Stage 1 session: "analyze only, write the finding + recommendation to
  DECISIONS.md, produce NO code diff."
- You read DECISIONS.md, accept or redirect.
- Stage 2 session: "implement the decision recorded in DECISIONS.md for P<n>."

Optional — for 12/13/14 it is worth it (they gate the P16 struct). For 19/22
the analysis is small enough to do in one session.

## Token hygiene

- Never paste the full prompt file into a session.
- Never ask a session to "review previous changes" — that pulls old diffs into
  context. STATE.md is the summary; trust it.
- Let each session read only the files its prompt names, plus the 3 state files.
- `git show HEAD:<file>` is cheaper than re-reading a whole subsystem when a
  session only needs to confirm one function's current shape.

## Stop conditions

- If `git status` is dirty at session start → stop, you have an unmerged branch.
  (The docs/optimizer-refactor/ state files do NOT count — they are excluded via
  .git/info/exclude and never show up in status.)
- If a Contract test that was green goes red in a later session → that session
  broke an invariant; do not merge it.
- If a session wants to change a decision in DECISIONS.md → it must append a new
  dated entry and say which prompt's assumption changed, not edit in place.
