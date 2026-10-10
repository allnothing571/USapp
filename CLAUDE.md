# USapp: instructions for Claude Code

Read docs/claude-code-handoff.md and docs/phase0-acceptance.md before any task.
Phase 0 was accepted by the owner on 2026-10-06 as a local development baseline,
within the verified scope and limitations in docs/phase0-acceptance.md. It is not
production-readiness approval, and its recorded limitations remain open.
Phase 1 planning is authorized (docs/phase1-plan.md).

M1 (nickname editing) was approved, implemented, and passed the owner-run
acceptance checks on 2026-10-06 (docs/phase1-plan.md Section 7.9).

M2 design (docs/phase1-plan.md Section 9) has approved directions (2026-10-06): D1
(rewards are newly created for the recipient), D2 bounds (balance and per-entry caps
only), D18 (narrowed grants plus triggers that survive init-roles reruns), D19 (no
new admin asset endpoints), D21 (own balances only).

M2-A (wallet/ledger database foundation) was approved and implemented on
2026-10-07 and passed owner-run verification on usapp_test (docs/phase1-plan.md
Section 9.14). During M2-B rollout, the owner also applied 0006 to the main
database on 2026-10-07. Migration 0006 is applied and must never be edited.

The owner explicitly assigned M2-B directly to Codex on 2026-10-07; Codex
implemented the read endpoint, OpenAPI 0.3.0, startup enforcement, web card,
regression tests and docs. After correcting two test audit-column references,
the owner rebuilt and passed all three backend CTest targets (exit 0). The first
web test run passed 52 tests across seven files but failed to load the HomeView
suite because Vitest lacked the existing Vue plugin. Codex enabled that plugin;
the owner reported web rerun exit 0, type checks/build exit 0, and successful
main migration, startup enforcement and live checks (plan Section 9.19).
Owner-run verification passed; a separate formal M2 acceptance decision has not
been recorded. On 2026-10-09 the owner committed and pushed M2-A/M2-B as
594a77bcadb0d2742c0704f93aea20cb09a1e726 (plan Section 9.20; dated observation,
later Git status is not assumed).

M3-A (task database foundation: migration 0007, init-roles section 8,
TaskPrivilegePolicy, tests) was approved on 2026-10-09, implemented, and passed the
recorded owner-run verification on usapp_test on 2026-10-09 (0007 applied there;
checksum a9c680a0...6161; design Section 11.3). No separate formal acceptance
decision is recorded; 0007 was applied to the main database on 2026-10-10 (below). On 2026-10-09 the owner
committed and pushed M3-A as 6a9da4bd31e4f2b7863501c39e70931fadd5a6e2 (design
Section 12.1; dated observation). The owner confirmed the product decisions in the
design's Section 2.3.

M3-B (task publication and read APIs, OpenAPI 0.4.0, task startup enforcement,
TasksView, per-run committed task fixtures under @m3-fixture.test) was approved on
2026-10-09, implemented by Claude Code (design Section 12), and on 2026-10-10
passed the recorded owner-run checks (design Section 12.9). The owner applied 0007
to the main database and deployed the M3-B runtime and web build. No separate formal
M3-B acceptance decision, commit or push is recorded. Current task: none; wait for
the owner's Git review/commit/push and the next assigned task.
M3-C (completion and settlement) is NOT approved; D29, D30, D32, D33 stay proposals.
Plan Section 9.8's lock order stays binding unless D30 is approved. Migrations 0006
and 0007 are applied and must never be edited. No new migration is authorized.
Never run the usapp_test reset procedure (design Section 6.5); it is owner-only.
M3-A is implemented and owner-verified; M3-B passed the recorded owner-run checks; M3-C and
M4-M10 implementations remain unapproved, as do the other proposed defaults. Do not
implement wallet posting or change applied migrations, grants, dependencies or
deployment.
Key task edits after publication require recipient confirmation (C3/D3).

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
