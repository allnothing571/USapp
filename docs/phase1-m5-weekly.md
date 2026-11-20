# Phase 1 M5-B: Weekly Counting Tasks

Status (updated 2026-10-25): the owner moved directly from M5-A to weekly counting tasks
and assigned the implementation to Claude Code; Codex runs builds and checks.
Source, migration 0010, contract 0.8.0 and tests are written. **Codex-run automated
checks passed; frozen 0010 is applied on test/main and the new API/web are
deployed (2026-10-25). Live weekly workflows remain unverified.** Custom ranges, edits/cancellation,
shop/redemption, undo and M6-M10 remain outside this batch. M5-A evidence and its
limits stay in [phase1-m5-daily.md](phase1-m5-daily.md).

## Design decisions

- **Week (plan D4, selected default):** Monday 00:00 inclusive to the next Monday
  00:00 exclusive, Asia/Shanghai, the same zone as M5-A. A stored week has
  `period_date` = its Monday, `deadline_local_date` = its Sunday (no local time)
  and `deadline_at` = next Monday 00:00. This fits the existing 0007 rule that a
  date-only deadline ends at the following midnight, so cards show "截止 <Sunday>
  当天结束". ISO weekday arithmetic is used, so a week can cross a year boundary
  (for example 2026-12-28 to 2027-01-03); there is no week-number field.
- **Same architecture as M5-A:** an immutable weekly template (`schedule_type =
  'weekly'`, count only, no user deadline) plus lazily materialized one-off
  children with `recurrence_id`/`period_date`. GET never writes: the read model
  projects the current week virtually. Progress never carries over; per-child M4
  source uniqueness gives at most one settlement per week.
- **Public identity:** the template id plus `period_date` (the Monday). Progress
  requests send `{period_date: <Monday>}`; a non-Monday date is a malformed request
  (400 VALIDATION_ERROR), not a stale period. Dated detail reads take the Monday.
- **Week-boundary safety (unchanged ordering):** membership, root advisory lock,
  key classification, period resolution, child lock, ordered wallet locks. A
  committed event replays its own week's facts. The same key for another week
  returns 409. A new request for an old week returns `ignored` if that week
  reached its target, otherwise 409 TASK_EXPIRED. The new week is never advanced
  by an old request: period creation and the progress insert both re-check the
  current week at the single decision timestamp, and the 0010 guards re-check it
  in the database.
- **Compatibility:** one-off and daily request/response shapes and fingerprints
  are unchanged (the weekly fingerprint appends `schedule_type=weekly`, like
  daily). Existing daily `period_date` values are not reinterpreted; daily rows
  and checks are byte-for-byte the M5-A rules (span 1). The internal service
  method keeps its M5-A name `progressDailyTask` and now serves both schedules.
- **Migration strategy:** additive `0010_weekly_tasks.sql`. It widens
  `tasks_schedule_valid`, adds `tasks_weekly_template_valid`, replaces the two
  0009 trigger functions in place with `CREATE OR REPLACE` (same names and
  triggers; daily behaviour identical, weekly gets the Monday/snapshot/date rules,
  and any recurring template is rejected as a progress/completion target), and
  replaces `task_read_model` with the same columns, `security_invoker` and
  SELECT-only grant. 0000-0009 files are untouched.
- **Grants/startup policy:** no new table, view, column or function, so the
  task privilege matrix (327 checks) and init-roles section 10 are unchanged.
  0010 restates the view's REVOKE/GRANT. No wallet change.

## Changed files

- Database: new `database/migrations/0010_weekly_tasks.sql`.
- Backend: `TaskService.h/.cc` (weekly publication rule, `isWeekStart`, schedule
  reported from the template, weekly JSON), `TaskCompletionService.h/.cc` (weekly
  validation, current-week checks for period creation and state; weekly
  `task.period.created` audit adds `schedule_type`). Controller unchanged.
- Contract: `contracts/openapi.yaml` 0.8.0, additive (`weekly` enum values,
  Monday period identity, week semantics).
