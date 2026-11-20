# Phase 1 M5-A: Daily Counting Tasks

Status (updated 2026-10-22): the owner asked Codex on 2026-10-16 to start the
first M5 batch, daily counting tasks. Source implemented; owner-run backend and
web automated checks passed (Verification below). 0009 is applied/frozen on
usapp_test and main. Compatibility checks (Batches A/B) and the main backup
(Batch C1) passed on 2026-10-21; on 2026-10-22 main 0009 was applied and the M5-A
runtime and web build were deployed (Verification). The owner waived the
remaining live checks on 2026-10-22 and moved to M5-B weekly counting
([phase1-m5-weekly.md](phase1-m5-weekly.md)). No exhaustive acceptance or new Git
publication is claimed.
On 2026-10-20 the owner handed the remaining M5-A work to Claude Code (review,
guided compatibility checks, guarded rollout); see "Remaining work runbook".
Weekly/custom periods, edits/cancellation, dependencies and deployment changes
remain outside this batch. Owner-run commands remain owner-only.

## Implementation direction

- Immutable daily templates; immutable per-day task instances, created lazily on
  first progress. Existing M4 progress and settlement are reused per instance.
- Asia/Shanghai midnight boundaries. Each day starts at zero; no carry-over,
  penalty or automatic reward. No background task/period generation is needed.
- A progress request carries the displayed period date. An older committed event
  replays its own facts; an uncommitted expired-day request cannot count today.
- A read model projects today's period without writing on GET. History contains
  touched periods; untouched days do not create stored rows or history entries.
- Additive 0009 migration only; applied migrations 0000-0008 stay unchanged.
  Publication, current-period display, retries and history keep their old
  one-off behaviour. No automatic mutation retry or offline queue.

## Schema and service

- 0009_daily_tasks.sql adds schedule_type, recurrence_id and period_date to tasks.
  A daily template is counting-only, without a user deadline. Each stored period
  snapshots its immutable parent and has a unique (recurrence_id, period_date),
  a composite same-couple foreign key, and the next Beijing midnight deadline.
  Guards reject snapshot/date mismatches and progress/completion on the template.
  Existing append-only and settlement guards still apply to each child task.
- task_read_model is a security_invoker view: one-off roots, today's virtual or
  stored daily period, and past stored periods. Its public id is the template id;
  id plus period_date identifies a daily row. A period's displayed created_at is
  max(template creation, period start), stable before/after materialization.
  Daily detail labels this as period start. Past untouched dates return 404.
  Open cursors use the stable template id; history cursors use the stored period
  id as their unique tie-breaker, so equal-time records cannot disappear between
  pages. Public JSON still uses the template id/date.
- API 0.7.0 adds schedule_type=daily to publication (count and target required,
  no custom deadline); progress accepts exactly {period_date: YYYY-MM-DD} for
  daily tasks. One-off request/response shapes and publication fingerprints stay
  compatible. GET detail accepts optional period_date; invalid dates return 400.
- Membership/assignee authorization precedes actions. Lock order is membership,
  root advisory lock (33004, hashtext(root UUID)), key classification, period
  resolution, existing M4 child advisory lock, then ordered wallet account locks.
  Hash collisions may serialize unrelated work. No task UPDATE grant is added.
- Existing committed progress is classified by root/date/key before expiry;
  same key for another root/date returns 409. An old completed period can return
  ignored; an old incomplete/unmaterialized period cannot advance or count today.
  First progress creates its child and task.period.created audit in the same
  transaction. The child uses a private random publication key. A publication
  collision with a private key is rejected, not replayed as a new template.
- Per-child M4 source uniqueness means one settlement per day. The final count,
  completion, entries and audits commit atomically; confirmed success is reported
  only after commit. Unknown commit results keep the original request key.
  New period creation also rolls back on a pre-commit error. No balances are
  calculated from response reward amounts.
- The read model grants only SELECT to usapp_api; PUBLIC/usapp_push get none.
  init-roles section 10 re-narrows it after broad grants. Startup checks cover
  this view and its column permissions; a new runtime needs 0009 first.

## Frontend

- Publication offers one-off or daily; daily forces counting and hides custom
  deadline controls. Daily cards, notices and detail include the selected date.
- Pending actions snapshot the date and key, survive list refresh and midnight,
  and retry only on an explicit click. An unresolved old action for a template
  must resolve before starting a new action for that template.
- History deduplication and render keys include the date. Details request that
  exact period. Old responses cannot overwrite the new day's progress; normal
  list reloads remain authoritative. Logout/account-generation guards remain.
- A next-Beijing-midnight timer refreshes reads; focus, visibility and online
  events also refresh when daily rows are present. These never submit +1.
  Background throttling/clock skew can delay display refresh; the server decides
  which period accepts an action and rejects stale dates.

## Changed files and lean coverage (owner-run automated checks passed)

- Database: new database/migrations/0009_daily_tasks.sql; init-roles section 10.
  No applied migration or existing wallet grant is changed.
