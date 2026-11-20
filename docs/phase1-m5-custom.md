# Phase 1 M5-C: Custom Date-Range Counting Tasks

Status (2026-10-25): the owner authorized M5-C and delegated the automated
test-scope verification and then the main rollout to Claude Code.
**Implemented; automated checks passed on usapp_test; main rollout completed
(0011 + 0012 applied, API and web deployed); live custom-range workflows
unverified; no acceptance recorded.** Automated evidence is in Verification (Run
2026-10-25); deployment evidence is in Main rollout. M5-A/M5-B
evidence stays in [phase1-m5-daily.md](phase1-m5-daily.md) and
[phase1-m5-weekly.md](phase1-m5-weekly.md).

## Design decisions

- **Model:** one ordinary root counting task with `schedule_type = 'custom'` and
  a new nullable `tasks.start_local_date`. It is not a template and has no
  periods; the existing M4 progress, final settlement, replay and expiry paths
  apply unchanged. Progress accumulates across all days of the range; reaching N
  settles once (per-task M4 source uniqueness).
- **Range (plan D5):** start and end dates are inclusive, Asia/Shanghai. The task
  opens at start-date 00:00 and expires at the midnight after the end date. The
  end date is stored as the existing date-only deadline (`deadline_local_date`,
  no local time), so the 0007 rule fixes `deadline_at` to end+1 00:00. Same-day
  ranges are valid.
