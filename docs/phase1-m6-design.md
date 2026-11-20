# Phase 1 M6: Task Editing and Cancellation

Status (2026-10-25): **approved by the owner for implementation and automated
verification (B1-B3: database, backend, contract, web, tests).** Main rollout is a
separate, later step. **Implemented and test-verified on usapp_test (Section 5);
main rollout completed 2026-10-25 (Section 5.1); live workflows unverified, no
acceptance recorded.** Migrations 0000-0012 are unchanged; M6 adds 0013, applied
to usapp_test and main and frozen.

Inputs: `docs/phase1-plan.md` (C3/D3, M6 row, 9.8, 10.2),
`docs/phase1-m3c-design.md` Section 4 (D30 Option A), `docs/phase1-m5-*.md`,
migrations 0007-0012, `TaskService.cc`, `TaskCompletionService.cc`,
`init-roles.sql` Sections 8-10, `TaskPrivilegePolicy.cc`.

## 1. Facts the design relies on

- `tasks`, `task_completions` and `task_progress_events` are append-only (UT001
  statement trigger, every role); `usapp_api` has SELECT/INSERT only. Guards read
  terms from the task row itself:
  - completion: snapshot UT004, deadline UT006;
  - progress: target UP001, deadline UP004;
  - periods: UD001/UD002;
  - custom start: UC001.
- Per-task serialization is the advisory lock `(33004, hashtext(task uuid))`. Count
  progress takes it on the public id (the template root for daily/weekly), then
  the instance lock after period resolution. Triggers take the root lock before
  the instance lock.
- Single completion (D30 Option A) previously took no advisory lock: membership
  `FOR SHARE` -> operation claim -> accounts -> completion.
- No member-removal path exists (RESTRICT FKs; no leave/unbind code).
- Status is derived at read time.

## 2. Approved rules (owner, 2026-10-25)

| Topic | Rule |
|---|---|
| Who proposes | Creator proposes edits; either member may propose cancellation. |
| Who decides | The other member approves or rejects; only the proposer withdraws. |
| Fields | Title, description, flower/sunshine reward, completion mode and target N; one-off deadline date/time; custom start/end dates. All of them use the same confirmation workflow. Schedule type, assignee and space are immutable. |
| While pending | Original terms and actions stay fully active. |
| Open proposals | At most one undecided proposal per root task; no automatic expiry. |
| Edit approval | Requires zero progress and no completion on the affected task: the one-off/custom root, or the current period of a daily/weekly template. Progress is never moved, reset or discarded. Otherwise 409 `TASK_HAS_PROGRESS` and the proposal stays undecided, so it can still be rejected or withdrawn. |
| Cancellation | Mutual confirmation, allowed with progress. Progress stays as history; no partial reward. |
| Completed one-off/custom | Final: no edit or cancellation. Issued rewards are never reversed. |
| Recurring edit | Replaces the template from the current period onward, only before that period has progress or completion. Past periods are unchanged. |
| Recurring cancel | Stops future periods even if the current period is already completed. A completed current period keeps its completion and reward; an unfinished current period is closed without partial reward. |
| Custom start | An already-started range with zero progress may keep its original start date. A changed start must be today or later (Beijing date at the decision). Range validity and a still-future end are checked at approval. |
| Expired one-off/custom | Final (409 `TASK_EXPIRED` on approval). |

Examples:

1. One-off "Wash the car", single, 5 flowers. A proposes 8 flowers, B approves:
   the old task becomes `superseded` with `closure.replaced_by` pointing to the
   successor, which is open with 8 flowers. One later settlement of 8.
2. Count task at 3/10. A proposes N=8, and B's approval returns 409
   `TASK_HAS_PROGRESS`. A withdraws, or B rejects. Cancellation can be proposed
   instead; after approval, 3/10 stays visible as `cancelled` with no reward.
3. Daily template; today's period already has +1. An edit approval today returns
   409. Tomorrow, before the first +1, approval succeeds: the old template closes
   and the successor's period starts today.
4. Weekly template; this week is completed and either member proposes cancel.
   After approval, this week stays `completed` (reward kept), the row carries
   `closure` (template cancelled), and no new week is materialized.

## 3. Architecture

### 3.1 Selected approach