- Backend: TaskService.h/.cc, TaskCompletionService.h/.cc, TaskController.cc and
  TaskPrivilegePolicy.h/.cc. Existing task/progress/completion routes are reused;
  contract changes are in contracts/openapi.yaml. No new service or dependency.
- Web: api/types.ts, api/tasks.ts, api/task-rules.ts, stores/tasks.ts and
  views/TasksView.vue. No wallet/session-store or service-worker changes.
- Existing unit/static suites cover daily publication validation, legacy
  fingerprints, date parsing, response shape, public-id pagination predicates,
  SELECT-only view privileges, the controller boundary and migration gate.
- New DailyTaskDbTests.h is db_gated block 17, behind the literal usapp_test and
  migration guards; TaskLedgerDbTests.h reuses its insert helper with optional
  period fields. Three grouped real-service cases cover: read-without-write,
  concurrent first progress and key replay, final minting/ignored excess; old-day
  replay versus new-day reset/settlement and two history pages of one template;
  authorization/stale date, D32 partial-write rollback, same-key retry, direct
  template-progress and wrong-period-snapshot rejection. Existing M3/M4 tests remain enabled.
- Existing tasks-progress.spec.ts, tasks-client.spec.ts and tasks-view.spec.ts
  cover the daily form and refresh, date-bound retries, wrong-period responses,
  old success versus today's list, period history/detail identity, client body,
  and a leap-day midnight delay. No new test framework or wording/layout suite.
- Successful DB tests permanently commit isolated @m3-fixture.test data in
  usapp_test, reusing the approved strategy. Approximately three spaces/six users,
  three templates/four stored periods, four final settlements/six entries per
  run; failed/faulted attempts are checked after teardown. No reset or cap fixture.
- Status links: phase1-plan.md, claude-code-handoff.md, CLAUDE.md, README.md and
  database/README.md. M4 evidence is recorded once in plan Section 10.8.

## Verification and limits

Owner-reported evidence (2026-10-20), not commands executed by Codex:

- Test DryRun: 0000-0008 matched, only 0009 pending; transaction-boundary scan
  passed. The owner applied 0009 with InitRoles to literal usapp_test, with no
  error/guard throw. SHA-256:
  9f739a6b2675e3ecf6624400433f8790c31f2d97a0b91c07f9fac5aed3c76c86.
  0009 is now applied and frozen, as are 0000-0008. No main application reported.
- Initial image builds failed fetching the Docker Hub anonymous token, before
  compilation. A subsequent 3/3 run used an existing image; it is NOT evidence
  for the current M5 changes. Host direct requests timed out; an explicit local
  FlClash proxy request returned 200. Docker Desktop proxy settings alone did
  not resolve the reported build request. A later build succeeded while the
  owner temporarily set HTTPS_PROXY and NO_PROXY for the client, restoring
  their previous process values in finally. No general network fix is inferred.
- Fresh test image built successfully in 37.7 s. Its full CTest passed: unit
  0.02 s, db_gated 7.60 s (not skipped), static_check 0.01 s, total 7.62 s.
  The script reached the following command and exited without its failure
  guards firing. No per-assertion counts or runtime startup summaries supplied.
  This exercises the written daily cases on usapp_test, including committed
  isolated fixtures. The 30 s timeout remains unchanged.
- Web full unit suite: 13 files, 126 tests passed, 20.26 s. Then vue-tsc and tsc
  completed with no error or failure-guard throw. No web dist build reported.
- Batches A/B (owner-run 2026-10-21 on usapp_test, wrapped script from the
  runbook below; Claude Code ran nothing): DryRun 10 applied migrations,
  checksums matched, 0 pending; A2 and A4 `-InitRoles` completed successfully;
  A3 and A5 privilege deviation query returned 0 rows each.
  `task_ledger_test.sql`: `usapp_api`/application 27 groups, exit 0, ROLLBACK;
  `postgres`/maintenance 27 groups, exit 0, ROLLBACK. `wallet_ledger_test.sql`:
  `usapp_api`/application 28 groups, exit 0, ROLLBACK; `postgres`/maintenance
  28 groups, exit 0, ROLLBACK. The whole wrapped batch completed without a
  failure guard firing. This confirms the 0009 compatibility prediction for these
  scripts on usapp_test only.
- Batch C1 (owner-run 2026-10-21 on main, stamp `20261021_115251`; Claude Code
  ran nothing): completed without a failure guard firing.
  Dump `backups/usapp_before_m5a_0009_20261021_115251.dump`, 261331 bytes,
  SHA-256 `aef20d90840c3c71bb67d52d24cb5ca65e87214c4c4f042ff251df485de6e25d`;
  scratch database `usapp_m5a_restore_20261021_115251`; saved API image
  `sha256:24980e42aad523b659e34374dda89c9cf13384c8ef0fe1a1aa0bc27771afc5dc`,
  rollback tag `usapp-api:before-m5a-20261021_115251`; served-web backup
  `backups/web_dist_before_m5a_20261021_115251`, 19 files, 245148 bytes,
  `index.html` SHA-256
  `1da04db87eb5d13898049516581eb4d8939e35422c94a358b08c44431b01410d`, `sw.js`
  SHA-256 `6ecb17f942c5032124a69f8bc45e5cc33e274d19719abed85734768126b3f63e`;
  metadata `backups/rollback_before_m5a_20261021_115251.json`. The count table
  was not supplied; completion indicates the script's count guards passed (base
  table counts, not a row-by-row comparison). No counts are recorded here.