- Web: `api/types.ts`, `api/task-rules.ts` (`isRecurring`, `periodLabel`),
  `api/tasks.ts` (comment), `stores/tasks.ts` (period label snapshot on actions and
  notices), `views/TasksView.vue` (weekly option and hint, Monday-Sunday range on
  cards/history/detail/pending actions, read refresh for any recurring row, the
  date-only deadline hint shown only for a one-off date without a time).
- Tests: see below. Docs: this file plus brief status links.

## Tests (lean; reuse existing helpers)

- `TaskServiceUnitTests.h` (M5 case): weekly publication rules and distinct
  fingerprint; `isWeekStart` across a year boundary, leap year and invalid dates;
  weekly JSON. The old "weekly is rejected" assertion now uses an unknown schedule.
- New `WeeklyTaskDbTests.h`, db_gated block 18 after the literal usapp_test and
  migration gates (gate list now includes `0010_weekly_tasks`). Two grouped cases:
  (1) read-only current week with Sunday deadline, non-Monday 400, two concurrent
  +1 with different keys materialize one week and settle once (Progressed +
  Completed), replay, ignored excess, wallet +1/+3; (2) historical last-week
  fixture (progress 1/2, unsettled): replay returns last week, same key for this
  week is 409, a new request for last week is TASK_EXPIRED, this week starts at 1,
  last-week detail is expired, history shows last week only; direct SQL rejects
  progress on the weekly template (UD003) and a Tuesday-dated week (UD001).
  Lock-path faults (D32) and daily rollover remain covered by DailyTaskDbTests.h.
- `StaticCheckTest.cc`: contract 0.8.0; 0010 keeps the view SELECT-only, has the
  Monday guard and no new table; db_gated order/gate for block 18.
- Web: `tasks-progress.spec.ts` parameterizes the two period-retry cases over
  daily and weekly and adds weekly form rules/label; `tasks-view.spec.ts` adds one
  weekly publication/display/hint case.
- Fixture growth per successful db_gated run (isolated `@m3-fixture.test`, never
  reset): +2 spaces/4 users, 2 weekly templates, 3 stored weeks, 1 final
  settlement with 2 entries, 3 progress events plus 1 historical event.

## Verification sequence for Codex (fail-fast; owner-approved grouping)

1. usapp_test only: `apply.ps1 -Mode Docker -Database usapp_test -User postgres
   -DryRun` must show 0000-0009 matched and only `0010_weekly_tasks` pending; then
   apply with `-InitRoles`; rerun `-InitRoles`; rerun the M5-A privilege deviation
   query (expected 0 rows).
2. Build the test image; one full CTest (unit, db_gated not skipped, static_check).
3. Web: full unit suite, then vue-tsc and tsc. Do not build the bind-mounted dist.
4. Reuse `task_ledger_test.sql` and `wallet_ledger_test.sql` in both roles on
   usapp_test (exit 0, ROLLBACK).
5. Main rollout only after the above pass and the owner says go: fresh backup and
   scratch restore, DryRun (only 0010 pending), apply with InitRoles, privilege
   query, then the new API (startup policies ok), then the web build.

Record the results once in this document.

## Verification results (2026-10-25, Codex-run)

- The owner explicitly authorized test-database migration, repeated InitRoles and
  database tests after the approval layer blocked the earlier migration attempt.
  Docker Desktop was started because its Linux engine was initially unavailable;
  the initial preview failed before a database connection and applied nothing.
- The successful usapp_test DryRun matched 0000-0009 and showed only 0010 pending.
  0010 SHA-256: `bd3c08d2eb9a9d8c90e0eca28efaacb42244720dfa74a2cb4bc2cf22534db6ea`.
  Application plus InitRoles exited 0. Repeated InitRoles skipped all 11 migrations
  with matching checksums. Full table/column privilege deviation queries after
  both passes returned 0 rows. 0010 is now frozen; never edit its applied bytes.
- Fresh test-image builds succeeded. The initial unit run passed (0.03 s), but
  static_check failed two assertions that still required contract 0.7.0. Codex
  changed only those two version assertions to 0.8.0 in StaticCheckTest.cc,
  preserving all route, schema and privilege assertions. Rebuilt targeted
  static_check passed (0.01 s; total 0.02 s).
