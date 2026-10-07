# USapp: instructions for Claude Code

Read docs/claude-code-handoff.md and docs/phase0-acceptance.md before any task.
Phase 0 was accepted by the owner on 2026-10-06 as a local development baseline,
within the verified scope and limitations in docs/phase0-acceptance.md. It is not
production-readiness approval, and its recorded limitations remain open.
Phase 1 planning is authorized (docs/phase1-plan.md).

M1 (nickname editing) was approved, implemented, and passed the owner-run
acceptance checks on 2026-10-06 (docs/phase1-plan.md Section 7.9).

Current task: documentation-only design of M2 (wallet and ledger) in
docs/phase1-plan.md Section 9. Approved design directions (2026-10-06): D1 (rewards
are newly created for the recipient), D2 bounds (balance and per-entry caps only),
D18 (narrowed grants plus triggers that survive init-roles reruns), D19 (no new
admin asset endpoints), D21 (own balances only). These directions are NOT
implementation approval. M2-M10 and all other proposed defaults are NOT approved:
do not implement any of them, or any other Phase 1 code, contract, migration,
grant, SQL script, or dependency change, and do not run anything against a
database, until the owner approves and assigns that specific milestone. Key task edits after
publication of the current period require recipient confirmation (C3/D3).

## Roles and authorization
- Claude Code implements assigned changes and the regression tests they need.
- Codex reviews changes and guides the owner through acceptance checks.
- The owner runs ALL builds, tests, database operations, container operations,
  installations, browser checks, and Git commands. Do not run any of these
  unless the owner explicitly authorizes that specific action.
- Ask before out-of-scope architecture, database, dependency, or feature changes.

## Safety
- Never read the real .env. Never disclose secrets, cookies, codes, or invite codes.
- Do not inspect database backups. Do not add .workbuddy/ or the V1 design document.
- Preserve unrelated changes. Never reset, overwrite, or revert others' work.

## Conventions
- .editorconfig applies: LF line endings, final newline; no trailing whitespace
  except in Markdown.
- README.md stays in Chinese. Handoff and acceptance docs stay in English.
- Treat Git hashes in the docs as dated observations, not current state.
- Git commands are denied in project settings, so produce diffs without Git
  (for example, show before/after hunks of the files you changed).

## Every report
- English. Include the actual diff.
- Distinguish: files modified / statically reviewed / build passed / tests passed /
  real workflow verified. If nothing was run, say "verification has not been run".
- List remaining unverified risks.
- End with a copy-paste-ready report block for Codex.