- Main rollout (owner/Codex-reported 2026-10-22; Claude Code ran nothing): main
  0009 applied successfully; the repeated InitRoles completed with zero privilege
  deviations on both passes; a subsequent migration run skipped all ten
  migrations with matching checksums. The owner confirmed existing task
  list/detail and wallet reads worked on the old runtime after the migration.
  Codex built and deployed API image
  `sha256:6509eac365ca05630a3b0046099c05d2f869945cb3e2a21e47474fe02c10d5f8`;
  container healthy; startup wallet policy 187 checks and task policy 327
  checks, both ok=true with zero mismatches/missing/unexpected. Codex's web build
  including type checks exited 0; service-worker precache 18 entries, 172.24 KiB.
  An owner screenshot shows the daily selector and counting form rendering; it
  does not prove a daily settlement workflow. The individual C2 DryRun output was
  not supplied separately.
- Waived, not executed (owner decision 2026-10-22): the Batch D live checks
  (daily N=2 publish, first/final +1, reward and duplicate checks, stale-date
  rejection, no-cookie 401, final main query). They are not passed. No
  exhaustive acceptance or later Git commit/push is reported.

Codex performed source/byte/checksum inspection and recorded the owner results;
it ran no compiler, tests, package, Git, container or database commands itself.
PostgreSQL migration acceptance, compilation/type checking and grouped service
cases now have the above automated evidence. Browser behaviour, load testing
and the later deployment steps remain unverified.
The historical rollover fixture uses normal inserts with honest past timestamps;
it is API-unproducible and does not prove a live request waiting across midnight.
No forced lost-COMMIT, real near-cap rejection (D33 deferred), weekly/custom period,
edit/cancel, automatic catch-up or pruning behaviour is claimed.

Daily templates and stored periods are immutable. Without the later cancel/edit
feature they continue to recur; successful test fixtures grow on each run. The
view projects all daily roots and touched history before pagination: indexes and
existing filters support this small local app, but no load/large-history benchmark
or automatic retention is claimed.

## Owner-run sequence

1. Test DB DryRun: 0000-0008 must match, only 0009 pending. Apply to usapp_test
   with InitRoles, rerun InitRoles and verify the view is SELECT-only while
   existing table grants remain unchanged. No main application yet.
2. Build the test image, run one full CTest; web unit suite then typecheck,
   fail-fast. Reuse the task/wallet SQL scripts in both roles after the new
   trigger/view migration. Report target timings, not full logs; do not build dist.
3. Before main rollout: fresh backup and scratch restore, matching basic counts;
   preserve the currently served API image and web dist. Main DryRun only 0009,
   apply plus InitRoles and repeat privilege checks. Then new API healthy and
   wallet/task startup policies ok, followed by web build (dist is bind-mounted).
4. Representative live check: publish daily N=2, first +1 no reward, final +1
   one reward, refresh no duplicate; check selected-date history. Broaden only
   when a concrete failure or unresolved concern justifies it.

Rollback limits: before any daily template exists, old M4 behaviour is expected
to survive the additive migration but has not been verified. After daily rows
exist, the old M4 runtime reads raw tasks and does not understand templates or
period identity; its root +1 is rejected by the new guard and children may be
listed separately. A plain image/web rollback is therefore not a transparent
daily-feature rollback. It also never undoes rewards. Restricting access, a
compatibility repair or restoring the saved database (losing later data) requires
an explicit owner decision; do not remove immutable rows or disable guards.

## Remaining work runbook (historical; A/B and C1 passed 2026-10-21; main rollout reported 2026-10-22; D waived)

Claude Code review before these steps (static, no commands): 0009 bytes match the
frozen SHA-256; init-roles section 10 re-narrows `task_read_model` after the broad
section 5 grant and is re-runnable; `TaskPrivilegePolicy` expects SELECT only on
the view (no INSERT, no column UPDATE). 0009 adds only a self-referencing FK on
`tasks` and BEFORE INSERT guards, so the existing maintenance-mode TRUNCATE cases in
`task_ledger_test.sql` (E5, with `task_progress_events`) and
`wallet_ledger_test.sql` (E9, with `task_completions` and `task_progress_events`)
are expected to stay compatible. No concrete defect was found; no source changed.
Batches A/B later confirmed these expectations on usapp_test (Verification); the
remaining batches are still predictions until the owner runs them.

Each batch is fail-fast and reported once (exit codes and the summary lines, not
full logs). Stop at the first failure and report it; do not continue to a later
batch. Batches A and B run on `usapp_test` only; nothing here touches main.

### Batches A and B: one owner-run script (usapp_test only)