Append-only proposals, decisions and closures, plus immutable successor tasks. No
task UPDATE grant and no new `tasks` column. Every task row keeps one immutable
term set, so all existing guards, settlement validation and replay construction
stay correct. Rejected alternatives:

- narrow mutation: breaks D26/D30, the guards and replay;
- revision overlay: every guard and read would need replacing, plus an invented
  carry-over rule for progress.

### 3.2 Schema (migration `0013_task_changes.sql`)

- `task_change_proposals`:
  - `id`, `couple_id`, `task_id` (root), `proposer_id`, `kind` (`edit` | `cancel`);
  - for an edit, the full term set: title, description, rewards, mode, target,
    deadline date/time, start date;
  - `idempotency_key`, `request_fingerprint`, `created_at`;
  - CHECKs mirror the `tasks` shapes; unique `(proposer_id, idempotency_key)`.
- `task_change_decisions`:
  - `proposal_id` is the PK, so a proposal has at most one decision;
  - `couple_id`, `decided_by`, `outcome` (`approved` | `rejected` | `withdrawn`);
  - `successor_task_id` (deferrable FK; an approved edit only);
  - `idempotency_key`, `request_fingerprint`, `decided_at`; unique
    `(decided_by, idempotency_key)`.
- `task_closures`:
  - `task_id` is the PK (a root task); `couple_id`, `reason`
    (`cancelled` | `superseded`), `proposal_id` (unique), `closed_at`.
  - Recurring instances are not closed individually. An instance counts as
    closed when its root's closure has `closed_at` before the instance's
    `deadline_at` and the instance is not completed. A completed period keeps
    `completed`.
- `task_read_model`: the same 22 columns. Live recurring roots are unchanged.
  - A closed template produces no virtual current row.
  - All of its stored instances stay visible (history pagination is stable).
  - If no stored instance exists for the closure period, one virtual row for
    that period is emitted (id = template id; derived from the immutable
    `closed_at`).
  - The row representing the closure period is that template's
    `current_period` row.
- Grants: SELECT and INSERT on the three tables for `usapp_api` (in the migration
  and in `init-roles.sql` Section 11); the startup task policy covers them.

### 3.3 Database invariants (SQLSTATE class UX)

| Code | Invariant |
|---|---|
| UT001 | The three new tables are append-only (UPDATE/DELETE/TRUNCATE). |
| UX002 | Proposal insert, under the root lock: the target is a root in the same couple; an edit requires proposer = creator and terms that fit the schedule type; the task is not closed; one-off/custom is not completed; no undecided proposal exists. |
| UX003 | Decision insert, under the lock: same couple; `withdrawn` only by the proposer, `approved`/`rejected` only by the other member; a successor only for an approved edit. |
| UX004 | Closure insert, under the lock: an approved decision of the matching kind exists for this task and `closed_at = decided_at`. One-off/custom: not completed, and `closed_at` is before `deadline_at`. Edit: zero progress on a one-off/custom task; for daily/weekly, no stored instance (any instance has progress) in the period containing `closed_at`. |
| UX005 | Deferred link check at commit (from decision, closure and `chg:`-keyed task inserts): approved <=> exactly one closure. An approved edit <=> exactly one successor that is a root with the same couple, creator, assignee and schedule type, terms equal to the proposal, `created_at = decided_at` and key `chg:<proposal id>`. The successor deadline is after the decision. A custom successor start either equals the original start or is on/after the decision's Beijing date. |
| UX006 | Progress and completion inserts take the root lock, then reject any task whose root is closed. Trigger names sort before the existing ones, so the root lock is taken first. |
| UX007 | A period instance cannot be materialized for a closed root. |

Settlement validation (UT007), the count invariants (UP003) and the wallet chain
are unchanged. M6 inserts no wallet rows. Completion and closure exclude each
other under one lock, so double settlement is impossible.

### 3.4 D30 revisited: lock order

D30 assumed immutable terms. Rows stay immutable; liveness (closure) is the new
mutable state, so the per-task advisory lock becomes the single serialization
point. The global order is:

1. `couple_members` `FOR SHARE` (the caller's row).
2. Advisory `(33004, hashtext(root/public id))`.
3. Advisory on the instance id: only on the recurring progress path, after
   period resolution.
4. Claims and key classification (operation, progress event, proposal,
   decision).
5. `wallet_accounts` `FOR UPDATE`, ascending: settlement paths only.
6. Decision-time inserts, audits, COMMIT.

Path changes:

- **Single completion** now takes step 2 before its claim. This is the only order
  change to an existing path; it has the same prefix as counting.
- **Approval, reject and withdraw** hold only steps 1-2 and never wait on accounts
  or on uncommitted operations (MVCC reads). There is no deadlock cycle.
- **Period materialization** already runs under the root lock; UX007 adds the
  closure check.
- **Membership:** no removal path exists. A future leave/unbind must take
  `couple_members` `FOR UPDATE` first.

### 3.5 Idempotency, replay and RESULT_UNKNOWN

- Both new POSTs require `Idempotency-Key` (16-128 characters `[A-Za-z0-9_-]`),
  scoped per actor per table, with a stored fingerprint:
  - propose: SHA-256 of `task-change:v1:` + task + kind + canonical edit terms;
  - decide: SHA-256 of `task-change-decision:v1:` + proposal + action.
- Key classification happens under the lock and before any state check, so a
  same-key replay still works after later closure, completion or expiry.
  - Same fingerprint -> 200 replay.
  - Different fingerprint -> 409 `IDEMPOTENCY_KEY_REUSED`.
  - A new key on a decided proposal -> 409 `TASK_CHANGE_DECIDED`.
- Responses separate **immutable facts** (`change`: the proposal and its
  decision) from the **current derived state** (`task`, `successor`: status,
  progress, `pending_change` and `closure` may change). Replays guarantee
  identical `change` facts, not identical whole bodies.
- The successor key is `chg:<proposal uuid>`. The colon is outside the client key
  alphabet, so no client can pre-claim it.
- An unconfirmed COMMIT returns 503 `RESULT_UNKNOWN` with a fixed label. The client
  keeps the action and retries with the same key. The exactly-once callback and
  `TxScope` patterns follow M3-C.

### 3.6 Affected paths

| Path | Change |
|---|---|
| Single completion | Advisory lock before claim. After a new claim, a closed root -> 409 `TASK_CANCELLED`/`TASK_SUPERSEDED` with zero writes. Same-key replays are classified before this check. |
| Progress (+1) | After key classification (replay first): closed root -> 409 with zero writes; no materialization. |
| Reads (`kTaskColumns`) | Status precedence: completed > cancelled/superseded > expired > upcoming > open. Additive fields, emitted only when present: `closure` {reason, closed_at, replaced_by}, `replaces`, `pending_change` {id, kind, proposer_id, created_at, terms, blocked_reason}. `pending_change` is shown on root and current-period rows. |
| Open list | Excludes closed rows. |
| History | Adds closed rows. Sort and cursor key: completed_at, else closed_at, else deadline_at; 30-day window. |
| New `POST /tasks/{id}/changes` | Propose. |
| New `POST /tasks/{id}/changes/{proposalId}/decision` | `approve` / `reject` / `withdraw`. |
| Startup policy | Seven task objects. |

## 4. Implementation scope

- Database: `0013_task_changes.sql`; `init-roles.sql` Section 11;
  `task_privilege_deviation.sql`; `task_ledger_test.sql` M6 groups (and its
  maintenance TRUNCATE list includes the new referencing tables).
- Backend: `TaskChangeService.h/.cc` (new); `TaskController` routes; `Errors`
  codes; `TaskService` read columns and JSON; `TaskCompletionService` lock and
  closure checks; `TaskPrivilegePolicy` table list.
- Contract 0.10.0 (additive): two operations, change schemas, Task fields, status
  values, error codes `TASK_CHANGE_PENDING`, `TASK_HAS_PROGRESS`,
  `TASK_CHANGE_DECIDED`, `TASK_CHANGE_FORBIDDEN` (403), `TASK_CHANGE_NOT_FOUND`
  (404), `TASK_CANCELLED`, `TASK_SUPERSEDED`.
- Web:
  - types, api, error messages;
  - store change actions: key kept on unknown/not-sent, explicit retry only;
  - view: propose edit/cancel, approve/reject/withdraw, pending/blocked notices,
    status badges, lineage.
- Not in scope: no dependency, framework, database reset, timeout increase, wallet
  reversal or shop/redemption work.

### Tests

| # | Layer | Case |
|---|---|---|
| U | unit | Change-request parsing, fingerprints, response JSON separation, error mapping, policy table list. |
| D1 | db_gated | One-off edit approved: superseded + successor; old actions 409 with zero writes; successor settles once; replay facts stable and current state truthful. |
| D2 | db_gated | Edit vs completion race on a zero-progress single task and a target-1 count task, both winning orders (advisory holder fixes the queue order). |
| D3 | db_gated | Cancel vs final +1 race on a task with existing progress, both orders: either cancelled with progress kept and no settlement, or completed with one settlement. |
| D4 | db_gated | Progress blocks edit (409, still rejectable/withdrawable); authorization (403/404); one-open-proposal (409); completed/expired (409). |
| D5 | db_gated | Daily/weekly: edit blocked by current progress; template cancel with a completed current period keeps the completion; materialization refused (service 409, direct SQL UX007). |
| D6 | db_gated | Custom: started range keeps its start; a changed past start is rejected; direct-SQL UX002-UX006. |
| S | SQL | `task_ledger_test.sql` M6 groups in both roles; `wallet_ledger_test.sql` unchanged. |
| W | web | Store change lifecycle including unknown/retry; view decisions and badges. |
| R | regression | Full CTest (blocks 1-19 unchanged), web unit/typecheck. |

## 5. Verification (2026-10-25, Claude Code, usapp_test only)

Implemented: B1-B3 as scoped in Section 4. **Test-verified on usapp_test; not
deployed; main not accessed.** Exact pre-existing assertions updated for the
intended changes:

- contract version `0.9.0` -> `0.10.0` (three StaticCheck lines);
- the M3-B history pagination-key text now includes `kc.closed_at`;
- the task-policy unit matrix lists the three new tables.

No test was removed or weakened.

- Web (before backend runs): `pnpm run typecheck` exit 0, 10.6 s; `pnpm run
  test:unit` exit 0, 14 files / 135 tests (130 existing + 5 new in
  `tasks-change.spec.ts`), 20.3 s. No dist build. No web file changed after this
  run.
- Rollback-only pre-validation, before 0013 was frozen. In a single transaction
  ending in ROLLBACK on usapp_test, the 0013 body was followed by
  `task_ledger_test.sql`:
  - maintenance role: exit 0, 35 groups (27 existing + 8 M6);
  - `SET ROLE usapp_api`: exit 0, 35 groups;
  - read model on existing data: 333 rows before and after the replaced view,
    diff 0;
  - afterwards 0013 was not registered and `task_closures` did not exist.
- DryRun: exit 0, 5.0 s; 0000-0012 skipped with matching checksums, only
  `0013_task_changes` pending.
- Apply + InitRoles: exit 0, 10.4 s. **0013 is frozen**, SHA-256
  `7e4855a97d51b92be2ca02e6c4606bc7654603f19402dc6b12e1f58fe5ede8de` (file =
  `schema_migrations`). Table/column privilege deviation query (now including the
  three M6 tables): exit 0, 0 rows. Repeat InitRoles: exit 0, 7.6 s, 14 skipped;
  privilege query again 0 rows.
- First image build: exit 1. Drogon's binder rejected a `const std::string`
  prvalue from `edit ? terms.x : std::string()` in `TaskChangeService.cc`.
  Repair: non-const copies (no behaviour change). Rebuild exit 0, 81 s.
- First full CTest: exit 8. `db_gated` passed (11.17 s, M6 block 20 included);
  `static_check` passed; `unit` failed two assertions that pinned the old
  history-key text (`TaskServiceUnitTests.h:489/491`). Repair: updated to the
  intended M6 key.
- Final run: fresh image exit 0, 84.8 s; full CTest **exit 0**, 15.2 s
  (unit 0.04 s, db_gated 11.82 s not skipped, static_check 0.02 s; 3/3). Each
  db_gated run added 13 proposals, 13 decisions and 8 closures (26/26/16 after
  two runs), confirming block 20 executed.
- SQL compatibility on the applied schema: `task_ledger_test.sql` 35 groups as
  `usapp_api` (application) and `postgres` (maintenance), and
  `wallet_ledger_test.sql` 28 groups in both roles; all exit 0 and rolled back.
- Startup task policy: expected 537 checks after 0013 (4 identity + 7 objects x
  38 + 3 x 99 columns). This reproduces 333 for the M5 objects. **Not observed;**
  no API runtime started.

Permanent usapp_test fixture growth per successful db_gated run (isolated
`@m3-fixture.test` spaces, no reset): 6 spaces, 13 proposals, 13 decisions, 8
closures, successor tasks and settlements from the cases in Section 4 (Tests).

Not done in this verification step: main access, deployment, browser checks, Git
publication (main rollout follows in Section 5.1).

### 5.1 Main rollout (2026-10-25, Claude Code, owner-delegated guarded workflow)

- Preflight: local file set 0000-0013 and 0013 hash equal the frozen value. Main
  DryRun exit 0, 5.1 s: 0000-0012 skipped with matching checksums, only
  `0013_task_changes` pending (sha256 `7e4855a9...8de`).
- Rollback artifacts: `backups/before-m6-20261025_204621/` (15.5 s; no
  overwrite, nothing deleted).
  - `usapp_before_0013.dump`: 325912 bytes, SHA-256
    `ceb32bbb17584194b49e630ca19a05b5516f4ed062cc9b19326c6b634da828d3`.
  - Restored from the exported file into scratch database
    `usapp_m6_restore_20261025_204621` (TEMPLATE template0, `--exit-on-error
    --single-transaction`). All 17 public base tables matched
    before/after/restore; no non-volatile change during the dump (count check,
    not row-by-row). Baseline: 7 tasks, 3 completions, 2 progress events,
    4 accounts, 3 operations, 5 entries, 23 audits, 13 migrations.
  - Running API image
    `sha256:e6b98f66f4d885b37dcc81b7106d57005d4a5d14cff1bbb9cda78f055e1b9ced`
    (the M5-C image) tagged `usapp-api:before-m6-20261025_204621`; the tag was
    verified by inspect.
  - Served `/srv/web` copied to `web-dist/` (19 files, 250756 bytes; index.html
    `ed73dc7a...d377`, sw.js `b698083f...7e19`, the M5-C values).
  - `rollback-info.json` SHA-256
    `883c3ed008f5ae1cf8034728c8c91101d0a5746130dcbda0190299d9aadf2f54`
    (records the retained container `/tmp` files and the host work directory).
- Migration: an immediate pre-apply DryRun again showed only 0013. Apply +
  InitRoles exit 0, 10.4 s (`[ok] 0013`). Privilege deviation query 0 rows.
  Repeat InitRoles exit 0, 7.6 s, 14 skipped; deviation query 0 rows. Main
  checksum for 0013 equals the frozen value.
- Backend: `docker compose build api` exit 0, 9.1 s (the builder layer from the
  verified test build was reused). New image
  `sha256:812506c9e9a9bdef1e00d7c58d370e435128eac2325a7778e35223d0604c66be`
  (created 12:49:13 UTC). `up -d --no-deps --no-build --wait --wait-timeout 120
  api` exit 0, 9.2 s; the running image matches and the container is healthy.
  Fresh startup log: wallet `ok=true checks=187 mismatches=0 missing=0
  unexpected=0`; task `ok=true checks=537 mismatches=0 missing=0 unexpected=0`.
- Web: `corepack.cmd pnpm run build` (vue-tsc, tsc, vite) exit 0, 24.3 s;
  precache 18 entries (188.06 KiB); dist 19 files, 264264 bytes. Caddy-served
  index.html `40f91c247069b01400f388c33dd3520a8bbe5822ba03da99b2fbecaaf1404aa1`
  and sw.js `a87f560e34904fc356c4ee8bc63bf634794a9bb88c83d1ad4e3f308b6743ccdc`
  equal the dist.
- Not done: live edit/cancel workflows on main, browser/PWA update acceptance,
  rollback exercise, Git publication. No formal M6 acceptance is recorded.
- Rollback limits: redeploying `usapp-api:before-m6-20261025_204621` and the
  saved `web-dist/` restores code only. 0013 stays applied. Once a closure
  exists, the M5-C runtime shows closed tasks as open, and their actions fail at
  UX006/UX007 (500). No settlement, ledger entry or audit is reversed.

## 6. Limits and risks

- The single-completion lock order changed (M3 path); covered by D2 and the M3-C
  regression.
- An older runtime on a 0013 database shows closed tasks as open; their actions
  fail at UX006/UX007 (500). Rollback is degraded once closures exist.
- The "zero progress" edit rule may feel strict for long count tasks;
  cancellation plus republish is the supported route.