- After test migration, one full CTest run passed: unit 0.06 s, db_gated 9.47 s
  (not skipped, including weekly block 18), static_check 0.03 s; total 9.56 s,
  exit 0. Successful service cases committed isolated per-run fixture data.
- Web unit tests: 13 files, 129 tests passed, 56.34 s; subsequent vue-tsc and tsc
  passed. Combined command exited 0. No web dist build was run.
- task_ledger_test.sql passed as usapp_api/application and postgres/maintenance,
  27 groups each; wallet_ledger_test.sql passed in both roles, 28 groups each.
  All four commands exited 0 and ended in ROLLBACK after SET CONSTRAINTS.
- No main migration, runtime replacement, live weekly workflow or Git publication
  was performed in this verification. Runtime rollback and load behavior remain
  unverified; passing automated checks are not production or exhaustive acceptance.

## Main rollout (2026-10-25, Codex-run, owner authorized)

- Main DryRun matched 0000-0009 and showed only frozen 0010 pending.
- Fresh rollback artifacts: `backups/before-m5b-20261025_174546/`.
  Dump `usapp_before_0010.dump`: 313174 bytes, SHA-256
  `a47a96bf98f2f374cee1d221c8d0feafb281b1465d25c318699b900827c2f94a`.
  The exported host dump was copied back and restored with exit-on-error and
  single-transaction into `usapp_m5b_restore_20261025_174546`. All 17 public base
  table counts matched before/after/restore; this is not row-by-row verification.
  Business baseline: 7 tasks, 3 completions, 2 progress events, 4 accounts,
  3 operations, 5 entries, 23 audits, 10 migrations.
- Saved running API image
  `sha256:6509eac365ca05630a3b0046099c05d2f869945cb3e2a21e47474fe02c10d5f8`
  as `usapp-api:before-m5b-20261025_174546`. Copied Caddy's served directory to
  `web-dist/` inside the artifact directory (19 files, 248062 bytes); index.html
  and sw.js were required and hashed. `rollback-info.json` records the artifacts,
  hashes, count comparison and retained container temp files. Nothing was deleted.
- Main 0010 application and InitRoles passed; repeated InitRoles skipped all
  11 matching migrations. Table/column privilege queries returned 0 deviations
  after each pass. The applied migration hash remains the value recorded above.
- Runtime image built successfully and deployed:
  `sha256:12b368de5fe68b67704d397590d296d51758431aaa0078b89c4d617e1f7c87d4`.
  Container image identity and healthy status matched. Startup wallet policy:
  187 checks; task policy: 327 checks; both ok=true with zero mismatches, missing
  or unexpected privileges. Temporary build proxy environment was restored.
- Web build including vue-tsc/tsc exited 0; precache 18 entries, 173.08 KiB.
  Caddy's index.html/sw.js hashes matched the built dist:
  `387e5f5c565769d211d9d4a2f98d7c5223664abf3de3bace2a55d5a925b9617f` /
  `14442dfa9650369fd845d4ed30a6bc9365d9702c1ccc737005583aeeb85e1c19`.
- No weekly publication/progress/settlement was exercised on main, and no
  browser acceptance, rollback exercise or Git publication is claimed. The
  owner's earlier waiver concerned M5-A live checks; it is not a passed M5-B test.

## Limits and risks

- Automated checks passed on usapp_test and main rollout completed. Live weekly
  behavior remains unverified.
- The new runtime needs 0010 first: on a 0009-only database, weekly publication
  fails the old schedule CHECK (500); one-off/daily keep working. The M5-A runtime
  on a 0010 database keeps daily behaviour, but once weekly rows exist it reports
  them as daily periods; an image rollback is not a transparent weekly rollback
  and never undoes rewards.
- The read model computes the current day/week once per query; rows are projected
  for every recurring root before pagination (no load benchmark).
- Tests run near Monday 00:00 Beijing time could straddle a week boundary (same
  class of risk as the M5-A midnight tests).
- Client refresh at Monday midnight reuses the daily midnight timer; background
  throttling can delay display, while the server decides the accepting week.