A: DryRun (0000-0009 match, nothing pending), `-InitRoles`, privilege deviation
query (expected **0 rows**), `-InitRoles` again, query again (0 rows).
B: `task_ledger_test.sql` and `wallet_ledger_test.sql`, each as `usapp_api` and as
`postgres`; each must exit 0, print its passed line and end with `ROLLBACK`.

The deviation query checks, for `tasks`, `task_completions`, `task_progress_events`,
`task_read_model` and the three wallet tables:

- table level (SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER,
  MAINTAIN): `usapp_api` has SELECT everywhere and INSERT on base tables only;
  everything else, and every privilege of `usapp_push` and PUBLIC, is false;
- column level (SELECT, INSERT, UPDATE, REFERENCES) for every column:
  `usapp_api` SELECT on all objects, INSERT on base tables only, UPDATE only on
  `wallet_accounts.lock_token`, REFERENCES nowhere; `usapp_push` and PUBLIC none.

It returns one row per deviation. The script writes SQL files with UTF-8 (no BOM)
through .NET, copies them into the `postgres` container with `docker compose cp`
and runs `psql -X -v ON_ERROR_STOP=1 -f` there, so no text is piped through
PowerShell's encoding. Every native exit code is checked; deviation rows are read
with `-At` and cause a throw even when psql exits 0.

```powershell
$ErrorActionPreference = 'Stop'
Set-Location D:\JYJ\USapp
$compose = @('compose', '--env-file', '.\.env', '-f', '.\deploy\docker-compose.yml')
$utf8 = New-Object System.Text.UTF8Encoding $false
$work = Join-Path $env:TEMP 'usapp-m5-checks'
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Assert-Exit([string]$Step) {
    if ($LASTEXITCODE -ne 0) { throw "$Step failed (exit $LASTEXITCODE)" }
}

function Invoke-Apply([string[]]$Extra, [string]$Step) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\database\apply.ps1 `
        -Mode Docker -Database usapp_test -User postgres @Extra
    Assert-Exit $Step
}

function Copy-ToPostgres([string]$Local, [string]$Remote, [string]$Step) {
    & docker @compose cp $Local "postgres:$Remote"
    Assert-Exit $Step
}

$privSql = @'
WITH objs(name, is_view) AS (VALUES ('tasks', false), ('task_completions', false),
                                    ('task_progress_events', false), ('task_read_model', true),
                                    ('wallet_accounts', false), ('wallet_operations', false),
                                    ('wallet_ledger_entries', false)),
roles(r) AS (VALUES ('usapp_api'), ('usapp_push'), ('public')),
tprivs(p) AS (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'),
                     ('REFERENCES'), ('TRIGGER'), ('MAINTAIN')),
cprivs(p) AS (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')),
tbl AS (
  SELECT o.name, r.r, 'table ' || p.p AS what,
         has_table_privilege(r.r, ('public.' || o.name)::regclass, p.p) AS granted,
         (r.r = 'usapp_api' AND (p.p = 'SELECT' OR (p.p = 'INSERT' AND NOT o.is_view))) AS want
    FROM objs o CROSS JOIN roles r CROSS JOIN tprivs p),
col AS (
  SELECT o.name, r.r, 'column ' || p.p || '(' || a.attname || ')' AS what,
         has_column_privilege(r.r, c.oid, a.attnum, p.p) AS granted,
         (r.r = 'usapp_api' AND (p.p = 'SELECT'
                                 OR (p.p = 'INSERT' AND NOT o.is_view)
                                 OR (p.p = 'UPDATE' AND o.name = 'wallet_accounts'
                                     AND a.attname = 'lock_token'))) AS want
    FROM objs o
    JOIN pg_class c ON c.oid = ('public.' || o.name)::regclass
    JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
    CROSS JOIN roles r CROSS JOIN cprivs p)
SELECT name || '|' || r || '|' || what || '|granted=' || granted || '|want=' || want
  FROM (SELECT * FROM tbl UNION ALL SELECT * FROM col) d
 WHERE granted IS DISTINCT FROM want
 ORDER BY 1;
'@
$privFile = Join-Path $work 'm5_privs.sql'
[System.IO.File]::WriteAllText($privFile, $privSql, $utf8)

function Assert-NoPrivilegeDeviation([string]$Step) {
    Copy-ToPostgres $privFile '/tmp/m5_privs.sql' "$Step copy"
    $rows = & docker @compose exec -T postgres psql -X -v ON_ERROR_STOP=1 -At `
        -U postgres -d usapp_test -f /tmp/m5_privs.sql
    Assert-Exit $Step
    $deviations = @($rows | Where-Object { $_ -and $_.Trim() -ne '' })
    if ($deviations.Count -gt 0) {
        throw "$Step returned $($deviations.Count) deviation row(s):`n$($deviations -join "`n")"
    }
    Write-Host "$Step : 0 deviation rows"
}