- **Publication (selected default):** the start must be today or later (Beijing
  date at the claim's decision time) and the end must not precede the start.
  Only a new publication is checked; a same-key replay returns the original
  even after its start. Custom is counting only and takes no `deadline` field.
- **Before the start:** reads report `status = upcoming`; `+1` returns 409
  `TASK_NOT_STARTED` after a read-only state check and rollback, so no event,
  operation, completion or audit is written. The progress insert re-checks the
  start at its decision timestamp and a new 0011 trigger rejects any progress or
  completion before the start (UC001), including direct SQL.
- **After expiry:** unchanged M4 behaviour: committed keys replay; a completed
  task returns ignored; a new incomplete request returns 409 TASK_EXPIRED.
- **Compatibility:** one-off/daily/weekly fingerprints are unchanged; custom
  appends `schedule_type=custom` and `start_date` (the end is already the
  deadline field). Responses add `start_date` only for custom; `period_date` is
  emitted only for daily/weekly. `upcoming` is an additive status value.
- **Read model:** `task_read_model` is replaced with the same 21 columns plus
  `start_local_date` appended; custom roots are listed like one-off roots.
  `security_invoker` and the SELECT-only grant are restated.
- **Grants/startup policy:** no new table or grant. Table-level SELECT/INSERT on
  `tasks` covers the new column. The startup task matrix enumerates columns, so
  its check count is expected to grow from 327 to 333 (one new column on `tasks`
  and one on the view, each checked for 3 subjects); expectations are unchanged.

## Changed files

- Database: new `database/migrations/0011_custom_range_tasks.sql`
  (SHA-256 `7d616bc671244399f5295618fe243dfbdca6de8f77e1e3f65a4f6540bf4878d1`).
  Applied to usapp_test and main 2026-10-25 and frozen; 0000-0010 untouched.
- Repair: new `database/migrations/0012_custom_action_guard.sql`
  (SHA-256 `8e2a7b18858a1ba193b5a299e89e17106b5c182ba335b8e3ea63729d1888a5ec`)
  replaces the 0010 `usapp_daily_action_guard` in place so custom roots are not
  rejected as templates (one-line change; daily/weekly rules unchanged). Applied
  to usapp_test and main 2026-10-25 and frozen. `DbGatedTest.cc` gate and the M5-C
  `StaticCheckTest.cc` case require it.
- Backend: `core/Errors.h/.cc` (`TASK_NOT_STARTED`, 409);
  `TaskService.h/.cc` (custom parsing/fingerprint, claim with `start_local_date`
  and start-not-past check, `start_date` column/JSON, `upcoming` status);
  `TaskCompletionService.h/.cc` (`NotStarted` outcome, start checks in state
  and insert SQL). Controller unchanged.
- Contract: `contracts/openapi.yaml` 0.9.0, additive (custom schedule,
  start/end dates, `upcoming`, `TASK_NOT_STARTED`).
- Web: `api/types.ts`, `api/task-rules.ts` (custom form rule, range label),
  `api/error-messages.ts`, `stores/tasks.ts` (range label on actions, reload on
  TASK_NOT_STARTED), `views/TasksView.vue` (custom option, start/end inputs,
  hint, range on cards/history/detail, `未开始` badge, midnight read refresh).
- Tests: `TaskServiceUnitTests.h`, `TaskCompletionUnitTests.h`,
  `TaskLedgerDbTests.h` (fixture helper column `start_local_date`, default NULL),
  new `CustomRangeDbTests.h` (db_gated block 19), `DbGatedTest.cc` (gate adds
  0011), `StaticCheckTest.cc` (contract 0.9.0, 0011 case),
  `tasks-progress.spec.ts`, `tasks-view.spec.ts`.

## Tests written (lean)

- Unit: custom parse rules (inclusive dates across a year boundary, same day,
  end before start, missing/invalid dates, deadline, single mode, dates on a
  non-custom task rejected), distinct fingerprint including the start, custom
  JSON, `NotStarted` -> 409 `TASK_NOT_STARTED`.
- db_gated block 19, two grouped cases: (1) a past start is refused with no row;
  an upcoming range is read-only (`upcoming`, start/end) and +1 returns
  NotStarted with zero business writes, a period body is rejected; a same-day
  range takes one +1 then two concurrent final +1 (exactly one Completed, one
  Ignored), one completion/operation/audit, two entries, replay, wallet +1/+3.
  (2) historical expired range (progress 1/2): committed key replays, a new key
  is TASK_EXPIRED, detail/history show `expired` with the start date; direct SQL
  progress before the start is UC001; a reversed range violates the CHECK.
  One-off/daily/weekly suites stay enabled as regression.
- Web: custom form rules and range label; one view case (upcoming badge, no +1,
  custom publish body without `deadline`).
- Fixture growth per successful db_gated run (isolated `@m3-fixture.test`, no
  reset): +2 spaces/4 users, 4 custom tasks, 2 progress events + 1 historical,
  1 settlement with 2 entries.

## Verification (2026-10-25, Claude Code)

- Executed: usapp_test DryRun only. Exit 0; 11 migrations skipped with matching
  checksums; exactly one pending, `0011_custom_range_tasks`, sha256 as above.
  Nothing was applied.
- Denied by the permission layer (reported verbatim as "Permission to use
  PowerShell with command ... has been denied."), and therefore not run:
  usapp_test apply + InitRoles + rerun + privilege queries; web unit tests and
  typecheck; test-image build. CTest and the SQL compatibility scripts depend
  on these and were not attempted. **No build or test has passed.**
- No main access, deployment, commit or push.
- Permissions (2026-10-25): narrowed rules were written to `.claude/settings.json`
  (exact usapp_test commands allowed; broad Docker/corepack PowerShell denies
  replaced; main/DB-mutating patterns set to ask), plus the read-only query
  `database/tests/task_privilege_deviation.sql`. They were **not loaded** in this
  session: `docker version` and `docker image ls usapp-api` were still denied
  after the edit. A new session (or `/permissions` in an interactive terminal) is
  needed before the remaining sequence can run.
- Resumed session (2026-10-25, auto permission mode): with the project
  `.claude/settings.json` above in place, `docker version` and
  `docker image ls usapp-api` were each run once, separately, and both denied
  ("Permission to use PowerShell with command ... has been denied."). The denial
  names no rule. No other settings tier exists (no `.claude/settings.local.json`,
  user `~/.claude/settings.json` or managed-settings file); app default
  permission mode is `default`. No file-based rule denies these commands, and the
  project file allows both exactly, so the source looks like the session/app
  permission layer (for example the auto-mode classifier). This is not
  confirmed. Permission probing stopped there. Nothing else was run, and the
  verification state is unchanged.
- Retry after switching the session to default (manual) mode: `docker version`
  was again denied immediately, with no approval prompt. This rules out the
  auto-mode classifier. In default mode an unmatched command prompts, so an
  immediate denial means a deny rule matched. The likely match is
  `PowerShell(docker.exe:*)` in `.claude/settings.json`, if the PowerShell tool
  resolves `docker` to `docker.exe` (deny beats allow). `PowerShell(corepack:*)`
  may block `corepack.cmd` in the same way. This is unconfirmed, and the rules
  were left unchanged (owner decision). Nothing else ran.
- On the owner's instruction, `PowerShell(docker.exe:*)` and
  `PowerShell(corepack:*)` were removed from the deny list. After that,
  `docker version` executed (confirming the deny-rule cause) but exited 1:
  client 29.8.1, context `desktop-linux`, and the daemon was unreachable
  (`npipe:////./pipe/dockerDesktopLinuxEngine` not found; Docker Desktop not
  running). The sequence stopped. Starting Docker Desktop is an owner action.

### Run 2026-10-25 (Docker running, default permission mode)

- `docker version` exit 0 (client/engine 29.8.1, Docker Desktop 4.93.0);
  `docker image ls usapp-api` exit 0.
- usapp_test DryRun: exit 0, 4.9 s; 0000-0010 skipped with matching checksums,
  only `0011_custom_range_tasks` pending, sha256 `7d616bc6...78d1`.
- Apply + InitRoles: exit 0, 10.6 s; 0011 applied and registered. **0011 is
  frozen.** `schema_migrations` lists 0011 with checksum
  `7d616bc671244399f5295618fe243dfbdca6de8f77e1e3f65a4f6540bf4878d1`.
- Privilege deviation query: exit 0, 0 rows. Repeat InitRoles: exit 0, 7.4 s,
  12 skipped. Privilege query again: exit 0, 0 rows.
- Web: `pnpm run test:unit` exit 0, 13 files / 130 tests passed (57.3 s);
  `pnpm run typecheck` exit 0 (12.2 s). No dist build.
- Fresh test image: exit 0, 84.7 s (toolchain/Drogon layers cached, project
  sources recompiled).
- Full CTest: **exit 8, 12.9 s.** `unit` and `static_check` passed; `db_gated`
  failed with 4 of 1450 assertions, all in block 19 (M5-C). Blocks 1-18
  (including M5-A/M5-B) passed. Cause: the 0010 `usapp_daily_action_guard`
  raises `USAPP_DAILY_TEMPLATE_NOT_PERIOD` (UD003) for every
  `schedule_type <> 'one_off'`, so any progress or completion insert on a custom
  root is rejected. That causes `CustomRangeDbTests.h:69/73`. The two wallet
  checks (`WalletLedgerDbTests.h:38`) fail downstream (no settlement, so an empty
  operation id). This is an 0011 design gap: the 0010 guard was not widened.
- Proposed repair (drafted, **not applied**, awaiting owner approval because
  0011 is frozen): new `database/migrations/0012_custom_action_guard.sql`
  (sha256 `8e2a7b18858a1ba193b5a299e89e17106b5c182ba335b8e3ea63729d1888a5ec`)
  replaces `usapp_daily_action_guard` in place. It differs from the 0010 body
  in one line: `NOT IN ('one_off', 'custom')`. The db_gated gate requires 0012,
  and the M5-C static check asserts both. The task/wallet SQL compatibility
  scripts have not run yet (they follow CTest).
- Repair applied (owner-approved 2026-10-25, usapp_test only):
  - DryRun: exit 0, 3.2 s; 0000-0011 skipped with matching checksums, only
    `0012_custom_action_guard` pending, sha256 `8e2a7b18...a5ec` as expected.
  - Apply + InitRoles: exit 0, 10.0 s; 0012 applied and registered. **0012 is
    frozen.** Privilege query exit 0, 0 rows. Repeat InitRoles exit 0, 7.1 s,
    13 skipped. Privilege query again exit 0, 0 rows.
  - `schema_migrations` checksums: 0011 `7d616bc6...78d1`, 0012
    `8e2a7b18...a5ec` (both match the files); 0000-0010 unchanged.
  - Test image rebuild: exit 0, 71.4 s (API and all test targets recompiled).
  - Full CTest: **exit 0**, 12.9 s; `unit` passed (0.04 s), `db_gated` passed
    (10.15 s; gate requires 0000-0012, not skipped), `static_check` passed;
    3/3 targets.
  - SQL compatibility: `task_ledger_test.sql` passed 27 groups as `usapp_api`
    (application) and `postgres` (maintenance); `wallet_ledger_test.sql` passed
    28 groups in both roles. All exit 0 and rolled back.
  - Web checks were not rerun: no web code changed after their pass above.

## Main rollout (2026-10-25, Claude Code, owner-delegated)

- Preflight: target `usapp` confirmed; local file set 0000-0012 and the
  0011/0012 file hashes matched the frozen values. DryRun (with the InitRoles
  plan line only) exit 0, 5.2 s: 0000-0010 skipped with matching checksums, only
  0011 and 0012 pending with the frozen hashes.
- Rollback artifacts: `backups/before-m5c-20261025_194006/` (15.7 s; no
  overwrite, nothing deleted).
  - Dump `usapp_before_0011.dump` (pg_dump custom format, exported to the host):
    320002 bytes, SHA-256
    `1b0f1bb70ad90e0613a594b7b6541449545418bb19abf11dc3993d9786f2f18d`.
  - The exported file was copied back and restored into new scratch database
    `usapp_m5c_restore_20261025_194006` (TEMPLATE template0, `--exit-on-error
    --single-transaction`). All 17 public base tables matched
    before/after/restore; no non-volatile table changed during the dump. This is
    a count check, not row-by-row verification. Business baseline: 7 tasks,
    3 completions, 2 progress events, 4 accounts, 3 operations, 5 entries,
    23 audits, 11 migrations.
  - Running API image
    `sha256:12b368de5fe68b67704d397590d296d51758431aaa0078b89c4d617e1f7c87d4`
    (the M5-B image) tagged `usapp-api:before-m5c-20261025_194006`; the tag was
    re-inspected and resolves to that ID.
  - Caddy's served `/srv/web` was copied to `web-dist/`: 19 files, 248929
    bytes. index.html
    `387e5f5c565769d211d9d4a2f98d7c5223664abf3de3bace2a55d5a925b9617f`, sw.js
    `14442dfa9650369fd845d4ed30a6bc9365d9702c1ccc737005583aeeb85e1c19` (the M5-B
    values).
  - `rollback-info.json` (SHA-256
    `a5deabc0857691f6e391b2a5c3db3425783c4d1b752fab83e1ca66a269d3885f`) records
    the artifacts, hashes, count table and the container `/tmp` files and host
    work directory left in place.
- Migration: an immediate pre-apply DryRun again showed only 0011 and 0012.
  Apply + InitRoles exit 0, 12.9 s: both `[ok]`, 11 skipped. Table/column
  privilege deviation query: 0 rows. Repeat InitRoles exit 0, 7.5 s: 13 skipped,
  none applied. Deviation query: 0 rows. Main `schema_migrations` checksums for
  0011/0012 equal the frozen values; 0000-0010 unchanged.
- Backend: `docker compose build api` exit 0 (no proxy variables needed).
  - A first attempt was aborted by the PowerShell wrapper treating Docker's
    stderr progress as an error (a script defect, not a build failure); the
    rerun took 4.1 s because the aborted attempt had already filled the cache.
  - New image
    `sha256:e6b98f66f4d885b37dcc81b7106d57005d4a5d14cff1bbb9cda78f055e1b9ced`,
    created 11:43:15 UTC.
  - `up -d --no-deps --no-build --wait --wait-timeout 120 api`: exit 0, 8.2 s.
    The running container's image matches, and it is `healthy`.
  - Fresh startup log: wallet privilege policy `ok=true checks=187
    mismatches=0 missing=0 unexpected=0`; task privilege policy `ok=true
    checks=333 mismatches=0 missing=0 unexpected=0` (the expected 327 -> 333).
- Web: `corepack.cmd pnpm run build` (vue-tsc, tsc, vite) exit 0, 23.4 s.
  Precache 18 entries (174.87 KiB); dist 19 files, 250756 bytes. Caddy-served
  index.html `ed73dc7aa5b08f9f9ed237a5bce6dc82c6ad277dec4c890f0c1d04769469d377`
  and sw.js `b698083f04f5716d10d9a13e925dff053cfd847ff84a76130ac9d8bf539e7e19`
  equal the built dist.
- Not done: no custom publication/progress/settlement on main, no browser or
  PWA-update check, no rollback exercise, no Git publication. Scratch database,
  container temp files and artifacts are retained for review.

## Remaining

1. Live custom-range workflows on main (upcoming badge and refused +1, same-day
   N=2 settles once, expiry), plus browser/PWA update acceptance.
2. A formal M5-C acceptance decision (none recorded).

## Limits and risks

- Automated checks on usapp_test and the main rollout completed; live
  custom-range behaviour is unverified.
- Rollback limits: redeploying `usapp-api:before-m5c-20261025_194006` and the
  saved `web-dist/` restores code only. It does not reverse settlements, wallet
  postings or audits made after the rollout. The schema keeps 0011/0012, and the
  M5-B code does not understand custom-range tasks (it ignores their start date
  and range semantics). Restoring the dump over main would discard all later data
  and is not part of any automated step.
- The new runtime needs 0011 and 0012 first (it reads `start_local_date`, and
  custom progress is rejected without 0012); the M5-B
  runtime on a 0011 database ignores custom semantics if custom rows exist.
- Upcoming/expiry transitions are evaluated at read time; client display
  refreshes at Beijing midnight but the server decides acceptance.
- No range length cap is imposed beyond valid dates (1970-9998).