# ---- Batch A: repeated InitRoles and post-rerun privileges (usapp_test) ----
Invoke-Apply @('-DryRun') 'A1 DryRun'
Invoke-Apply @('-InitRoles') 'A2 InitRoles'
Assert-NoPrivilegeDeviation 'A3 privileges after InitRoles'
Invoke-Apply @('-InitRoles') 'A4 InitRoles rerun'
Assert-NoPrivilegeDeviation 'A5 privileges after rerun'

# ---- Batch B: task/wallet SQL scripts in both roles (usapp_test) ----
foreach ($script in @('task_ledger_test.sql', 'wallet_ledger_test.sql')) {
    Copy-ToPostgres (Join-Path '.\database\tests' $script) "/tmp/$script" "B copy $script"
    foreach ($role in @('usapp_api', 'postgres')) {
        & docker @compose exec -T postgres psql -X -v ON_ERROR_STOP=1 `
            -U $role -d usapp_test -f "/tmp/$script"
        Assert-Exit "B $script as $role"
        Write-Host "B $script as $role : exit 0"
    }
}
Write-Host 'Batches A and B completed without a failure guard firing.'
```

Report back once: the A1 DryRun summary line, A2/A4 exit status, the two
"0 deviation rows" lines (or the thrown rows), and for each of the four B runs the
script's passed line, group count if printed, and `ROLLBACK`.

Assumptions (unverified until run): `apply.ps1 -Mode Docker -User postgres` and
local-socket `psql -U usapp_api` inside the container authenticate as in earlier
owner runs; PostgreSQL 17 accepts `MAINTAIN` in `has_table_privilege`.

### Batch C1: backup, scratch restore and rollback artifacts (prepared 2026-10-21; owner-run and passed 2026-10-21, see Verification)

Scope: main is only **read** (`pg_dump`, two count queries). C1 does not apply
migrations, run InitRoles on main, replace the runtime, build web `dist`, drop any
database or delete any file or row. It creates, all with one timestamp:

- `backups/usapp_before_m5a_0009_<stamp>.dump` (pg_dump custom format, exported
  from the container with `docker compose cp`) and its SHA-256;
- scratch database `usapp_m5a_restore_<stamp>`, restored from the exported file
  with `pg_restore --exit-on-error --single-transaction`;
- image tag `usapp-api:before-m5a-<stamp>` on the image id of the one running
  `api` container;
- `backups/web_dist_before_m5a_<stamp>`: the directory Caddy currently serves,
  copied out of the running container with `docker compose cp caddy:/srv/web`
  (not the local `apps/web/dist`). `index.html` and `sw.js` must exist as files,
  file count and total bytes must be nonzero; both files' SHA-256 are recorded;
- `backups/rollback_before_m5a_<stamp>.json` with all of the above and the
  count table.

Guards: refuses to start if any target path, the image tag, a container temp
file or the scratch database already exists; requires exactly one running `api`
container. Every native exit code is checked.

Counts: every public base table is counted on main before and after the dump and
in the restore (`wallet_accounts`, `task_progress_events` and the other business
tables are required to exist). A non-volatile table that changes during the dump
throws (stop using the app and rerun with a new stamp); volatile service-written
tables (`scheduler_ticks`, `sessions`, `admin_sessions`,
`email_verification_codes`, `audit_logs`) must restore within the before/after
range. Any mismatch, missing or extra table throws. This is a count check, not a
row-by-row comparison. Container temp files are left in `/tmp` (listed in the
metadata); nothing is removed. If C1 fails after creating artifacts, they are kept
for review and a rerun uses a new stamp.

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location D:\JYJ\USapp
    $compose = @('compose', '--env-file', '.\.env', '-f', '.\deploy\docker-compose.yml')
    $utf8 = New-Object System.Text.UTF8Encoding $false

    $stamp      = Get-Date -Format 'yyyyMMdd_HHmmss'
    $label      = "before_m5a_0009_$stamp"
    $mainDb     = 'usapp'
    $scratchDb  = "usapp_m5a_restore_$stamp"
    $backupDir  = Join-Path (Get-Location) 'backups'
    $dumpHost   = Join-Path $backupDir "usapp_$label.dump"
    $dumpInCtr  = "/tmp/usapp_$label.dump"
    $restoreCtr = "/tmp/usapp_restore_$label.dump"
    $countsCtr  = "/tmp/usapp_counts_$label.sql"
    $imageTag   = "usapp-api:before-m5a-$stamp"
    $servedWeb  = 'caddy:/srv/web'
    $distCopy   = Join-Path $backupDir "web_dist_before_m5a_$stamp"
    $metaFile   = Join-Path $backupDir "rollback_before_m5a_$stamp.json"
    $work       = Join-Path $env:TEMP "usapp-m5a-c1-$stamp"

    function Assert-Exit([string]$Step) {
        if ($LASTEXITCODE -ne 0) { throw "$Step failed (exit $LASTEXITCODE)" }
    }
    # Probe a native command without letting its stderr become a terminating error.
    function Test-Native([scriptblock]$Block) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { & $Block *> $null; return ($LASTEXITCODE -eq 0) } finally { $ErrorActionPreference = $old }
    }
    function Invoke-PsqlLines([string]$Db, [string]$Sql, [string]$Step) {
        $out = & docker @compose exec -T postgres psql -X -v ON_ERROR_STOP=1 -At -U postgres -d $Db -c $Sql
        Assert-Exit $Step
        return @($out | Where-Object { $_ -and $_.Trim() -ne '' })
    }

    # ---- 0. Guards: never overwrite an existing artifact ----
    foreach ($p in @($dumpHost, $distCopy, $metaFile, $work)) {
        if (Test-Path -LiteralPath $p) { throw "Refusing to overwrite existing path: $p" }
    }
    if (Test-Native { docker image inspect $imageTag }) { throw "Image tag already exists: $imageTag" }
    foreach ($f in @($dumpInCtr, $restoreCtr, $countsCtr)) {
        if (Test-Native { docker @compose exec -T postgres test -e $f }) { throw "Container file already exists: $f" }
    }
    $existing = Invoke-PsqlLines 'postgres' "SELECT count(*) FROM pg_database WHERE datname = '$scratchDb'" 'scratch db probe'
    if ($existing[0] -ne '0') { throw "Scratch database already exists: $scratchDb" }
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    New-Item -ItemType Directory -Path $work | Out-Null

    # ---- 1. Identify the currently running API container and image ----
    $apiCids = @(& docker @compose ps -q api | Where-Object { $_ -and $_.Trim() -ne '' })
    Assert-Exit 'docker compose ps api'
    if ($apiCids.Count -ne 1) { throw "Expected exactly one running api container, found $($apiCids.Count)" }
    $apiCid  = $apiCids[0].Trim()
    $imageOut = & docker inspect --format '{{.Image}}' $apiCid
    Assert-Exit 'docker inspect api image'
    $imageId = "$imageOut".Trim()
    if (-not $imageId) { throw 'Could not determine the running api image id' }

    # ---- 2. Base-table count query (written UTF-8 without BOM, copied into the container) ----
    $countSql = @'
SELECT c.relname || '|' ||
       (xpath('/row/n/text()',
              query_to_xml(format('SELECT count(*) AS n FROM public.%I', c.relname), false, true, '')))[1]::text
  FROM pg_class c
 WHERE c.relnamespace = 'public'::regnamespace AND c.relkind = 'r'
 ORDER BY c.relname;
'@
    $countsHost = Join-Path $work 'counts.sql'
    [System.IO.File]::WriteAllText($countsHost, $countSql, $utf8)
    & docker @compose cp $countsHost "postgres:$countsCtr"
    Assert-Exit 'copy count query'
    function Get-Counts([string]$Db, [string]$Step) {
        $rows = & docker @compose exec -T postgres psql -X -v ON_ERROR_STOP=1 -At -U postgres -d $Db -f $countsCtr
        Assert-Exit $Step
        $map = [ordered]@{}
        foreach ($r in @($rows | Where-Object { $_ -and $_.Trim() -ne '' })) {
            $parts = $r.Split('|')
            $map[$parts[0]] = [int64]$parts[1]
        }
        return $map
    }
    $required = @('schema_migrations', 'users', 'couples', 'couple_members', 'tasks', 'task_completions',
                  'task_progress_events', 'wallet_accounts', 'wallet_operations', 'wallet_ledger_entries',
                  'audit_logs')
    # Tables that the running service may write without user action (heartbeat, sessions, codes).
    $volatile = @('scheduler_ticks', 'sessions', 'admin_sessions', 'email_verification_codes', 'audit_logs')

    # ---- 3. Fresh main backup inside the container, then export to the host ----
    $before = Get-Counts $mainDb 'main counts before dump'
    & docker @compose exec -T postgres pg_dump -U postgres -d $mainDb -F c -f $dumpInCtr
    Assert-Exit 'pg_dump main'
    $after = Get-Counts $mainDb 'main counts after dump'
    & docker @compose cp "postgres:$dumpInCtr" $dumpHost
    Assert-Exit 'export dump to host'
    $dumpSize = (Get-Item -LiteralPath $dumpHost).Length
    if ($dumpSize -le 0) { throw 'Exported dump is empty' }
    $dumpHash = (Get-FileHash -LiteralPath $dumpHost -Algorithm SHA256).Hash.ToLowerInvariant()

    # ---- 4. Restore the exported file into a new scratch database ----
    & docker @compose cp $dumpHost "postgres:$restoreCtr"
    Assert-Exit 'copy dump back into container'
    $null = Invoke-PsqlLines 'postgres' "CREATE DATABASE $scratchDb OWNER postgres" 'create scratch db'
    & docker @compose exec -T postgres pg_restore -U postgres -d $scratchDb --exit-on-error --single-transaction $restoreCtr
    Assert-Exit 'pg_restore into scratch db'
    $restored = Get-Counts $scratchDb 'scratch counts'

    # ---- 5. Compare base-table counts ----
    foreach ($t in $required) {
        if (-not $before.Contains($t)) { throw "Required table missing on main: $t" }
        if (-not $restored.Contains($t)) { throw "Required table missing in restore: $t" }
    }
    $report = @()
    $unstable = @()
    foreach ($t in $before.Keys) {
        if (-not $restored.Contains($t)) { throw "Table missing in restore: $t" }
        $b = $before[$t]; $a = $after[$t]; $r = $restored[$t]
        $isVolatile = $volatile -contains $t
        if (-not $isVolatile -and $b -ne $a) { $unstable += $t }
        $ok = ($r -eq $b) -or ($r -eq $a) -or ($isVolatile -and $r -ge [Math]::Min($b, $a) -and $r -le [Math]::Max($b, $a))
        $report += [pscustomobject]@{ table = $t; main_before = $b; main_after = $a; restored = $r; volatile = $isVolatile; match = $ok }
    }
    foreach ($t in $restored.Keys) { if (-not $before.Contains($t)) { throw "Unexpected table in restore: $t" } }
    $report | Format-Table -AutoSize | Out-String | Write-Host
    if ($unstable.Count -gt 0) { throw "Main changed during the dump (stop app use and rerun): $($unstable -join ', ')" }
    $mismatch = @($report | Where-Object { -not $_.match })
    if ($mismatch.Count -gt 0) { throw "Restored counts differ: $(($mismatch | ForEach-Object { $_.table }) -join ', ')" }

    # ---- 6. Preserve the running API image and the served web dist ----
    & docker image tag $imageId $imageTag
    Assert-Exit 'tag running api image'
    # Copy what Caddy actually serves. $distCopy does not exist yet (guarded above), so
    # docker cp creates it with the contents of /srv/web.
    & docker @compose cp $servedWeb $distCopy
    Assert-Exit 'copy served web from caddy'
    $indexFile = Join-Path $distCopy 'index.html'
    $swFile    = Join-Path $distCopy 'sw.js'
    foreach ($req in @($indexFile, $swFile)) {
        if (-not (Test-Path -LiteralPath $req -PathType Leaf)) { throw "Served web copy is missing required file: $req" }
    }
    $distFiles = @(Get-ChildItem -LiteralPath $distCopy -Recurse -File)
    $distBytes = [int64]($distFiles | Measure-Object -Property Length -Sum).Sum
    if ($distFiles.Count -le 0 -or $distBytes -le 0) { throw 'Served web copy is empty' }
    $indexHash = (Get-FileHash -LiteralPath $indexFile -Algorithm SHA256).Hash.ToLowerInvariant()
    $swHash    = (Get-FileHash -LiteralPath $swFile -Algorithm SHA256).Hash.ToLowerInvariant()

    # ---- 7. Metadata (same timestamp for every artifact) ----
    $meta = [ordered]@{
        created_local      = (Get-Date).ToString('o')
        stamp              = $stamp
        main_database      = $mainDb
        dump_host_path     = $dumpHost
        dump_bytes         = $dumpSize
        dump_sha256        = $dumpHash
        scratch_database   = $scratchDb
        api_container_id   = $apiCid
        api_image_id       = $imageId
        api_image_tag      = $imageTag
        web_served_source  = $servedWeb
        web_dist_copy      = $distCopy
        web_dist_files     = $distFiles.Count
        web_dist_bytes     = $distBytes
        web_index_sha256   = $indexHash
        web_sw_sha256      = $swHash
        counts             = $report
        container_tmp_files_left = @($dumpInCtr, $restoreCtr, $countsCtr)
    }
    $json = $meta | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText($metaFile, $json, $utf8)

    Write-Host "C1 completed without a failure guard firing."
    Write-Host "dump: $dumpHost ($dumpSize bytes, sha256 $dumpHash)"
    Write-Host "scratch database: $scratchDb"
    Write-Host "api image: $imageId tagged $imageTag (container $apiCid)"
    Write-Host "web dist copy (from $servedWeb): $distCopy ($($distFiles.Count) files, $distBytes bytes, index.html $indexHash, sw.js $swHash)"
    Write-Host "metadata: $metaFile"
}
```

Report back once: the printed count table (or the thrown message), the six
summary lines, and confirmation that main was not otherwise touched.

### Batch C2: main DryRun only (prepared 2026-10-21; not run)

Scope: read-only. `apply.ps1 -DryRun` connects to main only for read queries
(database existence, `schema_migrations` versions/checksums); with `-DryRun`,
`-InitRoles` only prints the plan line and runs nothing. C2 applies no migration,
runs no grants, replaces no runtime and builds no web `dist`.

Local read-only pre-checks: the migration file set is exactly 0000-0009 and the
local 0009 file still has the frozen SHA-256. After the DryRun exits 0, the
script checks the ASCII parts of its output: one `[skip ]` line (applied, checksum
matched) for each of 0000-0008 and exactly one `[dry  ]` pending line,
`0009_daily_tasks` with sha256
`9f739a6b2675e3ecf6624400433f8790c31f2d97a0b91c07f9fac5aed3c76c86`. A checksum
mismatch on an applied migration makes `apply.ps1` itself throw (nonzero exit).

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location D:\JYJ\USapp
    $expected0009 = '9f739a6b2675e3ecf6624400433f8790c31f2d97a0b91c07f9fac5aed3c76c86'
    $applied = @('0000_schema_migrations', '0001_common', '0002_identity', '0003_couple_binding',
                 '0004_scheduler', '0005_admin_audit', '0006_wallet_ledger', '0007_tasks_one_off',
                 '0008_task_counting')

    function Assert-Exit([string]$Step) {
        if ($LASTEXITCODE -ne 0) { throw "$Step failed (exit $LASTEXITCODE)" }
    }

    # ---- Local pre-checks (read-only): migration file set and frozen 0009 bytes ----
    $files = @(Get-ChildItem -LiteralPath .\database\migrations -Filter '*.sql' | Sort-Object Name |
               ForEach-Object { $_.BaseName })
    $want = @($applied) + @('0009_daily_tasks')
    if (($files -join ',') -ne ($want -join ',')) { throw "Unexpected migration file set: $($files -join ', ')" }
    $local0009 = (Get-FileHash -LiteralPath .\database\migrations\0009_daily_tasks.sql -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($local0009 -ne $expected0009) { throw "Local 0009 SHA-256 differs from the frozen value: $local0009" }

    # ---- C2: main DryRun only. Under -DryRun, -InitRoles only prints the plan. ----
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\database\apply.ps1 `
        -Mode Docker -Database usapp -User postgres -DryRun -InitRoles | Tee-Object -Variable applyOut
    Assert-Exit 'C2 main DryRun'

    # ---- Output guards (ASCII parts only) ----
    $lines = @($applyOut | ForEach-Object { "$_" })
    foreach ($v in $applied) {
        $hit = @($lines | Where-Object { $_ -match ('^\[skip \] ' + [regex]::Escape($v) + ' ') })
        if ($hit.Count -ne 1) { throw "Expected one [skip ] line for $v, found $($hit.Count)" }
    }
    $pending = @($lines | Where-Object { $_ -match '^\[dry  \] \d{4}_' })
    if ($pending.Count -ne 1) { throw "Expected exactly one pending migration, found $($pending.Count)" }
    if ($pending[0] -notmatch ('^\[dry  \] 0009_daily_tasks .*sha256: ' + $expected0009)) {
        throw "Unexpected pending line: $($pending[0])"
    }

    Write-Host "C2 completed without a failure guard firing."
    Write-Host "main: 0000-0008 skipped with matching checksums; only 0009_daily_tasks pending (sha256 $expected0009); nothing applied."
}
```

Expected: nine `[skip ]` lines (0000-0008), one `[dry  ] 0009_daily_tasks` line
with the frozen sha256, the InitRoles plan line, the summary line (applied 9,
pending 1, no database change) and the two C2 lines. Report back once: the
`apply.ps1` summary line, the 0009 pending line and the two C2 lines (or the
thrown message). Main 0009 application is a later, separately guarded step,
prepared only after this output is reviewed.

### Batch C (remaining steps): guarded main rollout (only after C2 output is reviewed and the owner says go)

1. (Done by C1, 2026-10-21.) Fresh main backup; restore into a new scratch database with exit-on-error in a
   single transaction; compare basic counts (migrations, users, couples, members,
   tasks, task_progress_events, task_completions, wallet operations/entries,
   audit_logs). A count check is not a row-by-row comparison.
2. (Done by C1, 2026-10-21.) Preserve and tag the currently served API image and copy the currently served
   web `dist` with the same timestamp label.
3. (Batch C2.) Main `apply.ps1 -DryRun`: 0000-0008 match, only `0009_daily_tasks` pending.
4. Main `apply.ps1 -InitRoles`; checksum must equal the frozen value above; run the
   Batch A deviation query against main (expected 0 rows); rerun `-InitRoles`,
   query again. A main-specific script will be prepared only after C2 output is reviewed.
5. Confirm the old runtime still serves login, wallet and one-off/count task reads
   before switching (no daily rows exist yet).
6. Build/deploy the new API runtime; startup log must show wallet and task
   privilege policies `ok=true` with zero mismatches/missing/unexpected. If startup
   is refused, redeploy the preserved image and stop.
7. Only after the API is healthy: build web (`dist` is bind-mounted, so this
   replaces served files immediately); accept the PWA update.

### Batch D: representative live checks (main deployment)

- Publish one daily task with N=2 to the partner; partner sees it for today.
- First +1: progress 1/2, no reward; second +1: completed, one reward; refresh
  shows no duplicate and the wallet card reflects exactly one reward.
- History/detail for that date show the period; an explicit stale-date request is
  rejected (if convenient through the existing UI path).
- One no-cookie request to a daily read returns 401; responses carry `no-store`.
- Final main query: the daily template, one stored period, its progress events,
  one completion, one task-sourced operation with its entries and audits match the
  actions taken.

Rollback limits are in the paragraph above; a runtime/web rollback does not undo
rewards or remove daily rows.
